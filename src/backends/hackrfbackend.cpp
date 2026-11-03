#include "hackrfbackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <algorithm>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

constexpr int HACKRF_SUCCESS = 0;
constexpr int HACKRF_ERR_NOT_LOADED = -16001;
constexpr int HACKRF_ERR_NOT_OPEN = -16002;
constexpr int HACKRF_ERR_UNSUPPORTED_INDEX = -16003;

struct HackRfNativeDeviceList {
    char **serialNumbers = nullptr;
    int *usbBoardIds = nullptr;
    int *usbDeviceIndex = nullptr;
    int deviceCount = 0;
    void **usbDevices = nullptr;
    int usbDeviceCount = 0;
};

struct HackRfNativeM0State {
    std::uint16_t requestedMode;
    std::uint16_t requestFlag;
    std::uint32_t activeMode;
    std::uint32_t m0Count;
    std::uint32_t m4Count;
    std::uint32_t numShortfalls;
    std::uint32_t longestShortfall;
    std::uint32_t shortfallLimit;
    std::uint32_t threshold;
    std::uint32_t nextMode;
    std::uint32_t error;
};

struct HackRfNativeOperaCakeDwell {
    std::uint32_t dwell;
    std::uint8_t port;
};

struct HackRfNativeOperaCakeRange {
    std::uint16_t freqMin;
    std::uint16_t freqMax;
    std::uint8_t port;
};

struct HackRfApi {
    QLibrary library;
    QString loadedPath;
    QString lastError;
    bool initialized = false;

    int (*init)() = nullptr;
    int (*open)(void **device) = nullptr;
    HackRfNativeDeviceList *(*device_list)() = nullptr;
    int (*device_list_open)(HackRfNativeDeviceList *list, int index, void **device) = nullptr;
    void (*device_list_free)(HackRfNativeDeviceList *list) = nullptr;
    int (*close)(void *device) = nullptr;
    const char *(*error_name)(int status) = nullptr;
    int (*set_freq)(void *device, std::uint64_t frequencyHz) = nullptr;
    int (*set_sample_rate)(void *device, double sampleRateHz) = nullptr;
    int (*set_baseband_filter_bandwidth)(void *device, std::uint32_t bandwidthHz) = nullptr;
    std::uint32_t (*compute_baseband_filter_bw_round_down_lt)(std::uint32_t bandwidthHz) = nullptr;
    std::uint32_t (*compute_baseband_filter_bw)(std::uint32_t bandwidthHz) = nullptr;
    int (*set_lna_gain)(void *device, std::uint32_t gainDb) = nullptr;
    int (*set_vga_gain)(void *device, std::uint32_t gainDb) = nullptr;
    int (*set_txvga_gain)(void *device, std::uint32_t gainDb) = nullptr;
    int (*set_amp_enable)(void *device, std::uint8_t enabled) = nullptr;
    int (*set_antenna_enable)(void *device, std::uint8_t enabled) = nullptr;
    int (*set_freq_explicit)(void *device, std::uint64_t ifHz, std::uint64_t loHz, int path) = nullptr;
    int (*set_clkout_enable)(void *device, std::uint8_t enabled) = nullptr;
    int (*get_clkin_status)(void *device, std::uint8_t *status) = nullptr;
    int (*set_hw_sync_mode)(void *device, std::uint8_t enabled) = nullptr;
    int (*set_rx_overrun_limit)(void *device, std::uint32_t limit) = nullptr;
    int (*set_tx_underrun_limit)(void *device, std::uint32_t limit) = nullptr;
    int (*get_operacake_boards)(void *device, std::uint8_t *boards) = nullptr;
    int (*set_operacake_mode)(void *device, std::uint8_t address, int mode) = nullptr;
    int (*set_operacake_ports)(void *device, std::uint8_t address,
                              std::uint8_t portA, std::uint8_t portB) = nullptr;
    int (*set_operacake_freq_ranges)(void *device, HackRfNativeOperaCakeRange *ranges,
                                    std::uint8_t count) = nullptr;
    int (*set_operacake_dwell_times)(void *device, HackRfNativeOperaCakeDwell *dwells,
                                    std::uint8_t count) = nullptr;
    int (*init_sweep)(void *device, const std::uint16_t *frequencyList, int rangeCount,
                      std::uint32_t bytesPerTune, std::uint32_t stepWidth,
                      std::uint32_t offset, int style) = nullptr;
    int (*start_rx_sweep)(void *device, HackRfRxCallback callback, void *context) = nullptr;
    const char *(*library_version)() = nullptr;
    const char *(*library_release)() = nullptr;
    int (*board_id_read)(void *device, std::uint8_t *value) = nullptr;
    const char *(*board_id_name)(int value) = nullptr;
    int (*version_string_read)(void *device, char *version, std::uint8_t length) = nullptr;
    int (*usb_api_version_read)(void *device, std::uint16_t *version) = nullptr;
    int (*board_rev_read)(void *device, std::uint8_t *value) = nullptr;
    const char *(*board_rev_name)(int value) = nullptr;
    int (*supported_platform_read)(void *device, std::uint32_t *value) = nullptr;
    std::size_t (*get_transfer_buffer_size)(void *device) = nullptr;
    std::uint32_t (*get_transfer_queue_depth)(void *device) = nullptr;
    int (*get_m0_state)(void *device, HackRfNativeM0State *state) = nullptr;
    int (*start_rx)(void *device, HackRfRxCallback callback, void *context) = nullptr;
    int (*stop_rx)(void *device) = nullptr;
    int (*start_tx)(void *device, HackRfTxCallback callback, void *context) = nullptr;
    int (*set_tx_block_complete_callback)(void *device, HackRfTxBlockCompleteCallback callback) = nullptr;
    int (*enable_tx_flush)(void *device, HackRfFlushCallback callback, void *context) = nullptr;
    int (*stop_tx)(void *device) = nullptr;
    int (*is_streaming)(void *device) = nullptr;
};

QMutex &apiMutex() {
    static QMutex mutex;
    return mutex;
}

HackRfApi &api() {
    static HackRfApi instance;
    return instance;
}

QStringList libraryCandidates() {
    const QString appDir = QCoreApplication::applicationDirPath();
    return {
#ifdef _WIN32
        QDir(appDir).absoluteFilePath(QStringLiteral("hackrf/hackrf.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("hackrf/libhackrf.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("hackrf.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("libhackrf.dll")),
        QStringLiteral("hackrf.dll"),
        QStringLiteral("libhackrf.dll"),
#else
        QDir(appDir).absoluteFilePath(QStringLiteral("hackrf/libhackrf.so")),
        QDir(appDir).absoluteFilePath(QStringLiteral("libhackrf.so")),
        QStringLiteral("hackrf"),
        QStringLiteral("libhackrf"),
#endif
        QStringLiteral("hackrf"),
        QStringLiteral("libhackrf")
    };
}

template <typename T>
bool resolveSymbol(HackRfApi &hackrf, T &target, const char *name, bool required = true) {
    target = reinterpret_cast<T>(hackrf.library.resolve(name));
    if (!target && required) {
        hackrf.lastError = QStringLiteral("Missing libhackrf symbol: %1").arg(QString::fromLatin1(name));
        return false;
    }
    return true;
}

QString statusToStringLocked(HackRfApi &hackrf, int status) {
    if (hackrf.error_name) {
        const char *message = hackrf.error_name(status);
        if (message && *message) {
            return QString::fromLatin1(message);
        }
    }
    return QStringLiteral("HackRF status %1").arg(status);
}

bool ensureLoadedLocked() {
    HackRfApi &hackrf = api();
    if (hackrf.library.isLoaded()) {
        return true;
    }

    QStringList loadErrors;
    for (const QString &candidate : libraryCandidates()) {
        hackrf.library.setFileName(candidate);
#ifdef _WIN32
        const QFileInfo candidateInfo(candidate);
        const bool hasExplicitDirectory = candidateInfo.isAbsolute() && candidateInfo.dir().exists();
        if (hasExplicitDirectory) {
            SetDllDirectoryW(reinterpret_cast<LPCWSTR>(candidateInfo.dir().absolutePath().utf16()));
        }
#endif
        if (!hackrf.library.load()) {
            hackrf.lastError = hackrf.library.errorString();
            loadErrors << QStringLiteral("%1: %2").arg(candidate, hackrf.lastError);
#ifdef _WIN32
            if (hasExplicitDirectory) {
                SetDllDirectoryW(nullptr);
            }
#endif
            continue;
        }
#ifdef _WIN32
        if (hasExplicitDirectory) {
            SetDllDirectoryW(nullptr);
        }
#endif

        hackrf.loadedPath = candidate;
        const bool ok =
            resolveSymbol(hackrf, hackrf.init, "hackrf_init") &&
            resolveSymbol(hackrf, hackrf.open, "hackrf_open") &&
            resolveSymbol(hackrf, hackrf.close, "hackrf_close") &&
            resolveSymbol(hackrf, hackrf.set_freq, "hackrf_set_freq") &&
            resolveSymbol(hackrf, hackrf.set_sample_rate, "hackrf_set_sample_rate") &&
            resolveSymbol(hackrf, hackrf.set_baseband_filter_bandwidth,
                          "hackrf_set_baseband_filter_bandwidth") &&
            resolveSymbol(hackrf, hackrf.set_lna_gain, "hackrf_set_lna_gain") &&
            resolveSymbol(hackrf, hackrf.set_vga_gain, "hackrf_set_vga_gain") &&
            resolveSymbol(hackrf, hackrf.set_txvga_gain, "hackrf_set_txvga_gain") &&
            resolveSymbol(hackrf, hackrf.set_amp_enable, "hackrf_set_amp_enable") &&
            resolveSymbol(hackrf, hackrf.start_rx, "hackrf_start_rx") &&
            resolveSymbol(hackrf, hackrf.stop_rx, "hackrf_stop_rx") &&
            resolveSymbol(hackrf, hackrf.start_tx, "hackrf_start_tx") &&
            resolveSymbol(hackrf, hackrf.stop_tx, "hackrf_stop_tx") &&
            resolveSymbol(hackrf, hackrf.is_streaming, "hackrf_is_streaming");
        resolveSymbol(hackrf, hackrf.error_name, "hackrf_error_name", false);
        resolveSymbol(hackrf, hackrf.device_list, "hackrf_device_list", false);
        resolveSymbol(hackrf, hackrf.device_list_open, "hackrf_device_list_open", false);
        resolveSymbol(hackrf, hackrf.device_list_free, "hackrf_device_list_free", false);
        resolveSymbol(hackrf, hackrf.compute_baseband_filter_bw_round_down_lt,
                      "hackrf_compute_baseband_filter_bw_round_down_lt", false);
        resolveSymbol(hackrf, hackrf.compute_baseband_filter_bw,
                      "hackrf_compute_baseband_filter_bw", false);
        resolveSymbol(hackrf, hackrf.set_antenna_enable, "hackrf_set_antenna_enable", false);
        resolveSymbol(hackrf, hackrf.set_freq_explicit, "hackrf_set_freq_explicit", false);
        resolveSymbol(hackrf, hackrf.set_clkout_enable, "hackrf_set_clkout_enable", false);
        resolveSymbol(hackrf, hackrf.get_clkin_status, "hackrf_get_clkin_status", false);
        resolveSymbol(hackrf, hackrf.set_hw_sync_mode, "hackrf_set_hw_sync_mode", false);
        resolveSymbol(hackrf, hackrf.set_rx_overrun_limit, "hackrf_set_rx_overrun_limit", false);
        resolveSymbol(hackrf, hackrf.set_tx_underrun_limit, "hackrf_set_tx_underrun_limit", false);
        resolveSymbol(hackrf, hackrf.set_tx_block_complete_callback,
                      "hackrf_set_tx_block_complete_callback", false);
        resolveSymbol(hackrf, hackrf.enable_tx_flush, "hackrf_enable_tx_flush", false);
        resolveSymbol(hackrf, hackrf.get_operacake_boards, "hackrf_get_operacake_boards", false);
        resolveSymbol(hackrf, hackrf.set_operacake_mode, "hackrf_set_operacake_mode", false);
        resolveSymbol(hackrf, hackrf.set_operacake_ports, "hackrf_set_operacake_ports", false);
        resolveSymbol(hackrf, hackrf.set_operacake_freq_ranges,
                      "hackrf_set_operacake_freq_ranges", false);
        resolveSymbol(hackrf, hackrf.set_operacake_dwell_times,
                      "hackrf_set_operacake_dwell_times", false);
        resolveSymbol(hackrf, hackrf.init_sweep, "hackrf_init_sweep", false);
        resolveSymbol(hackrf, hackrf.start_rx_sweep, "hackrf_start_rx_sweep", false);
        resolveSymbol(hackrf, hackrf.library_version, "hackrf_library_version", false);
        resolveSymbol(hackrf, hackrf.library_release, "hackrf_library_release", false);
        resolveSymbol(hackrf, hackrf.board_id_read, "hackrf_board_id_read", false);
        resolveSymbol(hackrf, hackrf.board_id_name, "hackrf_board_id_name", false);
        resolveSymbol(hackrf, hackrf.version_string_read, "hackrf_version_string_read", false);
        resolveSymbol(hackrf, hackrf.usb_api_version_read, "hackrf_usb_api_version_read", false);
        resolveSymbol(hackrf, hackrf.board_rev_read, "hackrf_board_rev_read", false);
        resolveSymbol(hackrf, hackrf.board_rev_name, "hackrf_board_rev_name", false);
        resolveSymbol(hackrf, hackrf.supported_platform_read,
                      "hackrf_supported_platform_read", false);
        resolveSymbol(hackrf, hackrf.get_transfer_buffer_size,
                      "hackrf_get_transfer_buffer_size", false);
        resolveSymbol(hackrf, hackrf.get_transfer_queue_depth,
                      "hackrf_get_transfer_queue_depth", false);
        resolveSymbol(hackrf, hackrf.get_m0_state, "hackrf_get_m0_state", false);

        if (!ok) {
            loadErrors << QStringLiteral("%1: %2").arg(candidate, hackrf.lastError);
            hackrf.library.unload();
            continue;
        }
        return true;
    }

    hackrf.lastError = loadErrors.isEmpty()
                           ? QStringLiteral("libhackrf runtime was not found")
                           : loadErrors.join(QStringLiteral(" | "));
    return false;
}

bool ensureInitializedLocked() {
    HackRfApi &hackrf = api();
    if (!ensureLoadedLocked()) {
        return false;
    }
    if (hackrf.initialized) {
        return true;
    }
    const int result = hackrf.init();
    if (result != HACKRF_SUCCESS) {
        hackrf.lastError = statusToStringLocked(hackrf, result);
        return false;
    }
    hackrf.initialized = true;
    return true;
}

template <typename Callable>
int withApi(Callable callable) {
    QMutexLocker locker(&apiMutex());
    if (!ensureInitializedLocked()) {
        return HACKRF_ERR_NOT_LOADED;
    }
    return callable(api());
}

bool queryDiagnosticsLocked(HackRfApi &hackrf,
                            void *dev,
                            HackRfDeviceDiagnostics *diagnostics,
                            QString *errorMessage) {
    if (!dev || !diagnostics) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("HackRF device is not open");
        }
        return false;
    }

    HackRfDeviceDiagnostics result;
    if (hackrf.library_version) {
        if (const char *value = hackrf.library_version()) {
            result.libraryVersion = QString::fromLatin1(value);
        }
    }
    if (hackrf.library_release) {
        if (const char *value = hackrf.library_release()) {
            result.libraryRelease = QString::fromLatin1(value);
        }
    }
    if (hackrf.version_string_read) {
        char version[256] = {};
        const int status = hackrf.version_string_read(dev, version, 255);
        if (status == HACKRF_SUCCESS) {
            result.firmwareVersion = QString::fromLatin1(version);
        }
    }
    if (hackrf.usb_api_version_read) {
        hackrf.usb_api_version_read(dev, &result.usbApiVersion);
    }
    if (hackrf.board_id_read) {
        std::uint8_t board = 0xff;
        if (hackrf.board_id_read(dev, &board) == HACKRF_SUCCESS) {
            const char *name = hackrf.board_id_name ? hackrf.board_id_name(board) : nullptr;
            result.boardName = name ? QString::fromLatin1(name)
                                    : QStringLiteral("ID %1").arg(board);
        }
    }
    if (hackrf.board_rev_read) {
        std::uint8_t revision = 0xff;
        if (hackrf.board_rev_read(dev, &revision) == HACKRF_SUCCESS) {
            const char *name = hackrf.board_rev_name ? hackrf.board_rev_name(revision) : nullptr;
            result.boardRevision = name ? QString::fromLatin1(name)
                                        : QStringLiteral("ID %1").arg(revision);
        }
    }
    if (hackrf.supported_platform_read) {
        hackrf.supported_platform_read(dev, &result.supportedPlatforms);
    }
    if (hackrf.get_clkin_status) {
        std::uint8_t status = 0;
        if (hackrf.get_clkin_status(dev, &status) == HACKRF_SUCCESS) {
            result.clockInputStatusAvailable = true;
            result.clockInputDetected = status != 0;
        }
    }
    if (hackrf.get_transfer_buffer_size) {
        result.transferBufferBytes = hackrf.get_transfer_buffer_size(dev);
    }
    if (hackrf.get_transfer_queue_depth) {
        result.transferQueueDepth = hackrf.get_transfer_queue_depth(dev);
    }
    if (hackrf.get_operacake_boards) {
        std::uint8_t boards[8] = {};
        std::fill(std::begin(boards), std::end(boards), static_cast<std::uint8_t>(0xff));
        if (hackrf.get_operacake_boards(dev, boards) == HACKRF_SUCCESS) {
            for (const std::uint8_t address : boards) {
                if (address != 0xff) {
                    result.operaCakeAddresses.append(address);
                }
            }
        }
    }
    if (hackrf.get_m0_state) {
        HackRfNativeM0State state = {};
        if (hackrf.get_m0_state(dev, &state) == HACKRF_SUCCESS) {
            result.m0StateAvailable = true;
            result.m0.requestedMode = state.requestedMode;
            result.m0.requestPending = state.requestFlag;
            result.m0.activeMode = state.activeMode;
            result.m0.m0Count = state.m0Count;
            result.m0.m4Count = state.m4Count;
            result.m0.shortfallCount = state.numShortfalls;
            result.m0.longestShortfall = state.longestShortfall;
            result.m0.shortfallLimit = state.shortfallLimit;
            result.m0.threshold = state.threshold;
            result.m0.nextMode = state.nextMode;
            result.m0.error = state.error;
        }
    }
    result.valid = true;
    *diagnostics = result;
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

} // namespace

bool hackRfLibraryAvailable(QString *loadedPath, QString *errorMessage) {
    QMutexLocker locker(&apiMutex());
    const bool ok = ensureInitializedLocked();
    const HackRfApi &hackrf = api();
    if (loadedPath) {
        *loadedPath = hackrf.loadedPath;
    }
    if (errorMessage) {
        *errorMessage = ok ? QString() : hackrf.lastError;
    }
    return ok;
}

QString hackRfLastErrorMessage() {
    QMutexLocker locker(&apiMutex());
    return api().lastError;
}

QVector<HackRfDeviceInfo> enumerateHackRfDevices(HackRfRuntimeStatus *status) {
    QVector<HackRfDeviceInfo> devices;
    if (status) {
        *status = HackRfRuntimeStatus{};
    }
    const int result = withApi([&devices, status](HackRfApi &hackrf) {
        if (status) {
            status->libraryAvailable = true;
            status->loadedPath = hackrf.loadedPath;
            status->modernDeviceListAvailable =
                hackrf.device_list && hackrf.device_list_open && hackrf.device_list_free;
        }
        if (hackrf.device_list && hackrf.device_list_open && hackrf.device_list_free) {
            HackRfNativeDeviceList *list = hackrf.device_list();
            if (!list) {
                hackrf.lastError = QStringLiteral("hackrf_device_list returned null");
                if (status) {
                    status->errorMessage = hackrf.lastError;
                }
                return HACKRF_SUCCESS;
            }
            const int count = (std::max)(0, list->deviceCount);
            if (status) {
                status->deviceCount = count;
            }
            devices.reserve(count);
            for (int index = 0; index < count; ++index) {
                HackRfDeviceInfo info;
                info.nativeIndex = index;
                if (list->serialNumbers && list->serialNumbers[index]) {
                    info.serial = QString::fromLatin1(list->serialNumbers[index]).trimmed();
                }
                info.label = info.serial.isEmpty()
                                 ? QStringLiteral("HackRF native #%1").arg(index + 1)
                                 : QStringLiteral("HackRF native #%1 (%2)")
                                       .arg(index + 1)
                                       .arg(info.serial);
                devices.append(info);
            }
            hackrf.device_list_free(list);
            return HACKRF_SUCCESS;
        }

        HackRfDeviceInfo info;
        info.nativeIndex = 0;
        info.label = QStringLiteral("HackRF native auto (legacy libhackrf)");
        devices.append(info);
        if (status) {
            status->deviceCount = 1;
        }
        return HACKRF_SUCCESS;
    });
    if (status && result == HACKRF_ERR_NOT_LOADED) {
        status->errorMessage = hackRfLastErrorMessage();
    }
    return devices;
}

int openHackRfDeviceSafely(void **dev, int nativeIndex) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    *dev = nullptr;
    if (nativeIndex < 0) {
        return HACKRF_ERR_UNSUPPORTED_INDEX;
    }
    return withApi([dev, nativeIndex](HackRfApi &hackrf) {
        int result = HACKRF_ERR_UNSUPPORTED_INDEX;
        if (hackrf.device_list && hackrf.device_list_open && hackrf.device_list_free) {
            HackRfNativeDeviceList *list = hackrf.device_list();
            if (!list) {
                hackrf.lastError = QStringLiteral("hackrf_device_list returned null");
                return HACKRF_ERR_NOT_OPEN;
            }
            if (nativeIndex < list->deviceCount) {
                result = hackrf.device_list_open(list, nativeIndex, dev);
            }
            hackrf.device_list_free(list);
        } else if (nativeIndex == 0) {
            result = hackrf.open(dev);
        }
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int closeHackRfDeviceSafely(void *dev) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev](HackRfApi &hackrf) {
        const int result = hackrf.close(dev);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfCenterFrequencySafely(void *dev, std::uint64_t frequencyHz) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev, frequencyHz](HackRfApi &hackrf) {
        const int result = hackrf.set_freq(dev, frequencyHz);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfSampleRateSafely(void *dev, double sampleRateHz) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev, sampleRateHz](HackRfApi &hackrf) {
        const int result = hackrf.set_sample_rate(dev, sampleRateHz);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

std::uint32_t recommendedHackRfBandwidth(std::uint32_t sampleRateHz) {
    const std::uint32_t requested = static_cast<std::uint32_t>(
        (std::max)(1750000.0, static_cast<double>(sampleRateHz) * 0.75));
    QMutexLocker locker(&apiMutex());
    if (ensureInitializedLocked() && api().compute_baseband_filter_bw_round_down_lt) {
        return api().compute_baseband_filter_bw_round_down_lt(requested);
    }
    return requested;
}

std::uint32_t nearestHackRfBandwidth(std::uint32_t requestedHz) {
    QMutexLocker locker(&apiMutex());
    if (ensureInitializedLocked() && api().compute_baseband_filter_bw) {
        return api().compute_baseband_filter_bw(requestedHz);
    }
    static const std::uint32_t values[] = {
        1750000, 2500000, 3500000, 5000000, 5500000, 6000000,
        7000000, 8000000, 9000000, 10000000, 12000000, 14000000,
        15000000, 20000000, 24000000, 28000000
    };
    std::uint32_t best = values[0];
    std::uint32_t bestDistance = requestedHz > best ? requestedHz - best : best - requestedHz;
    for (const std::uint32_t value : values) {
        const std::uint32_t distance = requestedHz > value ? requestedHz - value : value - requestedHz;
        if (distance < bestDistance) {
            best = value;
            bestDistance = distance;
        }
    }
    return best;
}

int setHackRfBandwidthSafely(void *dev, std::uint32_t bandwidthHz) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev, bandwidthHz](HackRfApi &hackrf) {
        const int result = hackrf.set_baseband_filter_bandwidth(dev, bandwidthHz);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfLnaGainSafely(void *dev, std::uint32_t gainDb) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    const std::uint32_t value = (std::min)(40u, (gainDb / 8u) * 8u);
    return withApi([dev, value](HackRfApi &hackrf) {
        const int result = hackrf.set_lna_gain(dev, value);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfVgaGainSafely(void *dev, std::uint32_t gainDb) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    const std::uint32_t value = (std::min)(62u, (gainDb / 2u) * 2u);
    return withApi([dev, value](HackRfApi &hackrf) {
        const int result = hackrf.set_vga_gain(dev, value);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfTxVgaGainSafely(void *dev, std::uint32_t gainDb) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    const std::uint32_t value = (std::min)(47u, gainDb);
    return withApi([dev, value](HackRfApi &hackrf) {
        const int result = hackrf.set_txvga_gain(dev, value);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfAmpEnabledSafely(void *dev, bool enabled) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev, enabled](HackRfApi &hackrf) {
        const int result = hackrf.set_amp_enable(dev, enabled ? 1u : 0u);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfBiasTeeEnabledSafely(void *dev, bool enabled) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev, enabled](HackRfApi &hackrf) {
        if (!hackrf.set_antenna_enable) {
            return enabled ? HACKRF_ERR_NOT_LOADED : HACKRF_SUCCESS;
        }
        const int result = hackrf.set_antenna_enable(dev, enabled ? 1u : 0u);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfExplicitFrequencySafely(void *dev,
                                     std::uint64_t ifFrequencyHz,
                                     std::uint64_t loFrequencyHz,
                                     HackRfRfPath path) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([=](HackRfApi &hackrf) {
        if (!hackrf.set_freq_explicit) {
            hackrf.lastError = QStringLiteral("hackrf_set_freq_explicit is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        const int result = hackrf.set_freq_explicit(dev,
                                                    ifFrequencyHz,
                                                    loFrequencyHz,
                                                    static_cast<int>(path));
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfClockOutEnabledSafely(void *dev, bool enabled) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([=](HackRfApi &hackrf) {
        if (!hackrf.set_clkout_enable) {
            if (!enabled) {
                return HACKRF_SUCCESS;
            }
            hackrf.lastError = QStringLiteral("hackrf_set_clkout_enable is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        const int result = hackrf.set_clkout_enable(dev, enabled ? 1u : 0u);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfHardwareSyncEnabledSafely(void *dev, bool enabled) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([=](HackRfApi &hackrf) {
        if (!hackrf.set_hw_sync_mode) {
            if (!enabled) {
                return HACKRF_SUCCESS;
            }
            hackrf.lastError = QStringLiteral("hackrf_set_hw_sync_mode is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        const int result = hackrf.set_hw_sync_mode(dev, enabled ? 1u : 0u);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfRxOverrunLimitSafely(void *dev, std::uint32_t sampleLimit) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([=](HackRfApi &hackrf) {
        if (!hackrf.set_rx_overrun_limit) {
            if (sampleLimit == 0) {
                return HACKRF_SUCCESS;
            }
            hackrf.lastError = QStringLiteral("hackrf_set_rx_overrun_limit is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        const int result = hackrf.set_rx_overrun_limit(dev, sampleLimit);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int getHackRfClockInputStatusSafely(void *dev, bool *detected) {
    if (!dev || !detected) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([=](HackRfApi &hackrf) {
        if (!hackrf.get_clkin_status) {
            hackrf.lastError = QStringLiteral("hackrf_get_clkin_status is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        std::uint8_t status = 0;
        const int result = hackrf.get_clkin_status(dev, &status);
        if (result == HACKRF_SUCCESS) {
            *detected = status != 0;
        } else {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int configureHackRfOperaCakeManualSafely(void *dev,
                                         std::uint8_t address,
                                         std::uint8_t portA,
                                         std::uint8_t portB) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([=](HackRfApi &hackrf) {
        if (!hackrf.set_operacake_mode || !hackrf.set_operacake_ports) {
            hackrf.lastError = QStringLiteral("Opera Cake manual API is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        int result = hackrf.set_operacake_mode(dev, address, 0);
        if (result == HACKRF_SUCCESS) {
            result = hackrf.set_operacake_ports(dev, address, portA, portB);
        }
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int configureHackRfOperaCakeFrequencySafely(
    void *dev,
    std::uint8_t address,
    const QVector<HackRfOperaCakeFrequencyRange> &ranges) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([=](HackRfApi &hackrf) {
        if (!hackrf.set_operacake_mode || !hackrf.set_operacake_freq_ranges) {
            hackrf.lastError = QStringLiteral("Opera Cake frequency API is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        QVector<HackRfNativeOperaCakeRange> native;
        native.reserve((std::min)(8, ranges.size()));
        for (int i = 0; i < ranges.size() && i < 8; ++i) {
            native.append({ranges[i].minMhz, ranges[i].maxMhz, ranges[i].port});
        }
        int result = hackrf.set_operacake_mode(dev, address, 1);
        if (result == HACKRF_SUCCESS) {
            result = hackrf.set_operacake_freq_ranges(
                dev, native.isEmpty() ? nullptr : native.data(),
                static_cast<std::uint8_t>(native.size()));
        }
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int configureHackRfOperaCakeTimeSafely(
    void *dev,
    std::uint8_t address,
    const QVector<HackRfOperaCakeDwell> &dwells) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([=](HackRfApi &hackrf) {
        if (!hackrf.set_operacake_mode || !hackrf.set_operacake_dwell_times) {
            hackrf.lastError = QStringLiteral("Opera Cake time API is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        QVector<HackRfNativeOperaCakeDwell> native;
        native.reserve((std::min)(16, dwells.size()));
        for (int i = 0; i < dwells.size() && i < 16; ++i) {
            native.append({dwells[i].samples, dwells[i].port});
        }
        int result = hackrf.set_operacake_mode(dev, address, 2);
        if (result == HACKRF_SUCCESS) {
            result = hackrf.set_operacake_dwell_times(
                dev, native.isEmpty() ? nullptr : native.data(),
                static_cast<std::uint8_t>(native.size()));
        }
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

bool queryOpenHackRfDeviceDiagnostics(void *dev,
                                      HackRfDeviceDiagnostics *diagnostics,
                                      QString *errorMessage) {
    bool success = false;
    const int status = withApi([&](HackRfApi &hackrf) {
        success = queryDiagnosticsLocked(hackrf, dev, diagnostics, errorMessage);
        return success ? HACKRF_SUCCESS : HACKRF_ERR_NOT_OPEN;
    });
    if (status == HACKRF_ERR_NOT_LOADED && errorMessage) {
        *errorMessage = hackRfLastErrorMessage();
    }
    return success;
}

bool queryHackRfDeviceDiagnostics(int nativeIndex,
                                  HackRfDeviceDiagnostics *diagnostics,
                                  QString *errorMessage) {
    bool success = false;
    const int status = withApi([&](HackRfApi &hackrf) {
        void *dev = nullptr;
        int result = HACKRF_ERR_UNSUPPORTED_INDEX;
        if (hackrf.device_list && hackrf.device_list_open && hackrf.device_list_free) {
            HackRfNativeDeviceList *list = hackrf.device_list();
            if (list && nativeIndex >= 0 && nativeIndex < list->deviceCount) {
                result = hackrf.device_list_open(list, nativeIndex, &dev);
            }
            if (list) {
                hackrf.device_list_free(list);
            }
        } else if (nativeIndex == 0) {
            result = hackrf.open(&dev);
        }
        if (result != HACKRF_SUCCESS || !dev) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
            if (errorMessage) {
                *errorMessage = hackrf.lastError;
            }
            return result;
        }
        success = queryDiagnosticsLocked(hackrf, dev, diagnostics, errorMessage);
        const int closeResult = hackrf.close(dev);
        if (closeResult != HACKRF_SUCCESS && success) {
            hackrf.lastError = statusToStringLocked(hackrf, closeResult);
        }
        return success ? HACKRF_SUCCESS : HACKRF_ERR_NOT_OPEN;
    });
    if (status == HACKRF_ERR_NOT_LOADED && errorMessage) {
        *errorMessage = hackRfLastErrorMessage();
    }
    return success;
}

int initializeHackRfSweepSafely(void *dev,
                                const QVector<std::uint16_t> &rangePairsMhz,
                                std::uint32_t bytesPerTune,
                                std::uint32_t stepWidthHz,
                                std::uint32_t offsetHz,
                                bool interleaved) {
    if (!dev || rangePairsMhz.size() < 2 || (rangePairsMhz.size() % 2) != 0) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([=](HackRfApi &hackrf) {
        if (!hackrf.init_sweep) {
            hackrf.lastError = QStringLiteral("hackrf_init_sweep is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        const int result = hackrf.init_sweep(dev,
                                             rangePairsMhz.constData(),
                                             (std::min)(10, rangePairsMhz.size() / 2),
                                             bytesPerTune,
                                             stepWidthHz,
                                             offsetHz,
                                             interleaved ? 1 : 0);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int startHackRfSweepSafely(void *dev, HackRfRxCallback callback, void *context) {
    if (!dev || !callback) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([=](HackRfApi &hackrf) {
        if (!hackrf.start_rx_sweep) {
            hackrf.lastError = QStringLiteral("hackrf_start_rx_sweep is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        const int result = hackrf.start_rx_sweep(dev, callback, context);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int startHackRfRxSafely(void *dev, HackRfRxCallback callback, void *context) {
    if (!dev || !callback) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev, callback, context](HackRfApi &hackrf) {
        const int result = hackrf.start_rx(dev, callback, context);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int stopHackRfRxSafely(void *dev) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev](HackRfApi &hackrf) {
        const int result = hackrf.stop_rx(dev);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfTxUnderrunLimitSafely(void *dev, std::uint32_t sampleLimit) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev, sampleLimit](HackRfApi &hackrf) {
        if (!hackrf.set_tx_underrun_limit) {
            return HACKRF_SUCCESS;
        }
        const int result = hackrf.set_tx_underrun_limit(dev, sampleLimit);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int setHackRfTxBlockCompleteCallbackSafely(void *dev, HackRfTxBlockCompleteCallback callback) {
    if (!dev || !callback) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev, callback](HackRfApi &hackrf) {
        if (!hackrf.set_tx_block_complete_callback) {
            hackrf.lastError = QStringLiteral("hackrf_set_tx_block_complete_callback is unavailable");
            return HACKRF_ERR_NOT_LOADED;
        }
        const int result = hackrf.set_tx_block_complete_callback(dev, callback);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int enableHackRfTxFlushSafely(void *dev, HackRfFlushCallback callback, void *context) {
    if (!dev || !callback) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev, callback, context](HackRfApi &hackrf) {
        if (!hackrf.enable_tx_flush) {
            hackrf.lastError = QStringLiteral("hackrf_enable_tx_flush is unavailable; finite TX requires flush completion");
            return HACKRF_ERR_NOT_LOADED;
        }
        const int result = hackrf.enable_tx_flush(dev, callback, context);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int startHackRfTxSafely(void *dev, HackRfTxCallback callback, void *context) {
    if (!dev || !callback) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev, callback, context](HackRfApi &hackrf) {
        const int result = hackrf.start_tx(dev, callback, context);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

int stopHackRfTxSafely(void *dev) {
    if (!dev) {
        return HACKRF_ERR_NOT_OPEN;
    }
    return withApi([dev](HackRfApi &hackrf) {
        const int result = hackrf.stop_tx(dev);
        if (result != HACKRF_SUCCESS) {
            hackrf.lastError = statusToStringLocked(hackrf, result);
        }
        return result;
    });
}

bool isHackRfStreamingSafely(void *dev) {
    if (!dev) {
        return false;
    }
    return withApi([dev](HackRfApi &hackrf) {
        return hackrf.is_streaming(dev);
    }) == 1;
}
