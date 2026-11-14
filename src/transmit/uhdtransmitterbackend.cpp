#include "uhdtransmitterbackend.h"

#include "../backends/uhdbackend.h"
#include "../network/networkcontroller.h"

#include <QDebug>
#include <QUuid>
#include <QtEndian>

namespace {
bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}
}

UhdTransmitterBackend::UhdTransmitterBackend(bool remote) : remote_(remote) {
    pumpTimer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&pumpTimer_, &QTimer::timeout, &pumpTimer_, [this] { pumpRemote(); });
}

UhdTransmitterBackend::~UhdTransmitterBackend() { stop(); }

QString UhdTransmitterBackend::id() const {
    return remote_ ? QStringLiteral("uhd-remote") : QStringLiteral("uhd-native");
}

QString UhdTransmitterBackend::displayName() const {
    return remote_ ? QStringLiteral("USRP UHD (network)") : QStringLiteral("USRP UHD (local)");
}

bool UhdTransmitterBackend::isRunning() const {
    if (sharedSession_) return sharedSession_->transmitStatus().active;
    return running_.load();
}

void UhdTransmitterBackend::setSharedSessionProvider(std::function<std::shared_ptr<UhdRxSession>(int)> provider) {
    sharedSessionProvider_ = std::move(provider);
}

void UhdTransmitterBackend::setNetwork(NetworkController *network) {
    if (network_ == network) return;
    stop();
    QObject::disconnect(networkReply_);
    network_ = network;
    if (!network_) return;
    networkReply_ = QObject::connect(network_, &NetworkController::controlCommandReceived, &pumpTimer_,
        [this](const QJsonObject &reply) {
            if (!isRunning() || reply.value("action") != QLatin1String("uhdTxStreamReply") ||
                reply.value("session").toString() != session_) return;
            if (!reply.value("ok").toBool()) {
                failRemote(reply.value("reason").toString());
                return;
            }
            if (reply.value("state") == QLatin1String("started")) {
                remoteReady_ = true;
                creditTimeNs_ = timer_.nsecsElapsed();
                return;
            }
            if (!finishing_ || reply.value("state") != QLatin1String("completed")) {
                failRemote(QStringLiteral("The remote UHD TX session stopped"));
                return;
            }
            running_ = false;
            pumpTimer_.stop();
            session_.clear();
        });
}

bool UhdTransmitterBackend::start(const TxConfiguration &configuration, QString *error) {
    if (isRunning()) return fail(error, QStringLiteral("UHD TX is already active"));
    stop();
    rate_ = configuration.sampleRate;
    if (rate_ < 8000 || rate_ > UhdIqTx::DeviceRate)
        return fail(error, QStringLiteral("UHD TX requires a baseband rate of 8 to 1920 kS/s"));
    if (configuration.frequencyHz < 70e6 || configuration.frequencyHz > 6e9)
        return fail(error, QStringLiteral("USRP B210 TX frequency must be between 70 MHz and 6 GHz"));
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        error_.clear();
    }
    queueLimit_ = configuration.liveSource ? std::size_t(rate_) * 2 : 20000000;
    finishing_ = false;
    outputEnabled_ = true;
    cancel_ = false;
    if (!remote_ && sharedSessionProvider_) {
        if (auto session = sharedSessionProvider_(configuration.deviceIndex)) {
            const QString antenna = configuration.uhdAntenna.isEmpty()
                ? QStringLiteral("TX/RX") : configuration.uhdAntenna;
            if (!session->startTransmitIq(configuration.uhdChannel, rate_, configuration.frequencyHz,
                                          configuration.uhdGainDb, antenna, error, queueLimit_)) return false;
            sharedSession_ = std::move(session);
            qInfo() << "[UHD TX] sharing active RX session" << "channel" << configuration.uhdChannel
                    << "frequencyHz" << configuration.frequencyHz << "sourceRate" << rate_;
            return true;
        }
    }
    running_ = true;
    if (remote_) {
        if (!network_ || !network_->isControlReady() || !network_->clientHasControl() ||
            network_->mode() != NetworkMode::Client || configuration.uhdArmCode.isEmpty()) {
            running_ = false;
            return fail(error, QStringLiteral("Remote UHD TX requires a connected server, control rights and a new arm code"));
        }
        session_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const bool submitted = network_->sendControlCommand({{"type", "control"}, {"action", "uhdTxStream"},
            {"operation", "start"}, {"session", session_}, {"rfConfirm", true},
            {"code", configuration.uhdArmCode}, {"frequencyHz", configuration.frequencyHz},
            {"gainDb", configuration.uhdGainDb}, {"channel", configuration.uhdChannel},
            {"antenna", configuration.uhdAntenna}, {"sampleRate", rate_}});
        if (!submitted) {
            stop();
            return fail(error, QStringLiteral("Could not start remote UHD TX"));
        }
        timer_.start();
        keepaliveMs_ = creditTimeNs_ = 0;
        finishSentMs_ = -1;
        remoteReady_ = false;
        credit_ = rate_ * 0.18;
        pumpTimer_.start(10);
        return true;
    }

    queue_.reset(queueLimit_);
    try {
        worker_ = std::thread([this, configuration] {
            UhdTxSession device;
            QString error;
            std::uint64_t sent = 0;
            qInfo().noquote() << "[UHD TX] local start" << "channel" << configuration.uhdChannel
                              << "frequencyHz" << QString::number(configuration.frequencyHz, 'f', 0)
                              << "gainDb" << configuration.uhdGainDb
                              << "antenna" << configuration.uhdAntenna
                              << "sourceRate" << rate_;
            if (device.open(configuration.deviceIndex, configuration.uhdChannel, UhdIqTx::DeviceRate,
                    configuration.frequencyHz, configuration.uhdGainDb, 0.0,
                    configuration.uhdAntenna, configuration.uhdFpgaImage, &error)) {
                sent = UhdIqTx::run(queue_, rate_, 60, cancel_,
                    [&](const auto *iq, std::size_t count, bool start, QString *why) {
                        return device.send(iq, count, start, why, 0.1);
                    }, [&](QString *why) { return device.finish(why); }, &error);
            }
            device.close();
            if (error.isEmpty()) {
                qInfo() << "[UHD TX] local end" << "sentSamples" << sent << "cancelled" << cancel_.load();
            } else {
                qWarning().noquote() << "[UHD TX] local failure" << "sentSamples" << sent << error;
            }
            {
                std::lock_guard<std::mutex> lock(errorMutex_);
                error_ = error;
            }
            running_ = false;
        });
    } catch (const std::system_error &) {
        running_ = false;
        return fail(error, QStringLiteral("Could not start UHD TX worker"));
    }
    return true;
}

bool UhdTransmitterBackend::writeIq(const UhdIqTx::Sample *samples, int count, QString *error) {
    if (!isRunning()) {
        const QString cause = this->error();
        return fail(error, cause.isEmpty() ? QStringLiteral("UHD TX has stopped") : cause);
    }
    if (finishing_) return fail(error, QStringLiteral("UHD TX is finishing"));
    if (!outputEnabled_ || count <= 0) return true;
    if (std::size_t(count) > queueLimit_ || (remote_ && std::size_t(count) > queueLimit_ - pendingSamples_))
        return fail(error, QStringLiteral("UHD TX source queue is full"));
    UhdIqTx::Block block;
    block.iq.resize(std::size_t(count));
    std::transform(samples, samples + count, block.iq.begin(), [](const auto &sample) {
        using UhdIqTx::Interpolator;
        return UhdIqTx::Sample{Interpolator::sanitize(sample.real()), Interpolator::sanitize(sample.imag())};
    });
    if (remote_) {
        pendingSamples_ += block.iq.size();
        pending_.push_back(std::move(block));
    } else if (!(sharedSession_ ? sharedSession_->queueTransmitIq(std::move(block)) : queue_.push(std::move(block)))) {
        return fail(error, QStringLiteral("UHD TX source queue is full"));
    }
    return true;
}

bool UhdTransmitterBackend::setOutputEnabled(bool enabled, QString *error) {
    if (!isRunning()) return fail(error, QStringLiteral("UHD TX has stopped"));
    if (outputEnabled_ && !enabled) {
        if (remote_) pending_.push_back({{}, true});
        else if (!(sharedSession_ ? sharedSession_->queueTransmitIq({{}, true}) : queue_.push({{}, true})))
            return fail(error, QStringLiteral("UHD TX could not end the burst"));
    }
    outputEnabled_ = enabled;
    return true;
}

void UhdTransmitterBackend::finish() {
    if (!isRunning() || finishing_) return;
    finishing_ = true;
    if (sharedSession_) sharedSession_->finishTransmitIq();
    else if (!remote_) queue_.finish();
}

void UhdTransmitterBackend::stop() {
    if (sharedSession_) {
        const auto status = sharedSession_->transmitStatus();
        sharedSession_->requestTransmitStop();
        qInfo().noquote() << "[UHD TX] shared end" << "state" << status.state
                          << "sentSamples" << status.samples << status.error;
        {
            const std::lock_guard<std::mutex> lock(errorMutex_);
            error_ = status.error;
        }
        sharedSession_.reset();
    }
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
    queue_.reset(0);
    pumpTimer_.stop();
    if (!session_.isEmpty()) sendRemote(QStringLiteral("stop"));
    session_.clear();
    pending_.clear();
    pendingSamples_ = offset_ = 0;
    running_ = false;
}

QString UhdTransmitterBackend::error() const {
    if (sharedSession_) return sharedSession_->transmitStatus().error;
    std::lock_guard<std::mutex> lock(errorMutex_);
    return error_;
}

int UhdTransmitterBackend::bufferedSourceSamples() const {
    if (sharedSession_) return int(sharedSession_->queuedTransmitSamples());
    return remote_ ? int(pendingSamples_) : int(queue_.queuedSamples());
}

bool UhdTransmitterBackend::sendRemote(const QString &operation) const {
    return network_ && network_->mode() == NetworkMode::Client && network_->isControlReady() &&
        network_->clientHasControl() && network_->sendControlCommand({{"type", "control"},
            {"action", "uhdTxStream"}, {"session", session_}, {"operation", operation}});
}

void UhdTransmitterBackend::failRemote(const QString &error) {
    stop();
    std::lock_guard<std::mutex> lock(errorMutex_);
    error_ = error.isEmpty() ? QStringLiteral("Remote UHD TX failed") : error;
}

void UhdTransmitterBackend::pumpRemote() {
    if (!network_ || !network_->isControlReady() || !network_->clientHasControl() ||
        network_->mode() != NetworkMode::Client) {
        failRemote(QStringLiteral("Remote UHD TX lost server control"));
        return;
    }
    const qint64 elapsed = timer_.elapsed();
    if (!remoteReady_) {
        if (elapsed > 3000) failRemote(QStringLiteral("UHD TX start was not acknowledged"));
        return;
    }
    if (finishSentMs_ >= 0 && elapsed - finishSentMs_ > 3000) {
        failRemote(QStringLiteral("UHD TX completion was not acknowledged"));
        return;
    }
    if (elapsed - keepaliveMs_ >= 250) {
        if (!sendRemote(QStringLiteral("keepalive"))) {
            failRemote(QStringLiteral("Could not renew the UHD TX stream"));
            return;
        }
        keepaliveMs_ = elapsed;
    }
    const qint64 nowNs = timer_.nsecsElapsed();
    credit_ = (std::min)(rate_ * 0.18, credit_ + (nowNs - creditTimeNs_) * (rate_ * 1e-9));
    creditTimeNs_ = nowNs;
    while (!pending_.empty() && network_->pendingBytes() < rate_ * 0.5) {
        auto &block = pending_.front();
        if (block.endBurst) {
            if (!sendRemote(QStringLiteral("endBurst"))) {
                failRemote(QStringLiteral("Could not end the UHD TX burst"));
                return;
            }
            pending_.pop_front();
            continue;
        }
        const auto count = (std::min)({std::size_t(credit_), block.iq.size() - offset_,
                                     std::size_t(UhdIqTx::MaximumPacketSamples)});
        if (!count) break;
        QByteArray packet(int(count * 4), Qt::Uninitialized);
        auto *bytes = reinterpret_cast<uchar *>(packet.data());
        const auto quantize = [](float sample) { return qint16(std::round(sample * 32767.0f)); };
        for (std::size_t i = 0; i < count; ++i) {
            const auto sample = block.iq[offset_ + i];
            qToLittleEndian(quantize(sample.real()), bytes);
            qToLittleEndian(quantize(sample.imag()), bytes + 2);
            bytes += 4;
        }
        if (!network_->sendBinaryCommand({{"type", "uhdTxIq"}, {"session", session_}}, packet)) {
            failRemote(QStringLiteral("Could not send UHD TX IQ"));
            return;
        }
        offset_ += count;
        pendingSamples_ -= count;
        credit_ -= count;
        if (offset_ == block.iq.size()) {
            pending_.pop_front();
            offset_ = 0;
        }
    }
    if (finishing_ && pending_.empty() && finishSentMs_ < 0) {
        if (!sendRemote(QStringLiteral("finish"))) {
            failRemote(QStringLiteral("Could not finish remote UHD TX"));
            return;
        }
        finishSentMs_ = elapsed;
    }
}
