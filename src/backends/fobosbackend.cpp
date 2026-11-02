#include "fobosbackend.h"
#include "../core/fobosreceivercore.h"

#include <QDebug>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr unsigned int FOBOS_AGILE_SCAN_MIN_POINTS = 2;
constexpr unsigned int FOBOS_AGILE_SCAN_MAX_POINTS = 256;

} // namespace

const char *fobosApiKindName(FobosApiKind kind) {
    switch (kind) {
    case FobosApiKind::Standard:
        return "standard";
    case FobosApiKind::Agile:
        return "agile";
    }
    return "unknown";
}

QString fobosApiDisplayName(FobosApiKind kind) {
    return kind == FobosApiKind::Agile ? QStringLiteral("Agile") : QStringLiteral("Standard");
}

void *activeFobosDevice() {
    return activeFobosApiKind == FobosApiKind::Agile
               ? static_cast<void*>(agileDevice)
               : static_cast<void*>(device);
}

bool hasActiveFobosDevice() {
    return activeFobosDevice() != nullptr;
}

ReceiverBackendFeatures fobosBackendFeatures(FobosApiKind kind) {
    ReceiverBackendFeatures features = ReceiverBackendFeature::HardwareBandwidth |
                                       ReceiverBackendFeature::DirectSamplingHf1 |
                                       ReceiverBackendFeature::DirectSamplingHf2 |
                                       ReceiverBackendFeature::DirectSamplingHf1PlusHf2 |
                                       ReceiverBackendFeature::GpoControl |
                                       ReceiverBackendFeature::FastRetune;
    if (kind == FobosApiKind::Agile) {
        features |= ReceiverBackendFeature::AgileScan;
    }
    return features;
}

ReceiverStreamDescriptor makeFobosStreamDescriptor(void *nativeDevice,
                                                   FobosApiKind apiKind,
                                                   bool syncReader,
                                                   double sampleRateHz,
                                                   double centerFrequencyHz,
                                                   bool queueAudioBlocks,
                                                   bool publishIqSnapshot,
                                                   bool emitIqFrames,
                                                   bool agileScanEnabled,
                                                   const QVector<double> &agileScanFrequenciesHz,
                                                   double frequencyCalibrationOffsetHz) {
    ReceiverStreamDescriptor stream;
    stream.kind = apiKind == FobosApiKind::Agile
                      ? ReceiverBackendStreamKind::FobosAgile
                      : ReceiverBackendStreamKind::FobosStandard;
    stream.nativeDevice = nativeDevice;
    stream.fobosApiKind = apiKind;
    stream.backendId = apiKind == FobosApiKind::Agile
                           ? QStringLiteral("fobos-agile")
                           : QStringLiteral("fobos-standard");
    stream.backendName = apiKind == FobosApiKind::Agile
                             ? QStringLiteral("Fobos SDR Agile")
                             : QStringLiteral("Fobos SDR");
    stream.sampleRateHz = sampleRateHz;
    stream.centerFrequencyHz = centerFrequencyHz;
    stream.frequencyCalibrationOffsetHz = frequencyCalibrationOffsetHz;
    stream.syncReader = syncReader;
    stream.queueAudioBlocks = queueAudioBlocks;
    stream.publishIqSnapshot = publishIqSnapshot;
    stream.emitIqFrames = emitIqFrames;
    stream.agileScanEnabled = agileScanEnabled;
    stream.agileScanFrequenciesHz = agileScanFrequenciesHz;
    return stream;
}

int getFobosStandardApiInfoSafely(char *libVersion, char *driverVersion) {
#ifdef _WIN32
    __try {
        return fobos_rx_get_api_info(libVersion, driverVersion);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_get_api_info(libVersion, driverVersion);
#endif
}

int getFobosAgileApiInfoSafely(char *libVersion, char *driverVersion) {
#ifdef _WIN32
    __try {
        return fobos_sdr_get_api_info(libVersion, driverVersion);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_get_api_info(libVersion, driverVersion);
#endif
}

int getFobosStandardDeviceCountSafely() {
#ifdef _WIN32
    __try {
        return fobos_rx_get_device_count();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
#else
    return fobos_rx_get_device_count();
#endif
}

int getFobosAgileDeviceCountSafely() {
#ifdef _WIN32
    __try {
        return fobos_sdr_get_device_count();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
#else
    return fobos_sdr_get_device_count();
#endif
}

int openFobosDeviceSafely(fobos_dev_t **dev, uint32_t index) {
    if (!dev) {
        return FOBOS_ERR_BAD_PARAM;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_open(dev, index);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *dev = nullptr;
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_open(dev, index);
#endif
}

int openFobosAgileDeviceSafely(fobos_sdr_dev_t **dev, uint32_t index) {
    if (!dev) {
        return FOBOS_ERR_BAD_PARAM;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_open(dev, index);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *dev = nullptr;
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_open(dev, index);
#endif
}

int getFobosBoardInfoSafely(fobos_dev_t *dev,
                            char *hwRevision,
                            char *firmwareVersion,
                            char *manufacturer,
                            char *product,
                            char *serial) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_get_board_info(dev, hwRevision, firmwareVersion, manufacturer, product, serial);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_get_board_info(dev, hwRevision, firmwareVersion, manufacturer, product, serial);
#endif
}

int getFobosAgileBoardInfoSafely(fobos_sdr_dev_t *dev,
                                 char *hwRevision,
                                 char *firmwareVersion,
                                 char *manufacturer,
                                 char *product,
                                 char *serial) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_get_board_info(dev, hwRevision, firmwareVersion, manufacturer, product, serial);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_get_board_info(dev, hwRevision, firmwareVersion, manufacturer, product, serial);
#endif
}

int getFobosSampleRatesSafely(fobos_dev_t *dev, double *values, unsigned int *count) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_get_samplerates(dev, values, count);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_get_samplerates(dev, values, count);
#endif
}

int getFobosAgileSampleRatesSafely(fobos_sdr_dev_t *dev, double *values, unsigned int *count) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_get_samplerates(dev, values, count);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_get_samplerates(dev, values, count);
#endif
}

void logFobosApiInfo() {
    char libVersion[256] = {};
    char driverVersion[256] = {};
    const int result = getFobosStandardApiInfoSafely(libVersion, driverVersion);
    if (result == FOBOS_ERR_OK) {
        qDebug() << "[FobosLifecycle] libfobos info"
                 << "library" << libVersion
                 << "driver" << driverVersion;
    } else {
        qDebug() << "[FobosLifecycle] libfobos info unavailable"
                 << "result" << result;
    }

    char agileLibVersion[256] = {};
    char agileDriverVersion[256] = {};
    const int agileResult = getFobosAgileApiInfoSafely(agileLibVersion, agileDriverVersion);
    if (agileResult == FOBOS_ERR_OK) {
        qDebug() << "[FobosLifecycle] libfobos agile info"
                 << "library" << agileLibVersion
                 << "driver" << agileDriverVersion;
    } else {
        qDebug() << "[FobosLifecycle] libfobos agile info unavailable"
                 << "result" << agileResult;
    }
}

int closeFobosDeviceSafely(fobos_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_close(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_close(dev);
#endif
}

int resetFobosDeviceSafely(fobos_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_close(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_reset(dev);
#endif
}

int setFobosClockSourceSafely(fobos_dev_t *dev, unsigned int value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_set_clk_source(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_set_clk_source(dev, value);
#endif
}

int setFobosDirectSamplingSafely(fobos_dev_t *dev, unsigned int enabled) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_set_direct_sampling(dev, enabled);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_set_direct_sampling(dev, enabled);
#endif
}

int setFobosSampleRateSafely(fobos_dev_t *dev, double value, double *actual) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_set_samplerate(dev, value, actual);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_set_samplerate(dev, value, actual);
#endif
}

int setFobosFrequencySafely(fobos_dev_t *dev, double value, double *actual) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_set_frequency(dev, value, actual);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_set_frequency(dev, value, actual);
#endif
}

int setFobosLnaGainSafely(fobos_dev_t *dev, unsigned int value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_set_lna_gain(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_set_lna_gain(dev, value);
#endif
}

int setFobosVgaGainSafely(fobos_dev_t *dev, unsigned int value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_set_vga_gain(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_set_vga_gain(dev, value);
#endif
}

int setFobosGpoSafely(fobos_dev_t *dev, uint8_t value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_set_user_gpo(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_set_user_gpo(dev, value);
#endif
}

int closeFobosAgileDeviceSafely(fobos_sdr_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_close(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_close(dev);
#endif
}

int resetFobosAgileDeviceSafely(fobos_sdr_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_reset(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_reset(dev);
#endif
}

int setFobosAgileClockSourceSafely(fobos_sdr_dev_t *dev, int value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_set_clk_source(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_set_clk_source(dev, value);
#endif
}

int setFobosAgileDirectSamplingSafely(fobos_sdr_dev_t *dev, unsigned int enabled) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_set_direct_sampling(dev, enabled);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_set_direct_sampling(dev, enabled);
#endif
}

int setFobosAgileSampleRateSafely(fobos_sdr_dev_t *dev, double value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_set_samplerate(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_set_samplerate(dev, value);
#endif
}

int setFobosAgileFrequencySafely(fobos_sdr_dev_t *dev, double value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_set_frequency(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_set_frequency(dev, value);
#endif
}

int setFobosAgileLnaGainSafely(fobos_sdr_dev_t *dev, unsigned int value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_set_lna_gain(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_set_lna_gain(dev, value);
#endif
}

int setFobosAgileVgaGainSafely(fobos_sdr_dev_t *dev, unsigned int value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_set_vga_gain(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_set_vga_gain(dev, value);
#endif
}

int setFobosAgileAutoBandwidthSafely(fobos_sdr_dev_t *dev, double value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_set_auto_bandwidth(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_set_auto_bandwidth(dev, value);
#endif
}

int setFobosAgileGpoSafely(fobos_sdr_dev_t *dev, uint8_t value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_set_user_gpo(dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_set_user_gpo(dev, value);
#endif
}

int startFobosAgileScanSafely(fobos_sdr_dev_t *dev, double *frequencies, unsigned int count) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
    if (!frequencies ||
        count < FOBOS_AGILE_SCAN_MIN_POINTS ||
        count > FOBOS_AGILE_SCAN_MAX_POINTS) {
        return FOBOS_ERR_UNSUPPORTED;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_start_scan(dev, frequencies, count);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_start_scan(dev, frequencies, count);
#endif
}

int stopFobosAgileScanSafely(fobos_sdr_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_stop_scan(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_stop_scan(dev);
#endif
}

int isFobosAgileScanningSafely(fobos_sdr_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_is_scanning(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_is_scanning(dev);
#endif
}

int getFobosAgileScanIndexSafely(fobos_sdr_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_get_scan_index(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_get_scan_index(dev);
#endif
}

int startFobosSyncSafely(fobos_dev_t *dev, uint32_t blockSamples) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_start_sync(dev, blockSamples);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_start_sync(dev, blockSamples);
#endif
}

int startFobosAgileSyncSafely(fobos_sdr_dev_t *dev, uint32_t blockSamples) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_start_sync(dev, blockSamples);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_start_sync(dev, blockSamples);
#endif
}

int readFobosSyncSafely(fobos_dev_t *dev, float *buf, uint32_t *actualBufLength) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_read_sync(dev, buf, actualBufLength);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_read_sync(dev, buf, actualBufLength);
#endif
}

int readFobosAgileSyncSafely(fobos_sdr_dev_t *dev, float *buf, uint32_t *actualBufLength) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_read_sync(dev, buf, actualBufLength);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_read_sync(dev, buf, actualBufLength);
#endif
}

int stopFobosSyncSafely(fobos_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_stop_sync(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_stop_sync(dev);
#endif
}

int stopFobosAgileSyncSafely(fobos_sdr_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_stop_sync(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_stop_sync(dev);
#endif
}

int cancelFobosAsyncSafely(fobos_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_cancel_async(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_cancel_async(dev);
#endif
}

int cancelFobosAgileAsyncSafely(fobos_sdr_dev_t *dev) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_cancel_async(dev);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_cancel_async(dev);
#endif
}

int readFobosAsyncSafely(fobos_dev_t *dev,
                         void (*callback)(float *buf, uint32_t bufLength, void *ctx),
                         void *ctx,
                         uint32_t bufferCount,
                         uint32_t blockSamples) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_rx_read_async(dev, callback, ctx, bufferCount, blockSamples);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_rx_read_async(dev, callback, ctx, bufferCount, blockSamples);
#endif
}

int readFobosAgileAsyncSafely(fobos_sdr_dev_t *dev,
                              void (*callback)(float *buf, uint32_t bufLength, fobos_sdr_dev_t *dev, void *ctx),
                              void *ctx,
                              uint32_t bufferCount,
                              uint32_t blockSamples) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return fobos_sdr_read_async(dev, callback, ctx, bufferCount, blockSamples);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_read_async(dev, callback, ctx, bufferCount, blockSamples);
#endif
}

int setActiveClockSourceSafely(int value) {
    return FobosReceiverCore(activeFobosDevice(), activeFobosApiKind).clockSource(value);
}

int setActiveDirectSamplingSafely(unsigned int enabled) {
    return FobosReceiverCore(activeFobosDevice(), activeFobosApiKind).directSampling(enabled);
}

int setActiveSampleRateSafely(double value, double *actual) {
    return FobosReceiverCore(activeFobosDevice(), activeFobosApiKind).sampleRate(value, actual);
}

int setActiveFrequencySafely(double value, double *actual) {
    return FobosReceiverCore(activeFobosDevice(), activeFobosApiKind).frequency(value, actual);
}

int setActiveLnaGainSafely(unsigned int value) {
    return FobosReceiverCore(activeFobosDevice(), activeFobosApiKind).lna(value);
}

int setActiveVgaGainSafely(unsigned int value) {
    return FobosReceiverCore(activeFobosDevice(), activeFobosApiKind).vga(value);
}

int setActiveGpoSafely(uint8_t value) {
    return FobosReceiverCore(activeFobosDevice(), activeFobosApiKind).gpo(value);
}
