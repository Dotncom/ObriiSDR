#ifndef DATAPROCESSOR_H
#define DATAPROCESSOR_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <cmath>
#include <QDebug>
#include <cstring>
#include <QVector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <QElapsedTimer>
#include <QByteArray>
#include <QJsonArray>
#include <QString>
#include <atomic>
#include <array>
#include <complex>
#include <cstdint>
#include "radiosettings.h"
#include "iqchannelizer.h"
#include "receiverbackend.h"

class ZoomSpectrumProcessor;
struct HackRfDeviceDiagnostics;

class DataProcessor : public QThread {
    Q_OBJECT
public:
    explicit DataProcessor( QObject *parent = nullptr);
    ~DataProcessor();
    void handleData(float *buf, uint32_t buf_length, int agileScanIndex = -1);
    void run() override;
    void startProcessing(void *device,
                         FobosApiKind apiKind,
                         bool syncEnabled,
                         double sampleRate,
                         bool queueAudioBlocks,
                         bool publishIqSnapshot = true,
                         bool emitIqFrames = false,
                         bool agileScanEnabled = false);
    void startProcessing(const ReceiverStreamDescriptor &stream);
    void requestStop();
    void finalizeStopped();
    // Last-resort recovery only. This may terminate the reader thread if a
    // third-party backend does not return from its blocking read call.
    bool forceStop(int timeoutMs = 1000);
    bool stop(int timeoutMs = 5000);
    uint64_t callbackCount() const;
    bool wantsAgileScanMetadata() const;
    void setSampleRateHint(double sampleRate);
    void setCenterFrequencyHint(double centerFrequency);
    void setFrequencyCalibrationOffset(double offsetHz);
    uint64_t beginIqRetuneBarrier();
    void startRetuneRawDump(const QString &reason,
                            uint64_t epoch,
                            double previousCenterHz,
                            double requestedCenterHz,
                            double actualCenterHz,
                            int maxBlocks = 8);
    bool retuneCenterFrequency(double centerFrequencyHz);
    bool applyRtlGainSettings(bool agc, int gainTenthsDb);
    bool applyHackRfGainSettings(int lnaGainDb, int vgaGainDb);
    bool queryHackRfDiagnostics(HackRfDeviceDiagnostics *diagnostics,
                                QString *errorMessage = nullptr);
    void updateNetworkIqSettings(const RadioSettings &settings, bool channelizeFrames);
    void configureNetworkIqStreaming(const RadioSettings &settings, bool emitFrames, bool channelizeFrames);
    void setWifiIqStreamingEnabled(bool enabled);
    std::shared_ptr<ZoomSpectrumProcessor> zoomSpectrumProcessor() const { return zoomProcessor; }
signals:
    void iqFrameReady(const QByteArray &iqData, double sampleRate, int sampleCount, bool channelized);
    void wifiIqFrameReady(const QByteArray &iqData, double sampleRate,
                          double centerFrequency, int sampleCount);
    void readerFailed(int errorCode, bool stoppedByRequest);

private:
    void emitIqFrame(const float *samples, std::size_t floatCount);
    void emitFullIqFrame(const float *samples, std::size_t floatCount);
    void emitWifiIqFrame(const float *samples, std::size_t floatCount);
    void emitChannelIqFrame(const float *samples, std::size_t floatCount, const RadioSettings &settings);
    void resetNetworkIqState();
    void resetStreamDiagnostics();
    void updateStreamDiagnostics(const float *samples, uint32_t sampleCount, const char *readerMode);
    void captureRetuneRawDumpBlock(const float *samples, uint32_t sampleCount, uint64_t callbackEpoch);
    void finishRetuneRawDumpLocked(const QString &status);
    void runRtlTcpReader(const ReceiverStreamDescriptor &stream, uint32_t blockSamples);
    void runRtlSdrNativeReader(const ReceiverStreamDescriptor &stream, uint32_t blockSamples);
    void runSoapySdrReader(const ReceiverStreamDescriptor &stream, uint32_t blockSamples);
    void runBladeRfNativeReader(const ReceiverStreamDescriptor &stream, uint32_t blockSamples);
    void runHackRfNativeReader(const ReceiverStreamDescriptor &stream, uint32_t blockSamples);
    void handleUnsigned8IqData(const unsigned char *buf, uint32_t byteCount, const char *readerMode);

    std::atomic<bool> running;
    std::atomic<bool> activeSyncMode;
    std::atomic<bool> requestedSyncMode;
    std::atomic<bool> requestedQueueAudioBlocks;
    std::atomic<bool> requestedPublishIqSnapshot;
    std::atomic<bool> requestedEmitIqFrames;
    std::atomic<bool> requestedWifiIqFrames{false};
    float wifiIqGain = 0.0f;
    std::atomic<bool> requestedChannelizeIqFrames;
    std::atomic<bool> requestedAgileScanEnabled;
    std::atomic<bool> networkIqResetRequested;
    std::atomic<bool> asyncCancelRequested{false};
    std::atomic<uint64_t> iqRetuneEpoch;
    std::atomic<double> requestedSampleRate;
    std::atomic<double> requestedCenterFrequency;
    std::atomic<double> frequencyCalibrationOffsetHz;
    std::atomic<void*> activeDevice;
    std::atomic<FobosApiKind> activeApiKind;
    ReceiverBackendStreamKind activeStreamKind = ReceiverBackendStreamKind::FobosStandard;
    QString activeBackendId;
    QString activeBackendName;
    ReceiverStreamDescriptor activeStreamDescriptor;
    std::mutex networkIqSettingsMutex;
    RadioSettings networkIqSettings;
    double networkIqNcoPhase = 0.0;
    std::complex<float> networkIqDecimationSum = {0.0f, 0.0f};
    int networkIqDecimationCount = 0;
    std::complex<float> networkIqLowPassState = {0.0f, 0.0f};
    std::array<std::complex<float>, 3> networkIqPreLowPassStates = {};
    std::array<std::vector<std::complex<float>>, 4> networkIqCicBuffers = {};
    std::array<std::complex<float>, 4> networkIqCicSums = {};
    int networkIqCicIndex = 0;
    int networkIqCicLength = 0;
    IqChannelizer networkIqChannelizer;
    std::shared_ptr<ZoomSpectrumProcessor> zoomProcessor;
    std::vector<float> networkIqChannelizerOutput;
    int networkIqLastLoggedDmrOutputRate = 0;
    int networkIqLastLoggedDmrDecimationFactor = 0;
    float networkIqAgcLevel = 0.01f;
    std::complex<float> networkHfNoiseCancelCoeff = {0.0f, 0.0f};
    std::complex<float> networkHfNoiseCancelRefDecimationSum = {0.0f, 0.0f};
    QByteArray networkIqFrameBuffer;
    double networkIqFrameSampleRate = 0.0;
    QElapsedTimer asyncRateTimer;
    QElapsedTimer streamDiagnosticTimer;
    uint64_t asyncMeasuredSamples = 0;
    uint64_t asyncCallbackCounter = 0;
    int asyncRateReportCount = 0;
    std::atomic<uint64_t> totalCallbackCounter;
    qint64 streamDiagnosticLastNs = -1;
    uint64_t streamDiagnosticCallbacks = 0;
    uint64_t streamDiagnosticSamples = 0;
    uint64_t streamDiagnosticIntervals = 0;
    uint64_t streamDiagnosticLateCallbacks = 0;
    uint64_t streamDiagnosticReportCount = 0;
    uint32_t streamDiagnosticMinBlock = 0;
    uint32_t streamDiagnosticMaxBlock = 0;
    double streamDiagnosticIntervalMsSum = 0.0;
    double streamDiagnosticMinIntervalMs = 0.0;
    double streamDiagnosticMaxIntervalMs = 0.0;
    double streamDiagnosticMeanI = 0.0;
    double streamDiagnosticMeanQ = 0.0;
    double streamDiagnosticPower = 0.0;
    double streamDiagnosticPhaseStepSum = 0.0;
    double streamDiagnosticPhaseStepAbsSum = 0.0;
    uint64_t streamDiagnosticPhaseStepCount = 0;
    uint64_t streamDiagnosticInspectedSamples = 0;
    uint64_t streamDiagnosticNonFiniteSamples = 0;
    uint64_t streamDiagnosticClippedSamples = 0;
    std::mutex retuneRawDumpMutex;
    bool retuneRawDumpActive = false;
    uint64_t retuneRawDumpEpoch = 0;
    uint64_t retuneRawDumpTriggerCallback = 0;
    int retuneRawDumpBlocksRequested = 0;
    int retuneRawDumpBlocksRemaining = 0;
    int retuneRawDumpBlocksCaptured = 0;
    quint64 retuneRawDumpSamplesCaptured = 0;
    double retuneRawDumpPreviousCenterHz = 0.0;
    double retuneRawDumpRequestedCenterHz = 0.0;
    double retuneRawDumpActualCenterHz = 0.0;
    double retuneRawDumpSampleRateHz = 0.0;
    QString retuneRawDumpReason;
    QString retuneRawDumpBasePath;
    QByteArray retuneRawDumpBytes;
    QJsonArray retuneRawDumpBlockStats;
};

#endif // DATAPROCESSOR_H
