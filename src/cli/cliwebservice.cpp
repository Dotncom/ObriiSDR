#include "cliwebservice.h"

#include "iqbuffer.h"
#include "remotevfoservice.h"
#include "remotewebserver.h"

#include <QHostAddress>

#include <cmath>

namespace {
const QString PeerPrefix = QStringLiteral("cli-peer:");

bool validBlock(const float *iq, std::size_t count, double rate, double center) {
    return iq && count > 0 && count <= IqBuffer::MaxSnapshotComplexSamples &&
           std::isfinite(rate) && rate > 0.0 && std::isfinite(center);
}
}

CliWebService::CliWebService(QObject *parent) : QObject(parent) {}

CliWebService::~CliWebService() {
    stop();
}

bool CliWebService::start(const QString &address, quint16 portValue) {
    const QString trimmed = address.trimmed();
    QHostAddress parsed;
    // RemoteWebServer accepts a null QHostAddress as a wildcard. Reject invalid
    // input here so a misspelled loopback address cannot expose all interfaces.
    if (!parsed.setAddress(trimmed.isEmpty() ? QStringLiteral("127.0.0.1") : trimmed)) {
        status_ = QStringLiteral("Browser server requires a numeric bind address");
        return false;
    }
    const QString normalized = parsed.toString();
    if (isListening() && address_ == normalized &&
        (requestedPort_ == portValue || port() == portValue)) return true;

    stop();
    service_ = std::make_unique<RemoteVfoService>();
    service_->setSourceState(sourceSettings_, running_);
    connect(service_.get(), &RemoteVfoService::jsonFrameReady, this,
            [this](const QString &owner, const QJsonObject &frame) {
        if (owner.startsWith(PeerPrefix)) emit jsonFrameReady(owner.mid(PeerPrefix.size()), frame);
    });
    connect(service_.get(), &RemoteVfoService::binaryFrameReady, this,
            [this](const QString &owner, const QJsonObject &frame, const QByteArray &payload) {
        if (owner.startsWith(PeerPrefix))
            emit binaryFrameReady(owner.mid(PeerPrefix.size()), frame, payload);
    });
    web_ = std::make_unique<RemoteWebServer>(service_.get());
    if (!web_->start(normalized, portValue)) {
        const QString error = web_->statusText();
        stop();
        status_ = error;
        return false;
    }
    address_ = normalized;
    requestedPort_ = portValue;
    status_ = web_->statusText();
    return true;
}

void CliWebService::stop() {
    // Destroy the gateway first: it closes browser endpoints on the live broker.
    web_.reset();
    service_.reset();
    address_.clear();
    requestedPort_ = 0;
    status_ = QStringLiteral("Browser server stopped");
    // Shared IQ history belongs to the acquisition/graph owner, not this service.
}

bool CliWebService::isListening() const {
    return web_ && web_->isListening();
}

quint16 CliWebService::port() const {
    return web_ ? web_->port() : 0;
}

QString CliWebService::statusText() const {
    return status_;
}

QJsonObject CliWebService::status() const {
    QJsonObject result = service_ ? service_->statusJson() : QJsonObject{
        {QStringLiteral("sessions"), 0}, {QStringLiteral("activeVfos"), 0},
        {QStringLiteral("maxVfos"), RemoteVfoService::MaxVfos},
        {QStringLiteral("receiverRunning"), running_},
        {QStringLiteral("externalCompute"), false},
        {QStringLiteral("spectrumFrames"), QStringLiteral("0")},
        {QStringLiteral("audioFrames"), QStringLiteral("0")}};
    result.insert(QStringLiteral("enabled"), isListening());
    result.insert(QStringLiteral("listening"), isListening());
    result.insert(QStringLiteral("address"), address_);
    result.insert(QStringLiteral("port"), port());
    result.insert(QStringLiteral("status"), status_);
    result.insert(QStringLiteral("needsIq"), needsIq());
    return result;
}

QJsonObject CliWebService::command(const QJsonObject &request) {
    const QString action = request.value(QStringLiteral("action")).toString();
    bool okay = true;
    QString error;
    if (action == QStringLiteral("start")) {
        const QJsonValue value = request.value(QStringLiteral("port"));
        const double number = value.toDouble(-1.0);
        if (!value.isDouble() || !std::isfinite(number) || number < 0.0 ||
            number > 65535.0 || std::floor(number) != number ||
            (request.contains(QStringLiteral("address")) &&
             !request.value(QStringLiteral("address")).isString())) {
            okay = false;
            error = QStringLiteral("Expected a numeric bind address and an integer port in 0..65535");
        } else {
            okay = start(request.value(QStringLiteral("address")).toString(),
                         static_cast<quint16>(number));
            if (!okay) error = statusText();
        }
    } else if (action == QStringLiteral("stop")) {
        stop();
    } else if (action != QStringLiteral("status")) {
        okay = false;
        error = QStringLiteral("Unsupported web service action");
    }
    QJsonObject result = status();
    result.insert(QStringLiteral("ok"), okay);
    result.insert(QStringLiteral("action"), action);
    if (request.contains(QStringLiteral("requestId")))
        result.insert(QStringLiteral("requestId"), request.value(QStringLiteral("requestId")));
    if (!error.isEmpty()) result.insert(QStringLiteral("error"), error);
    return result;
}

void CliWebService::setSourceState(const RadioSettings &settings, bool running) {
    sourceSettings_ = settings;
    running_ = running;
    if (service_) service_->setSourceState(settings, running);
}

bool CliWebService::needsIq() const {
    return service_ && running_ && service_->activeVfoCount() > 0;
}

bool CliWebService::submitIq(const float *iq, std::size_t count, bool alreadyPublished,
                             std::uint64_t expectedEpoch) {
    if (alreadyPublished || !needsIq() ||
        !validBlock(iq, count, sourceSettings_.sampleRate, sourceSettings_.centerFrequency)) return false;
    IqBuffer::BlockMetadata metadata;
    metadata.valid = true;
    metadata.centerFrequencyHz = sourceSettings_.centerFrequency;
    IqBuffer::setSampleRateEstimate(sourceSettings_.sampleRate);
    return IqBuffer::publish(iq, count * 2, false, true, expectedEpoch, &metadata);
}

bool CliWebService::submitSourceIq(const DspSourceIdentity &source, const float *iq,
                                   std::size_t count, double rate, double center,
                                   bool alreadyPublished, bool discontinuity,
                                   const DspIqTimingMetadata *timing) {
    if (alreadyPublished || !needsIq() || !source.isValid() || source.isPrimary() ||
        !validBlock(iq, count, rate, center)) return false;
    return DspIqSourceRegistry::instance().publish(source, iq, count * 2, rate, center,
                                                  discontinuity, timing);
}

bool CliWebService::handleRemoteCommand(const QString &peerId, const QJsonObject &request) {
    if (!service_ || peerId.trimmed().isEmpty() ||
        request.value(QStringLiteral("type")).toString() != QStringLiteral("remoteVfo")) return false;
    service_->handleCommand(PeerPrefix + peerId, request);
    return true;
}

void CliWebService::closeRemoteEndpoint(const QString &peerId) {
    if (service_) service_->closeEndpoint(PeerPrefix + peerId);
}
