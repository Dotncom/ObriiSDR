#include "uhdbackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>

#include <cmath>
#include <cstdint>
#include <cstring>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

// ABI from UHD 4.x C API: uhd/usrp/usrp.h and uhd/types/{metadata,tune_request}.h.
// Keep these declarations private so UHD is an optional runtime dependency.
struct StreamArgs {
    char *cpuFormat;
    char *wireFormat;
    char *args;
    std::size_t *channels;
    int channelCount;
};
struct StreamCommand {
    int mode;
    std::size_t sampleCount;
    bool now;
    std::int64_t seconds;
    double fractionalSeconds;
};
struct TuneRequest {
    double target;
    int rfPolicy;
    double rf;
    int dspPolicy;
    double dsp;
    char *args;
};
struct TuneResult { double clippedRf, targetRf, actualRf, targetDsp, actualDsp; };

struct Api {
    QLibrary library;
    QMutex mutex;
    bool attempted = false;
    bool ready = false;
    QString error;
#ifdef Q_OS_WIN
    HMODULE pinnedModule = nullptr;
#endif
    int (*vectorMake)(void **) = nullptr;
    int (*vectorFree)(void **) = nullptr;
    int (*vectorSize)(void *, std::size_t *) = nullptr;
    int (*vectorAt)(void *, std::size_t, char *, std::size_t) = nullptr;
    int (*find)(const char *, void **) = nullptr;
    int (*make)(void **, const char *) = nullptr;
    int (*freeDevice)(void **) = nullptr;
    int (*lastError)(void *, char *, std::size_t) = nullptr;
    int (*channelCount)(void *, std::size_t *) = nullptr;
    int (*setRate)(void *, double, std::size_t) = nullptr;
    int (*getRate)(void *, std::size_t, double *) = nullptr;
    int (*setFrequency)(void *, TuneRequest *, std::size_t, TuneResult *) = nullptr;
    int (*setGain)(void *, double, std::size_t, const char *) = nullptr;
    int (*setAntenna)(void *, const char *, std::size_t) = nullptr;
    int (*streamMake)(void **) = nullptr;
    int (*streamFree)(void **) = nullptr;
    int (*getStream)(void *, StreamArgs *, void *) = nullptr;
    int (*streamCommand)(void *, const StreamCommand *) = nullptr;
    int (*receive)(void *, void **, std::size_t, void **, double, bool, std::size_t *) = nullptr;
    int (*metadataMake)(void **) = nullptr;
    int (*metadataFree)(void **) = nullptr;
    int (*metadataError)(void *, int *) = nullptr;

    bool load() {
        QMutexLocker lock(&mutex);
        if (attempted) return ready;
        attempted = true;
#ifdef Q_OS_WIN
        const QStringList candidates = {
            qEnvironmentVariable("OBRII_UHD_LIBRARY_PATH"),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("uhd.dll")),
            QDir(QCoreApplication::applicationDirPath()).filePath(
                QStringLiteral("wifi-phy/Library/bin/uhd.dll")),
            QStringLiteral("uhd.dll")
        };
#else
        const QStringList candidates = {qEnvironmentVariable("OBRII_UHD_LIBRARY_PATH"),
                                        QStringLiteral("libuhd.so"), QStringLiteral("uhd")};
#endif
        for (const QString &candidate : candidates) {
            if (candidate.isEmpty()) continue;
#ifdef Q_OS_WIN
            if (QFileInfo(candidate).isAbsolute()) {
                pinnedModule = LoadLibraryExW(reinterpret_cast<LPCWSTR>(candidate.utf16()), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (!pinnedModule) continue;
            }
#endif
            library.setFileName(candidate);
            if (library.load()) break;
#ifdef Q_OS_WIN
            if (pinnedModule) {
                FreeLibrary(pinnedModule);
                pinnedModule = nullptr;
            }
#endif
            error = library.errorString();
        }
        if (!library.isLoaded()) return false;
#define UHD_RESOLVE(field, symbol) field = reinterpret_cast<decltype(field)>(library.resolve(symbol)); if (!field) { error = QStringLiteral("UHD C API missing: ") + QStringLiteral(symbol); return false; }
        UHD_RESOLVE(vectorMake, "uhd_string_vector_make")
        UHD_RESOLVE(vectorFree, "uhd_string_vector_free")
        UHD_RESOLVE(vectorSize, "uhd_string_vector_size")
        UHD_RESOLVE(vectorAt, "uhd_string_vector_at")
        UHD_RESOLVE(find, "uhd_usrp_find")
        UHD_RESOLVE(make, "uhd_usrp_make")
        UHD_RESOLVE(freeDevice, "uhd_usrp_free")
        UHD_RESOLVE(lastError, "uhd_usrp_last_error")
        UHD_RESOLVE(channelCount, "uhd_usrp_get_rx_num_channels")
        UHD_RESOLVE(setRate, "uhd_usrp_set_rx_rate")
        UHD_RESOLVE(getRate, "uhd_usrp_get_rx_rate")
        UHD_RESOLVE(setFrequency, "uhd_usrp_set_rx_freq")
        UHD_RESOLVE(setGain, "uhd_usrp_set_rx_gain")
        UHD_RESOLVE(setAntenna, "uhd_usrp_set_rx_antenna")
        UHD_RESOLVE(streamMake, "uhd_rx_streamer_make")
        UHD_RESOLVE(streamFree, "uhd_rx_streamer_free")
        UHD_RESOLVE(getStream, "uhd_usrp_get_rx_stream")
        UHD_RESOLVE(streamCommand, "uhd_rx_streamer_issue_stream_cmd")
        UHD_RESOLVE(receive, "uhd_rx_streamer_recv")
        UHD_RESOLVE(metadataMake, "uhd_rx_metadata_make")
        UHD_RESOLVE(metadataFree, "uhd_rx_metadata_free")
        UHD_RESOLVE(metadataError, "uhd_rx_metadata_error_code")
#undef UHD_RESOLVE
        ready = true;
        return ready;
    }
};

Api &api() { static Api instance; return instance; }

QString deviceError(void *device, const QString &fallback) {
    char message[512] = {};
    if (device && api().lastError(device, message, sizeof(message)) == 0 && message[0])
        return QString::fromUtf8(message);
    return fallback;
}

} // namespace

QStringList enumerateUhdDeviceArgs(QString *errorMessage) {
    Api &a = api();
    if (!a.load()) {
        if (errorMessage) *errorMessage = a.error;
        return {};
    }
    void *list = nullptr;
    if (a.vectorMake(&list) != 0 || !list) {
        if (errorMessage) *errorMessage = QStringLiteral("UHD device list allocation failed");
        return {};
    }
    QStringList result;
    if (a.find("", &list) == 0) {
        std::size_t count = 0;
        if (a.vectorSize(list, &count) == 0) {
            for (std::size_t i = 0; i < count; ++i) {
                char args[1024] = {};
                if (a.vectorAt(list, i, args, sizeof(args)) == 0) result << QString::fromUtf8(args);
            }
        }
    } else if (errorMessage) {
        *errorMessage = QStringLiteral("UHD device discovery failed");
    }
    a.vectorFree(&list);
    return result;
}

UhdRxSession::~UhdRxSession() { close(); }

bool UhdRxSession::open(int deviceIndex, int channel, double sampleRateHz, double centerHz,
                        double gainDb, const QString &antenna, const QString &fpgaImagePath,
                        QString *errorMessage) {
    close();
    const QStringList devices = enumerateUhdDeviceArgs(errorMessage);
    if (deviceIndex < 0 || deviceIndex >= devices.size()) {
        if (errorMessage && errorMessage->isEmpty())
            *errorMessage = QStringLiteral("No UHD device at index %1").arg(deviceIndex);
        return false;
    }
    Api &a = api();
    QString deviceArgs = devices.at(deviceIndex);
    if (!fpgaImagePath.trimmed().isEmpty()) {
        const QFileInfo image(fpgaImagePath.trimmed());
        if (!image.isFile() || image.absoluteFilePath().contains(QLatin1Char(','))) {
            if (errorMessage) *errorMessage = QStringLiteral("UHD FPGA image is missing or its path contains a comma");
            return false;
        }
        if (!deviceArgs.isEmpty()) deviceArgs += QLatin1Char(',');
        deviceArgs += QStringLiteral("fpga=") + QDir::fromNativeSeparators(image.absoluteFilePath());
    }
    const QByteArray args = deviceArgs.toUtf8();
    if (a.make(&device_, args.constData()) != 0 || !device_) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD device open failed"));
        close();
        return false;
    }
    std::size_t channels = 0;
    if (a.channelCount(device_, &channels) != 0 || channel < 0 ||
        static_cast<std::size_t>(channel) >= channels ||
        a.setRate(device_, sampleRateHz, static_cast<std::size_t>(channel)) != 0) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD RX channel/rate rejected"));
        close();
        return false;
    }
    double actualRate = 0.0;
    if (a.getRate(device_, static_cast<std::size_t>(channel), &actualRate) != 0 ||
        std::abs(actualRate - sampleRateHz) > sampleRateHz * 0.001) {
        if (errorMessage) *errorMessage = QStringLiteral("UHD coerced RX rate to %1 S/s; choose a supported rate").arg(actualRate);
        close();
        return false;
    }
    channel_ = channel;
    const QByteArray antennaName = antenna.trimmed().toUtf8();
    if (a.setGain(device_, gainDb, static_cast<std::size_t>(channel_), "") != 0 ||
        (!antennaName.isEmpty() &&
         a.setAntenna(device_, antennaName.constData(), static_cast<std::size_t>(channel_)) != 0)) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD RX gain/antenna rejected"));
        close();
        return false;
    }
    if (!retune(centerHz, errorMessage) || a.streamMake(&streamer_) != 0 ||
        a.metadataMake(&metadata_) != 0) {
        if (errorMessage && errorMessage->isEmpty()) *errorMessage = deviceError(device_, QStringLiteral("UHD RX setup failed"));
        close();
        return false;
    }
    char cpu[] = "fc32";
    char wire[] = "sc16";
    char empty[] = "";
    std::size_t selectedChannel = static_cast<std::size_t>(channel);
    StreamArgs streamArgs{cpu, wire, empty, &selectedChannel, 1};
    if (a.getStream(device_, &streamArgs, streamer_) != 0) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD RX stream creation failed"));
        close();
        return false;
    }
    const StreamCommand start{97, 0, true, 0, 0.0};
    if (a.streamCommand(streamer_, &start) != 0) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD RX stream start failed"));
        close();
        return false;
    }
    return true;
}

bool UhdRxSession::retune(double centerHz, QString *errorMessage) {
    if (!device_ || !std::isfinite(centerHz) || centerHz <= 0.0) return false;
    TuneRequest request{centerHz, 65, 0.0, 65, 0.0, nullptr};
    TuneResult result{};
    if (api().setFrequency(device_, &request, static_cast<std::size_t>(channel_), &result) == 0)
        return true;
    if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD RX retune failed"));
    return false;
}

int UhdRxSession::receive(float *interleavedIq, std::size_t capacity, QString *errorMessage) {
    if (!streamer_ || !metadata_ || !interleavedIq || !capacity) return -1;
    void *buffers[] = {interleavedIq};
    std::size_t received = 0;
    if (api().receive(streamer_, buffers, capacity, &metadata_, 0.2, false, &received) != 0) {
        if (errorMessage) *errorMessage = QStringLiteral("UHD RX receive call failed");
        return -1;
    }
    int code = 0;
    if (api().metadataError(metadata_, &code) != 0) return -1;
    if (code == 0) return static_cast<int>(received);
    if (code == 1 || code == 8) return 0; // timeout or recoverable overflow
    if (errorMessage) *errorMessage = QStringLiteral("UHD RX metadata error %1").arg(code);
    return -1;
}

void UhdRxSession::close() {
    Api &a = api();
    if (streamer_) {
        const StreamCommand stop{111, 0, true, 0, 0.0};
        a.streamCommand(streamer_, &stop);
        a.streamFree(&streamer_);
    }
    if (metadata_) a.metadataFree(&metadata_);
    if (device_) a.freeDevice(&device_);
}
