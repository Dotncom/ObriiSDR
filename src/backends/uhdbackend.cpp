#include "uhdbackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
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
    bool txAttempted = false;
    bool txReady = false;
    QString txError;
    int (*txChannelCount)(void *, std::size_t *) = nullptr;
    int (*txSetRate)(void *, double, std::size_t) = nullptr;
    int (*txGetRate)(void *, std::size_t, double *) = nullptr;
    int (*txSetFrequency)(void *, TuneRequest *, std::size_t, TuneResult *) = nullptr;
    int (*txGetFrequency)(void *, std::size_t, double *) = nullptr;
    int (*txSetGain)(void *, double, std::size_t, const char *) = nullptr;
    int (*txSetAntenna)(void *, const char *, std::size_t) = nullptr;
    int (*txSetBandwidth)(void *, double, std::size_t) = nullptr;
    int (*txStreamMake)(void **) = nullptr;
    int (*txStreamFree)(void **) = nullptr;
    int (*txGetStream)(void *, StreamArgs *, void *) = nullptr;
    int (*txSend)(void *, const void **, std::size_t, void **, double, std::size_t *) = nullptr;
    int (*txStreamLastError)(void *, char *, std::size_t) = nullptr;
    int (*txMetadataMake)(void **, bool, std::int64_t, double, bool, bool) = nullptr;
    int (*txMetadataFree)(void **) = nullptr;

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

    bool loadTx() {
        if (!load()) return false;
        QMutexLocker lock(&mutex);
        if (txAttempted) return txReady;
        txAttempted = true;
#define UHD_TX_RESOLVE(field, symbol) field = reinterpret_cast<decltype(field)>(library.resolve(symbol)); if (!field) { txError = QStringLiteral("UHD C API missing: ") + QStringLiteral(symbol); return false; }
        UHD_TX_RESOLVE(txChannelCount, "uhd_usrp_get_tx_num_channels")
        UHD_TX_RESOLVE(txSetRate, "uhd_usrp_set_tx_rate")
        UHD_TX_RESOLVE(txGetRate, "uhd_usrp_get_tx_rate")
        UHD_TX_RESOLVE(txSetFrequency, "uhd_usrp_set_tx_freq")
        UHD_TX_RESOLVE(txGetFrequency, "uhd_usrp_get_tx_freq")
        UHD_TX_RESOLVE(txSetGain, "uhd_usrp_set_tx_gain")
        UHD_TX_RESOLVE(txSetAntenna, "uhd_usrp_set_tx_antenna")
        UHD_TX_RESOLVE(txSetBandwidth, "uhd_usrp_set_tx_bandwidth")
        UHD_TX_RESOLVE(txStreamMake, "uhd_tx_streamer_make")
        UHD_TX_RESOLVE(txStreamFree, "uhd_tx_streamer_free")
        UHD_TX_RESOLVE(txGetStream, "uhd_usrp_get_tx_stream")
        UHD_TX_RESOLVE(txSend, "uhd_tx_streamer_send")
        UHD_TX_RESOLVE(txStreamLastError, "uhd_tx_streamer_last_error")
        UHD_TX_RESOLVE(txMetadataMake, "uhd_tx_metadata_make")
        UHD_TX_RESOLVE(txMetadataFree, "uhd_tx_metadata_free")
#undef UHD_TX_RESOLVE
        txReady = true;
        return true;
    }
};

Api &api() { static Api instance; return instance; }

QString deviceError(void *device, const QString &fallback) {
    char message[512] = {};
    if (device && api().lastError(device, message, sizeof(message)) == 0 && message[0])
        return QString::fromUtf8(message);
    return fallback;
}

QString txStreamError(void *streamer, const QString &fallback) {
    char message[512] = {};
    if (streamer && api().txStreamLastError(streamer, message, sizeof(message)) == 0 && message[0])
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

UhdTxSession::~UhdTxSession() { close(); }

bool UhdTxSession::open(int deviceIndex, int channel, double sampleRateHz, double centerHz,
                        double gainDb, double bandwidthHz, const QString &antenna,
                        const QString &fpgaImagePath,
                        QString *errorMessage) {
    close();
    Api &a = api();
    if (!a.loadTx()) {
        if (errorMessage) *errorMessage = a.txError.isEmpty() ? a.error : a.txError;
        return false;
    }
    const QStringList devices = enumerateUhdDeviceArgs(errorMessage);
    if (deviceIndex < 0 || deviceIndex >= devices.size()) {
        if (errorMessage) *errorMessage = QStringLiteral("No UHD device at index %1").arg(deviceIndex);
        return false;
    }
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
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX device open failed"));
        close();
        return false;
    }
    std::size_t channels = 0;
    if (a.txChannelCount(device_, &channels) != 0 || channel < 0 ||
        static_cast<std::size_t>(channel) >= channels) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX channel unavailable"));
        close();
        return false;
    }
    channel_ = channel;
    const std::size_t selected = static_cast<std::size_t>(channel_);
    if (a.txSetRate(device_, sampleRateHz, selected) != 0) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX rate rejected"));
        close();
        return false;
    }
    double actualRate = 0.0;
    if (a.txGetRate(device_, selected, &actualRate) != 0 ||
        std::abs(actualRate - sampleRateHz) > sampleRateHz * 0.001) {
        if (errorMessage) *errorMessage = QStringLiteral("UHD coerced TX rate to %1 S/s; choose that rate explicitly").arg(actualRate);
        close();
        return false;
    }
    TuneRequest tune{centerHz, 65, 0.0, 65, 0.0, nullptr};
    TuneResult tuned{};
    double actualFrequency = 0.0;
    const QByteArray antennaName = antenna.trimmed().toUtf8();
    if (a.txSetFrequency(device_, &tune, selected, &tuned) != 0 ||
        a.txGetFrequency(device_, selected, &actualFrequency) != 0 ||
        std::abs(actualFrequency - centerHz) > (std::max)(100.0, centerHz * 1e-6) ||
        a.txSetGain(device_, gainDb, selected, "") != 0 ||
        (bandwidthHz > 0.0 && a.txSetBandwidth(device_, bandwidthHz, selected) != 0) ||
        (!antennaName.isEmpty() && a.txSetAntenna(device_, antennaName.constData(), selected) != 0)) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX frequency/gain/bandwidth/antenna rejected"));
        close();
        return false;
    }
    if (a.txStreamMake(&streamer_) != 0 || !streamer_) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX streamer allocation failed"));
        close();
        return false;
    }
    char cpu[] = "fc32";
    char wire[] = "sc16";
    char empty[] = "";
    std::size_t streamChannel = selected;
    StreamArgs streamArgs{cpu, wire, empty, &streamChannel, 1};
    if (a.txGetStream(device_, &streamArgs, streamer_) != 0) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX stream creation failed"));
        close();
        return false;
    }
    return true;
}

bool UhdTxSession::send(const std::complex<float> *iq, std::size_t count, bool startOfBurst,
                        QString *errorMessage) {
    if (!streamer_ || !iq || !count) return false;
    Api &a = api();
    void *metadata = nullptr;
    if (a.txMetadataMake(&metadata, false, 0, 0.0, startOfBurst, false) != 0 || !metadata) {
        if (errorMessage) *errorMessage = QStringLiteral("UHD TX metadata allocation failed");
        return false;
    }
    std::size_t offset = 0;
    bool okay = true;
    while (offset < count) {
        const void *buffers[] = {iq + offset};
        std::size_t sent = 0;
        if (a.txSend(streamer_, buffers, count - offset, &metadata, 1.0, &sent) != 0 || sent == 0) {
            if (errorMessage) *errorMessage = txStreamError(streamer_, QStringLiteral("UHD TX send failed or timed out"));
            okay = false;
            break;
        }
        offset += sent;
        if (offset < count && startOfBurst) {
            a.txMetadataFree(&metadata);
            metadata = nullptr;
            if (a.txMetadataMake(&metadata, false, 0, 0.0, false, false) != 0 || !metadata) {
                if (errorMessage) *errorMessage = QStringLiteral("UHD TX continuation metadata failed");
                okay = false;
                break;
            }
        }
    }
    if (metadata) a.txMetadataFree(&metadata);
    return okay;
}

bool UhdTxSession::finish(QString *errorMessage) {
    if (!streamer_) return false;
    Api &a = api();
    void *metadata = nullptr;
    if (a.txMetadataMake(&metadata, false, 0, 0.0, false, true) != 0 || !metadata) {
        if (errorMessage) *errorMessage = QStringLiteral("UHD TX end-of-burst metadata failed");
        return false;
    }
    const void *buffers[] = {nullptr};
    std::size_t sent = 0;
    const bool okay = a.txSend(streamer_, buffers, 0, &metadata, 1.0, &sent) == 0;
    a.txMetadataFree(&metadata);
    if (!okay && errorMessage) *errorMessage = txStreamError(streamer_, QStringLiteral("UHD TX end-of-burst failed"));
    return okay;
}

void UhdTxSession::close() {
    Api &a = api();
    if (streamer_) a.txStreamFree(&streamer_);
    if (device_) a.freeDevice(&device_);
}
