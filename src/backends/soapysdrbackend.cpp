#include "soapysdrbackend.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr int SOAPY_ERR_NOT_LOADED = -14001;
constexpr int SOAPY_ERR_NOT_OPEN = -14002;
constexpr int SOAPY_ERR_OPEN_FAILED = -14003;
constexpr int SOAPY_SDR_RX = 1;
constexpr int SOAPY_SDR_TX = 0;
constexpr const char *SOAPY_SDR_CF32 = "CF32";

struct SoapySdrKwargs {
    std::size_t size = 0;
    char **keys = nullptr;
    char **vals = nullptr;
};

struct SoapySdrRange {
    double minimum = 0.0;
    double maximum = 0.0;
    double step = 0.0;
};

using SoapySdrDevice = void;
using SoapySdrStream = void;

struct SoapySdrApi {
    QLibrary library;
    QString loadedPath;
    QString lastError;

    const char *(*last_error)() = nullptr;
    SoapySdrKwargs *(*enumerate)(const SoapySdrKwargs *args, std::size_t *length) = nullptr;
    void (*kwargs_list_clear)(SoapySdrKwargs *args, std::size_t length) = nullptr;
    const char *(*kwargs_get)(const SoapySdrKwargs *args, const char *key) = nullptr;
    SoapySdrDevice *(*make)(const SoapySdrKwargs *args) = nullptr;
    int (*unmake)(SoapySdrDevice *device) = nullptr;
    std::size_t (*get_num_channels)(const SoapySdrDevice *device, int direction) = nullptr;
    bool (*get_full_duplex)(const SoapySdrDevice *device, int direction, std::size_t channel) = nullptr;
    double *(*list_sample_rates)(const SoapySdrDevice *device, int direction,
                                std::size_t channel, std::size_t *length) = nullptr;
    double *(*list_bandwidths)(const SoapySdrDevice *device, int direction,
                              std::size_t channel, std::size_t *length) = nullptr;
    SoapySdrRange *(*get_frequency_range)(const SoapySdrDevice *device, int direction,
                                         std::size_t channel, std::size_t *length) = nullptr;
    void (*free_memory)(void *ptr) = nullptr;
    int (*set_sample_rate)(SoapySdrDevice *device, int direction, std::size_t channel, double sampleRate) = nullptr;
    int (*set_frequency)(SoapySdrDevice *device, int direction, std::size_t channel, double frequency, const SoapySdrKwargs *args) = nullptr;
    int (*set_bandwidth)(SoapySdrDevice *device, int direction, std::size_t channel, double bandwidth) = nullptr;
    int (*set_gain)(SoapySdrDevice *device, int direction, std::size_t channel, double gain) = nullptr;
    SoapySdrStream *(*setup_stream)(SoapySdrDevice *device,
                                    int direction,
                                    const char *format,
                                    const std::size_t *channels,
                                    std::size_t numChans,
                                    const SoapySdrKwargs *args) = nullptr;
    int (*activate_stream)(SoapySdrDevice *device,
                           SoapySdrStream *stream,
                           int flags,
                           long long timeNs,
                           std::size_t numElems) = nullptr;
    int (*read_stream)(SoapySdrDevice *device,
                       SoapySdrStream *stream,
                       void * const *buffs,
                       std::size_t numElems,
                       int *flags,
                       long long *timeNs,
                       long timeoutUs) = nullptr;
    int (*deactivate_stream)(SoapySdrDevice *device,
                             SoapySdrStream *stream,
                             int flags,
                             long long timeNs) = nullptr;
    int (*close_stream)(SoapySdrDevice *device, SoapySdrStream *stream) = nullptr;
};

QMutex &apiMutex() {
    static QMutex mutex;
    return mutex;
}

SoapySdrApi &api() {
    static SoapySdrApi instance;
    return instance;
}

QStringList libraryCandidates() {
    const QString appDir = QCoreApplication::applicationDirPath();
    return {
        QDir(appDir).absoluteFilePath(QStringLiteral("SoapySDR.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("soapysdr/SoapySDR.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("SoapySDR/SoapySDR.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("dsd-neo/SoapySDR.dll")),
        QDir(appDir).absoluteFilePath(QStringLiteral("pocketsdr/SoapySDR.dll")),
        QStringLiteral("SoapySDR"),
        QStringLiteral("SoapySDR.dll")
    };
}

template <typename T>
bool resolveSymbol(SoapySdrApi &soapy, T &target, const char *name, bool required = true) {
    target = reinterpret_cast<T>(soapy.library.resolve(name));
    if (!target && required) {
        soapy.lastError = QStringLiteral("Missing SoapySDR symbol: %1").arg(QString::fromLatin1(name));
        return false;
    }
    return true;
}

QString lastErrorLocked(SoapySdrApi &soapy) {
    if (soapy.last_error) {
        const char *message = soapy.last_error();
        if (message && *message) {
            return QString::fromLocal8Bit(message);
        }
    }
    return soapy.lastError;
}

bool ensureLoadedLocked() {
    SoapySdrApi &soapy = api();
    if (soapy.library.isLoaded()) {
        return true;
    }

    QStringList loadErrors;
    for (const QString &candidate : libraryCandidates()) {
        soapy.library.setFileName(candidate);
#ifdef _WIN32
        const QFileInfo candidateInfo(candidate);
        const bool hasExplicitDirectory = candidateInfo.isAbsolute() && candidateInfo.dir().exists();
        if (hasExplicitDirectory) {
            SetDllDirectoryW(reinterpret_cast<LPCWSTR>(candidateInfo.dir().absolutePath().utf16()));
        }
#endif
        if (!soapy.library.load()) {
            soapy.lastError = soapy.library.errorString();
            loadErrors << QStringLiteral("%1: %2").arg(candidate, soapy.lastError);
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

        soapy.loadedPath = candidate;
        const bool ok =
            resolveSymbol(soapy, soapy.enumerate, "SoapySDRDevice_enumerate") &&
            resolveSymbol(soapy, soapy.kwargs_list_clear, "SoapySDRKwargsList_clear") &&
            resolveSymbol(soapy, soapy.kwargs_get, "SoapySDRKwargs_get") &&
            resolveSymbol(soapy, soapy.make, "SoapySDRDevice_make") &&
            resolveSymbol(soapy, soapy.unmake, "SoapySDRDevice_unmake") &&
            resolveSymbol(soapy, soapy.set_sample_rate, "SoapySDRDevice_setSampleRate") &&
            resolveSymbol(soapy, soapy.set_frequency, "SoapySDRDevice_setFrequency") &&
            resolveSymbol(soapy, soapy.setup_stream, "SoapySDRDevice_setupStream") &&
            resolveSymbol(soapy, soapy.activate_stream, "SoapySDRDevice_activateStream") &&
            resolveSymbol(soapy, soapy.read_stream, "SoapySDRDevice_readStream") &&
            resolveSymbol(soapy, soapy.deactivate_stream, "SoapySDRDevice_deactivateStream") &&
            resolveSymbol(soapy, soapy.close_stream, "SoapySDRDevice_closeStream");
        resolveSymbol(soapy, soapy.last_error, "SoapySDRDevice_lastError", false);
        resolveSymbol(soapy, soapy.set_bandwidth, "SoapySDRDevice_setBandwidth", false);
        resolveSymbol(soapy, soapy.set_gain, "SoapySDRDevice_setGain", false);
        resolveSymbol(soapy, soapy.get_num_channels, "SoapySDRDevice_getNumChannels", false);
        resolveSymbol(soapy, soapy.get_full_duplex, "SoapySDRDevice_getFullDuplex", false);
        resolveSymbol(soapy, soapy.list_sample_rates, "SoapySDRDevice_listSampleRates", false);
        resolveSymbol(soapy, soapy.list_bandwidths, "SoapySDRDevice_listBandwidths", false);
        resolveSymbol(soapy, soapy.get_frequency_range, "SoapySDRDevice_getFrequencyRange", false);
        resolveSymbol(soapy, soapy.free_memory, "SoapySDR_free", false);

        if (!ok) {
            loadErrors << QStringLiteral("%1: %2").arg(candidate, soapy.lastError);
            soapy.library.unload();
            continue;
        }

        qDebug() << "[SoapySDR] loaded runtime" << soapy.loadedPath;
        return true;
    }

    if (soapy.lastError.isEmpty()) {
        soapy.lastError = QStringLiteral("SoapySDR runtime was not found");
    } else if (!loadErrors.isEmpty()) {
        soapy.lastError = loadErrors.join(QStringLiteral(" | "));
    }
    return false;
}

template <typename Callable>
int withApi(Callable callable) {
    QMutexLocker locker(&apiMutex());
    if (!ensureLoadedLocked()) {
        return SOAPY_ERR_NOT_LOADED;
    }
    return callable(api());
}

QString kwargValue(SoapySdrApi &soapy, const SoapySdrKwargs *kwargs, const char *key) {
    if (!kwargs || !soapy.kwargs_get) {
        return {};
    }
    const char *value = soapy.kwargs_get(kwargs, key);
    return value ? QString::fromLocal8Bit(value).trimmed() : QString();
}

} // namespace

bool soapySdrLibraryAvailable(QString *loadedPath, QString *errorMessage) {
    QMutexLocker locker(&apiMutex());
    const bool ok = ensureLoadedLocked();
    const SoapySdrApi &soapy = api();
    if (loadedPath) {
        *loadedPath = soapy.loadedPath;
    }
    if (errorMessage) {
        *errorMessage = ok ? QString() : soapy.lastError;
    }
    return ok;
}

QString soapySdrLastErrorMessage() {
    QMutexLocker locker(&apiMutex());
    SoapySdrApi &soapy = api();
    return lastErrorLocked(soapy);
}

QVector<SoapySdrDeviceInfo> enumerateSoapySdrDevices() {
    QVector<SoapySdrDeviceInfo> devices;
    QMutexLocker locker(&apiMutex());
    if (!ensureLoadedLocked()) {
        qDebug() << "[SoapySDR] runtime unavailable for enumeration" << api().lastError;
        return devices;
    }

    SoapySdrApi &soapy = api();
    std::size_t count = 0;
    SoapySdrKwargs *list = soapy.enumerate(nullptr, &count);
    for (std::size_t i = 0; list && i < count; ++i) {
        const SoapySdrKwargs *kwargs = &list[i];
        SoapySdrDeviceInfo info;
        info.nativeIndex = static_cast<int>(i);
        info.driver = kwargValue(soapy, kwargs, "driver");
        info.hardware = kwargValue(soapy, kwargs, "hardware");
        info.serial = kwargValue(soapy, kwargs, "serial");
        const QString label = kwargValue(soapy, kwargs, "label");
        QStringList parts;
        if (!info.driver.isEmpty()) {
            parts << info.driver;
        }
        if (!info.hardware.isEmpty()) {
            parts << info.hardware;
        }
        if (!label.isEmpty()) {
            parts << label;
        }
        if (!info.serial.isEmpty()) {
            parts << QStringLiteral("SN %1").arg(info.serial);
        }
        info.label = QStringLiteral("[SoapySDR] #%1  %2")
                         .arg(info.nativeIndex)
                         .arg(parts.isEmpty() ? QStringLiteral("device") : parts.join(QStringLiteral("  ")));
        devices.append(info);
    }
    if (list) {
        soapy.kwargs_list_clear(list, count);
    }
    return devices;
}

SoapySdrDeviceCapabilities querySoapySdrDeviceCapabilities(int nativeIndex, int rxChannel) {
    SoapySdrDeviceCapabilities capabilities;
    QMutexLocker locker(&apiMutex());
    if (!ensureLoadedLocked()) {
        capabilities.error = api().lastError;
        return capabilities;
    }

    SoapySdrApi &soapy = api();
    std::size_t count = 0;
    SoapySdrKwargs *list = soapy.enumerate(nullptr, &count);
    SoapySdrDevice *opened = nullptr;
    if (list && count > 0) {
        const std::size_t index = static_cast<std::size_t>(
            std::clamp(nativeIndex, 0, static_cast<int>(count - 1)));
        opened = soapy.make(&list[index]);
        soapy.kwargs_list_clear(list, count);
    } else {
        opened = soapy.make(nullptr);
    }
    if (!opened) {
        capabilities.error = lastErrorLocked(soapy);
        if (capabilities.error.isEmpty())
            capabilities.error = QStringLiteral("SoapySDRDevice_make returned null");
        return capabilities;
    }

    capabilities.rxChannels = soapy.get_num_channels
                                  ? static_cast<int>(soapy.get_num_channels(opened, SOAPY_SDR_RX))
                                  : 1;
    capabilities.txChannels = soapy.get_num_channels
                                  ? static_cast<int>(soapy.get_num_channels(opened, SOAPY_SDR_TX))
                                  : 0;
    const int channel = std::clamp(rxChannel, 0, std::max(0, capabilities.rxChannels - 1));
    capabilities.fullDuplex = capabilities.rxChannels > 0 && capabilities.txChannels > 0 &&
                              soapy.get_full_duplex &&
                              soapy.get_full_duplex(opened, SOAPY_SDR_RX,
                                                    static_cast<std::size_t>(channel));

    if (capabilities.rxChannels > 0 && soapy.free_memory) {
        std::size_t length = 0;
        if (soapy.list_sample_rates) {
            double *rates = soapy.list_sample_rates(opened, SOAPY_SDR_RX,
                                                    static_cast<std::size_t>(channel), &length);
            for (std::size_t index = 0; rates && index < length; ++index) {
                if (std::isfinite(rates[index]) && rates[index] > 0.0)
                    capabilities.rxSampleRatesHz.append(rates[index]);
            }
            if (rates) soapy.free_memory(rates);
        }
        length = 0;
        if (soapy.list_bandwidths) {
            double *bandwidths = soapy.list_bandwidths(opened, SOAPY_SDR_RX,
                                                       static_cast<std::size_t>(channel), &length);
            for (std::size_t index = 0; bandwidths && index < length; ++index) {
                if (std::isfinite(bandwidths[index]) && bandwidths[index] > 0.0)
                    capabilities.rxBandwidthsHz.append(bandwidths[index]);
            }
            if (bandwidths) soapy.free_memory(bandwidths);
        }
        length = 0;
        if (soapy.get_frequency_range) {
            SoapySdrRange *ranges = soapy.get_frequency_range(
                opened, SOAPY_SDR_RX, static_cast<std::size_t>(channel), &length);
            for (std::size_t index = 0; ranges && index < length; ++index) {
                capabilities.rxFrequencyRangesHz.append(
                    {ranges[index].minimum, ranges[index].maximum, ranges[index].step});
            }
            if (ranges) soapy.free_memory(ranges);
        }
    }
    std::sort(capabilities.rxSampleRatesHz.begin(), capabilities.rxSampleRatesHz.end());
    capabilities.rxSampleRatesHz.erase(
        std::unique(capabilities.rxSampleRatesHz.begin(), capabilities.rxSampleRatesHz.end()),
        capabilities.rxSampleRatesHz.end());
    std::sort(capabilities.rxBandwidthsHz.begin(), capabilities.rxBandwidthsHz.end());
    capabilities.rxBandwidthsHz.erase(
        std::unique(capabilities.rxBandwidthsHz.begin(), capabilities.rxBandwidthsHz.end()),
        capabilities.rxBandwidthsHz.end());
    capabilities.valid = capabilities.rxChannels > 0;
    if (!capabilities.valid && capabilities.error.isEmpty())
        capabilities.error = QStringLiteral("Device reports no RX channels");
    soapy.unmake(opened);
    return capabilities;
}

int openSoapySdrDeviceSafely(void **dev, int nativeIndex) {
    if (!dev) {
        return SOAPY_ERR_NOT_OPEN;
    }
    *dev = nullptr;
    return withApi([dev, nativeIndex](SoapySdrApi &soapy) {
        SoapySdrDevice *opened = nullptr;
        std::size_t count = 0;
        SoapySdrKwargs *list = soapy.enumerate(nullptr, &count);
        if (list && count > 0) {
            const std::size_t index = static_cast<std::size_t>((std::clamp)(nativeIndex, 0, static_cast<int>(count - 1)));
            opened = soapy.make(&list[index]);
            soapy.kwargs_list_clear(list, count);
        } else {
            opened = soapy.make(nullptr);
        }
        *dev = opened;
        if (!opened) {
            soapy.lastError = lastErrorLocked(soapy);
            if (soapy.lastError.isEmpty()) {
                soapy.lastError = QStringLiteral("SoapySDRDevice_make returned null");
            }
            return SOAPY_ERR_OPEN_FAILED;
        }
        return 0;
    });
}

int closeSoapySdrDeviceSafely(void *dev) {
    if (!dev) {
        return SOAPY_ERR_NOT_OPEN;
    }
    return withApi([dev](SoapySdrApi &soapy) {
        return soapy.unmake(static_cast<SoapySdrDevice*>(dev));
    });
}

int setSoapySdrSampleRateSafely(void *dev, double sampleRateHz, int channel) {
    if (!dev) {
        return SOAPY_ERR_NOT_OPEN;
    }
    return withApi([dev, sampleRateHz, channel](SoapySdrApi &soapy) {
        return soapy.set_sample_rate(static_cast<SoapySdrDevice*>(dev), SOAPY_SDR_RX,
                                     static_cast<std::size_t>(std::max(0, channel)), sampleRateHz);
    });
}

int setSoapySdrCenterFrequencySafely(void *dev, double frequencyHz, int channel) {
    if (!dev) {
        return SOAPY_ERR_NOT_OPEN;
    }
    return withApi([dev, frequencyHz, channel](SoapySdrApi &soapy) {
        return soapy.set_frequency(static_cast<SoapySdrDevice*>(dev), SOAPY_SDR_RX,
                                   static_cast<std::size_t>(std::max(0, channel)), frequencyHz, nullptr);
    });
}

int setSoapySdrBandwidthSafely(void *dev, double bandwidthHz, int channel) {
    if (!dev) {
        return SOAPY_ERR_NOT_OPEN;
    }
    return withApi([dev, bandwidthHz, channel](SoapySdrApi &soapy) {
        if (!soapy.set_bandwidth || bandwidthHz <= 0.0) {
            return 0;
        }
        return soapy.set_bandwidth(static_cast<SoapySdrDevice*>(dev), SOAPY_SDR_RX,
                                   static_cast<std::size_t>(std::max(0, channel)), bandwidthHz);
    });
}

int setSoapySdrGainSafely(void *dev, double gainDb, int channel) {
    if (!dev) return SOAPY_ERR_NOT_OPEN;
    return withApi([dev, gainDb, channel](SoapySdrApi &soapy) {
        if (!soapy.set_gain) return 0;
        return soapy.set_gain(static_cast<SoapySdrDevice*>(dev), SOAPY_SDR_RX,
                              static_cast<std::size_t>(std::max(0, channel)), gainDb);
    });
}

void *setupSoapySdrRxStreamSafely(void *dev, int channelIndex) {
    if (!dev) {
        return nullptr;
    }

    QMutexLocker locker(&apiMutex());
    if (!ensureLoadedLocked()) {
        return nullptr;
    }
    SoapySdrApi &soapy = api();
    const std::size_t channel = static_cast<std::size_t>(std::max(0, channelIndex));
    return soapy.setup_stream(static_cast<SoapySdrDevice*>(dev),
                              SOAPY_SDR_RX,
                              SOAPY_SDR_CF32,
                              &channel,
                              1,
                              nullptr);
}

int activateSoapySdrStreamSafely(void *dev, void *stream) {
    if (!dev || !stream) {
        return SOAPY_ERR_NOT_OPEN;
    }
    return withApi([dev, stream](SoapySdrApi &soapy) {
        return soapy.activate_stream(static_cast<SoapySdrDevice*>(dev),
                                     static_cast<SoapySdrStream*>(stream),
                                     0,
                                     0,
                                     0);
    });
}

int readSoapySdrStreamSafely(void *dev,
                             void *stream,
                             float *buffer,
                             uint32_t sampleCount,
                             long timeoutUs) {
    if (!dev || !stream || !buffer || sampleCount == 0) {
        return SOAPY_ERR_NOT_OPEN;
    }

    decltype(SoapySdrApi::read_stream) readStream = nullptr;
    {
        QMutexLocker locker(&apiMutex());
        if (!ensureLoadedLocked()) {
            return SOAPY_ERR_NOT_LOADED;
        }
        readStream = api().read_stream;
    }

    void *buffers[] = {buffer};
    int flags = 0;
    long long timeNs = 0;
    return readStream(static_cast<SoapySdrDevice*>(dev),
                      static_cast<SoapySdrStream*>(stream),
                      buffers,
                      sampleCount,
                      &flags,
                      &timeNs,
                      timeoutUs);
}

int deactivateSoapySdrStreamSafely(void *dev, void *stream) {
    if (!dev || !stream) {
        return SOAPY_ERR_NOT_OPEN;
    }
    return withApi([dev, stream](SoapySdrApi &soapy) {
        return soapy.deactivate_stream(static_cast<SoapySdrDevice*>(dev),
                                       static_cast<SoapySdrStream*>(stream),
                                       0,
                                       0);
    });
}

int closeSoapySdrStreamSafely(void *dev, void *stream) {
    if (!dev || !stream) {
        return SOAPY_ERR_NOT_OPEN;
    }
    return withApi([dev, stream](SoapySdrApi &soapy) {
        return soapy.close_stream(static_cast<SoapySdrDevice*>(dev),
                                  static_cast<SoapySdrStream*>(stream));
    });
}
