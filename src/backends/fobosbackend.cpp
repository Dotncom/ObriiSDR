#include "fobosbackend.h"
#include "fobosfrequencylimits.h"
#include "../core/fobosreceivercore.h"
#include "../core/fobosruntimecaps.h"

#include <QByteArray>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QLibrary>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <type_traits>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr unsigned int FOBOS_AGILE_SCAN_MIN_POINTS = 2;
constexpr unsigned int FOBOS_AGILE_SCAN_MAX_POINTS = 256;

#ifdef _WIN32
template <typename Device>
std::atomic<Device *> stockDevice{nullptr};

HMODULE loadStockLibrary(FobosApiKind kind) {
    const QString name = kind == FobosApiKind::Agile ? QStringLiteral("fobos_sdr_stock.dll")
                                                     : QStringLiteral("fobos_stock.dll");
    const QString path = QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QStringLiteral("/fobos-compat/") + name);
    return LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

HMODULE stockLibrary(FobosApiKind kind) {
    if (kind == FobosApiKind::Agile) {
        static const HMODULE agile = loadStockLibrary(FobosApiKind::Agile);
        return agile;
    }
    static const HMODULE standard = loadStockLibrary(FobosApiKind::Standard);
    return standard;
}

template <typename Device>
constexpr FobosApiKind stockApiKind = std::is_same_v<Device, fobos_sdr_dev_t>
    ? FobosApiKind::Agile : FobosApiKind::Standard;

template <typename Device>
bool usesStock(Device *dev) {
    return dev && dev == stockDevice<Device>.load(std::memory_order_acquire);
}

template <typename Function, typename Device, typename... Args>
int callFobos(Function linked, const char *name, Device *dev, Args... args) {
    if (usesStock(dev)) {
        const auto function = reinterpret_cast<Function>(GetProcAddress(stockLibrary(stockApiKind<Device>), name));
        return function ? function(dev, args...) : FOBOS_ERR_UNSUPPORTED;
    }
    return linked(dev, args...);
}

template <typename Function, typename... Args>
int callStockSafely(Function function, Args... args) {
    __try {
        return function(args...);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
}

template <typename Device>
int openStockFobos(Device **dev, const char *serial) {
    const HMODULE library = stockLibrary(stockApiKind<Device>);
    if (!library) return FOBOS_ERR_UNSUPPORTED;
    if (stockDevice<Device>.load(std::memory_order_acquire)) return FOBOS_ERR_ASYNC_IN_SYNC;
    const QByteArray prefix = stockApiKind<Device> == FobosApiKind::Agile ? "fobos_sdr_" : "fobos_rx_";
    const auto list = reinterpret_cast<int (*)(char *)>(GetProcAddress(library, prefix + "list_devices"));
    const auto open = reinterpret_cast<int (*)(Device **, uint32_t)>(GetProcAddress(library, prefix + "open"));
    const auto info = reinterpret_cast<int (*)(Device *, char *, char *, char *, char *, char *)>(
        GetProcAddress(library, prefix + "get_board_info"));
    const auto close = reinterpret_cast<int (*)(Device *)>(GetProcAddress(library, prefix + "close"));
    if (!list || !open || !info || !close) return FOBOS_ERR_UNSUPPORTED;

    // The vendor listing has no capacity parameter and appends a serial per USB address.
    QByteArray listing(1024 * 1024, '\0');
    const int count = callStockSafely(list, listing.data());
    const auto serials = QByteArray(listing.constData()).trimmed().split(' ');
    if (count <= 0 || serials.size() != count || serials.count(QByteArray(serial)) != 1) return FOBOS_ERR_NO_DEV;

    *dev = nullptr;
    const int result = callStockSafely(open, dev, static_cast<uint32_t>(serials.indexOf(serial)));
    if (result != FOBOS_ERR_OK || !*dev) return result ? result : FOBOS_ERR_NO_DEV;
    char hw[256] = {}, fw[256] = {}, actualSerial[256] = {};
    if (callStockSafely(info, *dev, hw, fw, static_cast<char *>(nullptr), static_cast<char *>(nullptr), actualSerial) != 0 ||
        std::strcmp(serial, actualSerial) || !isObriiDualFirmware(QString::fromLatin1(hw), QString::fromLatin1(fw))) {
        callStockSafely(close, *dev);
        *dev = nullptr;
        return FOBOS_ERR_NO_DEV;
    }
    stockDevice<Device>.store(*dev, std::memory_order_release);
    qInfo() << "[Fobos] opened with stock library" << fobosApiKindName(stockApiKind<Device>) << "serial" << serial;
    return FOBOS_ERR_OK;
}

template <typename Device, typename Function>
int closeStockFobos(Device *dev, Function linked, const char *name) {
    const int result = callStockSafely(callFobos<Function, Device>, linked, name, dev);
    stockDevice<Device>.store(nullptr, std::memory_order_release);
    return result;
}
#else
template <typename Device>
bool usesStock(Device *) { return false; }
#endif

QFunctionPointer resolveAgileExtension(const char *name) {
    static QLibrary library(QStringLiteral("fobos_sdr"));
    if (!library.isLoaded() && !library.load()) return nullptr;
    return library.resolve(name);
}

int validateAgileRfFrequencies(fobos_sdr_dev_t *dev, const double *values, unsigned int count) {
    bool needsBoardInfo = false;
    for (unsigned int i = 0; i < count; ++i) {
        if (!std::isfinite(values[i]) || values[i] < 0.0) {
            return FOBOS_ERR_UNSUPPORTED;
        }
        needsBoardInfo = needsBoardInfo || values[i] > FOBOS_AGILE_LEGACY_MAX_RF_HZ;
    }
    if (!needsBoardInfo) {
        return FOBOS_ERR_OK;
    }
    // get_board_info copies the library's cached strings; it does not issue USB requests.
    char hardware[128] = {};
    char firmware[128] = {};
    const int result = getFobosAgileBoardInfoSafely(dev, hardware, firmware, nullptr, nullptr, nullptr);
    if (result != FOBOS_ERR_OK) {
        return result;
    }
    if (fobosAgileHasKnown6900MhzLimit(hardware, firmware)) {
        qWarning() << "[Fobos] frequency rejected: this Agile firmware ignores RF above 6900 MHz"
                   << "hardware" << hardware << "firmware" << firmware;
        return FOBOS_ERR_UNSUPPORTED;
    }
    return FOBOS_ERR_OK;
}

} // namespace

bool isObriiDualFirmware(const QString &hardware, const QString &firmware) {
    return hardware == QStringLiteral("3.0.0") &&
           (firmware == QStringLiteral("3.0.0 Obrii Dual RAM 2") ||
            firmware == QStringLiteral("3.0.0 Obrii Dual EEPROM 1") ||
            firmware == QStringLiteral("3.0.0 Obrii Dual EEPROM 2"));
}

bool fobosStockLibraryAvailable(FobosApiKind kind) {
#ifdef _WIN32
    return stockLibrary(kind) != nullptr;
#else
    Q_UNUSED(kind);
    return false;
#endif
}

bool activeFobosUsesStockLibrary() {
    return activeFobosApiKind == FobosApiKind::Agile ? usesStock(agileDevice) : usesStock(device);
}

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

int openFobosDeviceSafely(fobos_dev_t **dev, uint32_t index, const char *stockSerial) {
    if (!dev) {
        return FOBOS_ERR_BAD_PARAM;
    }
#ifdef _WIN32
    if (stockSerial) return openStockFobos(dev, stockSerial);
    __try {
        return fobos_rx_open(dev, index);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *dev = nullptr;
        return FOBOS_ERR_LIBUSB;
    }
#else
    if (stockSerial) return FOBOS_ERR_UNSUPPORTED;
    return fobos_rx_open(dev, index);
#endif
}

int openFobosAgileDeviceSafely(fobos_sdr_dev_t **dev, uint32_t index, const char *stockSerial) {
    if (!dev) {
        return FOBOS_ERR_BAD_PARAM;
    }
#ifdef _WIN32
    if (stockSerial) return openStockFobos(dev, stockSerial);
    __try {
        return fobos_sdr_open(dev, index);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *dev = nullptr;
        return FOBOS_ERR_LIBUSB;
    }
#else
    if (stockSerial) return FOBOS_ERR_UNSUPPORTED;
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
        return callFobos(&fobos_rx_get_board_info, "fobos_rx_get_board_info", dev, hwRevision, firmwareVersion, manufacturer, product, serial);
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
        return callFobos(&fobos_sdr_get_board_info, "fobos_sdr_get_board_info", dev, hwRevision, firmwareVersion, manufacturer, product, serial);
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
        return callFobos(&fobos_rx_get_samplerates, "fobos_rx_get_samplerates", dev, values, count);
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
        return callFobos(&fobos_sdr_get_samplerates, "fobos_sdr_get_samplerates", dev, values, count);
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
    if (usesStock(dev)) return closeStockFobos(dev, &fobos_rx_close, "fobos_rx_close");
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
    if (usesStock(dev)) return closeStockFobos(dev, &fobos_rx_close, "fobos_rx_close");
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
        return callFobos(&fobos_rx_set_clk_source, "fobos_rx_set_clk_source", dev, value);
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
        return callFobos(&fobos_rx_set_direct_sampling, "fobos_rx_set_direct_sampling", dev, enabled);
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
        return callFobos(&fobos_rx_set_samplerate, "fobos_rx_set_samplerate", dev, value, actual);
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
        return callFobos(&fobos_rx_set_frequency, "fobos_rx_set_frequency", dev, value, actual);
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
        return callFobos(&fobos_rx_set_lna_gain, "fobos_rx_set_lna_gain", dev, value);
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
        return callFobos(&fobos_rx_set_vga_gain, "fobos_rx_set_vga_gain", dev, value);
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
        return callFobos(&fobos_rx_set_user_gpo, "fobos_rx_set_user_gpo", dev, value);
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
    if (usesStock(dev)) return closeStockFobos(dev, &fobos_sdr_close, "fobos_sdr_close");
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
    if (usesStock(dev)) return closeStockFobos(dev, &fobos_sdr_reset, "fobos_sdr_reset");
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
        return callFobos(&fobos_sdr_set_clk_source, "fobos_sdr_set_clk_source", dev, value);
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
        return callFobos(&fobos_sdr_set_direct_sampling, "fobos_sdr_set_direct_sampling", dev, enabled);
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
        return callFobos(&fobos_sdr_set_samplerate, "fobos_sdr_set_samplerate", dev, value);
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
    const int validation = validateAgileRfFrequencies(dev, &value, 1);
    if (validation != FOBOS_ERR_OK) {
        return validation;
    }
#ifdef _WIN32
    __try {
        return callFobos(&fobos_sdr_set_frequency, "fobos_sdr_set_frequency", dev, value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_set_frequency(dev, value);
#endif
}

int getFobosAgileIfModeSafely(fobos_sdr_dev_t *dev, unsigned int *mode, unsigned int *state) {
    if (usesStock(dev)) return FOBOS_ERR_UNSUPPORTED;
    if (!dev) return FOBOS_ERR_NOT_OPEN;
    using Function = int (*)(fobos_sdr_dev_t *, unsigned int *, unsigned int *);
    const auto function = reinterpret_cast<Function>(resolveAgileExtension("fobos_sdr_get_if_mode"));
    if (!function) return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try { return function(dev, mode, state); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return FOBOS_ERR_LIBUSB; }
#else
    return function(dev, mode, state);
#endif
}

int setFobosAgileIfModeSafely(fobos_sdr_dev_t *dev, unsigned int mode) {
    if (usesStock(dev)) return FOBOS_ERR_UNSUPPORTED;
    if (!dev) return FOBOS_ERR_NOT_OPEN;
    using Function = int (*)(fobos_sdr_dev_t *, unsigned int);
    const auto function = reinterpret_cast<Function>(resolveAgileExtension("fobos_sdr_set_if_mode"));
    if (!function) return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try { return function(dev, mode); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return FOBOS_ERR_LIBUSB; }
#else
    return function(dev, mode);
#endif
}

int getFobosAgileIfTestModeSafely(fobos_sdr_dev_t *dev, unsigned int *mode, unsigned int *state,
    unsigned int *actualIfMhz, unsigned int *capabilities) {
    if (!dev) return FOBOS_ERR_NOT_OPEN;
    if (capabilities) *capabilities = 0;
    if (actualIfMhz) *actualIfMhz = 0;
    if (usesStock(dev)) return FOBOS_ERR_UNSUPPORTED;
    using Function = int (*)(fobos_sdr_dev_t *, unsigned int *, unsigned int *, unsigned int *, unsigned int *);
    const auto function = reinterpret_cast<Function>(resolveAgileExtension("fobos_sdr_get_if_plan_state"));
    if (!function) return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try { return function(dev, mode, actualIfMhz, state, capabilities); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return FOBOS_ERR_LIBUSB; }
#else
    return function(dev, mode, actualIfMhz, state, capabilities);
#endif
}

int setFobosAgileIfTestModeSafely(fobos_sdr_dev_t *dev, unsigned int mode) {
    if (usesStock(dev)) return FOBOS_ERR_UNSUPPORTED;
    if (!dev) return FOBOS_ERR_NOT_OPEN;
    using Function = int (*)(fobos_sdr_dev_t *, unsigned int);
    const auto function = reinterpret_cast<Function>(resolveAgileExtension("fobos_sdr_set_if_plan_mode"));
    if (!function) return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try { return function(dev, mode); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return FOBOS_ERR_LIBUSB; }
#else
    return function(dev, mode);
#endif
}

int getFobosAgileStreamMetricsSafely(fobos_sdr_dev_t *dev, FobosStreamMetrics *metrics) {
    if (usesStock(dev)) return FOBOS_ERR_UNSUPPORTED;
    if (!dev || !metrics) return FOBOS_ERR_NOT_OPEN;
    using Function = int (*)(fobos_sdr_dev_t *, FobosStreamMetrics *, unsigned int);
    const auto function = reinterpret_cast<Function>(resolveAgileExtension("fobos_sdr_get_stream_metrics"));
    if (!function) return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try { return function(dev, metrics, sizeof(*metrics)); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return FOBOS_ERR_LIBUSB; }
#else
    return function(dev, metrics, sizeof(*metrics));
#endif
}

int getFobosAgileDeviceMetricsSafely(fobos_sdr_dev_t *dev, FobosDeviceMetrics *metrics) {
    if (usesStock(dev)) return FOBOS_ERR_UNSUPPORTED;
    if (!dev || !metrics) return FOBOS_ERR_NOT_OPEN;
    using Function = int (*)(fobos_sdr_dev_t *, FobosDeviceMetrics *, unsigned int);
    const auto function = reinterpret_cast<Function>(resolveAgileExtension("fobos_sdr_get_device_metrics"));
    if (!function) return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try { return function(dev, metrics, sizeof(*metrics)); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return FOBOS_ERR_LIBUSB; }
#else
    return function(dev, metrics, sizeof(*metrics));
#endif
}

int setFobosAgileLnaGainSafely(fobos_sdr_dev_t *dev, unsigned int value) {
    if (!dev) {
        return FOBOS_ERR_NOT_OPEN;
    }
#ifdef _WIN32
    __try {
        return callFobos(&fobos_sdr_set_lna_gain, "fobos_sdr_set_lna_gain", dev, value);
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
        return callFobos(&fobos_sdr_set_vga_gain, "fobos_sdr_set_vga_gain", dev, value);
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
        return callFobos(&fobos_sdr_set_auto_bandwidth, "fobos_sdr_set_auto_bandwidth", dev, value);
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
        return callFobos(&fobos_sdr_set_user_gpo, "fobos_sdr_set_user_gpo", dev, value);
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
    const int validation = validateAgileRfFrequencies(dev, frequencies, count);
    if (validation != FOBOS_ERR_OK) {
        return validation;
    }
#ifdef _WIN32
    __try {
        return callFobos(&fobos_sdr_start_scan, "fobos_sdr_start_scan", dev, frequencies, count);
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
        return callFobos(&fobos_sdr_stop_scan, "fobos_sdr_stop_scan", dev);
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
        return callFobos(&fobos_sdr_is_scanning, "fobos_sdr_is_scanning", dev);
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
        return callFobos(&fobos_sdr_get_scan_index, "fobos_sdr_get_scan_index", dev);
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
        return callFobos(&fobos_rx_start_sync, "fobos_rx_start_sync", dev, blockSamples);
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
        return callFobos(&fobos_sdr_start_sync, "fobos_sdr_start_sync", dev, blockSamples);
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
        return callFobos(&fobos_rx_read_sync, "fobos_rx_read_sync", dev, buf, actualBufLength);
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
        return callFobos(&fobos_sdr_read_sync, "fobos_sdr_read_sync", dev, buf, actualBufLength);
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
        return callFobos(&fobos_rx_stop_sync, "fobos_rx_stop_sync", dev);
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
        return callFobos(&fobos_sdr_stop_sync, "fobos_sdr_stop_sync", dev);
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
        return callFobos(&fobos_rx_cancel_async, "fobos_rx_cancel_async", dev);
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
        return callFobos(&fobos_sdr_cancel_async, "fobos_sdr_cancel_async", dev);
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
        return callFobos(&fobos_rx_read_async, "fobos_rx_read_async", dev, callback, ctx, bufferCount, blockSamples);
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
        return callFobos(&fobos_sdr_read_async, "fobos_sdr_read_async", dev, callback, ctx, bufferCount, blockSamples);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return FOBOS_ERR_LIBUSB;
    }
#else
    return fobos_sdr_read_async(dev, callback, ctx, bufferCount, blockSamples);
#endif
}

namespace {
QFunctionPointer resolveStandardIfExtension(const char *name) {
    static QLibrary library(QStringLiteral("fobos"));
    if (!library.isLoaded() && !library.load()) return nullptr;
    return library.resolve(name);
}
}
int setObriiDualUsbProfile(const QString &serial, FobosApiKind kind) {
    using Function = int (*)(const char *, unsigned int);
    const auto function = reinterpret_cast<Function>(resolveStandardIfExtension("fobos_rx_set_dual_usb_profile"));
    if (!function) return FOBOS_ERR_UNSUPPORTED;
    const QByteArray serialBytes = serial.toLatin1();
    const unsigned int profile = kind == FobosApiKind::Standard ? 1u : 0u;
#ifdef _WIN32
    return callStockSafely(function, serialBytes.constData(), profile);
#else
    return function(serialBytes.constData(), profile);
#endif
}
int getActiveFobosIfModeSafely(unsigned int *mode,unsigned int *state) {
    if (activeFobosUsesStockLibrary()) return FOBOS_ERR_UNSUPPORTED;
    if(activeFobosApiKind==FobosApiKind::Agile)return getFobosAgileIfModeSafely(agileDevice,mode,state);
    using Fn=int (*)(void *,unsigned int *,unsigned int *);
    auto fn=reinterpret_cast<Fn>(resolveStandardIfExtension("fobos_rx_get_if_mode"));
    if(!activeFobosDevice())return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(activeFobosDevice(),mode,state);} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(activeFobosDevice(),mode,state);
#endif
}
int getActiveFobosIfPlanSafely(unsigned int *mode,unsigned int *state,unsigned int *mhz,unsigned int *cap) {
    if(cap)*cap=0;if(mhz)*mhz=0;
    if (activeFobosUsesStockLibrary()) return FOBOS_ERR_UNSUPPORTED;
    if(activeFobosApiKind==FobosApiKind::Agile)return getFobosAgileIfTestModeSafely(agileDevice,mode,state,mhz,cap);
    using Fn=int (*)(void *,unsigned int *,unsigned int *,unsigned int *,unsigned int *);
    auto fn=reinterpret_cast<Fn>(resolveStandardIfExtension("fobos_rx_get_if_plan_state"));
    if(!activeFobosDevice())return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(activeFobosDevice(),mode,mhz,state,cap);} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(activeFobosDevice(),mode,mhz,state,cap);
#endif
}
static int setStandardIfSafely(const char *name,unsigned int mode) {
    if (activeFobosUsesStockLibrary()) return FOBOS_ERR_UNSUPPORTED;
    using Fn=int (*)(void *,unsigned int);
    auto fn=reinterpret_cast<Fn>(resolveStandardIfExtension(name));
    if(!activeFobosDevice())return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(activeFobosDevice(),mode);} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(activeFobosDevice(),mode);
#endif
}
int setActiveFobosIfModeSafely(unsigned int mode) {
    return activeFobosApiKind==FobosApiKind::Agile ? setFobosAgileIfModeSafely(agileDevice,mode)
        : setStandardIfSafely("fobos_rx_set_if_mode",mode);
}
int setActiveFobosIfPlanSafely(unsigned int mode) {
    return activeFobosApiKind==FobosApiKind::Agile ? setFobosAgileIfTestModeSafely(agileDevice,mode)
        : setStandardIfSafely("fobos_rx_set_if_plan_mode",mode);
}

namespace {
QFunctionPointer resolveReceiverRuntime(const char *suffix) {
    if (activeFobosUsesStockLibrary()) return nullptr;
    const QByteArray name=QByteArray(activeFobosApiKind==FobosApiKind::Agile ? "fobos_sdr_" : "fobos_rx_")+suffix;
    return activeFobosApiKind==FobosApiKind::Agile ? resolveAgileExtension(name.constData()) : resolveStandardIfExtension(name.constData());
}
}
static int readReceiverConfigSafely(void *dev,QFunctionPointer function,unsigned int *words,unsigned int bytes) {
    using Fn=int (*)(void*,void*,unsigned int);
    const auto fn=reinterpret_cast<Fn>(function);
    if(!dev)return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(dev,words,bytes);} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(dev,words,bytes);
#endif
}
int getActiveFobosReceiverConfigSafely(unsigned int *words,unsigned int bytes) {
    return readReceiverConfigSafely(activeFobosDevice(),resolveReceiverRuntime("get_receiver_config"),words,bytes);
}
bool fobosAgileSupportsRf80(fobos_sdr_dev_t *dev) {
    if (usesStock(dev)) return false;
    unsigned int config[16]={};
    return readReceiverConfigSafely(dev,resolveAgileExtension("fobos_sdr_get_receiver_config"),config,sizeof(config))==FOBOS_ERR_OK &&
           (config[3]&FobosRuntimeCaps::Rf80);
}
int getActiveFobosIfTableSafely(unsigned int *frame,unsigned int *bytes) {
    using Fn=int (*)(void*,unsigned int*,unsigned int,unsigned int*);
    const auto fn=reinterpret_cast<Fn>(resolveReceiverRuntime("get_if_table"));
    if(!activeFobosDevice())return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(activeFobosDevice(),frame,512,bytes);} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(activeFobosDevice(),frame,512,bytes);
#endif
}
int setActiveFobosIfTableSafely(const unsigned int *frame,unsigned int bytes) {
    using Fn=int (*)(void*,const unsigned int*,unsigned int);
    const auto fn=reinterpret_cast<Fn>(resolveReceiverRuntime("set_if_table"));
    if(!activeFobosDevice())return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(activeFobosDevice(),frame,bytes);} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(activeFobosDevice(),frame,bytes);
#endif
}
int resetActiveFobosIfTableSafely() {
    using Fn=int (*)(void*);const auto fn=reinterpret_cast<Fn>(resolveReceiverRuntime("reset_if_table"));
    if(!activeFobosDevice())return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(activeFobosDevice());} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(activeFobosDevice());
#endif
}
int setActiveFobosStreamMetadataSafely(unsigned int enabled,unsigned int settle) {
    using Fn=int (*)(void*,unsigned int,unsigned int);
    const auto fn=reinterpret_cast<Fn>(resolveReceiverRuntime("set_stream_metadata"));
    if(!activeFobosDevice())return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(activeFobosDevice(),enabled,settle);} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(activeFobosDevice(),enabled,settle);
#endif
}
static int setRuntimeUnsigned(const char *suffix,unsigned int value) {
    using Fn=int (*)(void*,unsigned int);const auto fn=reinterpret_cast<Fn>(resolveReceiverRuntime(suffix));
    if(!activeFobosDevice())return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(activeFobosDevice(),value);} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(activeFobosDevice(),value);
#endif
}
int setActiveFobosMixerBiasSafely(unsigned int code) {return setRuntimeUnsigned("set_mixer_bias",code);}
int setActiveFobosDmaCountSafely(unsigned int count) {return setRuntimeUnsigned("set_dma_count",count);}
int getActiveFobosRffcRegistersSafely(unsigned short *registers,unsigned int bytes) {
    using Fn=int (*)(void*,unsigned short*,unsigned int);const auto fn=reinterpret_cast<Fn>(resolveReceiverRuntime("get_rffc_registers"));
    if(!activeFobosDevice())return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(activeFobosDevice(),registers,bytes);} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(activeFobosDevice(),registers,bytes);
#endif
}
int getFobosAgileMetadataMetricsSafely(fobos_sdr_dev_t *dev,unsigned int *words,unsigned int bytes) {
    if (usesStock(dev)) return FOBOS_ERR_UNSUPPORTED;
    using Fn=int (*)(void*,void*,unsigned int);const auto fn=reinterpret_cast<Fn>(resolveAgileExtension("fobos_sdr_get_iq_metadata_metrics"));
    if(!dev)return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {return fn(dev,words,bytes);} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    return fn(dev,words,bytes);
#endif
}
int getFobosAgileConfiguredRateSafely(fobos_sdr_dev_t *dev,double *actual) {
    if (usesStock(dev)) return FOBOS_ERR_UNSUPPORTED;
    using Fn=int (*)(void*,void*,unsigned int);const auto fn=reinterpret_cast<Fn>(resolveAgileExtension("fobos_sdr_get_tune_state"));
    unsigned int words[24]={0};int result;
    if(!dev)return FOBOS_ERR_NOT_OPEN;if(!fn)return FOBOS_ERR_UNSUPPORTED;
#ifdef _WIN32
    __try {result=fn(dev,words,sizeof(words));} __except(EXCEPTION_EXECUTE_HANDLER){return FOBOS_ERR_LIBUSB;}
#else
    result=fn(dev,words,sizeof(words));
#endif
    if(result)return result;
    double value=0;std::memcpy(&value,words+16,sizeof(value));
    if(!std::isfinite(value) || value<=0)return FOBOS_ERR_CONTROL;
    if(actual)*actual=value;return 0;
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
