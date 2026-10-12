#include "dataprocessor.h"
#include "iqbuffer.h"
#include "channelizerutils.h"
#include "diagnosticlogging.h"
#include "bladerfbackend.h"
#include "fobosbackend.h"
#include "hackrfbackend.h"
#include "rtlsdrbackend.h"
#include "soapysdrbackend.h"
#include "zoomspectrumprocessor.h"

#include <algorithm>
#include <limits>
#include <vector>
#include <utility>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QTcpSocket>
#ifdef _WIN32
#include <Windows.h>
#endif

#define FLOATS_PER_IQ_SAMPLE 2

namespace {

constexpr double TWO_PI = 6.28318530717958647692;
constexpr bool FORCE_SYNC_READER = false;
constexpr double SYNC_BLOCKS_PER_SECOND = 20.0;
constexpr double ASYNC_BLOCKS_PER_SECOND = 400.0;
constexpr uint32_t MIN_SYNC_BLOCK_SAMPLES = 65536;
constexpr uint32_t MAX_SYNC_BLOCK_SAMPLES = 4000000;
constexpr uint32_t MIN_ASYNC_BLOCK_SAMPLES = 32768;
constexpr uint32_t MAX_ASYNC_BLOCK_SAMPLES = 262144;
constexpr int NETWORK_CHANNEL_FRAME_SAMPLES = 8192;
constexpr float NETWORK_IQ_QUANTIZE_GAIN = 16.0f;
constexpr float NETWORK_CHANNEL_IQ_TARGET_LEVEL = 0.45f;
constexpr float HF_NOISE_CANCEL_MAX_COEFF = 2.5f;
constexpr double SAMPLE_RATE_DIAGNOSTIC_RELATIVE_WARN = 0.03;
constexpr int SAMPLE_RATE_DIAGNOSTIC_INITIAL_REPORTS = 4;
constexpr int DMR_CHANNEL_CIC_STAGES = 2;
constexpr qint64 STREAM_DIAGNOSTIC_REPORT_MS = 3000;
constexpr qint64 STREAM_DIAGNOSTIC_INITIAL_REPORT_MS = 700;
constexpr uint32_t STREAM_DIAGNOSTIC_MAX_INSPECT_SAMPLES = 4096;
constexpr int RETUNE_RAW_DUMP_MAX_BLOCKS = 12;
constexpr uint32_t RETUNE_RAW_DUMP_MAX_INSPECT_SAMPLES = 8192;
constexpr int RTL_TCP_ERR_CONNECT = -12001;
constexpr int RTL_TCP_ERR_CONFIGURE = -12002;
constexpr int RTL_TCP_ERR_READ = -12003;
constexpr int RTLSDR_NATIVE_ERR_OPEN = -13101;
constexpr int RTLSDR_NATIVE_ERR_CONFIGURE = -13102;
constexpr uint32_t RTLSDR_NATIVE_BLOCK_BYTES = 131072;
constexpr int SOAPY_SDR_ERR_OPEN = -14101;
constexpr int SOAPY_SDR_ERR_CONFIGURE = -14102;
constexpr int SOAPY_SDR_ERR_STREAM = -14103;
constexpr int SOAPY_SDR_TIMEOUT = -1;
constexpr int BLADERF_NATIVE_ERR_OPEN = -15101;
constexpr int BLADERF_NATIVE_ERR_CONFIGURE = -15102;
constexpr int BLADERF_NATIVE_ERR_STREAM = -15103;
constexpr uint32_t BLADERF_SYNC_TIMEOUT_MS = 250;
constexpr int HACKRF_NATIVE_ERR_OPEN = -16101;
constexpr int HACKRF_NATIVE_ERR_CONFIGURE = -16102;
constexpr int HACKRF_NATIVE_ERR_STREAM = -16103;

QVector<HackRfOperaCakeFrequencyRange> parseHackRfOperaCakeRanges(const QString &json) {
    QVector<HackRfOperaCakeFrequencyRange> ranges;
    const QJsonArray array = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        const int minimum = (std::clamp)(object.value(QStringLiteral("minMhz")).toInt(), 0, 65535);
        const int maximum = (std::clamp)(object.value(QStringLiteral("maxMhz")).toInt(), 0, 65535);
        const int port = (std::clamp)(object.value(QStringLiteral("port")).toInt(), 0, 7);
        if (maximum > minimum) {
            ranges.append({static_cast<std::uint16_t>(minimum),
                           static_cast<std::uint16_t>(maximum),
                           static_cast<std::uint8_t>(port)});
        }
    }
    return ranges;
}

QVector<HackRfOperaCakeDwell> parseHackRfOperaCakeDwells(const QString &json) {
    QVector<HackRfOperaCakeDwell> dwells;
    const QJsonArray array = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        const qint64 samples = (std::max)(qint64(0),
                                         object.value(QStringLiteral("samples")).toVariant().toLongLong());
        const int port = (std::clamp)(object.value(QStringLiteral("port")).toInt(), 0, 7);
        if (samples > 0) {
            dwells.append({static_cast<std::uint32_t>((std::min)(samples, qint64(UINT32_MAX))),
                           static_cast<std::uint8_t>(port)});
        }
    }
    return dwells;
}

bool writeRtlTcpCommand(QTcpSocket &socket, quint8 command, quint32 parameter) {
    char packet[5] = {};
    packet[0] = static_cast<char>(command);
    packet[1] = static_cast<char>((parameter >> 24) & 0xff);
    packet[2] = static_cast<char>((parameter >> 16) & 0xff);
    packet[3] = static_cast<char>((parameter >> 8) & 0xff);
    packet[4] = static_cast<char>(parameter & 0xff);
    if (socket.write(packet, sizeof(packet)) != sizeof(packet)) {
        return false;
    }
    return socket.waitForBytesWritten(1000);
}

bool shouldReportMeasuredSampleRate(double configuredRate, double measuredRate, int reportCount) {
    if (!std::isfinite(configuredRate) || configuredRate <= 0.0 ||
        !std::isfinite(measuredRate) || measuredRate <= 0.0) {
        return false;
    }
    const double relativeError = std::abs(measuredRate - configuredRate) / configuredRate;
    return reportCount < SAMPLE_RATE_DIAGNOSTIC_INITIAL_REPORTS ||
           relativeError >= SAMPLE_RATE_DIAGNOSTIC_RELATIVE_WARN;
}

QString safeFileToken(QString value) {
    value = value.trimmed();
    if (value.isEmpty()) {
        return QStringLiteral("retune");
    }
    value = value.left(48);
    for (int i = 0; i < value.size(); ++i) {
        const QChar ch = value.at(i);
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('-') && ch != QLatin1Char('_')) {
            value[i] = QLatin1Char('_');
        }
    }
    while (value.contains(QStringLiteral("__"))) {
        value.replace(QStringLiteral("__"), QStringLiteral("_"));
    }
    return value;
}

char quantizeIqSample(float sample) {
    if (!std::isfinite(sample)) {
        sample = 0.0f;
    }
    const float scaled = (std::clamp)(sample * NETWORK_IQ_QUANTIZE_GAIN, -1.0f, 1.0f) * 127.0f;
    int value = scaled >= 0.0f
                    ? static_cast<int>(scaled + 0.5f)
                    : static_cast<int>(scaled - 0.5f);
    value = (std::clamp)(value, -128, 127);
    return static_cast<char>(value);
}

void appendInt16Le(QByteArray &buffer, float sample) {
    if (!std::isfinite(sample)) {
        sample = 0.0f;
    }
    const float clamped = (std::clamp)(sample, -1.0f, 1.0f);
    const auto value = static_cast<qint16>(std::lrint(clamped * 32767.0f));
    buffer.append(static_cast<char>(value & 0xff));
    buffer.append(static_cast<char>((value >> 8) & 0xff));
}

float estimateHfNoiseCancelCoefficient(const float *samples, std::size_t iqSamples) {
    if (!samples || iqSamples <= 8) {
        return 0.0f;
    }

    double sumMain = 0.0;
    double sumRef = 0.0;
    double sumCross = 0.0;
    double sumRefSquared = 0.0;
    std::size_t count = 0;

    for (std::size_t n = 0; n < iqSamples; ++n) {
        const float mainSample = samples[2 * n];
        const float refSample = samples[2 * n + 1];
        if (!std::isfinite(mainSample) || !std::isfinite(refSample)) {
            continue;
        }
        sumMain += mainSample;
        sumRef += refSample;
        sumCross += static_cast<double>(mainSample) * refSample;
        sumRefSquared += static_cast<double>(refSample) * refSample;
        ++count;
    }

    if (count <= 8) {
        return 0.0f;
    }

    const double meanMain = sumMain / static_cast<double>(count);
    const double meanRef = sumRef / static_cast<double>(count);
    const double covariance = sumCross - static_cast<double>(count) * meanMain * meanRef;
    const double refVariance = sumRefSquared - static_cast<double>(count) * meanRef * meanRef;
    if (!std::isfinite(covariance) || !std::isfinite(refVariance) || refVariance <= 1.0e-12) {
        return 0.0f;
    }

    return (std::clamp)(static_cast<float>(covariance / refVariance),
                        -HF_NOISE_CANCEL_MAX_COEFF,
                        HF_NOISE_CANCEL_MAX_COEFF);
}

std::complex<float> clampComplexMagnitude(std::complex<float> value, float maxMagnitude) {
    const float magnitude = std::abs(value);
    if (!std::isfinite(magnitude) || magnitude <= maxMagnitude || magnitude <= 0.0f) {
        return value;
    }
    return value * (maxMagnitude / magnitude);
}

uint32_t syncBlockSamplesForRate(double sampleRate) {
    if (sampleRate <= 0.0) {
        return 32768;
    }
    const double samplesPerBlock = sampleRate / SYNC_BLOCKS_PER_SECOND;
    return (std::clamp)(static_cast<uint32_t>(samplesPerBlock),
                        MIN_SYNC_BLOCK_SAMPLES,
                        MAX_SYNC_BLOCK_SAMPLES);
}

uint32_t asyncBlockSamplesForRate(double sampleRate) {
    if (sampleRate <= 0.0) {
        return 32768;
    }
    const double samplesPerBlock = sampleRate / ASYNC_BLOCKS_PER_SECOND;
    return (std::clamp)(static_cast<uint32_t>(samplesPerBlock),
                        MIN_ASYNC_BLOCK_SAMPLES,
                        MAX_ASYNC_BLOCK_SAMPLES);
}

uint32_t asyncBufferCountForRate(double sampleRate) {
    if (sampleRate >= 50000000.0) {
        return 16;
    }
    if (sampleRate >= 25000000.0) {
        return 24;
    }
    return 32;
}

} // namespace

DataProcessor::DataProcessor(QObject *parent)
    : QThread(parent),
      running(false),
      activeSyncMode(false),
      requestedSyncMode(false),
      requestedQueueAudioBlocks(false),
      requestedPublishIqSnapshot(false),
      requestedEmitIqFrames(false),
      requestedChannelizeIqFrames(false),
      requestedAgileScanEnabled(false),
      networkIqResetRequested(false),
      iqRetuneEpoch(1),
      requestedSampleRate(0.0),
      requestedCenterFrequency(0.0),
      frequencyCalibrationOffsetHz(0.0),
      activeDevice(nullptr),
      activeApiKind(FobosApiKind::Standard),
      activeBackendId(QStringLiteral("fobos-standard")),
      activeBackendName(QStringLiteral("Fobos SDR")),
      activeStreamDescriptor(),
      zoomProcessor(std::make_shared<ZoomSpectrumProcessor>()),
      totalCallbackCounter(0) {
}

DataProcessor::~DataProcessor() {
    requestStop();
    if (QThread::isRunning() && !QThread::wait(1500)) {
        forceStop(1000);
    }
    finalizeStopped();
}

void DataProcessor::startProcessing(void *device,
                                    FobosApiKind apiKind,
                                    bool syncEnabled,
                                    double sampleRate,
                                    bool queueAudioBlocks,
                                    bool publishIqSnapshot,
                                    bool emitIqFrames,
                                    bool agileScanEnabled) {
    const ReceiverStreamDescriptor stream = makeFobosStreamDescriptor(device,
                                                                      apiKind,
                                                                      syncEnabled,
                                                                      sampleRate,
                                                                      0.0,
                                                                      queueAudioBlocks,
                                                                      publishIqSnapshot,
                                                                      emitIqFrames,
                                                                      agileScanEnabled);
    startProcessing(stream);
}

void DataProcessor::startProcessing(const ReceiverStreamDescriptor &stream) {
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] startProcessing enter"
                 << "backend" << stream.backendId
                 << "backendName" << stream.backendName
                 << "streamKind" << static_cast<int>(stream.kind)
                 << "device" << stream.nativeDevice
                 << "apiKind" << static_cast<int>(stream.fobosApiKind)
                 << "syncEnabled" << stream.syncReader
                 << "sampleRate" << stream.sampleRateHz
                 << "queueAudioBlocks" << stream.queueAudioBlocks
                 << "publishIqSnapshot" << stream.publishIqSnapshot
                 << "emitIqFrames" << stream.emitIqFrames
                 << "agileScanEnabled" << stream.agileScanEnabled
                 << "threadRunning" << QThread::isRunning()
                 << "runningFlag" << running.load();
    }
    if (QThread::isRunning()) {
        qDebug() << "Warning: DataProcessor is already running.";
        return;
    }
    const bool nativeDeviceRequired =
        stream.kind == ReceiverBackendStreamKind::FobosStandard ||
        stream.kind == ReceiverBackendStreamKind::FobosAgile;
    if (nativeDeviceRequired && !stream.nativeDevice) {
        qDebug() << "Cannot start DataProcessor without an active device.";
        return;
    }
    const bool useSyncReader = FORCE_SYNC_READER || stream.syncReader;
    activeDevice = stream.nativeDevice;
    activeApiKind = stream.fobosApiKind;
    activeStreamKind = stream.kind;
    activeBackendId = stream.backendId;
    activeBackendName = stream.backendName;
    activeStreamDescriptor = stream;
    const uint64_t streamEpoch = iqRetuneEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    requestedSampleRate = stream.sampleRateHz;
    requestedCenterFrequency = stream.centerFrequencyHz;
    frequencyCalibrationOffsetHz = std::isfinite(stream.frequencyCalibrationOffsetHz)
                                       ? stream.frequencyCalibrationOffsetHz
                                       : 0.0;
    requestedQueueAudioBlocks = stream.queueAudioBlocks;
    requestedPublishIqSnapshot = stream.publishIqSnapshot;
    requestedEmitIqFrames = stream.emitIqFrames;
    requestedAgileScanEnabled = stream.agileScanEnabled;
    networkIqResetRequested = true;
    asyncCancelRequested = false;
    requestedSyncMode = useSyncReader;
    activeSyncMode = useSyncReader;
    totalCallbackCounter = 0;
    asyncRateReportCount = 0;
    resetStreamDiagnostics();
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] IQ stream epoch"
                 << "epoch" << static_cast<qulonglong>(streamEpoch)
                 << "backend" << activeBackendId
                 << "center" << stream.centerFrequencyHz
                 << "sampleRate" << stream.sampleRateHz;
    }
    if (FORCE_SYNC_READER) {
        qDebug() << "Using SDR++-style sync reader for sample rate:" << stream.sampleRateHz;
    } else if (fobosVerboseLoggingEnabled()) {
        if (stream.syncReader) {
            qDebug() << "Using sync reader for sample rate:" << stream.sampleRateHz;
        } else {
            qDebug() << "Using async reader for sample rate:" << stream.sampleRateHz;
        }
    }
    running = true;
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] QThread::start begin";
    }
    QThread::start();
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] QThread::start end" << "threadRunning" << QThread::isRunning();
    }
}

void DataProcessor::run() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] run enter";
    }
    void *readerDevice = activeDevice.load();
    const FobosApiKind readerApiKind = activeApiKind.load();
    const ReceiverBackendStreamKind readerStreamKind = activeStreamKind;
    const QString readerBackendId = activeBackendId;
    const QString readerBackendName = activeBackendName;
    const ReceiverStreamDescriptor readerStream = activeStreamDescriptor;
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] run state"
                 << "backend" << readerBackendId
                 << "backendName" << readerBackendName
                 << "streamKind" << static_cast<int>(readerStreamKind)
                 << "readerDevice" << readerDevice
                 << "apiKind" << static_cast<int>(readerApiKind)
                 << "runningFlag" << running.load()
                 << "requestedSyncMode" << requestedSyncMode.load()
                 << "queueAudioBlocks" << requestedQueueAudioBlocks.load()
                 << "publishIqSnapshot" << requestedPublishIqSnapshot.load()
                 << "emitIqFrames" << requestedEmitIqFrames.load()
                 << "agileScanEnabled" << requestedAgileScanEnabled.load()
                 << "requestedSampleRate" << requestedSampleRate.load();
    }
    const bool nativeDeviceRequired =
        readerStreamKind == ReceiverBackendStreamKind::FobosStandard ||
        readerStreamKind == ReceiverBackendStreamKind::FobosAgile;
    if (!running.load() || (nativeDeviceRequired && !readerDevice)) {
        qDebug() << "Cannot start DataProcessor without an active device.";
        running = false;
        return;
    }
    if (requestedQueueAudioBlocks.load() || requestedPublishIqSnapshot.load()) {
        IqBuffer::clear(iqRetuneEpoch.load(std::memory_order_acquire));
    }
    const bool useSyncReader = FORCE_SYNC_READER || requestedSyncMode.load();
    const double sampleRate = requestedSampleRate.load();
    uint32_t readBlockSamples = useSyncReader
                                    ? syncBlockSamplesForRate(sampleRate)
                                    : asyncBlockSamplesForRate(sampleRate);
    if (readerStreamKind == ReceiverBackendStreamKind::RtlTcp) {
        readBlockSamples = asyncBlockSamplesForRate(sampleRate);
        activeSyncMode = false;
        runRtlTcpReader(readerStream, readBlockSamples);
        return;
    }
    if (readerStreamKind == ReceiverBackendStreamKind::RtlSdrNative) {
        readBlockSamples = asyncBlockSamplesForRate(sampleRate);
        activeSyncMode = false;
        runRtlSdrNativeReader(readerStream, readBlockSamples);
        return;
    }
    if (readerStreamKind == ReceiverBackendStreamKind::SoapySdr) {
        readBlockSamples = asyncBlockSamplesForRate(sampleRate);
        activeSyncMode = false;
        runSoapySdrReader(readerStream, readBlockSamples);
        return;
    }
    if (readerStreamKind == ReceiverBackendStreamKind::BladeRfNative) {
        readBlockSamples = asyncBlockSamplesForRate(sampleRate);
        activeSyncMode = false;
        runBladeRfNativeReader(readerStream, readBlockSamples);
        return;
    }
    if (readerStreamKind == ReceiverBackendStreamKind::HackRfNative) {
        readBlockSamples = asyncBlockSamplesForRate(sampleRate);
        activeSyncMode = false;
        runHackRfNativeReader(readerStream, readBlockSamples);
        return;
    }
    if (readerApiKind == FobosApiKind::Agile && requestedAgileScanEnabled.load()) {
        readBlockSamples = (std::max)(readBlockSamples, MIN_SYNC_BLOCK_SAMPLES);
    }
    uint32_t asyncBufferCount = asyncBufferCountForRate(sampleRate);
    if (!useSyncReader &&
        readerApiKind == FobosApiKind::Agile &&
        !requestedAgileScanEnabled.load()) {
        // uSDR keeps the agile normal-RF reader on fixed async parameters.
        // Matching that avoids unnecessary USB transfer topology changes.
        readBlockSamples = 262144;
        asyncBufferCount = 32;
    }
    activeSyncMode = useSyncReader;
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] selected reader"
                 << "backend" << readerBackendId
                 << "useSyncReader" << useSyncReader
                 << "forceSync" << FORCE_SYNC_READER
                 << "buffers" << asyncBufferCount
                 << "readBlockSamples" << readBlockSamples
                 << "agileScanEnabled" << requestedAgileScanEnabled.load();
    }

    if (!useSyncReader) {
        IqBuffer::setSampleRateEstimate(sampleRate);
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DataProcessor] fobos_rx_read_async begin"
                     << "device" << readerDevice
                     << "buffers" << asyncBufferCount
                     << "blockSamples" << readBlockSamples;
        }
        asyncMeasuredSamples = 0;
        asyncCallbackCounter = 0;
        asyncRateTimer.restart();
        int ret = FOBOS_ERR_NOT_OPEN;
        if (readerApiKind == FobosApiKind::Agile) {
            ret = readFobosAgileAsyncSafely(static_cast<fobos_sdr_dev_t*>(readerDevice),
                                            [](float *buf, uint32_t buf_length, fobos_sdr_dev_t *dev, void *ctx) {
                                                auto *processor = static_cast<DataProcessor*>(ctx);
                                                const int scanIndex = processor->wantsAgileScanMetadata()
                                                                          ? getFobosAgileScanIndexSafely(dev)
                                                                          : -1;
                                                processor->handleData(buf, buf_length, scanIndex);
                                            },
                                            this,
                                            asyncBufferCount,
                                            readBlockSamples);
        } else {
            ret = readFobosAsyncSafely(static_cast<fobos_dev_t*>(readerDevice),
                                       [](float *buf, uint32_t buf_length, void *ctx) {
                                           auto *processor = static_cast<DataProcessor*>(ctx);
                                           processor->handleData(buf, buf_length);
                                       },
                                       this,
                                       asyncBufferCount,
                                       readBlockSamples);
        }
        const bool stoppedByRequest = !running.load();
        if (!stoppedByRequest || fobosVerboseLoggingEnabled()) {
            qDebug() << "[DataProcessor] fobos_rx_read_async end"
                     << "result" << ret
                     << "stoppedByRequest" << stoppedByRequest;
        }
        if (!stoppedByRequest) {
            if (ret == FOBOS_ERR_OK) {
                qDebug() << "[DataProcessor] async read ended unexpectedly with OK result";
            } else {
                qDebug() << "Failed to start async read, error code:" << ret;
            }
            emit readerFailed(ret, stoppedByRequest);
        } else if (ret != FOBOS_ERR_OK && stoppedByRequest && fobosVerboseLoggingEnabled()) {
            qDebug() << "Async read stopped after cancel, result:" << ret;
        }
        running = false;
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "Async read finished.";
        }

    } else {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DataProcessor] fobos_rx_start_sync begin"
                     << "device" << readerDevice
                     << "blockSamples" << readBlockSamples;
        }
        int ret = readerApiKind == FobosApiKind::Agile
                      ? startFobosAgileSyncSafely(static_cast<fobos_sdr_dev_t*>(readerDevice), readBlockSamples)
                      : startFobosSyncSafely(static_cast<fobos_dev_t*>(readerDevice), readBlockSamples);
        qDebug() << "[DataProcessor] fobos_rx_start_sync end" << "result" << ret;
        if (ret != FOBOS_ERR_OK) {
            const bool stoppedByRequest = !running.load();
            qDebug() << "Failed to start sync mode, error code:" << ret;
            if (!stoppedByRequest) {
                emit readerFailed(ret, stoppedByRequest);
            }
            running = false;
            return;
        }
        std::vector<float> syncBuffer(static_cast<size_t>(readBlockSamples) * FLOATS_PER_IQ_SAMPLE);
        IqBuffer::setSampleRateEstimate(sampleRate);
        QElapsedTimer rateTimer;
        rateTimer.start();
        uint64_t measuredSamples = 0;
        uint64_t measuredReads = 0;
        qint64 accumulatedReadMs = 0;
        qint64 accumulatedPublishMs = 0;
        uint64_t readCounter = 0;
        int syncRateReportCount = 0;
        while (running.load()) {
            const uint64_t readEpoch = iqRetuneEpoch.load(std::memory_order_acquire);
            uint32_t actual_buf_length = 0;
            const bool logRead = readCounter < 5 || (readCounter % 400) == 0;
            if (logRead && fobosVerboseLoggingEnabled()) {
                qDebug() << "[DataProcessor] fobos_rx_read_sync begin"
                         << "readCounter" << readCounter;
            }
            QElapsedTimer readTimer;
            readTimer.start();
            ret = readerApiKind == FobosApiKind::Agile
                      ? readFobosAgileSyncSafely(static_cast<fobos_sdr_dev_t*>(readerDevice), syncBuffer.data(), &actual_buf_length)
                      : readFobosSyncSafely(static_cast<fobos_dev_t*>(readerDevice), syncBuffer.data(), &actual_buf_length);
            accumulatedReadMs += readTimer.elapsed();
            if ((logRead && fobosVerboseLoggingEnabled()) || ret != FOBOS_ERR_OK) {
                qDebug() << "[DataProcessor] fobos_rx_read_sync end"
                         << "readCounter" << readCounter
                         << "result" << ret
                         << "actual_buf_length" << actual_buf_length;
            }
            if (ret != FOBOS_ERR_OK) {
                const bool stoppedByRequest = !running.load();
                if (!stoppedByRequest) {
                    qDebug() << "Failed to read sync data, error code:" << ret;
                    emit readerFailed(ret, stoppedByRequest);
                } else {
                    qDebug() << "Sync read stopped after stop request, result:" << ret;
                }
                running = false;
                break;
            }
            ++readCounter;
            ++measuredReads;
            measuredSamples += actual_buf_length;
            const size_t floatCount = (std::min)(
                syncBuffer.size(),
                static_cast<size_t>(actual_buf_length) * FLOATS_PER_IQ_SAMPLE
                );
            QElapsedTimer publishTimer;
            publishTimer.start();
            const bool queueAudioBlocks = requestedQueueAudioBlocks.load();
            const bool publishIqSnapshot = requestedPublishIqSnapshot.load();
            if (queueAudioBlocks || publishIqSnapshot) {
                if (!IqBuffer::publish(syncBuffer.data(),
                                       floatCount,
                                       queueAudioBlocks,
                                       publishIqSnapshot,
                                       readEpoch)) {
                    continue;
                }
            }
            updateStreamDiagnostics(syncBuffer.data(), actual_buf_length, "sync");
            if (requestedEmitIqFrames.load()) {
                emitIqFrame(syncBuffer.data(), floatCount);
            }
            accumulatedPublishMs += publishTimer.elapsed();

            const qint64 elapsedMs = rateTimer.elapsed();
            if (elapsedMs >= 500) {
                const double measuredRate = static_cast<double>(measuredSamples) * 1000.0 /
                                            static_cast<double>((std::max)(qint64(1), elapsedMs));
                const double avgReadMs = measuredReads > 0
                                             ? static_cast<double>(accumulatedReadMs) / static_cast<double>(measuredReads)
                                             : 0.0;
                const double avgPublishMs = measuredReads > 0
                                                ? static_cast<double>(accumulatedPublishMs) / static_cast<double>(measuredReads)
                                                : 0.0;
                IqBuffer::setSampleRateEstimate(measuredRate);
                if (shouldReportMeasuredSampleRate(sampleRate, measuredRate, syncRateReportCount)) {
                    const double errorPercent =
                        sampleRate > 0.0
                            ? ((measuredRate - sampleRate) / sampleRate) * 100.0
                            : 0.0;
                    qDebug() << "[DataProcessor] sync sample-rate check"
                             << "configured" << sampleRate
                             << "measured" << measuredRate
                             << "errorPercent" << errorPercent
                             << "elapsedMs" << elapsedMs
                             << "readCounter" << readCounter
                             << "avgReadMs" << avgReadMs
                             << "avgPublishMs" << avgPublishMs;
                    ++syncRateReportCount;
                }
                if (fobosVerboseLoggingEnabled()) {
                    qDebug() << "[DataProcessor] sync measured sample rate"
                             << "configured" << sampleRate
                             << "measured" << measuredRate
                             << "elapsedMs" << elapsedMs
                             << "readCounter" << readCounter
                             << "avgReadMs" << avgReadMs
                             << "avgPublishMs" << avgPublishMs;
                }
                measuredSamples = 0;
                measuredReads = 0;
                accumulatedReadMs = 0;
                accumulatedPublishMs = 0;
                rateTimer.restart();
            }
        }
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DataProcessor] sync read loop finished";
        }
    }
}

void DataProcessor::runRtlTcpReader(const ReceiverStreamDescriptor &stream, uint32_t blockSamples) {
    const QString host = stream.rtlTcpHost.isEmpty() ? QStringLiteral("127.0.0.1") : stream.rtlTcpHost;
    const quint16 port = stream.rtlTcpPort == 0 ? 1234 : stream.rtlTcpPort;
    const double sampleRate = stream.sampleRateHz > 0.0 ? stream.sampleRateHz : requestedSampleRate.load();
    double appliedCenterFrequency = stream.centerFrequencyHz + frequencyCalibrationOffsetHz.load();
    const uint32_t samplesPerBlock = (std::max)(blockSamples, MIN_ASYNC_BLOCK_SAMPLES);
    const int bytesPerBlock = static_cast<int>(samplesPerBlock * FLOATS_PER_IQ_SAMPLE);
    QByteArray byteBuffer;
    byteBuffer.reserve(bytesPerBlock * 2);

    qDebug() << "[RTL-TCP] connecting"
             << "host" << host
             << "port" << port
             << "frequency" << appliedCenterFrequency
             << "sampleRate" << sampleRate
             << "blockSamples" << samplesPerBlock;

    QTcpSocket socket;
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(3000)) {
        qDebug() << "[RTL-TCP] connect failed" << socket.errorString();
        emit readerFailed(RTL_TCP_ERR_CONNECT, !running.load());
        running = false;
        return;
    }

    auto commandOrFail = [&socket](quint8 command, quint32 parameter, const char *name) {
        const bool ok = writeRtlTcpCommand(socket, command, parameter);
        if (!ok) {
            qDebug() << "[RTL-TCP] command failed" << name << "parameter" << parameter << socket.errorString();
        }
        return ok;
    };

    bool configured = true;
    if (appliedCenterFrequency > 0.0) {
        configured = configured &&
                     commandOrFail(0x01,
                                   static_cast<quint32>((std::max)(0.0, appliedCenterFrequency)),
                                   "set frequency");
    }
    if (sampleRate > 0.0) {
        configured = configured &&
                     commandOrFail(0x02,
                                   static_cast<quint32>((std::max)(0.0, sampleRate)),
                                   "set sample rate");
    }
    if (stream.rtlTcpTunerGainTenthsDb >= 0) {
        configured = configured &&
                     commandOrFail(0x03, 1, "manual gain mode") &&
                     commandOrFail(0x04,
                                   static_cast<quint32>(stream.rtlTcpTunerGainTenthsDb),
                                   "set gain");
    } else if (stream.rtlTcpAgc) {
        configured = configured && commandOrFail(0x03, 0, "auto gain mode");
    }

    if (!configured) {
        emit readerFailed(RTL_TCP_ERR_CONFIGURE, !running.load());
        socket.disconnectFromHost();
        running = false;
        return;
    }

    IqBuffer::setSampleRateEstimate(sampleRate);
    asyncMeasuredSamples = 0;
    asyncCallbackCounter = 0;
    asyncRateReportCount = 0;
    asyncRateTimer.restart();
    qDebug() << "[RTL-TCP] stream configured";

    auto applyPendingRetune = [&]() {
        const double logicalCenter = requestedCenterFrequency.load();
        const double requestedCenter = logicalCenter + frequencyCalibrationOffsetHz.load();
        if (!std::isfinite(requestedCenter) ||
            requestedCenter <= 0.0 ||
            std::abs(requestedCenter - appliedCenterFrequency) <= 0.5) {
            return true;
        }
        const bool ok = commandOrFail(0x01,
                                      static_cast<quint32>((std::max)(0.0, requestedCenter)),
                                      "live set frequency");
        qDebug() << "[RTL-TCP] live center retune"
                 << "logical" << logicalCenter
                 << "requested" << requestedCenter
                 << "previous" << appliedCenterFrequency
                 << "ok" << ok;
        if (ok) {
            appliedCenterFrequency = requestedCenter;
        }
        return ok;
    };

    while (running.load()) {
        applyPendingRetune();
        if (!socket.waitForReadyRead(100)) {
            if (socket.state() != QAbstractSocket::ConnectedState) {
                qDebug() << "[RTL-TCP] disconnected while waiting" << socket.errorString();
                emit readerFailed(RTL_TCP_ERR_READ, !running.load());
                break;
            }
            continue;
        }

        byteBuffer.append(socket.readAll());
        while (running.load() && byteBuffer.size() >= bytesPerBlock) {
            const auto *raw = reinterpret_cast<const unsigned char*>(byteBuffer.constData());
            handleUnsigned8IqData(raw, static_cast<uint32_t>(bytesPerBlock), "rtl_tcp");
            byteBuffer.remove(0, bytesPerBlock);
        }
    }

    const bool stoppedByRequest = !running.load();
    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        socket.waitForDisconnected(500);
    }
    running = false;
    qDebug() << "[RTL-TCP] stream finished" << "stoppedByRequest" << stoppedByRequest;
}

void DataProcessor::runRtlSdrNativeReader(const ReceiverStreamDescriptor &stream, uint32_t blockSamples) {
    QString loadedPath;
    QString errorMessage;
    if (!rtlSdrLibraryAvailable(&loadedPath, &errorMessage)) {
        qDebug() << "[RTL-SDR] native library unavailable" << errorMessage;
        emit readerFailed(RTLSDR_NATIVE_ERR_OPEN, !running.load());
        running = false;
        return;
    }

    const QVector<RtlSdrDeviceInfo> devices = enumerateRtlSdrDevices();
    QStringList deviceLabels;
    deviceLabels.reserve(devices.size());
    for (const RtlSdrDeviceInfo &deviceInfo : devices) {
        deviceLabels.append(QStringLiteral("#%1 %2").arg(deviceInfo.nativeIndex).arg(deviceInfo.name));
    }
    qDebug() << "[RTL-SDR] enumerate before open"
             << "count" << devices.size()
             << "devices" << deviceLabels;

    QVector<int> openOrder;
    auto appendOpenIndex = [&openOrder](int index) {
        index = (std::max)(0, index);
        if (!openOrder.contains(index)) {
            openOrder.append(index);
        }
    };
    appendOpenIndex(stream.rtlSdrNativeDeviceIndex);
    for (const RtlSdrDeviceInfo &deviceInfo : devices) {
        appendOpenIndex(deviceInfo.nativeIndex);
    }

    void *rtlDevice = nullptr;
    int ret = RTLSDR_NATIVE_ERR_OPEN;
    int openedIndex = -1;
    for (int candidateIndex : std::as_const(openOrder)) {
        void *candidateDevice = nullptr;
        const int candidateResult =
            openRtlSdrDeviceSafely(&candidateDevice, static_cast<uint32_t>(candidateIndex));
        qDebug() << "[RTL-SDR] open candidate"
                 << "index" << candidateIndex
                 << "requested" << stream.rtlSdrNativeDeviceIndex
                 << "result" << candidateResult
                 << "device" << candidateDevice
                 << "library" << loadedPath;
        if (candidateResult == 0 && candidateDevice) {
            rtlDevice = candidateDevice;
            ret = candidateResult;
            openedIndex = candidateIndex;
            break;
        }
        if (candidateDevice) {
            closeRtlSdrDeviceSafely(candidateDevice);
        }
        ret = candidateResult;
    }
    if (ret != 0 || !rtlDevice) {
        emit readerFailed(RTLSDR_NATIVE_ERR_OPEN, !running.load());
        running = false;
        return;
    }

    qDebug() << "[RTL-SDR] opened"
             << "index" << openedIndex
             << "requested" << stream.rtlSdrNativeDeviceIndex
             << "device" << rtlDevice;

    activeDevice = rtlDevice;
    const double sampleRate = stream.sampleRateHz > 0.0 ? stream.sampleRateHz : requestedSampleRate.load();
    const double logicalCenterFrequency = requestedCenterFrequency.load() > 0.0
                                              ? requestedCenterFrequency.load()
                                              : stream.centerFrequencyHz;
    const double centerFrequency = logicalCenterFrequency + frequencyCalibrationOffsetHz.load();
    const uint32_t sampleRateHz = static_cast<uint32_t>((std::max)(0.0, sampleRate));
    const uint32_t centerFrequencyHz = static_cast<uint32_t>((std::max)(0.0, centerFrequency));
    bool configured = true;

    ret = setRtlSdrCenterFrequencySafely(rtlDevice, centerFrequencyHz);
    qDebug() << "[RTL-SDR] set center frequency" << centerFrequencyHz << "result" << ret;
    configured = configured && ret == 0;
    ret = setRtlSdrDirectSamplingSafely(rtlDevice, 0);
    qDebug() << "[RTL-SDR] set direct sampling off result" << ret;
    configured = configured && ret == 0;
    ret = setRtlSdrSampleRateSafely(rtlDevice, sampleRateHz);
    qDebug() << "[RTL-SDR] set sample rate" << sampleRateHz << "result" << ret;
    configured = configured && ret == 0;
    const bool useManualGain = stream.rtlTcpTunerGainTenthsDb >= 0;
    ret = setRtlSdrTunerGainModeSafely(rtlDevice, useManualGain ? 1 : 0);
    qDebug() << "[RTL-SDR] set tuner gain mode"
             << (useManualGain ? QStringLiteral("manual") : QStringLiteral("auto"))
             << "result" << ret;
    configured = configured && ret == 0;
    if (useManualGain) {
        ret = setRtlSdrTunerGainSafely(rtlDevice, stream.rtlTcpTunerGainTenthsDb);
        qDebug() << "[RTL-SDR] set tuner gain" << stream.rtlTcpTunerGainTenthsDb << "result" << ret;
        configured = configured && ret == 0;
    }
    ret = setRtlSdrAgcModeSafely(rtlDevice, (stream.rtlTcpAgc || !useManualGain) ? 1 : 0);
    qDebug() << "[RTL-SDR] set agc" << (stream.rtlTcpAgc || !useManualGain) << "result" << ret;
    configured = configured && ret == 0;
    ret = setRtlSdrFrequencyCorrectionSafely(rtlDevice, 0);
    qDebug() << "[RTL-SDR] set frequency correction best-effort result" << ret;
    ret = resetRtlSdrBufferSafely(rtlDevice);
    qDebug() << "[RTL-SDR] reset buffer result" << ret;
    configured = configured && ret == 0;

    if (!running.load()) {
        qDebug() << "[RTL-SDR] stop requested during configure; closing before read_async";
        closeRtlSdrDeviceSafely(rtlDevice);
        activeDevice = nullptr;
        running = false;
        return;
    }

    if (!configured) {
        closeRtlSdrDeviceSafely(rtlDevice);
        activeDevice = nullptr;
        emit readerFailed(RTLSDR_NATIVE_ERR_CONFIGURE, !running.load());
        running = false;
        return;
    }

    IqBuffer::setSampleRateEstimate(sampleRate);
    asyncMeasuredSamples = 0;
    asyncCallbackCounter = 0;
    asyncRateReportCount = 0;
    asyncRateTimer.restart();

    const uint32_t bytesPerBlock = (std::max)(blockSamples * FLOATS_PER_IQ_SAMPLE,
                                              RTLSDR_NATIVE_BLOCK_BYTES);
    const uint32_t bufferCount = asyncBufferCountForRate(sampleRate);
    qDebug() << "[RTL-SDR] read_async begin"
             << "buffers" << bufferCount
             << "blockBytes" << bytesPerBlock
             << "sampleRate" << sampleRate
             << "center" << centerFrequency;
    ret = readRtlSdrAsyncSafely(rtlDevice,
                                [](unsigned char *buf, uint32_t len, void *ctx) {
                                    auto *processor = static_cast<DataProcessor*>(ctx);
                                    processor->handleUnsigned8IqData(buf, len, "rtl_native");
                                },
                                this,
                                bufferCount,
                                bytesPerBlock);
    const bool stoppedByRequest = !running.load();
    qDebug() << "[RTL-SDR] read_async end" << "result" << ret << "stoppedByRequest" << stoppedByRequest;
    if (ret != 0 && !stoppedByRequest) {
        emit readerFailed(ret, stoppedByRequest);
    }

    closeRtlSdrDeviceSafely(rtlDevice);
    activeDevice = nullptr;
    running = false;
}

void DataProcessor::runSoapySdrReader(const ReceiverStreamDescriptor &stream, uint32_t blockSamples) {
    QString loadedPath;
    QString errorMessage;
    if (!soapySdrLibraryAvailable(&loadedPath, &errorMessage)) {
        qDebug() << "[SoapySDR] runtime unavailable" << errorMessage;
        emit readerFailed(SOAPY_SDR_ERR_OPEN, !running.load());
        running = false;
        return;
    }

    const QVector<SoapySdrDeviceInfo> devices = enumerateSoapySdrDevices();
    QStringList deviceLabels;
    deviceLabels.reserve(devices.size());
    for (const SoapySdrDeviceInfo &deviceInfo : devices) {
        deviceLabels.append(QStringLiteral("#%1 %2").arg(deviceInfo.nativeIndex).arg(deviceInfo.label));
    }
    qDebug() << "[SoapySDR] enumerate before open"
             << "count" << devices.size()
             << "devices" << deviceLabels;

    void *soapyDevice = nullptr;
    int ret = openSoapySdrDeviceSafely(&soapyDevice, stream.soapySdrDeviceIndex);
    qDebug() << "[SoapySDR] open"
             << "index" << stream.soapySdrDeviceIndex
             << "result" << ret
             << "device" << soapyDevice
             << "runtime" << loadedPath;
    if (ret != 0 || !soapyDevice) {
        qDebug() << "[SoapySDR] open failed" << soapySdrLastErrorMessage();
        emit readerFailed(SOAPY_SDR_ERR_OPEN, !running.load());
        running = false;
        return;
    }

    activeDevice = soapyDevice;
    const double sampleRate = stream.sampleRateHz > 0.0 ? stream.sampleRateHz : requestedSampleRate.load();
    const double logicalCenterFrequency = requestedCenterFrequency.load() > 0.0
                                              ? requestedCenterFrequency.load()
                                              : stream.centerFrequencyHz;
    const double centerFrequency = logicalCenterFrequency + frequencyCalibrationOffsetHz.load();
    bool configured = true;

    ret = setSoapySdrSampleRateSafely(soapyDevice, sampleRate);
    qDebug() << "[SoapySDR] set sample rate" << sampleRate << "result" << ret;
    configured = configured && ret == 0;
    ret = setSoapySdrCenterFrequencySafely(soapyDevice, centerFrequency);
    qDebug() << "[SoapySDR] set center frequency" << centerFrequency << "result" << ret;
    configured = configured && ret == 0;
    if (stream.centerFrequencyHz > 0.0 && stream.sampleRateHz > 0.0) {
        ret = setSoapySdrBandwidthSafely(soapyDevice, stream.sampleRateHz);
        qDebug() << "[SoapySDR] set bandwidth best-effort" << stream.sampleRateHz << "result" << ret;
    }

    if (!running.load()) {
        qDebug() << "[SoapySDR] stop requested during configure; closing device";
        closeSoapySdrDeviceSafely(soapyDevice);
        activeDevice = nullptr;
        running = false;
        return;
    }

    if (!configured) {
        qDebug() << "[SoapySDR] configure failed" << soapySdrLastErrorMessage();
        closeSoapySdrDeviceSafely(soapyDevice);
        activeDevice = nullptr;
        emit readerFailed(SOAPY_SDR_ERR_CONFIGURE, !running.load());
        running = false;
        return;
    }

    void *rxStream = setupSoapySdrRxStreamSafely(soapyDevice);
    qDebug() << "[SoapySDR] setup RX stream" << rxStream;
    if (!rxStream) {
        qDebug() << "[SoapySDR] setup stream failed" << soapySdrLastErrorMessage();
        closeSoapySdrDeviceSafely(soapyDevice);
        activeDevice = nullptr;
        emit readerFailed(SOAPY_SDR_ERR_STREAM, !running.load());
        running = false;
        return;
    }

    ret = activateSoapySdrStreamSafely(soapyDevice, rxStream);
    qDebug() << "[SoapySDR] activate stream result" << ret;
    if (ret != 0) {
        qDebug() << "[SoapySDR] activate failed" << soapySdrLastErrorMessage();
        closeSoapySdrStreamSafely(soapyDevice, rxStream);
        closeSoapySdrDeviceSafely(soapyDevice);
        activeDevice = nullptr;
        emit readerFailed(SOAPY_SDR_ERR_STREAM, !running.load());
        running = false;
        return;
    }

    const uint32_t samplesPerBlock = (std::max)(blockSamples, MIN_ASYNC_BLOCK_SAMPLES);
    std::vector<float> floatBuffer(static_cast<std::size_t>(samplesPerBlock) * FLOATS_PER_IQ_SAMPLE);
    IqBuffer::setSampleRateEstimate(sampleRate);
    asyncMeasuredSamples = 0;
    asyncCallbackCounter = 0;
    asyncRateReportCount = 0;
    asyncRateTimer.restart();

    qDebug() << "[SoapySDR] readStream begin"
             << "blockSamples" << samplesPerBlock
             << "sampleRate" << sampleRate
             << "center" << centerFrequency;
    while (running.load()) {
        ret = readSoapySdrStreamSafely(soapyDevice,
                                       rxStream,
                                       floatBuffer.data(),
                                       samplesPerBlock,
                                       200000);
        if (ret > 0) {
            handleData(floatBuffer.data(), static_cast<uint32_t>(ret));
            continue;
        }
        if (ret == SOAPY_SDR_TIMEOUT) {
            continue;
        }
        const bool stoppedByRequest = !running.load();
        qDebug() << "[SoapySDR] readStream returned"
                 << "result" << ret
                 << "stoppedByRequest" << stoppedByRequest
                 << "error" << soapySdrLastErrorMessage();
        if (!stoppedByRequest) {
            emit readerFailed(ret, stoppedByRequest);
        }
        break;
    }

    const bool stoppedByRequest = !running.load();
    qDebug() << "[SoapySDR] readStream end" << "stoppedByRequest" << stoppedByRequest;
    deactivateSoapySdrStreamSafely(soapyDevice, rxStream);
    closeSoapySdrStreamSafely(soapyDevice, rxStream);
    closeSoapySdrDeviceSafely(soapyDevice);
    activeDevice = nullptr;
    running = false;
}

void DataProcessor::runBladeRfNativeReader(const ReceiverStreamDescriptor &stream, uint32_t blockSamples) {
    QString loadedPath;
    QString errorMessage;
    if (!bladeRfLibraryAvailable(&loadedPath, &errorMessage)) {
        qDebug() << "[bladeRF] native library unavailable" << errorMessage;
        emit readerFailed(BLADERF_NATIVE_ERR_OPEN, !running.load());
        running = false;
        return;
    }

    const QVector<BladeRfDeviceInfo> devices = enumerateBladeRfDevices();
    QStringList deviceLabels;
    deviceLabels.reserve(devices.size());
    for (const BladeRfDeviceInfo &deviceInfo : devices) {
        deviceLabels.append(QStringLiteral("#%1 %2").arg(deviceInfo.nativeIndex).arg(deviceInfo.label));
    }
    qDebug() << "[bladeRF] enumerate before open"
             << "count" << devices.size()
             << "devices" << deviceLabels;

    void *bladeDevice = nullptr;
    int ret = openBladeRfDeviceSafely(&bladeDevice, stream.bladeRfNativeDeviceIndex);
    qDebug() << "[bladeRF] open"
             << "index" << stream.bladeRfNativeDeviceIndex
             << "result" << ret
             << "device" << bladeDevice
             << "library" << loadedPath;
    if (ret != 0 || !bladeDevice) {
        qDebug() << "[bladeRF] open failed" << bladeRfLastErrorMessage();
        emit readerFailed(BLADERF_NATIVE_ERR_OPEN, !running.load());
        running = false;
        return;
    }

    activeDevice = bladeDevice;
    const double sampleRate = stream.sampleRateHz > 0.0 ? stream.sampleRateHz : requestedSampleRate.load();
    const double logicalCenterFrequency = requestedCenterFrequency.load() > 0.0
                                              ? requestedCenterFrequency.load()
                                              : stream.centerFrequencyHz;
    const double centerFrequency = logicalCenterFrequency + frequencyCalibrationOffsetHz.load();
    const uint32_t sampleRateHz = static_cast<uint32_t>((std::clamp)(sampleRate, 1.0, 61440000.0));
    const uint64_t centerFrequencyHz = static_cast<uint64_t>((std::max)(0.0, centerFrequency));
    uint32_t actualSampleRateHz = 0;
    uint32_t actualBandwidthHz = 0;
    bool configured = true;

    ret = setBladeRfSampleRateSafely(bladeDevice, sampleRateHz, &actualSampleRateHz);
    qDebug() << "[bladeRF] set sample rate"
             << sampleRateHz
             << "actual" << actualSampleRateHz
             << "result" << ret;
    configured = configured && ret == 0;
    ret = setBladeRfBandwidthSafely(bladeDevice,
                                    sampleRateHz,
                                    &actualBandwidthHz);
    qDebug() << "[bladeRF] set bandwidth best-effort"
             << sampleRateHz
             << "actual" << actualBandwidthHz
             << "result" << ret;
    ret = setBladeRfCenterFrequencySafely(bladeDevice, centerFrequencyHz);
    qDebug() << "[bladeRF] set center frequency" << centerFrequencyHz << "result" << ret;
    configured = configured && ret == 0;
    ret = setBladeRfGainModeSafely(bladeDevice, 0);
    qDebug() << "[bladeRF] set default gain mode best-effort result" << ret;

    const uint32_t samplesPerBlock = (std::max)(blockSamples, MIN_ASYNC_BLOCK_SAMPLES);
    const uint32_t bufferCount = (std::max)(asyncBufferCountForRate(sampleRate), 8u);
    const uint32_t transferCount = (std::min)(bufferCount - 1, 16u);
    ret = configureBladeRfSyncRxSafely(bladeDevice,
                                       bufferCount,
                                       samplesPerBlock,
                                       transferCount,
                                       1000);
    qDebug() << "[bladeRF] sync_config"
             << "buffers" << bufferCount
             << "samplesPerBuffer" << samplesPerBlock
             << "transfers" << transferCount
             << "result" << ret;
    configured = configured && ret == 0;

    if (!running.load()) {
        qDebug() << "[bladeRF] stop requested during configure; closing device";
        closeBladeRfDeviceSafely(bladeDevice);
        activeDevice = nullptr;
        running = false;
        return;
    }

    if (!configured) {
        qDebug() << "[bladeRF] configure failed" << bladeRfLastErrorMessage();
        closeBladeRfDeviceSafely(bladeDevice);
        activeDevice = nullptr;
        emit readerFailed(BLADERF_NATIVE_ERR_CONFIGURE, !running.load());
        running = false;
        return;
    }

    ret = enableBladeRfRxSafely(bladeDevice, true);
    qDebug() << "[bladeRF] enable RX result" << ret;
    if (ret != 0) {
        qDebug() << "[bladeRF] enable RX failed" << bladeRfLastErrorMessage();
        closeBladeRfDeviceSafely(bladeDevice);
        activeDevice = nullptr;
        emit readerFailed(BLADERF_NATIVE_ERR_STREAM, !running.load());
        running = false;
        return;
    }

    const double effectiveSampleRate = actualSampleRateHz > 0 ? static_cast<double>(actualSampleRateHz) : sampleRate;
    std::vector<int16_t> sc16Buffer(static_cast<std::size_t>(samplesPerBlock) * FLOATS_PER_IQ_SAMPLE);
    std::vector<float> floatBuffer(static_cast<std::size_t>(samplesPerBlock) * FLOATS_PER_IQ_SAMPLE);
    IqBuffer::setSampleRateEstimate(effectiveSampleRate);
    asyncMeasuredSamples = 0;
    asyncCallbackCounter = 0;
    asyncRateReportCount = 0;
    asyncRateTimer.restart();
    QElapsedTimer bladeRfStatsTimer;
    bladeRfStatsTimer.start();
    uint64_t bladeRfBlocksSinceLog = 0;
    uint64_t bladeRfSamplesSinceLog = 0;
    int16_t bladeRfMinI = 0;
    int16_t bladeRfMaxI = 0;
    int16_t bladeRfMinQ = 0;
    int16_t bladeRfMaxQ = 0;
    double bladeRfMeanI = 0.0;
    double bladeRfMeanQ = 0.0;
    uint64_t bladeRfInspected = 0;
    uint64_t bladeRfClipped = 0;

    qDebug() << "[bladeRF] sync_rx begin"
             << "blockSamples" << samplesPerBlock
             << "sampleRate" << effectiveSampleRate
             << "center" << centerFrequency;
    while (running.load()) {
        ret = readBladeRfSyncRxSafely(bladeDevice,
                                      sc16Buffer.data(),
                                      samplesPerBlock,
                                      BLADERF_SYNC_TIMEOUT_MS);
        if (ret == 0) {
            const bool collectBladeRfStats = fobosVerboseLoggingEnabled();
            const uint32_t inspectStride = collectBladeRfStats
                                               ? (std::max)(uint32_t(1), samplesPerBlock / 4096U)
                                               : samplesPerBlock + 1U;
            for (uint32_t i = 0; i < samplesPerBlock; ++i) {
                const int16_t iRaw = sc16Buffer[i * 2];
                const int16_t qRaw = sc16Buffer[i * 2 + 1];
                floatBuffer[i * 2] = static_cast<float>(iRaw) / 2048.0f;
                floatBuffer[i * 2 + 1] = static_cast<float>(qRaw) / 2048.0f;
                if (collectBladeRfStats && (i % inspectStride) == 0) {
                    if (bladeRfInspected == 0) {
                        bladeRfMinI = bladeRfMaxI = iRaw;
                        bladeRfMinQ = bladeRfMaxQ = qRaw;
                    } else {
                        bladeRfMinI = (std::min)(bladeRfMinI, iRaw);
                        bladeRfMaxI = (std::max)(bladeRfMaxI, iRaw);
                        bladeRfMinQ = (std::min)(bladeRfMinQ, qRaw);
                        bladeRfMaxQ = (std::max)(bladeRfMaxQ, qRaw);
                    }
                    bladeRfMeanI += static_cast<double>(iRaw);
                    bladeRfMeanQ += static_cast<double>(qRaw);
                    if (std::abs(static_cast<int>(iRaw)) >= 2040 ||
                        std::abs(static_cast<int>(qRaw)) >= 2040) {
                        ++bladeRfClipped;
                    }
                    ++bladeRfInspected;
                }
            }
            if (collectBladeRfStats) {
                ++bladeRfBlocksSinceLog;
                bladeRfSamplesSinceLog += samplesPerBlock;
            }
            if (collectBladeRfStats && bladeRfStatsTimer.elapsed() >= 3000) {
                const double elapsedSeconds =
                    static_cast<double>((std::max)(qint64(1), bladeRfStatsTimer.elapsed())) / 1000.0;
                const double measuredRate = static_cast<double>(bladeRfSamplesSinceLog) / elapsedSeconds;
                const double inspected = static_cast<double>((std::max)(uint64_t(1), bladeRfInspected));
                const double clipPercent = 100.0 * static_cast<double>(bladeRfClipped) / inspected;
                qDebug() << "[bladeRF] RX stats"
                         << "blocks" << static_cast<qulonglong>(bladeRfBlocksSinceLog)
                         << "samples" << static_cast<qulonglong>(bladeRfSamplesSinceLog)
                         << "rateSps" << measuredRate
                         << "minI" << bladeRfMinI
                         << "maxI" << bladeRfMaxI
                         << "minQ" << bladeRfMinQ
                         << "maxQ" << bladeRfMaxQ
                         << "meanI" << (bladeRfMeanI / inspected)
                         << "meanQ" << (bladeRfMeanQ / inspected)
                         << "clipPercent" << clipPercent;
                bladeRfStatsTimer.restart();
                bladeRfBlocksSinceLog = 0;
                bladeRfSamplesSinceLog = 0;
                bladeRfMinI = bladeRfMaxI = 0;
                bladeRfMinQ = bladeRfMaxQ = 0;
                bladeRfMeanI = 0.0;
                bladeRfMeanQ = 0.0;
                bladeRfInspected = 0;
                bladeRfClipped = 0;
            } else if (!collectBladeRfStats) {
                bladeRfStatsTimer.restart();
                bladeRfBlocksSinceLog = 0;
                bladeRfSamplesSinceLog = 0;
                bladeRfMinI = bladeRfMaxI = 0;
                bladeRfMinQ = bladeRfMaxQ = 0;
                bladeRfMeanI = 0.0;
                bladeRfMeanQ = 0.0;
                bladeRfInspected = 0;
                bladeRfClipped = 0;
            }
            handleData(floatBuffer.data(), samplesPerBlock);
            continue;
        }
        const bool stoppedByRequest = !running.load();
        qDebug() << "[bladeRF] sync_rx returned"
                 << "result" << ret
                 << "stoppedByRequest" << stoppedByRequest
                 << "error" << bladeRfLastErrorMessage();
        if (!stoppedByRequest) {
            emit readerFailed(ret, stoppedByRequest);
        }
        break;
    }

    const bool stoppedByRequest = !running.load();
    qDebug() << "[bladeRF] sync_rx end" << "stoppedByRequest" << stoppedByRequest;
    enableBladeRfRxSafely(bladeDevice, false);
    closeBladeRfDeviceSafely(bladeDevice);
    activeDevice = nullptr;
    running = false;
}

void DataProcessor::runHackRfNativeReader(const ReceiverStreamDescriptor &stream, uint32_t blockSamples) {
    Q_UNUSED(blockSamples)

    QString loadedPath;
    QString errorMessage;
    if (!hackRfLibraryAvailable(&loadedPath, &errorMessage)) {
        qWarning() << "[HackRF] native library unavailable" << errorMessage;
        emit readerFailed(HACKRF_NATIVE_ERR_OPEN, !running.load());
        running = false;
        return;
    }

    void *hackRfDevice = nullptr;
    int result = openHackRfDeviceSafely(&hackRfDevice, stream.hackRfNativeDeviceIndex);
    qDebug() << "[HackRF] open"
             << "index" << stream.hackRfNativeDeviceIndex
             << "result" << result
             << "device" << hackRfDevice
             << "library" << loadedPath;
    if (result != 0 || !hackRfDevice) {
        qWarning() << "[HackRF] open failed" << hackRfLastErrorMessage();
        emit readerFailed(HACKRF_NATIVE_ERR_OPEN, !running.load());
        running = false;
        return;
    }

    activeDevice = hackRfDevice;
    const double requestedRate = stream.sampleRateHz > 0.0
                                     ? stream.sampleRateHz
                                     : requestedSampleRate.load();
    const double sampleRate = (std::clamp)(requestedRate, 2000000.0, 20000000.0);
    const double logicalCenterFrequency = requestedCenterFrequency.load() > 0.0
                                              ? requestedCenterFrequency.load()
                                              : stream.centerFrequencyHz;
    const double hardwareCenterFrequency = logicalCenterFrequency + frequencyCalibrationOffsetHz.load();
    const std::uint64_t centerFrequencyHz = static_cast<std::uint64_t>(
        (std::clamp)(hardwareCenterFrequency, 1000000.0, 6000000000.0));
    const std::uint32_t sampleRateHz = static_cast<std::uint32_t>(sampleRate);
    const std::uint32_t bandwidthHz =
        stream.hackRfAutomaticBandwidth || stream.hackRfBandwidthHz == 0
            ? recommendedHackRfBandwidth(sampleRateHz)
            : nearestHackRfBandwidth((std::min)(stream.hackRfBandwidthHz, sampleRateHz));

    bool configured = true;
    result = setHackRfSampleRateSafely(hackRfDevice, sampleRate);
    qDebug() << "[HackRF] set sample rate" << sampleRate << "result" << result;
    configured = configured && result == 0;
    result = setHackRfBandwidthSafely(hackRfDevice, bandwidthHz);
    qDebug() << "[HackRF] set baseband bandwidth" << bandwidthHz << "result" << result;
    configured = configured && result == 0;
    if (stream.hackRfExplicitTuningEnabled) {
        result = setHackRfExplicitFrequencySafely(
            hackRfDevice,
            stream.hackRfExplicitIfHz,
            stream.hackRfExplicitLoHz,
            static_cast<HackRfRfPath>((std::clamp)(stream.hackRfExplicitPath, 0, 2)));
        qDebug() << "[HackRF] set explicit tuning"
                 << "IF" << stream.hackRfExplicitIfHz
                 << "LO" << stream.hackRfExplicitLoHz
                 << "path" << stream.hackRfExplicitPath
                 << "result" << result;
    } else {
        result = setHackRfCenterFrequencySafely(hackRfDevice, centerFrequencyHz);
        qDebug() << "[HackRF] set center frequency" << centerFrequencyHz << "result" << result;
    }
    configured = configured && result == 0;
    result = setHackRfAmpEnabledSafely(hackRfDevice, stream.hackRfAmpEnabled);
    qDebug() << "[HackRF] RF amp" << stream.hackRfAmpEnabled << "result" << result;
    configured = configured && result == 0;
    result = setHackRfLnaGainSafely(hackRfDevice,
                                    static_cast<std::uint32_t>((std::max)(0, stream.hackRfLnaGainDb)));
    qDebug() << "[HackRF] LNA gain" << stream.hackRfLnaGainDb << "result" << result;
    configured = configured && result == 0;
    result = setHackRfVgaGainSafely(hackRfDevice,
                                    static_cast<std::uint32_t>((std::max)(0, stream.hackRfVgaGainDb)));
    qDebug() << "[HackRF] VGA gain" << stream.hackRfVgaGainDb << "result" << result;
    configured = configured && result == 0;
    result = setHackRfBiasTeeEnabledSafely(hackRfDevice, stream.hackRfBiasTeeEnabled);
    qDebug() << "[HackRF] bias tee" << stream.hackRfBiasTeeEnabled << "result" << result;
    configured = configured && result == 0;
    result = setHackRfClockOutEnabledSafely(hackRfDevice, stream.hackRfClockOutEnabled);
    qDebug() << "[HackRF] CLKOUT" << stream.hackRfClockOutEnabled << "result" << result;
    configured = configured && result == 0;
    result = setHackRfHardwareSyncEnabledSafely(hackRfDevice, stream.hackRfHardwareSyncEnabled);
    qDebug() << "[HackRF] hardware sync" << stream.hackRfHardwareSyncEnabled << "result" << result;
    configured = configured && result == 0;
    result = setHackRfRxOverrunLimitSafely(hackRfDevice, stream.hackRfRxOverrunLimit);
    qDebug() << "[HackRF] RX overrun limit" << stream.hackRfRxOverrunLimit << "result" << result;
    configured = configured && result == 0;
    if (stream.hackRfOperaCakeEnabled) {
        if (stream.hackRfOperaCakeMode == 1) {
            result = configureHackRfOperaCakeFrequencySafely(
                hackRfDevice,
                static_cast<std::uint8_t>(stream.hackRfOperaCakeAddress),
                parseHackRfOperaCakeRanges(stream.hackRfOperaCakeRangesJson));
        } else if (stream.hackRfOperaCakeMode == 2) {
            result = configureHackRfOperaCakeTimeSafely(
                hackRfDevice,
                static_cast<std::uint8_t>(stream.hackRfOperaCakeAddress),
                parseHackRfOperaCakeDwells(stream.hackRfOperaCakeDwellsJson));
        } else {
            result = configureHackRfOperaCakeManualSafely(
                hackRfDevice,
                static_cast<std::uint8_t>(stream.hackRfOperaCakeAddress),
                static_cast<std::uint8_t>(stream.hackRfOperaCakePortA),
                static_cast<std::uint8_t>(stream.hackRfOperaCakePortB));
        }
        qDebug() << "[HackRF] Opera Cake"
                 << "address" << stream.hackRfOperaCakeAddress
                 << "mode" << stream.hackRfOperaCakeMode
                 << "result" << result;
        configured = configured && result == 0;
    }

    if (!running.load()) {
        closeHackRfDeviceSafely(hackRfDevice);
        activeDevice = nullptr;
        return;
    }
    if (!configured) {
        qWarning() << "[HackRF] configure failed" << hackRfLastErrorMessage();
        closeHackRfDeviceSafely(hackRfDevice);
        activeDevice = nullptr;
        emit readerFailed(HACKRF_NATIVE_ERR_CONFIGURE, !running.load());
        running = false;
        return;
    }

    struct RxContext {
        DataProcessor *processor = nullptr;
        std::vector<float> floatIq;
    } context;
    context.processor = this;

    IqBuffer::setSampleRateEstimate(sampleRate);
    asyncMeasuredSamples = 0;
    asyncCallbackCounter = 0;
    asyncRateReportCount = 0;
    asyncRateTimer.restart();

    result = startHackRfRxSafely(
        hackRfDevice,
        [](HackRfTransfer *transfer) -> int {
            if (!transfer || !transfer->rxContext || !transfer->buffer || transfer->validLength < 2) {
                return 0;
            }
            auto *rx = static_cast<RxContext*>(transfer->rxContext);
            if (!rx->processor || !rx->processor->running.load()) {
                return -1;
            }
            const int validBytes = transfer->validLength - (transfer->validLength % 2);
            const std::uint32_t sampleCount = static_cast<std::uint32_t>(validBytes / 2);
            rx->floatIq.resize(static_cast<std::size_t>(validBytes));
            for (int index = 0; index < validBytes; ++index) {
                const auto value = static_cast<std::int8_t>(transfer->buffer[index]);
                rx->floatIq[static_cast<std::size_t>(index)] = static_cast<float>(value) / 128.0f;
            }
            rx->processor->handleData(rx->floatIq.data(), sampleCount);
            return rx->processor->running.load() ? 0 : -1;
        },
        &context);
    qDebug() << "[HackRF] async RX start"
             << "result" << result
             << "sampleRate" << sampleRate
             << "center" << centerFrequencyHz
             << "bandwidth" << bandwidthHz;
    if (result != 0) {
        qWarning() << "[HackRF] RX start failed" << hackRfLastErrorMessage();
        closeHackRfDeviceSafely(hackRfDevice);
        activeDevice = nullptr;
        emit readerFailed(HACKRF_NATIVE_ERR_STREAM, !running.load());
        running = false;
        return;
    }

    while (running.load() && isHackRfStreamingSafely(hackRfDevice)) {
        QThread::msleep(10);
    }

    const bool stoppedByRequest = !running.load();
    if (isHackRfStreamingSafely(hackRfDevice)) {
        stopHackRfRxSafely(hackRfDevice);
    }
    qDebug() << "[HackRF] async RX end" << "stoppedByRequest" << stoppedByRequest;
    closeHackRfDeviceSafely(hackRfDevice);
    activeDevice = nullptr;
    if (!stoppedByRequest) {
        emit readerFailed(HACKRF_NATIVE_ERR_STREAM, false);
    }
    running = false;
}

void DataProcessor::handleUnsigned8IqData(const unsigned char *buf, uint32_t byteCount, const char *readerMode) {
    if (!running.load() || !buf || byteCount < 2) {
        return;
    }
    const uint64_t callbackEpoch = iqRetuneEpoch.load(std::memory_order_acquire);
    const uint32_t sampleCount = byteCount / FLOATS_PER_IQ_SAMPLE;
    std::vector<float> floatBuffer(static_cast<size_t>(sampleCount) * FLOATS_PER_IQ_SAMPLE);
    for (uint32_t i = 0; i < sampleCount; ++i) {
        const int iByte = static_cast<int>(buf[i * 2]);
        const int qByte = static_cast<int>(buf[i * 2 + 1]);
        floatBuffer[i * 2] = (static_cast<float>(iByte) - 127.5f) / 127.5f;
        floatBuffer[i * 2 + 1] = (static_cast<float>(qByte) - 127.5f) / 127.5f;
    }

    ++totalCallbackCounter;
    ++asyncCallbackCounter;
    asyncMeasuredSamples += sampleCount;
    const bool queueAudioBlocks = requestedQueueAudioBlocks.load();
    const bool publishIqSnapshot = requestedPublishIqSnapshot.load();
    if (zoomProcessor && zoomProcessor->enabled()) {
        zoomProcessor->consumeIq(floatBuffer.data(),
                                 sampleCount,
                                 requestedSampleRate.load(),
                                 requestedCenterFrequency.load());
    }
    if (queueAudioBlocks || publishIqSnapshot) {
        if (!IqBuffer::publish(floatBuffer.data(),
                               floatBuffer.size(),
                               queueAudioBlocks,
                               publishIqSnapshot,
                               callbackEpoch)) {
            return;
        }
    }
    updateStreamDiagnostics(floatBuffer.data(), sampleCount, readerMode);
    if (requestedEmitIqFrames.load()) {
        emitIqFrame(floatBuffer.data(), floatBuffer.size());
    }

    const double sampleRate = requestedSampleRate.load();
    const qint64 elapsedMs = asyncRateTimer.elapsed();
    if (elapsedMs >= 500) {
        const double measuredRate =
            static_cast<double>(asyncMeasuredSamples) * 1000.0 /
            static_cast<double>((std::max)(qint64(1), elapsedMs));
        IqBuffer::setSampleRateEstimate(measuredRate);
        if (shouldReportMeasuredSampleRate(sampleRate, measuredRate, asyncRateReportCount)) {
            const double errorPercent =
                sampleRate > 0.0
                    ? ((measuredRate - sampleRate) / sampleRate) * 100.0
                    : 0.0;
            qDebug() << "[RTL-SDR] sample-rate check"
                     << "reader" << readerMode
                     << "configured" << sampleRate
                     << "measured" << measuredRate
                     << "errorPercent" << errorPercent
                     << "callbacks" << asyncCallbackCounter;
            ++asyncRateReportCount;
        }
        asyncMeasuredSamples = 0;
        asyncCallbackCounter = 0;
        asyncRateTimer.restart();
    }
}

void DataProcessor::handleData(float *buf, uint32_t buf_length, int agileScanIndex) {
    if (!running.load()) {
        return;
    }
    const uint64_t callbackEpoch = iqRetuneEpoch.load(std::memory_order_acquire);
    ++totalCallbackCounter;
    captureRetuneRawDumpBlock(buf, buf_length, callbackEpoch);
    const bool queueAudioBlocks = requestedQueueAudioBlocks.load();
    const bool publishIqSnapshot = requestedPublishIqSnapshot.load();
    const bool agileScanMetadataEnabled = wantsAgileScanMetadata();
    IqBuffer::BlockMetadata blockMetadata;
    if (agileScanMetadataEnabled) {
        blockMetadata.tuning = agileScanIndex < 0;
        blockMetadata.scanIndex = agileScanIndex;
        if (agileScanIndex >= 0 &&
            agileScanIndex < activeStreamDescriptor.agileScanFrequenciesHz.size()) {
            blockMetadata.valid = true;
            blockMetadata.centerFrequencyHz =
                activeStreamDescriptor.agileScanFrequenciesHz.at(agileScanIndex);
        }
    }
    if (agileScanMetadataEnabled && !blockMetadata.valid) {
        updateStreamDiagnostics(buf, buf_length, "async-agile-tuning");
        ++asyncCallbackCounter;
        asyncMeasuredSamples += buf_length;
        return;
    }
    if (zoomProcessor && zoomProcessor->enabled()) {
        const double blockCenterHz = blockMetadata.valid
                                         ? blockMetadata.centerFrequencyHz
                                         : requestedCenterFrequency.load();
        zoomProcessor->consumeIq(buf,
                                 buf_length,
                                 requestedSampleRate.load(),
                                 blockCenterHz);
    }
    if (queueAudioBlocks || publishIqSnapshot) {
        if (!IqBuffer::publish(buf,
                               static_cast<size_t>(buf_length) * FLOATS_PER_IQ_SAMPLE,
                               queueAudioBlocks,
                               publishIqSnapshot,
                               callbackEpoch,
                               agileScanMetadataEnabled ? &blockMetadata : nullptr)) {
            return;
        }
    }
    updateStreamDiagnostics(buf, buf_length, "async");
    if (requestedEmitIqFrames.load()) {
        emitIqFrame(buf, static_cast<size_t>(buf_length) * FLOATS_PER_IQ_SAMPLE);
    }
    ++asyncCallbackCounter;
    asyncMeasuredSamples += buf_length;
    const qint64 elapsedMs = asyncRateTimer.isValid() ? asyncRateTimer.elapsed() : 0;
    if (elapsedMs >= 500) {
        const double measuredRate = static_cast<double>(asyncMeasuredSamples) * 1000.0 /
                                    static_cast<double>((std::max)(qint64(1), elapsedMs));
        IqBuffer::setSampleRateEstimate(measuredRate);
        const double configuredRate = requestedSampleRate.load();
        if (shouldReportMeasuredSampleRate(configuredRate, measuredRate, asyncRateReportCount)) {
            const double errorPercent =
                configuredRate > 0.0
                    ? ((measuredRate - configuredRate) / configuredRate) * 100.0
                    : 0.0;
            const bool severeMismatch = std::abs(errorPercent) >= 10.0;
            const bool periodicMismatchReport = severeMismatch &&
                                                (asyncRateReportCount < 4 ||
                                                 (asyncRateReportCount % 20) == 0);
            if (periodicMismatchReport) {
                qWarning() << "[DataProcessor] async sample-rate mismatch"
                           << "configured" << configuredRate
                           << "measured" << measuredRate
                           << "errorPercent" << errorPercent
                           << "elapsedMs" << elapsedMs
                           << "callbackCounter" << asyncCallbackCounter
                           << "blockSamples" << buf_length;
            } else if (fobosVerboseLoggingEnabled()) {
                qDebug() << "[DataProcessor] async sample-rate check"
                         << "configured" << configuredRate
                         << "measured" << measuredRate
                         << "errorPercent" << errorPercent
                         << "elapsedMs" << elapsedMs
                         << "callbackCounter" << asyncCallbackCounter
                         << "blockSamples" << buf_length;
            }
            ++asyncRateReportCount;
        }
        asyncMeasuredSamples = 0;
        asyncCallbackCounter = 0;
        asyncRateTimer.restart();
    }
}

void DataProcessor::resetStreamDiagnostics() {
    streamDiagnosticTimer.invalidate();
    streamDiagnosticLastNs = -1;
    streamDiagnosticCallbacks = 0;
    streamDiagnosticSamples = 0;
    streamDiagnosticIntervals = 0;
    streamDiagnosticLateCallbacks = 0;
    streamDiagnosticReportCount = 0;
    streamDiagnosticMinBlock = 0;
    streamDiagnosticMaxBlock = 0;
    streamDiagnosticIntervalMsSum = 0.0;
    streamDiagnosticMinIntervalMs = 0.0;
    streamDiagnosticMaxIntervalMs = 0.0;
    streamDiagnosticMeanI = 0.0;
    streamDiagnosticMeanQ = 0.0;
    streamDiagnosticPower = 0.0;
    streamDiagnosticPhaseStepSum = 0.0;
    streamDiagnosticPhaseStepAbsSum = 0.0;
    streamDiagnosticPhaseStepCount = 0;
    streamDiagnosticInspectedSamples = 0;
    streamDiagnosticNonFiniteSamples = 0;
    streamDiagnosticClippedSamples = 0;
}

void DataProcessor::updateStreamDiagnostics(const float *samples,
                                            uint32_t sampleCount,
                                            const char *readerMode) {
    if (!fobosVerboseLoggingEnabled()) {
        return;
    }
    if (!samples || sampleCount == 0) {
        return;
    }

    if (!streamDiagnosticTimer.isValid()) {
        streamDiagnosticTimer.start();
        streamDiagnosticLastNs = -1;
    }

    const qint64 nowNs = streamDiagnosticTimer.nsecsElapsed();
    const double configuredRate = requestedSampleRate.load();
    const double expectedIntervalMs =
        configuredRate > 0.0 && std::isfinite(configuredRate)
            ? (static_cast<double>(sampleCount) * 1000.0 / configuredRate)
            : 0.0;

    if (streamDiagnosticLastNs >= 0) {
        const double intervalMs =
            static_cast<double>(nowNs - streamDiagnosticLastNs) / 1000000.0;
        if (std::isfinite(intervalMs) && intervalMs >= 0.0) {
            ++streamDiagnosticIntervals;
            streamDiagnosticIntervalMsSum += intervalMs;
            if (streamDiagnosticIntervals == 1) {
                streamDiagnosticMinIntervalMs = intervalMs;
                streamDiagnosticMaxIntervalMs = intervalMs;
            } else {
                streamDiagnosticMinIntervalMs = (std::min)(streamDiagnosticMinIntervalMs, intervalMs);
                streamDiagnosticMaxIntervalMs = (std::max)(streamDiagnosticMaxIntervalMs, intervalMs);
            }

            const double lateThresholdMs =
                expectedIntervalMs > 0.0
                    ? (std::max)(expectedIntervalMs * 2.5, expectedIntervalMs + 20.0)
                    : 50.0;
            if (intervalMs > lateThresholdMs) {
                ++streamDiagnosticLateCallbacks;
            }
        }
    }
    streamDiagnosticLastNs = nowNs;

    ++streamDiagnosticCallbacks;
    streamDiagnosticSamples += sampleCount;
    streamDiagnosticMinBlock =
        streamDiagnosticMinBlock == 0
            ? sampleCount
            : (std::min)(streamDiagnosticMinBlock, sampleCount);
    streamDiagnosticMaxBlock = (std::max)(streamDiagnosticMaxBlock, sampleCount);

    const uint32_t inspectCount = (std::min)(sampleCount, STREAM_DIAGNOSTIC_MAX_INSPECT_SAMPLES);
    const uint32_t stride = (std::max)(uint32_t(1), sampleCount / inspectCount);
    bool havePreviousPhaseSample = false;
    double previousI = 0.0;
    double previousQ = 0.0;
    for (uint32_t n = 0, inspected = 0; n < sampleCount && inspected < inspectCount; n += stride, ++inspected) {
        const float iSample = samples[static_cast<size_t>(n) * 2U];
        const float qSample = samples[static_cast<size_t>(n) * 2U + 1U];
        if (!std::isfinite(iSample) || !std::isfinite(qSample)) {
            ++streamDiagnosticNonFiniteSamples;
            continue;
        }
        streamDiagnosticMeanI += static_cast<double>(iSample);
        streamDiagnosticMeanQ += static_cast<double>(qSample);
        streamDiagnosticPower += static_cast<double>(iSample) * iSample +
                                 static_cast<double>(qSample) * qSample;
        ++streamDiagnosticInspectedSamples;
        if (havePreviousPhaseSample) {
            const double cross = previousI * static_cast<double>(qSample) -
                                 previousQ * static_cast<double>(iSample);
            const double dot = previousI * static_cast<double>(iSample) +
                               previousQ * static_cast<double>(qSample);
            const double phaseStep = std::atan2(cross, dot);
            if (std::isfinite(phaseStep)) {
                streamDiagnosticPhaseStepSum += phaseStep;
                streamDiagnosticPhaseStepAbsSum += std::abs(phaseStep);
                ++streamDiagnosticPhaseStepCount;
            }
        }
        previousI = iSample;
        previousQ = qSample;
        havePreviousPhaseSample = true;
        if (std::abs(iSample) >= 0.999f || std::abs(qSample) >= 0.999f) {
            ++streamDiagnosticClippedSamples;
        }
    }

    const qint64 elapsedMs = streamDiagnosticTimer.elapsed();
    const bool initialReport =
        streamDiagnosticReportCount < 3 && elapsedMs >= STREAM_DIAGNOSTIC_INITIAL_REPORT_MS;
    if (!initialReport && elapsedMs < STREAM_DIAGNOSTIC_REPORT_MS) {
        return;
    }

    const double elapsedSeconds =
        static_cast<double>((std::max)(qint64(1), elapsedMs)) / 1000.0;
    const double measuredRate = static_cast<double>(streamDiagnosticSamples) / elapsedSeconds;
    const double avgIntervalMs =
        streamDiagnosticIntervals > 0
            ? streamDiagnosticIntervalMsSum / static_cast<double>(streamDiagnosticIntervals)
            : 0.0;
    const double inspected = static_cast<double>((std::max)(uint64_t(1), streamDiagnosticInspectedSamples));
    const double meanI = streamDiagnosticMeanI / inspected;
    const double meanQ = streamDiagnosticMeanQ / inspected;
    const double rms = std::sqrt((std::max)(0.0, streamDiagnosticPower / inspected * 0.5));
    const double phaseCount =
        static_cast<double>((std::max)(uint64_t(1), streamDiagnosticPhaseStepCount));
    const double phaseStepMean = streamDiagnosticPhaseStepSum / phaseCount;
    const double phaseStepAbsMean = streamDiagnosticPhaseStepAbsSum / phaseCount;
    const double clipPercent =
        100.0 * static_cast<double>(streamDiagnosticClippedSamples) / inspected;
    const double nonFinitePercent =
        100.0 * static_cast<double>(streamDiagnosticNonFiniteSamples) /
        static_cast<double>((std::max)(uint64_t(1),
                                       streamDiagnosticInspectedSamples +
                                           streamDiagnosticNonFiniteSamples));
    const double errorPercent =
        configuredRate > 0.0 && std::isfinite(configuredRate)
            ? ((measuredRate - configuredRate) / configuredRate) * 100.0
            : 0.0;

    qDebug() << "[IQ stream]"
              << "mode" << readerMode
              << "configuredRate" << configuredRate
              << "centerFrequency" << requestedCenterFrequency.load()
              << "measuredRate" << measuredRate
             << "errorPercent" << errorPercent
             << "callbacks" << streamDiagnosticCallbacks
             << "samples" << streamDiagnosticSamples
             << "blockSamples" << sampleCount
             << "blockMin" << streamDiagnosticMinBlock
             << "blockMax" << streamDiagnosticMaxBlock
             << "expectedIntervalMs" << expectedIntervalMs
             << "avgIntervalMs" << avgIntervalMs
             << "minIntervalMs" << streamDiagnosticMinIntervalMs
             << "maxIntervalMs" << streamDiagnosticMaxIntervalMs
             << "lateCallbacks" << streamDiagnosticLateCallbacks
             << "queuedBlocks" << IqBuffer::queuedBlocks()
             << "queuedFloats" << IqBuffer::queuedFloatCount()
              << "rms" << rms
              << "meanI" << meanI
              << "meanQ" << meanQ
              << "phaseStepMeanRad" << phaseStepMean
              << "phaseStepAbsMeanRad" << phaseStepAbsMean
              << "clipPercent" << clipPercent
              << "nonFinitePercent" << nonFinitePercent;

    ++streamDiagnosticReportCount;
    streamDiagnosticTimer.restart();
    streamDiagnosticLastNs = -1;
    streamDiagnosticCallbacks = 0;
    streamDiagnosticSamples = 0;
    streamDiagnosticIntervals = 0;
    streamDiagnosticLateCallbacks = 0;
    streamDiagnosticMinBlock = 0;
    streamDiagnosticMaxBlock = 0;
    streamDiagnosticIntervalMsSum = 0.0;
    streamDiagnosticMinIntervalMs = 0.0;
    streamDiagnosticMaxIntervalMs = 0.0;
    streamDiagnosticMeanI = 0.0;
    streamDiagnosticMeanQ = 0.0;
    streamDiagnosticPower = 0.0;
    streamDiagnosticPhaseStepSum = 0.0;
    streamDiagnosticPhaseStepAbsSum = 0.0;
    streamDiagnosticPhaseStepCount = 0;
    streamDiagnosticInspectedSamples = 0;
    streamDiagnosticNonFiniteSamples = 0;
    streamDiagnosticClippedSamples = 0;
}

void DataProcessor::updateNetworkIqSettings(const RadioSettings &settings, bool channelizeFrames) {
    configureNetworkIqStreaming(settings, true, channelizeFrames);
}

void DataProcessor::configureNetworkIqStreaming(const RadioSettings &settings, bool emitFrames, bool channelizeFrames) {
    bool reset;
    {
        std::lock_guard<std::mutex> lock(networkIqSettingsMutex);
        const RadioSettings &old = networkIqSettings;
        reset = old.sampleRate != settings.sampleRate || old.centerFrequency != settings.centerFrequency ||
                old.listeningFrequency != settings.listeningFrequency || old.inputMode != settings.inputMode ||
                old.modulationType != settings.modulationType || old.bandwidth != settings.bandwidth ||
                old.dmrBasebandSampleRate != settings.dmrBasebandSampleRate ||
                old.hfNoiseCancelDepth != settings.hfNoiseCancelDepth ||
                old.hfNoiseCancelRefGainDb != settings.hfNoiseCancelRefGainDb ||
                old.hfNoiseCancelRefDelayNs != settings.hfNoiseCancelRefDelayNs ||
                old.hfNoiseCancelRefTiltDb != settings.hfNoiseCancelRefTiltDb;
        networkIqSettings = settings;
    }
    const bool wasEnabled = requestedEmitIqFrames.exchange(emitFrames);
    const bool wasChannelized = requestedChannelizeIqFrames.exchange(channelizeFrames);
    if (reset || (!wasEnabled && emitFrames) || wasChannelized != channelizeFrames) {
        networkIqResetRequested = true;
    }
}

void DataProcessor::resetNetworkIqState() {
    networkIqNcoPhase = 0.0;
    networkIqDecimationSum = std::complex<float>(0.0f, 0.0f);
    networkIqDecimationCount = 0;
    networkIqLowPassState = std::complex<float>(0.0f, 0.0f);
    networkIqPreLowPassStates.fill(std::complex<float>(0.0f, 0.0f));
    for (auto &buffer : networkIqCicBuffers) {
        buffer.clear();
    }
    networkIqCicSums.fill(std::complex<float>(0.0f, 0.0f));
    networkIqCicIndex = 0;
    networkIqCicLength = 0;
    networkIqChannelizer.reset();
    networkIqChannelizerOutput.clear();
    networkIqLastLoggedDmrOutputRate = 0;
    networkIqLastLoggedDmrDecimationFactor = 0;
    networkIqAgcLevel = 0.01f;
    networkHfNoiseCancelCoeff = {0.0f, 0.0f};
    networkHfNoiseCancelRefDecimationSum = {0.0f, 0.0f};
    networkIqFrameBuffer.clear();
    networkIqFrameSampleRate = 0.0;
}

void DataProcessor::emitIqFrame(const float *samples, std::size_t floatCount) {
    if (!samples || floatCount == 0 || floatCount > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return;
    }

    if (networkIqResetRequested.exchange(false)) {
        resetNetworkIqState();
    }

    if (requestedChannelizeIqFrames.load()) {
        RadioSettings settings;
        {
            std::lock_guard<std::mutex> lock(networkIqSettingsMutex);
            settings = networkIqSettings;
        }
        emitChannelIqFrame(samples, floatCount, settings);
        return;
    }

    emitFullIqFrame(samples, floatCount);
}

void DataProcessor::emitFullIqFrame(const float *samples, std::size_t floatCount) {
    QByteArray quantized;
    quantized.resize(static_cast<int>(floatCount));
    char *dst = quantized.data();
    for (std::size_t i = 0; i < floatCount; ++i) {
        dst[i] = quantizeIqSample(samples[i]);
    }

    emit iqFrameReady(quantized,
                      requestedSampleRate.load(),
                      static_cast<int>(floatCount / FLOATS_PER_IQ_SAMPLE), false);
}

void DataProcessor::emitChannelIqFrame(const float *samples,
                                       std::size_t floatCount,
                                       const RadioSettings &settings) {
    const double inputRate = settings.sampleRate;
    if (inputRate <= 0.0 || floatCount < FLOATS_PER_IQ_SAMPLE) {
        return;
    }

    const bool dmrChannelMode = settings.modulationType == MOD_DMR;
    const bool adaptiveNoiseCancel = settings.inputMode == INPUT_HF_NOISE_CANCEL;
    if (!dmrChannelMode && !adaptiveNoiseCancel) {
        const IqChannelizer::Result result =
            networkIqChannelizer.processFloatIq(samples,
                                                floatCount,
                                                settings,
                                                networkIqChannelizerOutput);
        if (!result.valid || result.outputRate <= 0.0) {
            return;
        }
        if (std::abs(networkIqFrameSampleRate - result.outputRate) > 0.5) {
            networkIqFrameBuffer.clear();
            networkIqFrameSampleRate = result.outputRate;
        }
        const int frameSamples =
            channelizerFrameSamplesForRate(result.outputRate, NETWORK_CHANNEL_FRAME_SAMPLES);
        for (std::size_t i = 0; i + 1 < networkIqChannelizerOutput.size(); i += 2U) {
            appendInt16Le(networkIqFrameBuffer, networkIqChannelizerOutput[i]);
            appendInt16Le(networkIqFrameBuffer, networkIqChannelizerOutput[i + 1U]);
            if (networkIqFrameBuffer.size() >=
                frameSamples * FLOATS_PER_IQ_SAMPLE * static_cast<int>(sizeof(qint16))) {
                emit iqFrameReady(networkIqFrameBuffer,
                                  result.outputRate,
                                  networkIqFrameBuffer.size() /
                                      (FLOATS_PER_IQ_SAMPLE * static_cast<int>(sizeof(qint16))), true);
                networkIqFrameBuffer.clear();
            }
        }
        return;
    }

    const std::size_t iqSamples = floatCount / FLOATS_PER_IQ_SAMPLE;
    const double targetRate = channelizerTargetRate(settings);
    const int decimationFactor = (std::max)(1, static_cast<int>(std::floor(inputRate / targetRate)));
    const double outputRate = inputRate / static_cast<double>(decimationFactor);
    const double cutoff = channelizerCutoff(settings, outputRate);
    const int frameSamples = channelizerFrameSamplesForRate(outputRate, NETWORK_CHANNEL_FRAME_SAMPLES);
    const float lowPassAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI * cutoff / outputRate),
        0.000001,
        1.0
        ));
    if (dmrChannelMode &&
        (networkIqLastLoggedDmrOutputRate != static_cast<int>(std::lround(outputRate)) ||
         networkIqLastLoggedDmrDecimationFactor != decimationFactor)) {
        networkIqLastLoggedDmrOutputRate = static_cast<int>(std::lround(outputRate));
        networkIqLastLoggedDmrDecimationFactor = decimationFactor;
        qDebug() << "[DMR channel IQ] channelizer"
                 << "inputRate" << inputRate
                 << "targetRate" << targetRate
                 << "outputRate" << outputRate
                 << "decimation" << decimationFactor
                 << "requested4fskRate"
                 << normalizedDmrBasebandSampleRate(settings.dmrBasebandSampleRate)
                 << "cicStages" << DMR_CHANNEL_CIC_STAGES
                 << "cutoffHz" << cutoff;
    }
    const float preLowPassAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI * cutoff / inputRate),
        0.000001,
        1.0
        ));
    if (dmrChannelMode && networkIqCicLength != decimationFactor) {
        for (auto &buffer : networkIqCicBuffers) {
            buffer.assign(static_cast<std::size_t>(decimationFactor),
                          std::complex<float>(0.0f, 0.0f));
        }
        networkIqCicSums.fill(std::complex<float>(0.0f, 0.0f));
        networkIqCicIndex = 0;
        networkIqCicLength = decimationFactor;
        networkIqDecimationSum = std::complex<float>(0.0f, 0.0f);
        networkIqDecimationCount = 0;
    }
    const float cicInvLength = dmrChannelMode
                                   ? 1.0f / static_cast<float>(
                                         (std::max)(1, networkIqCicLength))
                                   : 1.0f;

    if (std::abs(networkIqFrameSampleRate - outputRate) > 0.5) {
        networkIqFrameBuffer.clear();
        networkIqFrameSampleRate = outputRate;
    }

    const double fShift = settings.listeningFrequency - settings.centerFrequency;
    const double phaseIncrement = -TWO_PI * fShift / inputRate;
    const bool noFrequencyShift = std::abs(fShift) < 0.5;
    float rotI = 1.0f;
    float rotQ = 0.0f;
    float rotStepI = 1.0f;
    float rotStepQ = 0.0f;
    if (!noFrequencyShift) {
        rotI = static_cast<float>(std::cos(networkIqNcoPhase));
        rotQ = static_cast<float>(std::sin(networkIqNcoPhase));
        rotStepI = static_cast<float>(std::cos(phaseIncrement));
        rotStepQ = static_cast<float>(std::sin(phaseIncrement));
    }

    std::complex<float> decimationSum = networkIqDecimationSum;
    int decimationCount = networkIqDecimationCount;
    std::complex<float> lowPass = networkIqLowPassState;
    std::array<std::complex<float>, 3> preLowPassStates = networkIqPreLowPassStates;
    std::complex<float> refDecimationSum = networkHfNoiseCancelRefDecimationSum;
    std::complex<float> adaptiveCoeff = networkHfNoiseCancelCoeff;
    if (!adaptiveNoiseCancel) {
        networkHfNoiseCancelCoeff = {0.0f, 0.0f};
        networkHfNoiseCancelRefDecimationSum = {0.0f, 0.0f};
    }
    const float noiseCancelDepth =
        static_cast<float>((std::clamp)(settings.hfNoiseCancelDepth, 0.0, 2.0));
    const std::complex<float> manualRefCoeff =
        hfNoiseCancelReferenceCoefficient(settings, settings.listeningFrequency);
    constexpr float adaptiveMu = 0.035f;
    constexpr float adaptiveEpsilon = 1.0e-8f;

    for (std::size_t n = 0; n < iqSamples; ++n) {
        float iSample = samples[2 * n];
        float qSample = samples[2 * n + 1];
        if (!std::isfinite(iSample)) {
            iSample = 0.0f;
        }
        if (!std::isfinite(qSample)) {
            qSample = 0.0f;
        }

        if (adaptiveNoiseCancel) {
            if (noFrequencyShift) {
                decimationSum += std::complex<float>(iSample, 0.0f);
                refDecimationSum += std::complex<float>(qSample, 0.0f);
            } else {
                const std::complex<float> oscillator(rotI, rotQ);
                decimationSum += iSample * oscillator;
                refDecimationSum += qSample * oscillator;
            }
        } else {
            if (settings.inputMode == INPUT_HF_COMBINED) {
                if (fShift < 0.0) {
                    qSample = 0.0f;
                } else {
                    iSample = qSample;
                    qSample = 0.0f;
                }
            } else if (settings.inputMode == INPUT_HF1) {
                qSample = 0.0f;
            } else if (settings.inputMode == INPUT_HF2) {
                iSample = qSample;
                qSample = 0.0f;
            }
            std::complex<float> mixedSample;
            if (noFrequencyShift) {
                mixedSample = std::complex<float>(iSample, qSample);
            } else {
                const float mixedI = iSample * rotI - qSample * rotQ;
                const float mixedQ = iSample * rotQ + qSample * rotI;
                mixedSample = std::complex<float>(mixedI, mixedQ);
            }
            if (dmrChannelMode && decimationFactor > 1) {
                std::complex<float> cicSample = mixedSample;
                for (int stage = 0; stage < DMR_CHANNEL_CIC_STAGES; ++stage) {
                    auto &buffer = networkIqCicBuffers[static_cast<std::size_t>(stage)];
                    const std::complex<float> delayed =
                        buffer[static_cast<std::size_t>(networkIqCicIndex)];
                    buffer[static_cast<std::size_t>(networkIqCicIndex)] = cicSample;
                    networkIqCicSums[static_cast<std::size_t>(stage)] +=
                        cicSample - delayed;
                    cicSample =
                        networkIqCicSums[static_cast<std::size_t>(stage)] * cicInvLength;
                }
                ++networkIqCicIndex;
                if (networkIqCicIndex >= networkIqCicLength) {
                    networkIqCicIndex = 0;
                }
                mixedSample = cicSample;
            } else if (decimationFactor > 1) {
                preLowPassStates[0] += preLowPassAlpha *
                                       (mixedSample - preLowPassStates[0]);
                preLowPassStates[1] += preLowPassAlpha *
                                       (preLowPassStates[0] - preLowPassStates[1]);
                preLowPassStates[2] += preLowPassAlpha *
                                       (preLowPassStates[1] - preLowPassStates[2]);
                mixedSample = preLowPassStates[2];
            }
            decimationSum += mixedSample;
        }
        ++decimationCount;

        if (!noFrequencyShift) {
            const float nextRotI = rotI * rotStepI - rotQ * rotStepQ;
            const float nextRotQ = rotI * rotStepQ + rotQ * rotStepI;
            rotI = nextRotI;
            rotQ = nextRotQ;
            if ((n & 4095) == 4095) {
                const float norm = std::sqrt(rotI * rotI + rotQ * rotQ);
                if (norm > 0.0f) {
                    rotI /= norm;
                    rotQ /= norm;
                }
            }
        }

        if (decimationCount < decimationFactor) {
            continue;
        }

        const float invCount = 1.0f / static_cast<float>(decimationCount);
        std::complex<float> channelSample = decimationSum * invCount;
        if (adaptiveNoiseCancel) {
            const std::complex<float> refSample = manualRefCoeff * refDecimationSum * invCount;
            if (!settings.hfNoiseCancelFreeze) {
                const float refPower = std::norm(refSample);
                if (std::isfinite(refPower) && refPower > adaptiveEpsilon) {
                    const std::complex<float> prediction = adaptiveCoeff * refSample;
                    const std::complex<float> error = channelSample - prediction;
                    adaptiveCoeff += adaptiveMu * error * std::conj(refSample) /
                                     (refPower + adaptiveEpsilon);
                    adaptiveCoeff = clampComplexMagnitude(adaptiveCoeff, HF_NOISE_CANCEL_MAX_COEFF);
                    networkHfNoiseCancelCoeff = adaptiveCoeff;
                }
            }
            channelSample -= noiseCancelDepth * adaptiveCoeff * refSample;
            refDecimationSum = {0.0f, 0.0f};
        }
        decimationSum = std::complex<float>(0.0f, 0.0f);
        decimationCount = 0;

        lowPass += lowPassAlpha * (channelSample - lowPass);
        const float magnitude = std::abs(lowPass);
        const float agcCoeff = magnitude > networkIqAgcLevel ? 0.01f : 0.0002f;
        networkIqAgcLevel += agcCoeff * (magnitude - networkIqAgcLevel);
        networkIqAgcLevel = (std::max)(networkIqAgcLevel, 0.00001f);
        const float gain = NETWORK_CHANNEL_IQ_TARGET_LEVEL / networkIqAgcLevel;
        appendInt16Le(networkIqFrameBuffer, std::real(lowPass) * gain);
        appendInt16Le(networkIqFrameBuffer, std::imag(lowPass) * gain);

        if (networkIqFrameBuffer.size() >= frameSamples * FLOATS_PER_IQ_SAMPLE * static_cast<int>(sizeof(qint16))) {
            emit iqFrameReady(networkIqFrameBuffer,
                              outputRate,
                              networkIqFrameBuffer.size() / (FLOATS_PER_IQ_SAMPLE * static_cast<int>(sizeof(qint16))), true);
            networkIqFrameBuffer.clear();
        }
    }

    networkIqDecimationSum = decimationSum;
    networkIqDecimationCount = decimationCount;
    networkIqPreLowPassStates = preLowPassStates;
    networkHfNoiseCancelRefDecimationSum =
        adaptiveNoiseCancel ? refDecimationSum : std::complex<float>(0.0f, 0.0f);
    networkIqLowPassState = lowPass;
    networkIqNcoPhase = noFrequencyShift
                            ? 0.0
                            : std::remainder(networkIqNcoPhase +
                                                 phaseIncrement * static_cast<double>(iqSamples),
                                             TWO_PI);
}

uint64_t DataProcessor::callbackCount() const {
    return totalCallbackCounter.load();
}

bool DataProcessor::wantsAgileScanMetadata() const {
    return requestedAgileScanEnabled.load(std::memory_order_acquire) &&
           activeStreamKind == ReceiverBackendStreamKind::FobosAgile &&
           !activeStreamDescriptor.agileScanFrequenciesHz.isEmpty();
}

void DataProcessor::setSampleRateHint(double sampleRate) {
    if (sampleRate <= 0.0 || !std::isfinite(sampleRate)) {
        return;
    }
    requestedSampleRate = sampleRate;
    IqBuffer::setSampleRateEstimate(sampleRate);
}

void DataProcessor::setCenterFrequencyHint(double centerFrequency) {
    if (centerFrequency <= 0.0 || !std::isfinite(centerFrequency)) {
        return;
    }
    requestedCenterFrequency = centerFrequency;
}

void DataProcessor::setFrequencyCalibrationOffset(double offsetHz) {
    frequencyCalibrationOffsetHz = std::isfinite(offsetHz) ? offsetHz : 0.0;
    activeStreamDescriptor.frequencyCalibrationOffsetHz = frequencyCalibrationOffsetHz.load();
}

uint64_t DataProcessor::beginIqRetuneBarrier() {
    const uint64_t epoch = iqRetuneEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    return epoch;
}

void DataProcessor::startRetuneRawDump(const QString &reason,
                                       uint64_t epoch,
                                       double previousCenterHz,
                                       double requestedCenterHz,
                                       double actualCenterHz,
                                       int maxBlocks) {
    if (!fobosVerboseLoggingEnabled()) {
        return;
    }

    maxBlocks = (std::clamp)(maxBlocks, 1, RETUNE_RAW_DUMP_MAX_BLOCKS);
    const QString appDirPath = QCoreApplication::applicationDirPath();
    const QString baseDirPath = QDir(appDirPath.isEmpty()
                                         ? QDir::currentPath()
                                         : appDirPath)
                                    .filePath(QStringLiteral("recordings/retune_raw"));
    QDir baseDir(baseDirPath);
    if (!baseDir.exists() && !baseDir.mkpath(QStringLiteral("."))) {
        qDebug() << "[RetuneRawDump] failed to create directory" << baseDirPath;
        return;
    }

    const QString timestamp = QDateTime::currentDateTime()
                                  .toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString token = safeFileToken(reason);
    const QString basePath =
        baseDir.filePath(QStringLiteral("ObriiSDR_retune_%1_epoch%2_%3")
                             .arg(timestamp,
                                  QString::number(static_cast<qulonglong>(epoch)),
                                  token));

    std::lock_guard<std::mutex> lock(retuneRawDumpMutex);
    if (retuneRawDumpActive || !retuneRawDumpBytes.isEmpty()) {
        finishRetuneRawDumpLocked(QStringLiteral("interrupted_by_new_retune"));
    }

    retuneRawDumpActive = true;
    retuneRawDumpEpoch = epoch;
    retuneRawDumpTriggerCallback = totalCallbackCounter.load();
    retuneRawDumpBlocksRequested = maxBlocks;
    retuneRawDumpBlocksRemaining = maxBlocks;
    retuneRawDumpBlocksCaptured = 0;
    retuneRawDumpSamplesCaptured = 0;
    retuneRawDumpPreviousCenterHz = previousCenterHz;
    retuneRawDumpRequestedCenterHz = requestedCenterHz;
    retuneRawDumpActualCenterHz = actualCenterHz;
    retuneRawDumpSampleRateHz = requestedSampleRate.load();
    retuneRawDumpReason = reason;
    retuneRawDumpBasePath = basePath;
    retuneRawDumpBytes.clear();
    retuneRawDumpBytes.reserve(maxBlocks * 262144 * FLOATS_PER_IQ_SAMPLE * static_cast<int>(sizeof(float)));
    retuneRawDumpBlockStats = QJsonArray();

    qDebug() << "[RetuneRawDump] armed"
             << "path" << (basePath + QStringLiteral(".f32iq"))
             << "epoch" << static_cast<qulonglong>(epoch)
             << "blocks" << maxBlocks
             << "previous" << previousCenterHz
             << "requested" << requestedCenterHz
             << "sampleRate" << retuneRawDumpSampleRateHz;
}

void DataProcessor::captureRetuneRawDumpBlock(const float *samples,
                                              uint32_t sampleCount,
                                              uint64_t callbackEpoch) {
    if (!samples || sampleCount == 0 || !fobosVerboseLoggingEnabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(retuneRawDumpMutex);
    if (!retuneRawDumpActive || retuneRawDumpBlocksRemaining <= 0) {
        return;
    }

    const std::size_t floatCount =
        static_cast<std::size_t>(sampleCount) * FLOATS_PER_IQ_SAMPLE;
    const int byteCount = static_cast<int>((std::min)(
        floatCount * sizeof(float),
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    retuneRawDumpBytes.append(reinterpret_cast<const char*>(samples), byteCount);

    double meanI = 0.0;
    double meanQ = 0.0;
    double power = 0.0;
    double phaseStepSum = 0.0;
    double phaseStepAbsSum = 0.0;
    uint64_t inspectedSamples = 0;
    uint64_t phaseStepCount = 0;
    uint64_t nonFiniteSamples = 0;
    uint64_t clippedSamples = 0;
    bool havePreviousPhaseSample = false;
    double previousI = 0.0;
    double previousQ = 0.0;
    const uint32_t inspectCount =
        (std::min)(sampleCount, RETUNE_RAW_DUMP_MAX_INSPECT_SAMPLES);
    const uint32_t stride = (std::max)(uint32_t(1), sampleCount / inspectCount);
    for (uint32_t n = 0, inspected = 0; n < sampleCount && inspected < inspectCount; n += stride, ++inspected) {
        const float iSample = samples[static_cast<std::size_t>(n) * 2U];
        const float qSample = samples[static_cast<std::size_t>(n) * 2U + 1U];
        if (!std::isfinite(iSample) || !std::isfinite(qSample)) {
            ++nonFiniteSamples;
            continue;
        }
        meanI += static_cast<double>(iSample);
        meanQ += static_cast<double>(qSample);
        power += static_cast<double>(iSample) * iSample +
                 static_cast<double>(qSample) * qSample;
        ++inspectedSamples;
        if (havePreviousPhaseSample) {
            const double cross = previousI * static_cast<double>(qSample) -
                                 previousQ * static_cast<double>(iSample);
            const double dot = previousI * static_cast<double>(iSample) +
                               previousQ * static_cast<double>(qSample);
            const double phaseStep = std::atan2(cross, dot);
            if (std::isfinite(phaseStep)) {
                phaseStepSum += phaseStep;
                phaseStepAbsSum += std::abs(phaseStep);
                ++phaseStepCount;
            }
        }
        previousI = iSample;
        previousQ = qSample;
        havePreviousPhaseSample = true;
        if (std::abs(iSample) >= 0.999f || std::abs(qSample) >= 0.999f) {
            ++clippedSamples;
        }
    }

    const double inspected = static_cast<double>((std::max)(uint64_t(1), inspectedSamples));
    const double phaseCount = static_cast<double>((std::max)(uint64_t(1), phaseStepCount));
    QJsonObject block;
    block.insert(QStringLiteral("index"), retuneRawDumpBlocksCaptured);
    block.insert(QStringLiteral("callback"), static_cast<double>(totalCallbackCounter.load()));
    block.insert(QStringLiteral("epoch"), static_cast<double>(callbackEpoch));
    block.insert(QStringLiteral("sampleCount"), static_cast<double>(sampleCount));
    block.insert(QStringLiteral("centerFrequencyHint"), requestedCenterFrequency.load());
    block.insert(QStringLiteral("meanI"), meanI / inspected);
    block.insert(QStringLiteral("meanQ"), meanQ / inspected);
    block.insert(QStringLiteral("rms"), std::sqrt((std::max)(0.0, power / inspected * 0.5)));
    block.insert(QStringLiteral("phaseStepMeanRad"), phaseStepSum / phaseCount);
    block.insert(QStringLiteral("phaseStepAbsMeanRad"), phaseStepAbsSum / phaseCount);
    block.insert(QStringLiteral("nonFiniteSamples"), static_cast<double>(nonFiniteSamples));
    block.insert(QStringLiteral("clippedSamples"), static_cast<double>(clippedSamples));
    retuneRawDumpBlockStats.append(block);

    ++retuneRawDumpBlocksCaptured;
    retuneRawDumpSamplesCaptured += sampleCount;
    --retuneRawDumpBlocksRemaining;
    if (retuneRawDumpBlocksRemaining <= 0) {
        finishRetuneRawDumpLocked(QStringLiteral("complete"));
    }
}

void DataProcessor::finishRetuneRawDumpLocked(const QString &status) {
    if (!retuneRawDumpActive && retuneRawDumpBytes.isEmpty()) {
        return;
    }

    const QString rawPath = retuneRawDumpBasePath + QStringLiteral(".f32iq");
    const QString jsonPath = retuneRawDumpBasePath + QStringLiteral(".json");
    QJsonObject root;
    root.insert(QStringLiteral("app"), QStringLiteral("ObriiSDR"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("status"), status);
    root.insert(QStringLiteral("format"), QStringLiteral("float32_le_interleaved_iq"));
    root.insert(QStringLiteral("rawFile"), QFileInfo(rawPath).fileName());
    root.insert(QStringLiteral("jsonFile"), QFileInfo(jsonPath).fileName());
    root.insert(QStringLiteral("reason"), retuneRawDumpReason);
    root.insert(QStringLiteral("epoch"), static_cast<double>(retuneRawDumpEpoch));
    root.insert(QStringLiteral("triggerCallback"), static_cast<double>(retuneRawDumpTriggerCallback));
    root.insert(QStringLiteral("blocksRequested"), retuneRawDumpBlocksRequested);
    root.insert(QStringLiteral("blocksCaptured"), retuneRawDumpBlocksCaptured);
    root.insert(QStringLiteral("complexSamplesCaptured"), static_cast<double>(retuneRawDumpSamplesCaptured));
    root.insert(QStringLiteral("floatCount"), static_cast<double>(retuneRawDumpBytes.size() / sizeof(float)));
    root.insert(QStringLiteral("byteCount"), static_cast<double>(retuneRawDumpBytes.size()));
    root.insert(QStringLiteral("sampleRateHz"), retuneRawDumpSampleRateHz);
    root.insert(QStringLiteral("previousCenterHz"), retuneRawDumpPreviousCenterHz);
    root.insert(QStringLiteral("requestedCenterHz"), retuneRawDumpRequestedCenterHz);
    root.insert(QStringLiteral("actualCenterAtTriggerHz"), retuneRawDumpActualCenterHz);
    root.insert(QStringLiteral("blocks"), retuneRawDumpBlockStats);

    QByteArray rawBytes = std::move(retuneRawDumpBytes);
    QByteArray jsonBytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    const int blocksCaptured = retuneRawDumpBlocksCaptured;
    const quint64 samplesCaptured = retuneRawDumpSamplesCaptured;

    retuneRawDumpActive = false;
    retuneRawDumpEpoch = 0;
    retuneRawDumpTriggerCallback = 0;
    retuneRawDumpBlocksRequested = 0;
    retuneRawDumpBlocksRemaining = 0;
    retuneRawDumpBlocksCaptured = 0;
    retuneRawDumpSamplesCaptured = 0;
    retuneRawDumpPreviousCenterHz = 0.0;
    retuneRawDumpRequestedCenterHz = 0.0;
    retuneRawDumpActualCenterHz = 0.0;
    retuneRawDumpSampleRateHz = 0.0;
    retuneRawDumpReason.clear();
    retuneRawDumpBasePath.clear();
    retuneRawDumpBlockStats = QJsonArray();

    std::thread([rawPath,
                 jsonPath,
                 rawBytes = std::move(rawBytes),
                 jsonBytes = std::move(jsonBytes),
                 blocksCaptured,
                 samplesCaptured]() mutable {
        QFile rawFile(rawPath);
        bool rawOk = false;
        if (rawFile.open(QIODevice::WriteOnly)) {
            rawOk = rawFile.write(rawBytes) == rawBytes.size();
            rawFile.close();
        }

        QFile jsonFile(jsonPath);
        bool jsonOk = false;
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            jsonOk = jsonFile.write(jsonBytes) == jsonBytes.size();
            jsonFile.close();
        }

        qDebug() << "[RetuneRawDump] saved"
                 << "raw" << rawPath
                 << "rawOk" << rawOk
                 << "json" << jsonPath
                 << "jsonOk" << jsonOk
                 << "blocks" << blocksCaptured
                 << "samples" << static_cast<qulonglong>(samplesCaptured);
    }).detach();
}

bool DataProcessor::retuneCenterFrequency(double centerFrequencyHz) {
    if (!std::isfinite(centerFrequencyHz) || centerFrequencyHz <= 0.0) {
        return false;
    }

    requestedCenterFrequency = centerFrequencyHz;
    const double hardwareFrequencyHz = centerFrequencyHz + frequencyCalibrationOffsetHz.load();
    if (!std::isfinite(hardwareFrequencyHz) || hardwareFrequencyHz <= 0.0) {
        return false;
    }
    const ReceiverBackendStreamKind streamKind = activeStreamKind;
    if (streamKind == ReceiverBackendStreamKind::RtlTcp) {
        qDebug() << "[RTL-TCP] live center retune queued" << centerFrequencyHz;
        return running.load();
    }
    if (streamKind == ReceiverBackendStreamKind::SoapySdr) {
        void *soapyDevice = activeDevice.load();
        if (!running.load() || !soapyDevice) {
            qDebug() << "[SoapySDR] live center retune requested without active handle"
                     << "frequency" << centerFrequencyHz
                     << "running" << running.load()
                     << "device" << soapyDevice;
            return false;
        }
        const int result = setSoapySdrCenterFrequencySafely(soapyDevice, hardwareFrequencyHz);
        qDebug() << "[SoapySDR] live center retune"
                 << "logical" << centerFrequencyHz
                 << "hardware" << hardwareFrequencyHz
                 << "result" << result;
        return result == 0;
    }
    if (streamKind == ReceiverBackendStreamKind::BladeRfNative) {
        void *bladeDevice = activeDevice.load();
        if (!running.load() || !bladeDevice) {
            qDebug() << "[bladeRF] live center retune requested without active handle"
                     << "frequency" << centerFrequencyHz
                     << "running" << running.load()
                     << "device" << bladeDevice;
            return false;
        }
        const uint64_t frequencyHz = static_cast<uint64_t>((std::max)(0.0, hardwareFrequencyHz));
        const int result = setBladeRfCenterFrequencySafely(bladeDevice, frequencyHz);
        qDebug() << "[bladeRF] live center retune"
                 << "logical" << centerFrequencyHz
                 << "frequency" << frequencyHz
                 << "result" << result;
        return result == 0;
    }
    if (streamKind == ReceiverBackendStreamKind::HackRfNative) {
        void *hackRfDevice = activeDevice.load();
        if (!running.load() || !hackRfDevice) {
            qDebug() << "[HackRF] live center retune requested without active handle"
                     << "frequency" << centerFrequencyHz
                     << "running" << running.load()
                     << "device" << hackRfDevice;
            return false;
        }
        const std::uint64_t frequencyHz = static_cast<std::uint64_t>(
            (std::clamp)(hardwareFrequencyHz, 1000000.0, 6000000000.0));
        int result = 0;
        if (activeStreamDescriptor.hackRfExplicitTuningEnabled) {
            std::uint64_t ifHz = frequencyHz;
            const std::uint64_t loHz = activeStreamDescriptor.hackRfExplicitLoHz;
            const int path = (std::clamp)(activeStreamDescriptor.hackRfExplicitPath, 0, 2);
            if (path == static_cast<int>(HackRfRfPath::LowPass)) {
                ifHz = frequencyHz + loHz;
            } else if (path == static_cast<int>(HackRfRfPath::HighPass)) {
                ifHz = frequencyHz > loHz ? frequencyHz - loHz : 0;
            }
            result = setHackRfExplicitFrequencySafely(
                hackRfDevice, ifHz, loHz, static_cast<HackRfRfPath>(path));
        } else {
            result = setHackRfCenterFrequencySafely(hackRfDevice, frequencyHz);
        }
        qDebug() << "[HackRF] live center retune"
                 << "logical" << centerFrequencyHz
                 << "frequency" << frequencyHz
                 << "result" << result;
        return result == 0;
    }
    if (streamKind != ReceiverBackendStreamKind::RtlSdrNative) {
        return false;
    }

    void *rtlDevice = activeDevice.load();
    if (!running.load() || !rtlDevice) {
        qDebug() << "[RTL-SDR] live center retune requested without active native handle"
                 << "frequency" << centerFrequencyHz
                 << "running" << running.load()
                 << "device" << rtlDevice;
        return false;
    }

    const uint32_t frequencyHz = static_cast<uint32_t>((std::max)(0.0, hardwareFrequencyHz));
    const int result = setRtlSdrCenterFrequencySafely(rtlDevice, frequencyHz);
    qDebug() << "[RTL-SDR] live center retune"
             << "logical" << centerFrequencyHz
             << "frequency" << frequencyHz
             << "result" << result;
    return result == 0;
}

bool DataProcessor::applyHackRfGainSettings(int lnaGainDb, int vgaGainDb) {
    if (activeStreamKind != ReceiverBackendStreamKind::HackRfNative) {
        return false;
    }
    void *hackRfDevice = activeDevice.load();
    if (!running.load() || !hackRfDevice) {
        return false;
    }

    const int snappedLnaDb = (std::clamp)(lnaGainDb, 0, 40) / 8 * 8;
    const int snappedVgaDb = (std::clamp)(vgaGainDb, 0, 62) / 2 * 2;
    const int lnaResult = setHackRfLnaGainSafely(hackRfDevice,
                                                 static_cast<std::uint32_t>(snappedLnaDb));
    const int vgaResult = setHackRfVgaGainSafely(hackRfDevice,
                                                 static_cast<std::uint32_t>(snappedVgaDb));
    qDebug() << "[HackRF] live gains"
             << "LNA" << snappedLnaDb << "result" << lnaResult
             << "VGA" << snappedVgaDb << "result" << vgaResult;
    return lnaResult == 0 && vgaResult == 0;
}

bool DataProcessor::queryHackRfDiagnostics(HackRfDeviceDiagnostics *diagnostics,
                                           QString *errorMessage) {
    if (activeStreamKind != ReceiverBackendStreamKind::HackRfNative) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("HackRF is not the active receiver");
        }
        return false;
    }
    void *hackRfDevice = activeDevice.load();
    if (!running.load() || !hackRfDevice) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("HackRF stream is not active");
        }
        return false;
    }
    return queryOpenHackRfDeviceDiagnostics(hackRfDevice, diagnostics, errorMessage);
}

bool DataProcessor::applyRtlGainSettings(bool agc, int gainTenthsDb) {
    if (activeStreamKind != ReceiverBackendStreamKind::RtlSdrNative) {
        qDebug() << "[RTL-SDR] live gain skipped: backend is not native RTL"
                 << "backend" << static_cast<int>(activeStreamKind);
        return false;
    }

    void *rtlDevice = activeDevice.load();
    if (!running.load() || !rtlDevice) {
        qDebug() << "[RTL-SDR] live gain requested without active native handle"
                 << "agc" << agc
                 << "gainTenthsDb" << gainTenthsDb
                 << "running" << running.load()
                 << "device" << rtlDevice;
        return false;
    }

    const bool manualGain = !agc;
    int result = setRtlSdrTunerGainModeSafely(rtlDevice, manualGain ? 1 : 0);
    qDebug() << "[RTL-SDR] live gain mode"
             << (manualGain ? QStringLiteral("manual") : QStringLiteral("auto"))
             << "result" << result;
    bool ok = result == 0;
    if (manualGain) {
        result = setRtlSdrTunerGainSafely(rtlDevice, gainTenthsDb);
        qDebug() << "[RTL-SDR] live tuner gain" << gainTenthsDb << "result" << result;
        ok = ok && result == 0;
    }
    result = setRtlSdrAgcModeSafely(rtlDevice, agc ? 1 : 0);
    qDebug() << "[RTL-SDR] live agc" << agc << "result" << result;
    ok = ok && result == 0;
    return ok;
}

void DataProcessor::requestStop() {
    if (!running.load() && !QThread::isRunning() && activeDevice.load() == nullptr) {
        return;
    }

    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] requestStop enter"
                 << "runningFlag" << running.load()
                 << "threadRunning" << QThread::isRunning()
                 << "activeSyncMode" << activeSyncMode.load()
                 << "activeDevice" << activeDevice.load();
    }
    const bool wasRunning = running.exchange(false);
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] running flag cleared" << "wasRunning" << wasRunning;
    }
    if (wasRunning || QThread::isRunning()) {
        void *readerDevice = activeDevice.load();
        if (!activeSyncMode.load()) {
            if (readerDevice) {
                if (fobosVerboseLoggingEnabled()) {
                    qDebug() << "[DataProcessor] fobos_rx_cancel_async begin" << readerDevice;
                }
                if (activeStreamKind == ReceiverBackendStreamKind::RtlTcp) {
                    if (fobosVerboseLoggingEnabled()) {
                        qDebug() << "[DataProcessor] rtl_tcp stop requested; socket reader will exit from running flag";
                    }
                } else if (activeStreamKind == ReceiverBackendStreamKind::SoapySdr) {
                    if (fobosVerboseLoggingEnabled()) {
                        qDebug() << "[DataProcessor] SoapySDR stop requested; readStream loop will exit after timeout";
                    }
                } else if (activeStreamKind == ReceiverBackendStreamKind::BladeRfNative) {
                    if (readerDevice) {
                        enableBladeRfRxSafely(readerDevice, false);
                    }
                    if (fobosVerboseLoggingEnabled()) {
                        qDebug() << "[DataProcessor] bladeRF stop requested; sync_rx loop will exit after timeout";
                    }
                } else if (activeStreamKind == ReceiverBackendStreamKind::HackRfNative) {
                    const int result = stopHackRfRxSafely(readerDevice);
                    if (fobosVerboseLoggingEnabled() || result != 0) {
                        qDebug() << "[DataProcessor] HackRF async stop" << "result" << result;
                    }
                } else {
                    bool expected = false;
                    if (asyncCancelRequested.compare_exchange_strong(expected, true)) {
                        const ReceiverBackendStreamKind streamKind = activeStreamKind;
                        const FobosApiKind apiKind = activeApiKind.load();
                        std::thread([readerDevice, streamKind, apiKind]() {
                            int ret = FOBOS_ERR_OK;
                            if (streamKind == ReceiverBackendStreamKind::RtlSdrNative) {
                                ret = cancelRtlSdrAsyncSafely(readerDevice);
                            } else {
                                ret = apiKind == FobosApiKind::Agile
                                          ? cancelFobosAgileAsyncSafely(static_cast<fobos_sdr_dev_t*>(readerDevice))
                                          : cancelFobosAsyncSafely(static_cast<fobos_dev_t*>(readerDevice));
                            }
                            if (fobosVerboseLoggingEnabled() || ret != FOBOS_ERR_OK) {
                                qDebug() << "[DataProcessor] async cancel worker finished"
                                         << "streamKind" << static_cast<int>(streamKind)
                                         << "result" << ret;
                            }
                        }).detach();
                    } else if (fobosVerboseLoggingEnabled()) {
                        qDebug() << "[DataProcessor] async cancel already requested";
                    }
                }
            }
        } else {
            if (readerDevice) {
                if (fobosVerboseLoggingEnabled()) {
                    qDebug() << "[DataProcessor] fobos_rx_stop_sync begin" << readerDevice;
                }
                const int ret = activeApiKind.load() == FobosApiKind::Agile
                                    ? stopFobosAgileSyncSafely(static_cast<fobos_sdr_dev_t*>(readerDevice))
                                    : stopFobosSyncSafely(static_cast<fobos_dev_t*>(readerDevice));
                if (fobosVerboseLoggingEnabled() || ret != FOBOS_ERR_OK) {
                    qDebug() << "[DataProcessor] fobos_rx_stop_sync end" << "result" << ret;
                }
            }
        }
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DataProcessor] QThread::quit begin";
        }
        QThread::quit();
    }
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] requestStop exit" << "threadRunning" << QThread::isRunning();
    }
}

void DataProcessor::finalizeStopped() {
    if (!QThread::isRunning()) {
        const bool wasRunning = running.load();
        void *previousDevice = activeDevice.load();
        running = false;
        activeDevice = nullptr;
        if (fobosVerboseLoggingEnabled() && (wasRunning || previousDevice)) {
            qDebug() << "[DataProcessor] finalizeStopped: thread is stopped";
        }
    }
}

bool DataProcessor::forceStop(int timeoutMs) {
    qDebug() << "[DataProcessor] forceStop enter"
             << "timeoutMs" << timeoutMs
             << "threadRunning" << QThread::isRunning()
             << "activeDevice" << activeDevice.load();
    running = false;
    if (QThread::isRunning()) {
        qDebug() << "[DataProcessor] QThread::terminate begin";
        QThread::terminate();
        qDebug() << "[DataProcessor] QThread::wait after terminate begin" << "timeoutMs" << timeoutMs;
        if (!QThread::wait(timeoutMs)) {
            qDebug() << "[DataProcessor] QThread::terminate did not stop the thread within timeout.";
            return false;
        }
    }
    activeDevice = nullptr;
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] forceStop exit" << "threadRunning" << QThread::isRunning();
    }
    return !QThread::isRunning();
}

bool DataProcessor::stop(int timeoutMs) {
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] stop enter" << "timeoutMs" << timeoutMs;
    }
    requestStop();
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] QThread::wait begin" << "timeoutMs" << timeoutMs;
    }
    if (QThread::isRunning() && !QThread::wait(timeoutMs)) {
        qDebug() << "Error: DataProcessor thread did not quit within timeout.";
        return false;
    }
    finalizeStopped();
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DataProcessor] stop exit" << "threadRunning" << QThread::isRunning();
    }
    return !QThread::isRunning();
}
