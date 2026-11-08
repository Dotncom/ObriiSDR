#include "bladerfbackend.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <algorithm>
#include <cstddef>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr int BLADERF_ERR_NOT_LOADED = -15001;
constexpr int BLADERF_ERR_NOT_OPEN = -15002;
constexpr int BLADERF_ERR_OPEN_FAILED = -15003;

constexpr int BLADERF_BACKEND_ANY = 0;
constexpr int BLADERF_RX_CHANNEL_0 = 0;
constexpr int BLADERF_RX_X1 = 0;
constexpr int BLADERF_FORMAT_SC16_Q11 = 0;
constexpr int BLADERF_GAIN_DEFAULT = 0;
constexpr int BLADERF_GAIN_MGC = 1;

constexpr int BLADERF_DESCRIPTION_LENGTH = 33;
constexpr int BLADERF_SERIAL_LENGTH = 33;

using bladerf_dev_t = void;
using bladerf_channel = int;
using bladerf_channel_layout = int;
using bladerf_format = int;
using bladerf_gain_mode = int;
using bladerf_gain = int;
using bladerf_frequency = uint64_t;
using bladerf_sample_rate = unsigned int;
using bladerf_bandwidth = unsigned int;

struct bladerf_devinfo {
    int backend = BLADERF_BACKEND_ANY;
    char serial[BLADERF_SERIAL_LENGTH] = {};
    uint8_t usb_bus = 0;
    uint8_t usb_addr = 0;
    unsigned int instance = 0;
    char manufacturer[BLADERF_DESCRIPTION_LENGTH] = {};
    char product[BLADERF_DESCRIPTION_LENGTH] = {};
};

struct BladeRfApi {
    QLibrary library;
    QString loadedPath;
    QString lastError;

    int (*open)(bladerf_dev_t **device, const char *deviceIdentifier) = nullptr;
    int (*open_with_devinfo)(bladerf_dev_t **device, bladerf_devinfo *devinfo) = nullptr;
    void (*close)(bladerf_dev_t *device) = nullptr;
    std::ptrdiff_t (*get_device_list)(bladerf_devinfo **devices) = nullptr;
    void (*free_device_list)(bladerf_devinfo *devices) = nullptr;
    const char *(*strerror_fn)(int status) = nullptr;
    int (*set_frequency)(bladerf_dev_t *dev, bladerf_channel ch, bladerf_frequency frequency) = nullptr;
    int (*set_sample_rate)(bladerf_dev_t *dev,
                           bladerf_channel ch,
                           bladerf_sample_rate rate,
                           bladerf_sample_rate *actual) = nullptr;
    int (*set_bandwidth)(bladerf_dev_t *dev,
                         bladerf_channel ch,
                         bladerf_bandwidth bandwidth,
                         bladerf_bandwidth *actual) = nullptr;
    int (*set_gain_mode)(bladerf_dev_t *dev, bladerf_channel ch, bladerf_gain_mode mode) = nullptr;
    int (*set_gain)(bladerf_dev_t *dev, bladerf_channel ch, bladerf_gain gain) = nullptr;
    int (*sync_config)(bladerf_dev_t *dev,
                       bladerf_channel_layout layout,
                       bladerf_format format,
                       unsigned int numBuffers,
                       unsigned int bufferSize,
                       unsigned int numTransfers,
                       unsigned int streamTimeout) = nullptr;
    int (*enable_module)(bladerf_dev_t *dev, bladerf_channel ch, bool enable) = nullptr;
    int (*sync_rx)(bladerf_dev_t *dev,
                   void *samples,
                   unsigned int numSamples,
                   void *metadata,
                   unsigned int timeoutMs) = nullptr;
};

QMutex &apiMutex() {
    static QMutex mutex;
    return mutex;
}

BladeRfApi &api() {
    static BladeRfApi instance;
    return instance;
}

QStringList libraryCandidates() {
    const QString appDir = QCoreApplication::applicationDirPath();
    return {
#ifdef _WIN32
        QDir(appDir).absoluteFilePath(QStringLiteral("bladerf/bladeRF.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("bladerf/libbladeRF.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("bladeRF.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("libbladeRF.dll")),
        QStringLiteral("bladeRF.dll"),
        QStringLiteral("libbladeRF.dll"),
#else
        QDir(appDir).absoluteFilePath(QStringLiteral("bladerf/libbladeRF.so")),
        QDir(appDir).absoluteFilePath(QStringLiteral("libbladeRF.so")),
        QStringLiteral("bladeRF"),
        QStringLiteral("libbladeRF"),
#endif
        QStringLiteral("bladeRF"),
        QStringLiteral("libbladeRF")
    };
}

template <typename T>
bool resolveSymbol(BladeRfApi &blade, T &target, const char *name, bool required = true) {
    target = reinterpret_cast<T>(blade.library.resolve(name));
    if (!target && required) {
        blade.lastError = QStringLiteral("Missing libbladeRF symbol: %1").arg(QString::fromLatin1(name));
        return false;
    }
    return true;
}

QString statusToStringLocked(BladeRfApi &blade, int status) {
    if (blade.strerror_fn) {
        const char *message = blade.strerror_fn(status);
        if (message && *message) {
            return QString::fromLocal8Bit(message);
        }
    }
    return QStringLiteral("bladeRF status %1").arg(status);
}

bool ensureLoadedLocked() {
    BladeRfApi &blade = api();
    if (blade.library.isLoaded()) {
        return true;
    }

    QStringList loadErrors;
    for (const QString &candidate : libraryCandidates()) {
        blade.library.setFileName(candidate);
#ifdef _WIN32
        const QFileInfo candidateInfo(candidate);
        const bool hasExplicitDirectory = candidateInfo.isAbsolute() && candidateInfo.dir().exists();
        if (hasExplicitDirectory) {
            SetDllDirectoryW(reinterpret_cast<LPCWSTR>(candidateInfo.dir().absolutePath().utf16()));
        }
#endif
        if (!blade.library.load()) {
            blade.lastError = blade.library.errorString();
            loadErrors << QStringLiteral("%1: %2").arg(candidate, blade.lastError);
#ifdef _WIN32
            if (candidate.startsWith(QCoreApplication::applicationDirPath(), Qt::CaseInsensitive)) {
                SetDllDirectoryW(nullptr);
            }
#endif
            continue;
        }
#ifdef _WIN32
        if (candidate.startsWith(QCoreApplication::applicationDirPath(), Qt::CaseInsensitive)) {
            SetDllDirectoryW(nullptr);
        }
#endif

        blade.loadedPath = candidate;
        const bool ok =
            resolveSymbol(blade, blade.open, "bladerf_open") &&
            resolveSymbol(blade, blade.open_with_devinfo, "bladerf_open_with_devinfo") &&
            resolveSymbol(blade, blade.close, "bladerf_close") &&
            resolveSymbol(blade, blade.get_device_list, "bladerf_get_device_list") &&
            resolveSymbol(blade, blade.free_device_list, "bladerf_free_device_list") &&
            resolveSymbol(blade, blade.set_frequency, "bladerf_set_frequency") &&
            resolveSymbol(blade, blade.set_sample_rate, "bladerf_set_sample_rate") &&
            resolveSymbol(blade, blade.set_bandwidth, "bladerf_set_bandwidth") &&
            resolveSymbol(blade, blade.sync_config, "bladerf_sync_config") &&
            resolveSymbol(blade, blade.enable_module, "bladerf_enable_module") &&
            resolveSymbol(blade, blade.sync_rx, "bladerf_sync_rx");
        resolveSymbol(blade, blade.strerror_fn, "bladerf_strerror", false);
        resolveSymbol(blade, blade.set_gain_mode, "bladerf_set_gain_mode", false);
        resolveSymbol(blade, blade.set_gain, "bladerf_set_gain", false);

        if (!ok) {
            loadErrors << QStringLiteral("%1: %2").arg(candidate, blade.lastError);
            blade.library.unload();
            continue;
        }

        qDebug() << "[bladeRF] loaded libbladeRF" << blade.loadedPath;
        return true;
    }

    if (blade.lastError.isEmpty()) {
        blade.lastError = QStringLiteral("libbladeRF runtime was not found");
    } else if (!loadErrors.isEmpty()) {
        blade.lastError = loadErrors.join(QStringLiteral(" | "));
    }
    return false;
}

template <typename Callable>
int withApi(Callable callable) {
    QMutexLocker locker(&apiMutex());
    if (!ensureLoadedLocked()) {
        return BLADERF_ERR_NOT_LOADED;
    }
    return callable(api());
}

QString cleanField(const char *value, int maxLength) {
    if (!value || maxLength <= 0) {
        return {};
    }
    int length = 0;
    while (length < maxLength && value[length] != '\0') {
        ++length;
    }
    return QString::fromLocal8Bit(value, length).trimmed();
}

} // namespace

bool bladeRfLibraryAvailable(QString *loadedPath, QString *errorMessage) {
    QMutexLocker locker(&apiMutex());
    const bool ok = ensureLoadedLocked();
    const BladeRfApi &blade = api();
    if (loadedPath) {
        *loadedPath = blade.loadedPath;
    }
    if (errorMessage) {
        *errorMessage = ok ? QString() : blade.lastError;
    }
    return ok;
}

QString bladeRfLastErrorMessage() {
    QMutexLocker locker(&apiMutex());
    return api().lastError;
}

QVector<BladeRfDeviceInfo> enumerateBladeRfDevices() {
    QVector<BladeRfDeviceInfo> devices;
    QMutexLocker locker(&apiMutex());
    if (!ensureLoadedLocked()) {
        qDebug() << "[bladeRF] library unavailable for enumeration" << api().lastError;
        return devices;
    }

    BladeRfApi &blade = api();
    bladerf_devinfo *list = nullptr;
    const std::ptrdiff_t count = blade.get_device_list(&list);
    if (count < 0) {
        const int status = static_cast<int>(count);
        blade.lastError = statusToStringLocked(blade, status);
        qDebug() << "[bladeRF] get_device_list failed" << status << blade.lastError;
        return devices;
    }

    for (std::ptrdiff_t i = 0; list && i < count; ++i) {
        const bladerf_devinfo &raw = list[i];
        BladeRfDeviceInfo info;
        info.nativeIndex = static_cast<int>(i);
        info.serial = cleanField(raw.serial, BLADERF_SERIAL_LENGTH);
        info.manufacturer = cleanField(raw.manufacturer, BLADERF_DESCRIPTION_LENGTH);
        info.product = cleanField(raw.product, BLADERF_DESCRIPTION_LENGTH);

        QStringList parts;
        if (!info.product.isEmpty()) {
            parts << info.product;
        }
        if (!info.serial.isEmpty()) {
            parts << QStringLiteral("SN %1").arg(info.serial);
        }
        info.label = QStringLiteral("bladeRF native #%1 (%2)")
                         .arg(i)
                         .arg(parts.isEmpty() ? QStringLiteral("device") : parts.join(QStringLiteral("  ")));
        devices.append(info);
    }
    if (list) {
        blade.free_device_list(list);
    }
    return devices;
}

int openBladeRfDeviceSafely(void **dev, int nativeIndex) {
    if (!dev) {
        return BLADERF_ERR_NOT_OPEN;
    }
    *dev = nullptr;
    return withApi([dev, nativeIndex](BladeRfApi &blade) {
        bladerf_devinfo *list = nullptr;
        const std::ptrdiff_t count = blade.get_device_list(&list);
        int ret = BLADERF_ERR_OPEN_FAILED;
        bladerf_dev_t *opened = nullptr;
        if (count > 0 && list) {
            const int index = (std::clamp)(nativeIndex, 0, static_cast<int>(count - 1));
            ret = blade.open_with_devinfo(&opened, &list[index]);
        } else if (count == 0) {
            ret = blade.open(&opened, nullptr);
        } else {
            ret = static_cast<int>(count);
        }
        if (list) {
            blade.free_device_list(list);
        }
        *dev = opened;
        if (ret != 0) {
            blade.lastError = statusToStringLocked(blade, ret);
        }
        return ret;
    });
}

int closeBladeRfDeviceSafely(void *dev) {
    if (!dev) {
        return BLADERF_ERR_NOT_OPEN;
    }
    return withApi([dev](BladeRfApi &blade) {
        blade.close(static_cast<bladerf_dev_t*>(dev));
        return 0;
    });
}

int setBladeRfCenterFrequencySafely(void *dev, uint64_t frequencyHz) {
    if (!dev) {
        return BLADERF_ERR_NOT_OPEN;
    }
    return withApi([dev, frequencyHz](BladeRfApi &blade) {
        const int ret = blade.set_frequency(static_cast<bladerf_dev_t*>(dev),
                                            BLADERF_RX_CHANNEL_0,
                                            frequencyHz);
        if (ret != 0) {
            blade.lastError = statusToStringLocked(blade, ret);
        }
        return ret;
    });
}

int setBladeRfSampleRateSafely(void *dev, uint32_t sampleRateHz, uint32_t *actualSampleRateHz) {
    if (!dev) {
        return BLADERF_ERR_NOT_OPEN;
    }
    return withApi([dev, sampleRateHz, actualSampleRateHz](BladeRfApi &blade) {
        bladerf_sample_rate actual = 0;
        const int ret = blade.set_sample_rate(static_cast<bladerf_dev_t*>(dev),
                                              BLADERF_RX_CHANNEL_0,
                                              sampleRateHz,
                                              &actual);
        if (actualSampleRateHz) {
            *actualSampleRateHz = static_cast<uint32_t>(actual);
        }
        if (ret != 0) {
            blade.lastError = statusToStringLocked(blade, ret);
        }
        return ret;
    });
}

int setBladeRfBandwidthSafely(void *dev, uint32_t bandwidthHz, uint32_t *actualBandwidthHz) {
    if (!dev) {
        return BLADERF_ERR_NOT_OPEN;
    }
    return withApi([dev, bandwidthHz, actualBandwidthHz](BladeRfApi &blade) {
        bladerf_bandwidth actual = 0;
        const int ret = blade.set_bandwidth(static_cast<bladerf_dev_t*>(dev),
                                            BLADERF_RX_CHANNEL_0,
                                            bandwidthHz,
                                            &actual);
        if (actualBandwidthHz) {
            *actualBandwidthHz = static_cast<uint32_t>(actual);
        }
        if (ret != 0) {
            blade.lastError = statusToStringLocked(blade, ret);
        }
        return ret;
    });
}

int setBladeRfGainModeSafely(void *dev, int gainMode) {
    if (!dev) {
        return BLADERF_ERR_NOT_OPEN;
    }
    return withApi([dev, gainMode](BladeRfApi &blade) {
        if (!blade.set_gain_mode) {
            return 0;
        }
        const int normalizedGainMode = gainMode == BLADERF_GAIN_MGC ? BLADERF_GAIN_MGC : BLADERF_GAIN_DEFAULT;
        const int ret = blade.set_gain_mode(static_cast<bladerf_dev_t*>(dev),
                                            BLADERF_RX_CHANNEL_0,
                                            normalizedGainMode);
        if (ret != 0) {
            blade.lastError = statusToStringLocked(blade, ret);
        }
        return ret;
    });
}

int setBladeRfGainSafely(void *dev, int gainDb) {
    if (!dev) {
        return BLADERF_ERR_NOT_OPEN;
    }
    return withApi([dev, gainDb](BladeRfApi &blade) {
        if (!blade.set_gain) {
            return 0;
        }
        const int ret = blade.set_gain(static_cast<bladerf_dev_t*>(dev),
                                       BLADERF_RX_CHANNEL_0,
                                       static_cast<bladerf_gain>(gainDb));
        if (ret != 0) {
            blade.lastError = statusToStringLocked(blade, ret);
        }
        return ret;
    });
}

int configureBladeRfSyncRxSafely(void *dev,
                                 uint32_t bufferCount,
                                 uint32_t bufferSamples,
                                 uint32_t transferCount,
                                 uint32_t timeoutMs) {
    if (!dev) {
        return BLADERF_ERR_NOT_OPEN;
    }
    return withApi([dev, bufferCount, bufferSamples, transferCount, timeoutMs](BladeRfApi &blade) {
        const uint32_t safeBuffers = (std::max)(bufferCount, transferCount + 1);
        const uint32_t safeSamples = (std::max)(bufferSamples, 2048u);
        const uint32_t safeTransfers = (std::max)(transferCount, 1u);
        const int ret = blade.sync_config(static_cast<bladerf_dev_t*>(dev),
                                          BLADERF_RX_X1,
                                          BLADERF_FORMAT_SC16_Q11,
                                          safeBuffers,
                                          safeSamples,
                                          safeTransfers,
                                          timeoutMs);
        if (ret != 0) {
            blade.lastError = statusToStringLocked(blade, ret);
        }
        return ret;
    });
}

int enableBladeRfRxSafely(void *dev, bool enabled) {
    if (!dev) {
        return BLADERF_ERR_NOT_OPEN;
    }
    return withApi([dev, enabled](BladeRfApi &blade) {
        const int ret = blade.enable_module(static_cast<bladerf_dev_t*>(dev),
                                            BLADERF_RX_CHANNEL_0,
                                            enabled);
        if (ret != 0) {
            blade.lastError = statusToStringLocked(blade, ret);
        }
        return ret;
    });
}

int readBladeRfSyncRxSafely(void *dev, int16_t *samples, uint32_t sampleCount, uint32_t timeoutMs) {
    if (!dev || !samples || sampleCount == 0) {
        return BLADERF_ERR_NOT_OPEN;
    }

    decltype(BladeRfApi::sync_rx) syncRx = nullptr;
    {
        QMutexLocker locker(&apiMutex());
        if (!ensureLoadedLocked()) {
            return BLADERF_ERR_NOT_LOADED;
        }
        syncRx = api().sync_rx;
    }
    if (!syncRx) {
        return BLADERF_ERR_NOT_LOADED;
    }
    return syncRx(static_cast<bladerf_dev_t*>(dev), samples, sampleCount, nullptr, timeoutMs);
}

const char *bladeRfErrorString(int status) {
    QMutexLocker locker(&apiMutex());
    BladeRfApi &blade = api();
    if (ensureLoadedLocked() && blade.strerror_fn) {
        return blade.strerror_fn(status);
    }
    Q_UNUSED(status)
    return "bladeRF error";
}
