#ifndef HACKRFBACKEND_H
#define HACKRFBACKEND_H

#include <QString>
#include <QVector>

#include <cstddef>
#include <cstdint>

struct HackRfDeviceInfo {
    int nativeIndex = 0;
    QString label;
    QString serial;
};

struct HackRfRuntimeStatus {
    bool libraryAvailable = false;
    bool modernDeviceListAvailable = false;
    QString loadedPath;
    QString errorMessage;
    int deviceCount = 0;
};

struct HackRfTransfer {
    void *device = nullptr;
    unsigned char *buffer = nullptr;
    int bufferLength = 0;
    int validLength = 0;
    void *rxContext = nullptr;
    void *txContext = nullptr;
};

using HackRfRxCallback = int (*)(HackRfTransfer *transfer);
using HackRfTxCallback = int (*)(HackRfTransfer *transfer);
using HackRfTxBlockCompleteCallback = void (*)(HackRfTransfer *transfer, int success);
using HackRfFlushCallback = void (*)(void *context, int success);

enum class HackRfRfPath {
    Bypass = 0,
    LowPass = 1,
    HighPass = 2
};

struct HackRfOperaCakeFrequencyRange {
    std::uint16_t minMhz = 0;
    std::uint16_t maxMhz = 0;
    std::uint8_t port = 0;
};

struct HackRfOperaCakeDwell {
    std::uint32_t samples = 0;
    std::uint8_t port = 0;
};

struct HackRfM0State {
    std::uint16_t requestedMode = 0;
    std::uint16_t requestPending = 0;
    std::uint32_t activeMode = 0;
    std::uint32_t m0Count = 0;
    std::uint32_t m4Count = 0;
    std::uint32_t shortfallCount = 0;
    std::uint32_t longestShortfall = 0;
    std::uint32_t shortfallLimit = 0;
    std::uint32_t threshold = 0;
    std::uint32_t nextMode = 0;
    std::uint32_t error = 0;
};

struct HackRfDeviceDiagnostics {
    bool valid = false;
    QString libraryVersion;
    QString libraryRelease;
    QString firmwareVersion;
    QString boardName;
    QString boardRevision;
    std::uint16_t usbApiVersion = 0;
    std::uint32_t supportedPlatforms = 0;
    bool clockInputDetected = false;
    bool clockInputStatusAvailable = false;
    std::size_t transferBufferBytes = 0;
    std::uint32_t transferQueueDepth = 0;
    QVector<int> operaCakeAddresses;
    HackRfM0State m0;
    bool m0StateAvailable = false;
    QString errorMessage;
};

bool hackRfLibraryAvailable(QString *loadedPath = nullptr, QString *errorMessage = nullptr);
QString hackRfLastErrorMessage();
QVector<HackRfDeviceInfo> enumerateHackRfDevices(HackRfRuntimeStatus *status = nullptr);

int openHackRfDeviceSafely(void **dev, int nativeIndex);
int closeHackRfDeviceSafely(void *dev);
int setHackRfCenterFrequencySafely(void *dev, std::uint64_t frequencyHz);
int setHackRfSampleRateSafely(void *dev, double sampleRateHz);
int setHackRfBandwidthSafely(void *dev, std::uint32_t bandwidthHz);
std::uint32_t recommendedHackRfBandwidth(std::uint32_t sampleRateHz);
std::uint32_t nearestHackRfBandwidth(std::uint32_t requestedHz);
int setHackRfLnaGainSafely(void *dev, std::uint32_t gainDb);
int setHackRfVgaGainSafely(void *dev, std::uint32_t gainDb);
int setHackRfTxVgaGainSafely(void *dev, std::uint32_t gainDb);
int setHackRfAmpEnabledSafely(void *dev, bool enabled);
int setHackRfBiasTeeEnabledSafely(void *dev, bool enabled);
int setHackRfExplicitFrequencySafely(void *dev,
                                     std::uint64_t ifFrequencyHz,
                                     std::uint64_t loFrequencyHz,
                                     HackRfRfPath path);
int setHackRfClockOutEnabledSafely(void *dev, bool enabled);
int setHackRfHardwareSyncEnabledSafely(void *dev, bool enabled);
int setHackRfRxOverrunLimitSafely(void *dev, std::uint32_t sampleLimit);
int getHackRfClockInputStatusSafely(void *dev, bool *detected);
int configureHackRfOperaCakeManualSafely(void *dev,
                                         std::uint8_t address,
                                         std::uint8_t portA,
                                         std::uint8_t portB);
int configureHackRfOperaCakeFrequencySafely(
    void *dev,
    std::uint8_t address,
    const QVector<HackRfOperaCakeFrequencyRange> &ranges);
int configureHackRfOperaCakeTimeSafely(
    void *dev,
    std::uint8_t address,
    const QVector<HackRfOperaCakeDwell> &dwells);
bool queryHackRfDeviceDiagnostics(int nativeIndex,
                                  HackRfDeviceDiagnostics *diagnostics,
                                  QString *errorMessage = nullptr);
bool queryOpenHackRfDeviceDiagnostics(void *dev,
                                      HackRfDeviceDiagnostics *diagnostics,
                                      QString *errorMessage = nullptr);
int initializeHackRfSweepSafely(void *dev,
                                const QVector<std::uint16_t> &rangePairsMhz,
                                std::uint32_t bytesPerTune,
                                std::uint32_t stepWidthHz,
                                std::uint32_t offsetHz,
                                bool interleaved);
int startHackRfSweepSafely(void *dev, HackRfRxCallback callback, void *context);
int startHackRfRxSafely(void *dev, HackRfRxCallback callback, void *context);
int stopHackRfRxSafely(void *dev);
int setHackRfTxUnderrunLimitSafely(void *dev, std::uint32_t sampleLimit);
int setHackRfTxBlockCompleteCallbackSafely(void *dev, HackRfTxBlockCompleteCallback callback);
int enableHackRfTxFlushSafely(void *dev, HackRfFlushCallback callback, void *context);
int startHackRfTxSafely(void *dev, HackRfTxCallback callback, void *context);
int stopHackRfTxSafely(void *dev);
bool isHackRfStreamingSafely(void *dev);

#endif // HACKRFBACKEND_H
