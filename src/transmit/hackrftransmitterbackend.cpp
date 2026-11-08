#include "hackrftransmitterbackend.h"

#include "hackrfbackend.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>
#include <QDebug>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {
constexpr int HackRfSuccess = 0;

qint8 quantize(float value) {
    const float limited = (std::clamp)(value, -0.999f, 0.999f);
    return static_cast<qint8>(std::lround(limited * 127.0f));
}
}

HackRfTransmitterBackend::HackRfTransmitterBackend() = default;

HackRfTransmitterBackend::~HackRfTransmitterBackend() {
    stop();
}

QString HackRfTransmitterBackend::id() const {
    return QStringLiteral("hackrf-native");
}

QString HackRfTransmitterBackend::displayName() const {
    return QStringLiteral("HackRF native");
}

bool HackRfTransmitterBackend::isRfCapable() const {
    return true;
}

bool HackRfTransmitterBackend::start(const TxConfiguration &configuration, QString *error) {
    stop();
    if (configuration.sampleRate < 8000 || configuration.sampleRate > 2000000) {
        if (error) *error = QStringLiteral("Unsupported baseband sample rate: %1").arg(configuration.sampleRate);
        return false;
    }
    if (configuration.deviceSampleRate < 2000000 || configuration.deviceSampleRate > 20000000) {
        if (error) *error = QStringLiteral("HackRF sample rate must be between 2 and 20 Msps");
        return false;
    }
    if (configuration.frequencyHz < 1000000.0 || configuration.frequencyHz > 6000000000.0) {
        if (error) *error = QStringLiteral("HackRF TX frequency must be between 1 and 6000 MHz");
        return false;
    }

    currentConfiguration = configuration;
    currentConfiguration.txVgaGainDb = (std::clamp)(configuration.txVgaGainDb, 0, 47);
    sourcePosition = 0.0;
    sourceQueue.clear();
    bufferedSamples.store(0, std::memory_order_relaxed);
    generatedSamples.store(0, std::memory_order_relaxed);
    missingSourceSamples.store(0, std::memory_order_relaxed);
    finiteInputDrained.store(false, std::memory_order_relaxed);
    finiteEosReturned.store(false, std::memory_order_relaxed);
    finiteState.store(FiniteState::Inactive, std::memory_order_release);
    stopping.store(false, std::memory_order_release);
    outputEnabled.store(true, std::memory_order_release);

    int status = -1;
    for (int attempt = 0; attempt < 4; ++attempt) {
        status = openHackRfDeviceSafely(&device, currentConfiguration.deviceIndex);
        if (status == HackRfSuccess) break;
        if (attempt < 3) QThread::msleep(40);
    }
    if (status == HackRfSuccess) {
        status = setHackRfAmpEnabledSafely(device, false);
    }
    if (status == HackRfSuccess) {
        status = setHackRfSampleRateSafely(device, currentConfiguration.deviceSampleRate);
    }
    if (status == HackRfSuccess) {
        status = setHackRfCenterFrequencySafely(
            device, static_cast<std::uint64_t>(std::llround(currentConfiguration.frequencyHz)));
    }
    const std::uint32_t bandwidth = currentConfiguration.bandwidthHz == 0
                                        ? recommendedHackRfBandwidth(currentConfiguration.deviceSampleRate)
                                        : nearestHackRfBandwidth(currentConfiguration.bandwidthHz);
    if (status == HackRfSuccess) {
        status = setHackRfBandwidthSafely(device, bandwidth);
    }
    if (status == HackRfSuccess) {
        status = setHackRfTxVgaGainSafely(
            device, static_cast<std::uint32_t>(currentConfiguration.txVgaGainDb));
    }
    if (status == HackRfSuccess) {
        status = setHackRfTxUnderrunLimitSafely(device, currentConfiguration.txUnderrunLimit);
    }
    if (status != HackRfSuccess) {
        if (error) *error = hackRfLastErrorMessage();
        qWarning().noquote() << "[HackRFTX] configuration failed:" << hackRfLastErrorMessage();
        closeDevice();
        return false;
    }

    running.store(true, std::memory_order_release);
    qInfo().noquote() << "[HackRFTX] armed"
                      << "device" << currentConfiguration.deviceIndex
                      << "frequencyHz" << QString::number(currentConfiguration.frequencyHz, 'f', 0)
                      << "basebandRate" << currentConfiguration.sampleRate
                      << "signalBandwidthHz" << currentConfiguration.signalBandwidthHz
                      << "deviceRate" << currentConfiguration.deviceSampleRate
                      << "bandwidthHz" << bandwidth
                      << "txVgaDb" << currentConfiguration.txVgaGainDb
                      << "rfAmp" << currentConfiguration.rfAmpEnabled
                      << "txUnderrunLimit" << currentConfiguration.txUnderrunLimit;
    if (error) error->clear();
    return true;
}

bool HackRfTransmitterBackend::writeIq(const std::complex<float> *samples,
                                       int count,
                                       QString *error) {
    if (!running.load(std::memory_order_acquire) || !samples || count <= 0) {
        if (error) *error = QStringLiteral("HackRF TX is not armed or no IQ samples were provided");
        return false;
    }
    if (finiteState.load(std::memory_order_acquire) != FiniteState::Inactive) {
        if (error) *error = QStringLiteral("HackRF finite burst cannot accept additional IQ; start a new session");
        return false;
    }
    if (!outputEnabled.load(std::memory_order_acquire)) {
        if (error) error->clear();
        return true;
    }
    {
        QMutexLocker locker(&queueMutex);
        const qint64 maximumSamples =
            static_cast<qint64>(currentConfiguration.sampleRate) *
            (currentConfiguration.liveSource ? 30 : 180);
        const qint64 unread = (std::max<qint64>)(
            0, static_cast<qint64>(sourceQueue.size()) -
                   static_cast<qint64>(sourcePosition));
        if (unread + static_cast<qint64>(count) > maximumSamples) {
            if (error) *error = QStringLiteral("HackRF TX source queue exceeded the 30 second limit");
            return false;
        }
        const int previousSize = sourceQueue.size();
        sourceQueue.resize(previousSize + count);
        std::copy_n(samples, count, sourceQueue.data() + previousSize);
        bufferedSamples.store(
            (std::max)(0, sourceQueue.size() - static_cast<int>(sourcePosition)),
            std::memory_order_release);
    }
    if (!streaming.load(std::memory_order_acquire)) {
        const qint64 prebufferSamples =
            (static_cast<qint64>(currentConfiguration.sampleRate) *
             (std::clamp)(currentConfiguration.prebufferMilliseconds, 20, 2000)) / 1000;
        const bool prebuffered = !currentConfiguration.liveSource ||
                                 bufferedSourceSamples() >= prebufferSamples;
        if (prebuffered && !beginStreaming(error)) {
            return false;
        }
    }
    if (error) error->clear();
    return true;
}

bool HackRfTransmitterBackend::stageFiniteBurst(const std::complex<float> *samples,
                                               int count, QString *error) {
    QMutexLocker locker(&queueMutex);
    const qint64 maximumSamples = static_cast<qint64>(currentConfiguration.sampleRate) * 180;
    if (!running.load(std::memory_order_acquire) || streaming.load(std::memory_order_acquire) ||
        stopping.load(std::memory_order_acquire) || !outputEnabled.load(std::memory_order_acquire) ||
        currentConfiguration.liveSource || !sourceQueue.isEmpty() ||
        finiteState.load(std::memory_order_acquire) != FiniteState::Inactive ||
        !samples || count <= 0 || count > maximumSamples) {
        if (error) *error = QStringLiteral("Finite HackRF TX requires one bounded burst in a fresh, armed non-live session");
        return false;
    }
    sourceQueue.resize(count + 1);
    std::copy_n(samples, count, sourceQueue.data());
    // A zero lookahead lets the existing interpolator emit the final input sample.
    sourceQueue[count] = std::complex<float>(0.0f, 0.0f);
    sourcePosition = 0.0;
    bufferedSamples.store(count + 1, std::memory_order_release);
    finiteInputDrained.store(false, std::memory_order_relaxed);
    finiteEosReturned.store(false, std::memory_order_relaxed);
    finiteState.store(FiniteState::Pending, std::memory_order_release);
    if (error) error->clear();
    return true;
}

bool HackRfTransmitterBackend::writeFiniteBurst(const std::complex<float> *samples,
                                               int count, QString *error) {
    if (!device || !running.load(std::memory_order_acquire) ||
        streaming.load(std::memory_order_acquire) ||
        finiteState.load(std::memory_order_acquire) != FiniteState::Inactive) {
        if (error) *error = QStringLiteral("Finite HackRF TX must be staged after start and before streaming");
        return false;
    }
    if (enableHackRfTxFlushSafely(device, &HackRfTransmitterBackend::flushCallback, this) != HackRfSuccess ||
        setHackRfTxBlockCompleteCallbackSafely(device, &HackRfTransmitterBackend::blockCompleteCallback) != HackRfSuccess) {
        if (error) *error = hackRfLastErrorMessage();
        stop();
        return false;
    }
    if (!stageFiniteBurst(samples, count, error)) return false;
    if (!beginStreaming(error)) {
        finiteState.store(FiniteState::Failed, std::memory_order_release);
        return false;
    }
    return true;
}

bool HackRfTransmitterBackend::finiteBurstComplete() const {
    const auto state = finiteState.load(std::memory_order_acquire);
    return state == FiniteState::Succeeded || state == FiniteState::Failed;
}

bool HackRfTransmitterBackend::finiteBurstSucceeded() const {
    return finiteState.load(std::memory_order_acquire) == FiniteState::Succeeded;
}

bool HackRfTransmitterBackend::drain(int timeoutMs, QString *error) {
    if (!running.load(std::memory_order_acquire)) {
        if (error) *error = QStringLiteral("HackRF TX backend is not running");
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    const bool finite = finiteState.load(std::memory_order_acquire) != FiniteState::Inactive;
    while (finite ? !finiteBurstComplete() : bufferedSourceSamples() > 1) {
        if (timeoutMs >= 0 && timer.elapsed() >= timeoutMs) {
            if (error) *error = QStringLiteral("Timed out while draining the HackRF TX queue");
            return false;
        }
        QThread::msleep(5);
    }
    if (finite && !finiteBurstSucceeded()) {
        if (error) *error = QStringLiteral("HackRF finite TX failed or was cancelled before a successful flush");
        return false;
    }
    if (error) error->clear();
    return true;
}

bool HackRfTransmitterBackend::setOutputEnabled(bool enabled, QString *error) {
    if (!running.load(std::memory_order_acquire)) {
        if (error) *error = QStringLiteral("HackRF TX backend is not running");
        return false;
    }
    outputEnabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        stopping.store(true, std::memory_order_release);
        auto pending = FiniteState::Pending;
        finiteState.compare_exchange_strong(pending, FiniteState::Failed, std::memory_order_acq_rel);
        const bool wasStreaming = streaming.exchange(false, std::memory_order_acq_rel);
        if (device && wasStreaming) stopHackRfTxSafely(device);
        if (device) setHackRfAmpEnabledSafely(device, false);
        {
            QMutexLocker locker(&queueMutex);
            sourceQueue.clear();
            sourcePosition = 0.0;
            bufferedSamples.store(0, std::memory_order_release);
        }
        stopping.store(false, std::memory_order_release);
    }
    if (error) error->clear();
    return true;
}

bool HackRfTransmitterBackend::beginStreaming(QString *error) {
    if (!device || !running.load(std::memory_order_acquire)) {
        if (error) *error = QStringLiteral("HackRF TX device is not open");
        return false;
    }
    int status = setHackRfAmpEnabledSafely(device, currentConfiguration.rfAmpEnabled);
    if (status == HackRfSuccess) {
        // Finite callbacks may have consumed input even when startup fails; never replay it.
        const int attempts = finiteState.load(std::memory_order_acquire) == FiniteState::Inactive ? 4 : 1;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            status = startHackRfTxSafely(device, &HackRfTransmitterBackend::transferCallback, this);
            if (status == HackRfSuccess) break;
            if (attempt + 1 < attempts) QThread::msleep(30);
        }
    }
    if (status != HackRfSuccess) {
        setHackRfAmpEnabledSafely(device, false);
        if (error) *error = hackRfLastErrorMessage();
        qWarning().noquote() << "[HackRFTX] start failed:" << hackRfLastErrorMessage();
        running.store(false, std::memory_order_release);
        closeDevice();
        return false;
    }
    streaming.store(true, std::memory_order_release);
    qInfo().noquote() << "[HackRFTX] USB stream started";
    return true;
}

int HackRfTransmitterBackend::transferCallback(HackRfTransfer *transfer) {
    auto *backend = transfer ? static_cast<HackRfTransmitterBackend *>(transfer->txContext) : nullptr;
    return backend ? backend->fillTransfer(transfer) : -1;
}

void HackRfTransmitterBackend::blockCompleteCallback(HackRfTransfer *transfer, int success) {
    auto *backend = transfer ? static_cast<HackRfTransmitterBackend *>(transfer->txContext) : nullptr;
    if (backend && !success) {
        auto pending = FiniteState::Pending;
        backend->finiteState.compare_exchange_strong(pending, FiniteState::Failed, std::memory_order_acq_rel);
    }
}

void HackRfTransmitterBackend::flushCallback(void *context, int success) {
    auto *backend = static_cast<HackRfTransmitterBackend *>(context);
    if (!backend) return;
    const bool okay = success && backend->finiteEosReturned.load(std::memory_order_acquire) &&
                      !backend->stopping.load(std::memory_order_acquire);
    auto pending = FiniteState::Pending;
    backend->finiteState.compare_exchange_strong(
        pending, okay ? FiniteState::Succeeded : FiniteState::Failed, std::memory_order_acq_rel);
}

int HackRfTransmitterBackend::fillTransfer(HackRfTransfer *transfer) {
    if (!transfer || !transfer->buffer || transfer->bufferLength <= 0) {
        return -1;
    }
    const auto state = finiteState.load(std::memory_order_acquire);
    const bool finite = state != FiniteState::Inactive;
    if (finite && (state != FiniteState::Pending || stopping.load(std::memory_order_acquire) ||
                   finiteInputDrained.load(std::memory_order_acquire))) {
        transfer->validLength = 0;
        if (state == FiniteState::Pending && finiteInputDrained.load(std::memory_order_acquire))
            finiteEosReturned.store(true, std::memory_order_release);
        return 1;
    }
    int bytes = transfer->validLength > 0
                    ? (std::min)(transfer->validLength, transfer->bufferLength)
                    : transfer->bufferLength;
    bytes &= ~1;
    auto *output = reinterpret_cast<qint8 *>(transfer->buffer);
    const int complexSamples = bytes / 2;
    quint64 missing = 0;
    constexpr int OutputChunkSamples = 2048;
    int outputIndex = 0;
    while (outputIndex < complexSamples) {
        if (stopping.load(std::memory_order_acquire)) {
            std::memset(output + outputIndex * 2, 0,
                        static_cast<std::size_t>((complexSamples - outputIndex) * 2));
            break;
        }
        const int chunkEnd = (std::min)(complexSamples,
                                        outputIndex + OutputChunkSamples);
        QMutexLocker locker(&queueMutex);
        const double step = static_cast<double>(currentConfiguration.sampleRate) /
                            static_cast<double>(currentConfiguration.deviceSampleRate);
        for (int index = outputIndex; index < chunkEnd; ++index) {
            const int first = static_cast<int>(sourcePosition);
            const int second = first + 1;
            std::complex<float> value(0.0f, 0.0f);
            if (second < sourceQueue.size()) {
                const float fraction = static_cast<float>(sourcePosition - first);
                value = sourceQueue[first] + (sourceQueue[second] - sourceQueue[first]) * fraction;
                sourcePosition += step;
            } else if (!finite) {
                ++missing;
            }
            output[index * 2] = quantize(value.real());
            output[index * 2 + 1] = quantize(value.imag());
        }
        const int removable = (std::max)(0, static_cast<int>(sourcePosition) - 1);
        if (removable >= 32768) {
            sourceQueue.remove(0, removable);
            sourcePosition -= removable;
        }
        bufferedSamples.store(
            (std::max)(0, sourceQueue.size() - static_cast<int>(sourcePosition)),
            std::memory_order_release);
        outputIndex = chunkEnd;
    }
    transfer->validLength = bytes;
    // Hand off the final valid buffer now; EOS must be returned on the NEXT callback.
    if (finite && bufferedSamples.load(std::memory_order_acquire) <= 1)
        finiteInputDrained.store(true, std::memory_order_release);
    generatedSamples.fetch_add(static_cast<quint64>(complexSamples), std::memory_order_relaxed);
    missingSourceSamples.fetch_add(missing, std::memory_order_relaxed);
    return 0;
}

void HackRfTransmitterBackend::stop() {
    stopping.store(true, std::memory_order_release);
    auto pending = FiniteState::Pending;
    finiteState.compare_exchange_strong(pending, FiniteState::Failed, std::memory_order_acq_rel);
    const bool wasStreaming = streaming.exchange(false, std::memory_order_acq_rel);
    running.store(false, std::memory_order_release);
    QElapsedTimer stopTimer;
    stopTimer.start();
    if (device && wasStreaming) {
        stopHackRfTxSafely(device);
    }
    if (device) {
        setHackRfAmpEnabledSafely(device, false);
    }
    if (wasStreaming) {
        qInfo().noquote() << "[HackRFTX] stopped"
                          << "outputSamples" << generatedSamples.load(std::memory_order_relaxed)
                          << "zeroFillSamples" << missingSourceSamples.load(std::memory_order_relaxed)
                          << "stopMs" << stopTimer.elapsed();
    }
    closeDevice();
    QMutexLocker locker(&queueMutex);
    sourceQueue.clear();
    sourcePosition = 0.0;
    bufferedSamples.store(0, std::memory_order_release);
}

void HackRfTransmitterBackend::closeDevice() {
    if (device) {
        closeHackRfDeviceSafely(device);
        device = nullptr;
    }
}

bool HackRfTransmitterBackend::isRunning() const {
    return running.load(std::memory_order_acquire);
}

bool HackRfTransmitterBackend::streamStarted() const {
    return streaming.load(std::memory_order_acquire);
}

bool HackRfTransmitterBackend::hardwareStreamAlive() const {
    return device && isHackRfStreamingSafely(device);
}

quint64 HackRfTransmitterBackend::outputSamples() const {
    return generatedSamples.load(std::memory_order_relaxed);
}

quint64 HackRfTransmitterBackend::sourceUnderrunSamples() const {
    return missingSourceSamples.load(std::memory_order_relaxed);
}

int HackRfTransmitterBackend::bufferedSourceSamples() const {
    return bufferedSamples.load(std::memory_order_acquire);
}

const TxConfiguration &HackRfTransmitterBackend::configuration() const {
    return currentConfiguration;
}
