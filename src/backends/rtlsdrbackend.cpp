#include "rtlsdrbackend.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr int RTLSDR_ERR_NOT_LOADED = -13001;
constexpr int RTLSDR_ERR_NOT_OPEN = -13002;

using rtlsdr_dev_t = void;
using rtlsdr_read_async_cb_t = void (*)(unsigned char *buf, uint32_t len, void *ctx);

struct RtlSdrApi {
    QLibrary library;
    QString loadedPath;
    QString lastError;

    uint32_t (*get_device_count)() = nullptr;
    const char *(*get_device_name)(uint32_t index) = nullptr;
    int (*open)(rtlsdr_dev_t **dev, uint32_t index) = nullptr;
    int (*close)(rtlsdr_dev_t *dev) = nullptr;
    int (*set_center_freq)(rtlsdr_dev_t *dev, uint32_t freq) = nullptr;
    int (*set_sample_rate)(rtlsdr_dev_t *dev, uint32_t rate) = nullptr;
    int (*set_direct_sampling)(rtlsdr_dev_t *dev, int on) = nullptr;
    int (*set_freq_correction)(rtlsdr_dev_t *dev, int ppm) = nullptr;
    int (*set_tuner_gain_mode)(rtlsdr_dev_t *dev, int manual) = nullptr;
    int (*set_tuner_gain)(rtlsdr_dev_t *dev, int gain) = nullptr;
    int (*set_agc_mode)(rtlsdr_dev_t *dev, int on) = nullptr;
    int (*reset_buffer)(rtlsdr_dev_t *dev) = nullptr;
    int (*read_sync)(rtlsdr_dev_t *dev, void *buffer, int bytes, int *bytesRead) = nullptr;
    int (*read_async)(rtlsdr_dev_t *dev,
                      rtlsdr_read_async_cb_t cb,
                      void *ctx,
                      uint32_t buf_num,
                      uint32_t buf_len) = nullptr;
    int (*cancel_async)(rtlsdr_dev_t *dev) = nullptr;
};

QMutex &apiMutex() {
    static QMutex mutex;
    return mutex;
}

RtlSdrApi &api() {
    static RtlSdrApi instance;
    return instance;
}

QStringList libraryCandidates() {
    const QString appDir = QCoreApplication::applicationDirPath();
    return {
#ifdef _WIN32
        QDir(appDir).absoluteFilePath(QStringLiteral("rtlsdr/rtlsdr.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("rtlsdr/librtlsdr.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("rtlsdr/rtl-sdr.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("rtlsdr.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("librtlsdr.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("rtl-sdr.dll")),
#else
        QDir(appDir).absoluteFilePath(QStringLiteral("rtlsdr/librtlsdr.so")),
        QDir(appDir).absoluteFilePath(QStringLiteral("rtlsdr/rtl-sdr.so")),
        QDir(appDir).absoluteFilePath(QStringLiteral("librtlsdr.so")),
        QDir(appDir).absoluteFilePath(QStringLiteral("rtl-sdr.so")),
#endif
        QStringLiteral("rtlsdr"),
        QStringLiteral("librtlsdr"),
        QStringLiteral("rtl-sdr")
    };
}

template <typename T>
bool resolveSymbol(RtlSdrApi &rtl, T &target, const char *name) {
    target = reinterpret_cast<T>(rtl.library.resolve(name));
    if (!target) {
        rtl.lastError = QStringLiteral("Missing librtlsdr symbol: %1").arg(QString::fromLatin1(name));
        return false;
    }
    return true;
}

bool ensureLoadedLocked() {
    RtlSdrApi &rtl = api();
    if (rtl.library.isLoaded()) {
        return true;
    }

    QStringList loadErrors;
    for (const QString &candidate : libraryCandidates()) {
        rtl.library.setFileName(candidate);
#ifdef _WIN32
        const QFileInfo candidateInfo(candidate);
        const bool hasExplicitDirectory = candidateInfo.isAbsolute() && candidateInfo.dir().exists();
        if (hasExplicitDirectory) {
            SetDllDirectoryW(reinterpret_cast<LPCWSTR>(candidateInfo.dir().absolutePath().utf16()));
        }
#endif
        if (!rtl.library.load()) {
            rtl.lastError = rtl.library.errorString();
            loadErrors << QStringLiteral("%1: %2").arg(candidate, rtl.lastError);
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

        rtl.loadedPath = candidate;
        if (!resolveSymbol(rtl, rtl.get_device_count, "rtlsdr_get_device_count") ||
            !resolveSymbol(rtl, rtl.get_device_name, "rtlsdr_get_device_name") ||
            !resolveSymbol(rtl, rtl.open, "rtlsdr_open") ||
            !resolveSymbol(rtl, rtl.close, "rtlsdr_close") ||
            !resolveSymbol(rtl, rtl.set_center_freq, "rtlsdr_set_center_freq") ||
            !resolveSymbol(rtl, rtl.set_sample_rate, "rtlsdr_set_sample_rate") ||
            !resolveSymbol(rtl, rtl.set_tuner_gain_mode, "rtlsdr_set_tuner_gain_mode") ||
            !resolveSymbol(rtl, rtl.set_tuner_gain, "rtlsdr_set_tuner_gain") ||
            !resolveSymbol(rtl, rtl.set_agc_mode, "rtlsdr_set_agc_mode") ||
            !resolveSymbol(rtl, rtl.reset_buffer, "rtlsdr_reset_buffer") ||
            !resolveSymbol(rtl, rtl.read_async, "rtlsdr_read_async") ||
            !resolveSymbol(rtl, rtl.cancel_async, "rtlsdr_cancel_async")) {
            loadErrors << QStringLiteral("%1: %2").arg(candidate, rtl.lastError);
            rtl.library.unload();
            continue;
        }
        rtl.set_direct_sampling =
            reinterpret_cast<decltype(rtl.set_direct_sampling)>(rtl.library.resolve("rtlsdr_set_direct_sampling"));
        rtl.set_freq_correction =
            reinterpret_cast<decltype(rtl.set_freq_correction)>(rtl.library.resolve("rtlsdr_set_freq_correction"));
        rtl.read_sync =
            reinterpret_cast<decltype(rtl.read_sync)>(rtl.library.resolve("rtlsdr_read_sync"));

        qDebug() << "[RTL-SDR] loaded librtlsdr" << rtl.loadedPath;
        return true;
    }

    if (rtl.lastError.isEmpty()) {
        rtl.lastError = QStringLiteral("librtlsdr DLL was not found");
    } else if (!loadErrors.isEmpty()) {
        rtl.lastError = loadErrors.join(QStringLiteral(" | "));
    }
    return false;
}

template <typename Callable>
int withApi(Callable callable) {
    QMutexLocker locker(&apiMutex());
    if (!ensureLoadedLocked()) {
        return RTLSDR_ERR_NOT_LOADED;
    }
    return callable(api());
}

} // namespace

bool rtlSdrLibraryAvailable(QString *loadedPath, QString *errorMessage) {
    QMutexLocker locker(&apiMutex());
    const bool ok = ensureLoadedLocked();
    const RtlSdrApi &rtl = api();
    if (loadedPath) {
        *loadedPath = rtl.loadedPath;
    }
    if (errorMessage) {
        *errorMessage = ok ? QString() : rtl.lastError;
    }
    return ok;
}

QVector<RtlSdrDeviceInfo> enumerateRtlSdrDevices() {
    QVector<RtlSdrDeviceInfo> devices;
    QMutexLocker locker(&apiMutex());
    if (!ensureLoadedLocked()) {
        qDebug() << "[RTL-SDR] library unavailable for enumeration" << api().lastError;
        return devices;
    }

    RtlSdrApi &rtl = api();
    const uint32_t count = rtl.get_device_count();
    for (uint32_t i = 0; i < count; ++i) {
        RtlSdrDeviceInfo info;
        info.nativeIndex = static_cast<int>(i);
        const char *name = rtl.get_device_name ? rtl.get_device_name(i) : nullptr;
        info.name = name ? QString::fromLocal8Bit(name).trimmed() : QString();
        if (info.name.isEmpty()) {
            info.name = QStringLiteral("RTL-SDR");
        }
        info.label = QStringLiteral("[RTL-SDR native] #%1  %2").arg(info.nativeIndex).arg(info.name);
        devices.append(info);
    }
    return devices;
}

int openRtlSdrDeviceSafely(void **dev, uint32_t index) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }
    *dev = nullptr;
    return withApi([dev, index](RtlSdrApi &rtl) {
        rtlsdr_dev_t *opened = nullptr;
        const int result = rtl.open(&opened, index);
        *dev = opened;
        return result;
    });
}

int closeRtlSdrDeviceSafely(void *dev) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }
    return withApi([dev](RtlSdrApi &rtl) {
        return rtl.close(static_cast<rtlsdr_dev_t*>(dev));
    });
}

int setRtlSdrCenterFrequencySafely(void *dev, uint32_t frequencyHz) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }
    return withApi([dev, frequencyHz](RtlSdrApi &rtl) {
        return rtl.set_center_freq(static_cast<rtlsdr_dev_t*>(dev), frequencyHz);
    });
}

int setRtlSdrSampleRateSafely(void *dev, uint32_t sampleRateHz) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }
    return withApi([dev, sampleRateHz](RtlSdrApi &rtl) {
        return rtl.set_sample_rate(static_cast<rtlsdr_dev_t*>(dev), sampleRateHz);
    });
}

int setRtlSdrDirectSamplingSafely(void *dev, int enabled) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }
    return withApi([dev, enabled](RtlSdrApi &rtl) {
        if (!rtl.set_direct_sampling) {
            return 0;
        }
        return rtl.set_direct_sampling(static_cast<rtlsdr_dev_t*>(dev), enabled);
    });
}

int setRtlSdrFrequencyCorrectionSafely(void *dev, int ppm) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }
    return withApi([dev, ppm](RtlSdrApi &rtl) {
        if (!rtl.set_freq_correction) {
            return 0;
        }
        return rtl.set_freq_correction(static_cast<rtlsdr_dev_t*>(dev), ppm);
    });
}

int setRtlSdrTunerGainModeSafely(void *dev, int manual) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }
    return withApi([dev, manual](RtlSdrApi &rtl) {
        return rtl.set_tuner_gain_mode(static_cast<rtlsdr_dev_t*>(dev), manual);
    });
}

int setRtlSdrTunerGainSafely(void *dev, int gainTenthsDb) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }
    return withApi([dev, gainTenthsDb](RtlSdrApi &rtl) {
        return rtl.set_tuner_gain(static_cast<rtlsdr_dev_t*>(dev), gainTenthsDb);
    });
}

int setRtlSdrAgcModeSafely(void *dev, int enabled) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }
    return withApi([dev, enabled](RtlSdrApi &rtl) {
        return rtl.set_agc_mode(static_cast<rtlsdr_dev_t*>(dev), enabled);
    });
}

int resetRtlSdrBufferSafely(void *dev) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }
    return withApi([dev](RtlSdrApi &rtl) {
        return rtl.reset_buffer(static_cast<rtlsdr_dev_t*>(dev));
    });
}

int readRtlSdrSyncSafely(void *dev, void *buffer, int bytes, int *bytesRead) {
    if (!dev || !buffer || bytes <= 0 || !bytesRead) return RTLSDR_ERR_NOT_OPEN;
    *bytesRead = 0;
    decltype(RtlSdrApi::read_sync) readSync = nullptr;
    {
        QMutexLocker locker(&apiMutex());
        if (!ensureLoadedLocked()) return RTLSDR_ERR_NOT_LOADED;
        readSync = api().read_sync;
    }
    if (!readSync) return RTLSDR_ERR_NOT_LOADED;
    return readSync(static_cast<rtlsdr_dev_t *>(dev), buffer, bytes, bytesRead);
}

int readRtlSdrAsyncSafely(void *dev,
                          RtlSdrAsyncCallback callback,
                          void *ctx,
                          uint32_t bufferCount,
                          uint32_t blockBytes) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }

    decltype(RtlSdrApi::read_async) readAsync = nullptr;
    {
        QMutexLocker locker(&apiMutex());
        if (!ensureLoadedLocked()) {
            return RTLSDR_ERR_NOT_LOADED;
        }
        readAsync = api().read_async;
    }

    if (!readAsync) {
        return RTLSDR_ERR_NOT_LOADED;
    }
    return readAsync(static_cast<rtlsdr_dev_t*>(dev),
                     callback,
                     ctx,
                     bufferCount,
                     blockBytes);
}

int cancelRtlSdrAsyncSafely(void *dev) {
    if (!dev) {
        return RTLSDR_ERR_NOT_OPEN;
    }

    decltype(RtlSdrApi::cancel_async) cancelAsync = nullptr;
    {
        QMutexLocker locker(&apiMutex());
        if (!ensureLoadedLocked()) {
            return RTLSDR_ERR_NOT_LOADED;
        }
        cancelAsync = api().cancel_async;
    }

    if (!cancelAsync) {
        return RTLSDR_ERR_NOT_LOADED;
    }
    return cancelAsync(static_cast<rtlsdr_dev_t*>(dev));
}
