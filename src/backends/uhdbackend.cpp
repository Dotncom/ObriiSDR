#include "uhdbackend.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
QMutex rxDevicesMutex;
QSet<QString> openRxDevices;

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
    int (*rxStreamLastError)(void *, char *, std::size_t) = nullptr;
    int (*receive)(void *, void **, std::size_t, void **, double, bool, std::size_t *) = nullptr;
    int (*metadataMake)(void **) = nullptr;
    int (*metadataFree)(void **) = nullptr;
    int (*metadataError)(void *, int *) = nullptr;
    int (*getTimeNow)(void *, std::size_t, std::int64_t *, double *) = nullptr;
    int (*metadataHasTime)(void *, bool *) = nullptr;
    int (*metadataTime)(void *, std::int64_t *, double *) = nullptr;
    int (*setClockSource)(void *, const char *, std::size_t) = nullptr;
    int (*getClockSource)(void *, std::size_t, char *, std::size_t) = nullptr;
    int (*sensorMake)(void **) = nullptr;
    int (*sensorFree)(void **) = nullptr;
    int (*getMboardSensor)(void *, const char *, std::size_t, void **) = nullptr;
    int (*getMboardSensorNames)(void *, std::size_t, void **) = nullptr;
    int (*sensorToBool)(void *, bool *) = nullptr;
    int (*sensorValue)(void *, char *, std::size_t) = nullptr;
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
        getTimeNow = reinterpret_cast<decltype(getTimeNow)>(library.resolve("uhd_usrp_get_time_now"));
        rxStreamLastError = reinterpret_cast<decltype(rxStreamLastError)>(library.resolve("uhd_rx_streamer_last_error"));
        metadataHasTime = reinterpret_cast<decltype(metadataHasTime)>(library.resolve("uhd_rx_metadata_has_time_spec"));
        metadataTime = reinterpret_cast<decltype(metadataTime)>(library.resolve("uhd_rx_metadata_time_spec"));
        setClockSource = reinterpret_cast<decltype(setClockSource)>(library.resolve("uhd_usrp_set_clock_source"));
        getClockSource = reinterpret_cast<decltype(getClockSource)>(library.resolve("uhd_usrp_get_clock_source"));
        sensorMake = reinterpret_cast<decltype(sensorMake)>(library.resolve("uhd_sensor_value_make"));
        sensorFree = reinterpret_cast<decltype(sensorFree)>(library.resolve("uhd_sensor_value_free"));
        getMboardSensor = reinterpret_cast<decltype(getMboardSensor)>(library.resolve("uhd_usrp_get_mboard_sensor"));
        getMboardSensorNames = reinterpret_cast<decltype(getMboardSensorNames)>(library.resolve("uhd_usrp_get_mboard_sensor_names"));
        sensorToBool = reinterpret_cast<decltype(sensorToBool)>(library.resolve("uhd_sensor_value_to_bool"));
        sensorValue = reinterpret_cast<decltype(sensorValue)>(library.resolve("uhd_sensor_value_value"));
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

constexpr int MaxGsvPayloadBytes = 16384;
constexpr int MaxUbxHexBytes = 16416;

QByteArray readNmeaSensor(void *device, const char *name, int capacity) {
    if (!name) return {};
    Api &a = api();
    void *sensor = nullptr;
    QByteArray bytes(capacity, '\0');
    const bool read = a.sensorMake(&sensor) == 0 && sensor &&
        a.getMboardSensor(device, name, 0, &sensor) == 0 && sensor &&
        a.sensorValue(sensor, bytes.data(), static_cast<std::size_t>(bytes.size())) == 0;
    if (sensor) a.sensorFree(&sensor);
    const int end = bytes.indexOf('\0');
    if (!read || end < 0) return {};
    bytes.truncate(end);
    return bytes;
}

bool validNmeaSentence(const QByteArray &sentence, const char *kind) {
    if (sentence.size() < 10 || sentence[0] != '$' || sentence.mid(3, 3) != kind ||
        sentence[6] != ',' || !std::all_of(sentence.begin(), sentence.end(),
            [](char c) { return c >= ' ' && c <= '~'; })) return false;
    const auto checksum = sentence.indexOf('*');
    if (checksum < 7 || checksum + 3 != sentence.size() ||
        !std::isxdigit(static_cast<unsigned char>(sentence[checksum + 1])) ||
        !std::isxdigit(static_cast<unsigned char>(sentence[checksum + 2]))) return false;
    unsigned int sum = 0;
    for (int i = 1; i < checksum; ++i) sum ^= static_cast<unsigned char>(sentence[i]);
    return sum == sentence.mid(checksum + 1).toUInt(nullptr, 16);
}

QString nmeaSensorValue(void *device, const char *name, const char *kind) {
    const auto sentence = readNmeaSensor(device, name, 256).trimmed();
    return validNmeaSentence(sentence, kind) ? QString::fromLatin1(sentence) : QString();
}

QString gsvSensorValue(void *device, const char *name) {
    QStringList sentences;
    const auto payload = readNmeaSensor(device, name, MaxGsvPayloadBytes + 1);
    for (const auto &line : payload.split('\n')) {
        const auto sentence = line.trimmed();
        if (validNmeaSentence(sentence, "GSV")) sentences << QString::fromLatin1(sentence);
    }
    return sentences.join(QLatin1Char('\n'));
}

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

QString rxStreamError(void *streamer, const QString &fallback) {
    char message[512] = {};
    auto error = api().rxStreamLastError;
    if (streamer && error && error(streamer, message, sizeof(message) - 1) == 0 && message[0])
        return QString::fromUtf8(message);
    return fallback;
}

} // namespace

QString validatedUhdGnssUbx(const QByteArray &payload) {
    if (payload.size() > MaxUbxHexBytes) return {};
    QStringList frames;
    for (const QByteArray &line : payload.split('\n')) {
        if (line.size() < 16 || line.size() > 8208 || line.size() % 2 ||
            !std::all_of(line.cbegin(), line.cend(), [](unsigned char c) { return std::isxdigit(c); }))
            continue;
        const auto frame = QByteArray::fromHex(line);
        const auto octet = [&frame](int i) { return static_cast<unsigned char>(frame.at(i)); };
        if (octet(0) != 0xb5 || octet(1) != 0x62 || (octet(4) | octet(5) << 8) != frame.size() - 8)
            continue;
        unsigned char a = 0, b = 0;
        for (int i = 2; i < frame.size() - 2; ++i) {
            a = static_cast<unsigned char>(a + octet(i));
            b = static_cast<unsigned char>(b + a);
        }
        if (a == octet(frame.size() - 2) && b == octet(frame.size() - 1))
            frames.append(QString::fromLatin1(line.toLower()));
    }
    return frames.join(QLatin1Char('\n'));
}

UhdGnssSkySummary summarizeUhdGnssGsv(const QString &gsv) {
    if (gsv.isEmpty() || gsv.size() > MaxGsvPayloadBytes) return {};

    struct Satellite {
        int id;
        int elevation;
        int azimuth;
        int cn0;
    };
    struct GsvCycle {
        QByteArray talker;
        int partCount = 0;
        int satelliteCount = 0;
        QHash<int, QVector<Satellite>> parts;
    };
    const auto integer = [](const QByteArray &text) {
        bool ok = false;
        const int value = text.toInt(&ok);
        return ok ? value : -1;
    };
    QHash<QByteArray, GsvCycle> cycles;
    UhdGnssSkySummary summary;
    for (const auto &line : gsv.toLatin1().split('\n')) {
        const auto sentence = line.trimmed();
        if (sentence.isEmpty()) continue;
        if (!validNmeaSentence(sentence, "GSV")) {
            summary.incomplete = true;
            continue;
        }
        const auto fields = sentence.left(sentence.indexOf('*')).split(',');
        const int satelliteFields = fields.size() - 4;
        if (satelliteFields < 0 || satelliteFields > 17 || satelliteFields % 4 > 1) {
            summary.incomplete = true;
            continue;
        }
        const int partCount = integer(fields.at(1));
        const int part = integer(fields.at(2));
        const int satelliteCount = integer(fields.at(3));
        if (partCount < 1 || partCount > 64 || part < 1 || part > partCount ||
            satelliteCount < 0 || satelliteCount > partCount * 4) {
            summary.incomplete = true;
            continue;
        }

        QByteArray talker = fields.first().mid(1, 2);
        if (talker == "BD") talker = "GB";
        if (talker == "QZ") talker = "GQ";
        const auto signalId = satelliteFields % 4 == 1 ? fields.last() : QByteArray();
        auto &cycle = cycles[talker + ':' + signalId];
        if (cycle.partCount != partCount || cycle.satelliteCount != satelliteCount) {
            cycle = {talker, partCount, satelliteCount, {}};
        }
        QVector<Satellite> satellites;
        for (int i = 4; i + 3 < fields.size(); i += 4) {
            const int id = integer(fields.at(i));
            if (id > 0) {
                satellites.push_back({id, integer(fields.at(i + 1)),
                                      integer(fields.at(i + 2)), integer(fields.at(i + 3))});
            }
        }
        cycle.parts.insert(part, satellites);
    }

    QHash<QByteArray, UhdGnssSatellite> specificSatellites;
    QHash<QByteArray, UhdGnssSatellite> mixedSatellites;
    bool haveSpecificCycle = false;
    bool haveMixedCycle = false;
    for (const auto &cycle : cycles) {
        int listedSatellites = 0;
        for (const auto &part : cycle.parts) listedSatellites += part.size();
        if (cycle.parts.size() != cycle.partCount || listedSatellites != cycle.satelliteCount) {
            summary.incomplete = true;
            continue;
        }
        const bool mixed = cycle.talker == "GN";
        if (mixed) haveMixedCycle = true;
        else haveSpecificCycle = true;
        auto &satellites = mixed ? mixedSatellites : specificSatellites;
        const QString talker = QString::fromLatin1(cycle.talker);
        for (const auto &part : cycle.parts) {
            for (const auto &satellite : part) {
                const auto key = cycle.talker + ':' + QByteArray::number(satellite.id);
                auto &entry = satellites[key];
                entry.talker = talker;
                entry.id = satellite.id;
                if (satellite.elevation >= 0 && satellite.elevation <= 90) entry.elevationDeg = satellite.elevation;
                if (satellite.azimuth >= 0 && satellite.azimuth < 360) entry.azimuthDeg = satellite.azimuth;
                if (satellite.cn0 > 0 && satellite.cn0 <= 99) {
                    entry.cn0DbHz = (std::max)(entry.cn0DbHz, satellite.cn0);
                }
            }
        }
    }
    // The mixed talker can repeat constellations reported under their own IDs.
    if (!haveSpecificCycle && !haveMixedCycle) return summary;
    const auto &satellites = haveSpecificCycle ? specificSatellites : mixedSatellites;
    summary.satellitesInView = satellites.size();
    summary.satellites.reserve(satellites.size());
    for (const auto &satellite : satellites) {
        summary.satellites.push_back(satellite);
        if (satellite.cn0DbHz > 0) ++summary.satellitesWithSignal;
        summary.strongestCn0DbHz = (std::max)(summary.strongestCn0DbHz, satellite.cn0DbHz);
    }
    return summary;
}

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

bool uhdClockControlAvailable() {
    Api &a = api();
    return a.load() && a.setClockSource && a.getClockSource;
}

bool UhdRxSession::open(int deviceIndex, int channel, double sampleRateHz, double centerHz,
                        double gainDb, const QString &antenna, const QString &fpgaImagePath,
                        QString *errorMessage, const QString &clockSource, int gnssBaud) {
    return open(deviceIndex, {{channel, gainDb, antenna}}, sampleRateHz, centerHz,
                fpgaImagePath, errorMessage, -1, clockSource, gnssBaud);
}

bool UhdRxSession::open(int deviceIndex, const std::vector<UhdRxChannelSettings> &channels,
                        double sampleRateHz, double centerHz, const QString &fpgaImagePath,
                        QString *errorMessage, int duplexTxChannel, const QString &clockSource,
                        int gnssBaud) {
    close();
    if (errorMessage) errorMessage->clear();
    const QStringList devices = enumerateUhdDeviceArgs(errorMessage);
    if (deviceIndex < 0 || deviceIndex >= devices.size()) {
        if (errorMessage && errorMessage->isEmpty())
            *errorMessage = QStringLiteral("No UHD device at index %1").arg(deviceIndex);
        return false;
    }
    return open(devices.at(deviceIndex), channels, sampleRateHz, centerHz, fpgaImagePath,
                errorMessage, duplexTxChannel, clockSource, gnssBaud);
}

bool UhdRxSession::open(const QString &deviceAddress, int channel, double sampleRateHz,
                        double centerHz, double gainDb, const QString &antenna,
                        const QString &fpgaImagePath, QString *errorMessage, const QString &clockSource,
                        int gnssBaud) {
    return open(deviceAddress, {{channel, gainDb, antenna}}, sampleRateHz, centerHz,
                fpgaImagePath, errorMessage, -1, clockSource, gnssBaud);
}

bool UhdRxSession::open(const QString &deviceAddress,
                        const std::vector<UhdRxChannelSettings> &channels, double sampleRateHz,
                        double centerHz, const QString &fpgaImagePath, QString *errorMessage,
                        int duplexTxChannel, const QString &clockSource, int gnssBaud) {
    close();
    if (errorMessage) errorMessage->clear();
    if (channels.empty() || channels.size() > channels_.size() ||
        (channels.size() == 2 && channels[0].channel == channels[1].channel) ||
        !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0 ||
        !std::isfinite(centerHz) || centerHz <= 0.0 ||
        std::any_of(channels.begin(), channels.end(), [](const UhdRxChannelSettings &channel) {
            return channel.channel < 0 || !std::isfinite(channel.gainDb);
        })) {
        if (errorMessage) *errorMessage = QStringLiteral("Invalid UHD RX channel, rate, center, or gain");
        return false;
    }
    Api &a = api();
    if (!a.load() || deviceAddress.isEmpty()) {
        if (errorMessage)
            *errorMessage = deviceAddress.isEmpty() ? QStringLiteral("No UHD device selected") : a.error;
        return false;
    }
    QString deviceArgs = deviceAddress;
    if (gnssBaud != 0) {
        if ((gnssBaud != 9600 && gnssBaud != 38400) ||
            !deviceAddress.split(QLatin1Char(',')).contains(QStringLiteral("type=b200")) ||
            deviceAddress.contains(QStringLiteral("b210_gnss_baud="))) {
            if (errorMessage) *errorMessage = QStringLiteral("B2xx GNSS baud must be 9600 or 38400");
            return false;
        }
        deviceArgs += QStringLiteral(",b210_gnss_baud=%1").arg(gnssBaud);
    }
    if (duplexTxChannel >= 0) {
        const auto fields = deviceAddress.split(QLatin1Char(','));
        const double decimation = DuplexClockHz / sampleRateHz;
        if (!fields.contains(QStringLiteral("type=b200")) || duplexTxChannel > 1 ||
            decimation < 1.0 || std::abs(decimation - std::round(decimation)) > 1e-6 ||
            deviceAddress.contains(QStringLiteral("master_clock_rate="))) {
            if (errorMessage)
                *errorMessage = QStringLiteral("Shared TX requires B2xx and an RX rate that divides 30.72 MS/s");
            return false;
        }
        if ((channels.size() == 1 && channels[0].channel != duplexTxChannel) ||
            std::any_of(channels.begin(), channels.end(), [](const UhdRxChannelSettings &channel) {
                const auto antenna = channel.antenna.trimmed();
                return !antenna.isEmpty() && antenna != QLatin1String("RX2");
            })) {
            if (errorMessage)
                *errorMessage = QStringLiteral("Shared UHD TX requires RX2 and matching RX/TX channels in 1+1 mode");
            return false;
        }
        if (!a.loadTx()) {
            if (errorMessage) *errorMessage = a.txError;
            return false;
        }
        deviceArgs += QStringLiteral(",master_clock_rate=30720000");
    }
    QString receiveFrames = qEnvironmentVariable("OBRII_UHD_NUM_RECV_FRAMES");
#if defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM)
    // Give B2xx USB reads room for short DSP and compositor scheduling delays.
    if (receiveFrames.isNull() && deviceAddress.split(QLatin1Char(',')).contains(QStringLiteral("type=b200"))) {
        receiveFrames = QStringLiteral("256");
    }
#endif
    if (!receiveFrames.isNull()) {
        const auto addressFields = deviceAddress.split(QLatin1Char(','));
        const auto hasReceiveFrames = std::any_of(addressFields.begin(), addressFields.end(),
            [](const QString &field) {
                return field.section(QLatin1Char('='), 0, 0).trimmed() == QStringLiteral("num_recv_frames");
            });
        if (!hasReceiveFrames) {
            bool valid = false;
            const int frames = receiveFrames.toInt(&valid);
            if (!valid || frames < 1 || frames > 1024) {
                if (errorMessage) *errorMessage = QStringLiteral("OBRII_UHD_NUM_RECV_FRAMES must be in 1..1024");
                return false;
            }
            deviceArgs += QStringLiteral(",num_recv_frames=%1").arg(frames);
        }
    }
    if (!fpgaImagePath.trimmed().isEmpty()) {
        const QFileInfo image(fpgaImagePath.trimmed());
        if (!image.isFile() || image.absoluteFilePath().contains(QLatin1Char(','))) {
            if (errorMessage) *errorMessage = QStringLiteral("UHD FPGA image is missing or its path contains a comma");
            return false;
        }
        if (!deviceArgs.isEmpty()) deviceArgs += QLatin1Char(',');
        deviceArgs += QStringLiteral("fpga=") + QDir::fromNativeSeparators(image.absoluteFilePath());
    }
    {
        QMutexLocker lock(&rxDevicesMutex);
        if (openRxDevices.contains(deviceAddress)) {
            if (errorMessage) *errorMessage = QStringLiteral("This USRP is already used by another RX");
            return false;
        }
        openRxDevices.insert(deviceAddress);
        deviceAddress_ = deviceAddress;
    }
    const QByteArray args = deviceArgs.toUtf8();
    if (a.make(&device_, args.constData()) != 0 || !device_) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD device open failed"));
        close();
        return false;
    }
    if (!clockSource.isEmpty() && clockSource != QLatin1String("auto")) {
        const auto source = clockSource.toLatin1();
        char actual[64]{};
        if ((clockSource != QLatin1String("internal") && clockSource != QLatin1String("external")) ||
            !a.setClockSource || !a.getClockSource ||
            a.setClockSource(device_, source.constData(), 0) != 0 ||
            a.getClockSource(device_, 0, actual, sizeof(actual)) != 0 ||
            QString::fromLatin1(actual) != clockSource) {
            if (errorMessage)
                *errorMessage = deviceError(device_, QStringLiteral("UHD could not select the %1 clock source").arg(clockSource));
            close();
            return false;
        }
    }
    std::size_t availableChannels = 0;
    if (a.channelCount(device_, &availableChannels) != 0) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD RX channel count unavailable"));
        close();
        return false;
    }
    for (const auto &configuration : channels) {
        const auto channel = static_cast<std::size_t>(configuration.channel);
        if (channel >= availableChannels ||
            a.setRate(device_, sampleRateHz, channel) != 0) {
            if (errorMessage)
                *errorMessage = deviceError(device_, QStringLiteral("UHD RX %1 channel/rate rejected").arg(channel));
            close();
            return false;
        }
        const QByteArray antennaName = duplexTxChannel >= 0 ? QByteArray("RX2")
                                                           : configuration.antenna.trimmed().toUtf8();
        if (a.setGain(device_, configuration.gainDb, channel, "") != 0 ||
            (!antennaName.isEmpty() && a.setAntenna(device_, antennaName.constData(), channel) != 0)) {
            if (errorMessage)
                *errorMessage = deviceError(device_, QStringLiteral("UHD RX %1 gain/antenna rejected").arg(channel));
            close();
            return false;
        }
        channels_[channelCount_++] = channel;
    }
    if (duplexTxChannel >= 0) {
        sharedTx_.device_ = device_;
        if (!sharedTx_.setRate(duplexTxChannel, DuplexTxRateHz, errorMessage, channelCount_ == 2)) {
            close();
            return false;
        }
        for (std::size_t i = 0; i < sharedTx_.channelCount_; ++i) {
            if (a.txSetGain(device_, 0.0, sharedTx_.channels_[i], "") != 0) {
                if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX idle gain rejected"));
                close();
                return false;
            }
        }
    }
    if (!retune(centerHz, errorMessage) || a.streamMake(&streamer_) != 0 || !streamer_ ||
        a.metadataMake(&metadata_) != 0 || !metadata_) {
        if (errorMessage && errorMessage->isEmpty()) *errorMessage = deviceError(device_, QStringLiteral("UHD RX setup failed"));
        close();
        return false;
    }
    char cpu[] = "fc32";
    char wire[] = "sc16";
    char empty[] = "";
    StreamArgs streamArgs{cpu, wire, empty, channels_.data(), static_cast<int>(channelCount_)};
    if (a.getStream(device_, &streamArgs, streamer_) != 0) {
        if (errorMessage) *errorMessage = rxStreamError(streamer_, QStringLiteral("UHD RX stream creation failed"));
        close();
        return false;
    }
    if (duplexTxChannel >= 0) {
        // B210 full duplex requires equal RX and TX channel counts.
        // Set up the entire TX group before starting RX.
        if (!sharedTx_.openStreamer(errorMessage)) {
            close();
            return false;
        }
        duplexTxChannel_ = duplexTxChannel;
        std::lock_guard<std::mutex> lock(transmitStatusMutex_);
        transmitStatus_ = {};
    }
    // B2x0 can reselect its master clock when the second RX channel is enabled.
    double commonRate = 0.0;
    for (std::size_t i = 0; i < channelCount_; ++i) {
        const auto channel = channels_[i];
        double actualRate = 0.0;
        if (a.getRate(device_, channel, &actualRate) != 0 || !std::isfinite(actualRate) ||
            std::abs(actualRate - sampleRateHz) > sampleRateHz * 0.001 ||
            (i > 0 && std::abs(actualRate - commonRate) > 1.0e-6)) {
            if (errorMessage)
                *errorMessage = QStringLiteral("UHD RX %1 returned %2 S/s. Select a common supported rate.")
                    .arg(channel).arg(actualRate);
            close();
            return false;
        }
        commonRate = actualRate;
    }
    StreamCommand start{97, 0, true, 0, 0.0};
    if (channelCount_ > 1) {
        if (!a.getTimeNow || a.getTimeNow(device_, 0, &start.seconds, &start.fractionalSeconds) != 0 ||
            !std::isfinite(start.fractionalSeconds)) {
            if (errorMessage) *errorMessage = QStringLiteral("UHD RX clock unavailable for synchronized start");
            close();
            return false;
        }
        start.now = false;
        start.fractionalSeconds += 0.050;
        if (start.fractionalSeconds >= 1.0) {
            ++start.seconds;
            start.fractionalSeconds -= 1.0;
        }
    }
    streamStarted_ = true;
    if (a.streamCommand(streamer_, &start) != 0) {
        if (errorMessage) *errorMessage = rxStreamError(streamer_, QStringLiteral("UHD RX stream start failed"));
        close();
        return false;
    }
    receiveSampleRateHz_ = commonRate;
    return true;
}

UhdClockStatus UhdRxSession::clockStatus() const {
    UhdClockStatus status;
    if (!device_) return status;
    const std::unique_lock<std::mutex> lock(deviceControlMutex_, std::try_to_lock);
    if (!lock.owns_lock()) return lastClockStatus_;
    Api &a = api();
    char source[64]{};
    if (a.getClockSource && a.getClockSource(device_, 0, source, sizeof(source)) == 0)
        status.source = QString::fromLatin1(source);
    if (a.sensorMake && a.sensorFree && a.getMboardSensor && a.sensorToBool) {
        void *sensor = nullptr;
        if (a.sensorMake(&sensor) == 0 && sensor) {
            bool locked = false;
            if (a.getMboardSensor(device_, "ref_locked", 0, &sensor) == 0 &&
                sensor && a.sensorToBool(sensor, &locked) == 0)
                status.locked = locked;
        }
        if (sensor) a.sensorFree(&sensor);
    }
    lastClockStatus_ = status;
    return status;
}

void UhdRxSession::requestGnssStatus() {
    if (!device_ || (sharedTx_.streamer_ && transmitStatus().active)) return;
    const auto now = std::chrono::steady_clock::now();
    const auto sincePoll = now - gnssLastPoll_;
    if (gnssJob_.valid()) {
        if (gnssJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            if (sincePoll > std::chrono::seconds(5)) gnssStatus_ = {QStringLiteral("waiting"), {}, {}};
            return;
        }
        try {
            const auto status = gnssJob_.get();
            gnssStatus_ = sincePoll <= std::chrono::seconds(5) || status.state == QLatin1String("unsupported")
                ? status : UhdGnssStatus{QStringLiteral("waiting"), {}, {}};
        } catch (...) {
            gnssStatus_ = {QStringLiteral("waiting"), {}, {}};
        }
    }
    if (gnssStatus_.state == QLatin1String("unsupported") || sincePoll < std::chrono::seconds(2)) return;
    gnssLastPoll_ = now;
    if (gnssStatus_.state == QLatin1String("idle")) gnssStatus_.state = QStringLiteral("polling");
    try {
        gnssJob_ = std::async(std::launch::async, [this] { return readGnssSensors(); });
    } catch (...) {
        gnssStatus_ = {QStringLiteral("waiting"), {}, {}};
    }
}

UhdGnssStatus UhdRxSession::readGnssSensors() {
    const std::lock_guard<std::mutex> lock(deviceControlMutex_);
    Api &a = api();
    if (!a.sensorMake || !a.sensorFree || !a.getMboardSensorNames || !a.getMboardSensor || !a.sensorValue)
        return {QStringLiteral("unsupported"), {}, {}};
    if (!gnssSensorsDiscovered_) {
        gnssSensorsDiscovered_ = true;
        void *names = nullptr;
        std::size_t count = 0;
        QSet<QByteArray> available;
        if (a.vectorMake(&names) == 0 && names &&
            a.getMboardSensorNames(device_, 0, &names) == 0 && names &&
            a.vectorSize(names, &count) == 0) {
            for (std::size_t i = 0; i < count; ++i) {
                char name[128]{};
                if (a.vectorAt(names, i, name, sizeof(name) - 1) == 0)
                    available.insert(QByteArray(name));
            }
        }
        if (names) a.vectorFree(&names);
        const auto pick = [&available](const char *trial, const char *stock) {
            if (available.contains(trial)) return trial;
            return available.contains(stock) ? stock : nullptr;
        };
        gnssGgaSensor_ = pick("gnss_gga", "gps_gpgga");
        gnssRmcSensor_ = pick("gnss_rmc", "gps_gprmc");
        if (available.contains("gnss_gsv")) gnssGsvSensor_ = "gnss_gsv";
        if (available.contains("gnss_ubx")) gnssUbxSensor_ = "gnss_ubx";
    }
    if (!gnssGgaSensor_ && !gnssRmcSensor_ && !gnssGsvSensor_ && !gnssUbxSensor_)
        return {QStringLiteral("unsupported"), {}, {}};
    UhdGnssStatus status{QStringLiteral("waiting"), nmeaSensorValue(device_, gnssGgaSensor_, "GGA"),
                         nmeaSensorValue(device_, gnssRmcSensor_, "RMC"),
                         gsvSensorValue(device_, gnssGsvSensor_),
                         validatedUhdGnssUbx(readNmeaSensor(device_, gnssUbxSensor_, MaxUbxHexBytes + 1))};
    if (!status.gga.isEmpty() || !status.rmc.isEmpty() || !status.gsv.isEmpty() || !status.ubxHex.isEmpty())
        status.state = QStringLiteral("available");
    return status;
}

bool UhdRxSession::retune(double centerHz, QString *errorMessage) {
    if (sharedTx_.streamer_ && transmitStatus().active) {
        if (errorMessage) *errorMessage = QStringLiteral("UHD TX burst is active");
        return false;
    }
    if (!device_ || channelCount_ == 0 || !std::isfinite(centerHz) || centerHz <= 0.0) {
        if (errorMessage) *errorMessage = QStringLiteral("Invalid UHD RX retune");
        return false;
    }
    std::unique_lock<std::mutex> lock(deviceControlMutex_);
    tuneCutoffPending_ = false;
    const auto tune = [this](double frequency, std::size_t channel) {
        TuneRequest request{frequency, 65, 0.0, 65, 0.0, nullptr};
        TuneResult result{};
        return api().setFrequency(device_, &request, channel, &result) == 0;
    };
    for (std::size_t i = 0; i < channelCount_; ++i) {
        if (tune(centerHz, channels_[i])) continue;
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD RX retune failed"));
        if (channelCount_ > 1 && streamStarted_) {
            bool restored = true;
            for (std::size_t j = 0; j < channelCount_; ++j)
                restored = tune(centerHz_, channels_[j]) && restored;
            lock.unlock();
            if (!restored) {
                if (errorMessage) *errorMessage += QStringLiteral(". Restoring the shared RX center failed.");
                close();
            } else {
                beginTimestampedTuneSettling();
            }
        }
        return false;
    }
    centerHz_ = centerHz;
    return true;
}

void UhdRxSession::beginTimestampedTuneSettling() {
    const std::lock_guard<std::mutex> lock(deviceControlMutex_);
    auto &a = api();
    tuneCutoffPending_ = streamer_ && a.getTimeNow && a.metadataHasTime && a.metadataTime &&
        a.getTimeNow(device_, 0, &tuneCutoffSeconds_, &tuneCutoffFraction_) == 0 &&
        std::isfinite(tuneCutoffFraction_);
    // Allow nearby WFM retunes to settle after the device acknowledges the tune.
    tuneCutoffFraction_ += 0.008;
}

std::optional<std::size_t> UhdRxSession::samplesBeforeTuneCutoff(std::size_t received, double sampleRateHz) {
    if (!tuneCutoffPending_) return std::nullopt;
    const auto timestamp = lastReceiveTimestamp();
    if (!timestamp) {
        tuneCutoffPending_ = false;
        return std::nullopt;
    }
    const double delta = double(tuneCutoffSeconds_ - timestamp->seconds) +
                         (tuneCutoffFraction_ - timestamp->fractionalSeconds);
    if (std::abs(delta) > 1.0) {
        tuneCutoffPending_ = false;
        return std::nullopt;
    }
    const auto skip = static_cast<std::size_t>((std::clamp)(std::ceil(delta * sampleRateHz), 0.0, double(received)));
    tuneCutoffPending_ = skip == received;
    return skip;
}

int UhdRxSession::receive(float *interleavedIq, std::size_t capacity, QString *errorMessage) {
    void *buffers[] = {interleavedIq};
    return receiveBuffers(buffers, 1, capacity, errorMessage);
}

int UhdRxSession::receive(float *firstInterleavedIq, float *secondInterleavedIq,
                           std::size_t capacityPerChannel, QString *errorMessage) {
    void *buffers[] = {firstInterleavedIq, secondInterleavedIq};
    return receiveBuffers(buffers, 2, capacityPerChannel, errorMessage);
}

int UhdRxSession::receiveBuffers(void **buffers, std::size_t bufferCount, std::size_t capacity,
                                 QString *errorMessage) {
    lastReceiveHasSamples_ = false;
    if (!streamStarted_ || bufferCount != channelCount_ || !capacity ||
        capacity > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        (bufferCount == 2 && buffers[0] == buffers[1]) ||
        std::any_of(buffers, buffers + bufferCount, [](void *buffer) { return !buffer; })) {
        if (errorMessage) *errorMessage = QStringLiteral("Invalid UHD RX buffers or stream is closed");
        return -1;
    }
    std::size_t received = 0;
    if (api().receive(streamer_, buffers, capacity, &metadata_, 0.2, false, &received) != 0) {
        if (errorMessage) *errorMessage = rxStreamError(streamer_, QStringLiteral("UHD RX receive call failed"));
        return -1;
    }
    int code = 0;
    if (api().metadataError(metadata_, &code) != 0 || received > capacity) {
        if (errorMessage) *errorMessage = QStringLiteral("Invalid UHD RX receive metadata or sample count");
        return -1;
    }
    if (code == 0) {
        lastReceiveHasSamples_ = received != 0;
        if (bufferCount == 2 && received != 0) accountReceiveTimestamp(received);
        return static_cast<int>(received);
    }
    if (code == 1) {
        receiveTimeouts_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (code == 8) {
        receiveOverflows_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (errorMessage) *errorMessage = QStringLiteral("UHD RX metadata error %1").arg(code);
    return -1;
}

std::optional<UhdRxTimestamp> UhdRxSession::lastReceiveTimestamp() const {
    if (!lastReceiveHasSamples_) return std::nullopt;
    auto &a = api();
    UhdRxTimestamp timestamp;
    bool hasTime = false;
    if (!a.metadataHasTime || !a.metadataTime ||
        a.metadataHasTime(metadata_, &hasTime) != 0 || !hasTime ||
        a.metadataTime(metadata_, &timestamp.seconds, &timestamp.fractionalSeconds) != 0 ||
        !std::isfinite(timestamp.fractionalSeconds))
        return std::nullopt;
    return timestamp;
}

void UhdRxSession::accountReceiveTimestamp(std::size_t received) {
    const auto timestamp = lastReceiveTimestamp();
    if (!timestamp) {
        receiveTimestampMissingBlocks_.fetch_add(1, std::memory_order_relaxed);
    } else if (previousReceiveTimestamp_) {
        const double elapsed = double(timestamp->seconds) - double(previousReceiveTimestamp_->seconds) +
                               (timestamp->fractionalSeconds - previousReceiveTimestamp_->fractionalSeconds);
        const double gap = elapsed * receiveSampleRateHz_ - double(previousReceiveSamples_);
        // Ignore one sample of rounding difference between UHD and the host.
        if (gap >= 1.5 && gap < double((std::numeric_limits<std::int64_t>::max)())) {
            receiveTimestampGaps_.fetch_add(1, std::memory_order_relaxed);
            receiveMissingSamples_.fetch_add(static_cast<std::uint64_t>(std::llround(gap)),
                                             std::memory_order_relaxed);
        }
    }
    previousReceiveTimestamp_ = timestamp;
    previousReceiveSamples_ = received;
}

void UhdRxSession::close() {
    stopTransmit();
    if (gnssJob_.valid()) gnssJob_.wait();
    gnssJob_ = {};
    gnssLastPoll_ = {};
    gnssStatus_ = {};
    gnssSensorsDiscovered_ = false;
    gnssGgaSensor_ = gnssRmcSensor_ = gnssGsvSensor_ = nullptr;
    gnssUbxSensor_ = nullptr;
    lastClockStatus_ = {};
    tuneCutoffPending_ = false;
    Api &a = api();
    if (streamStarted_) {
        const StreamCommand stop{111, 0, true, 0, 0.0};
        a.streamCommand(streamer_, &stop);
    }
    sharedTx_.close();
    duplexTxChannel_ = -1;
    streamStarted_ = false;
    lastReceiveHasSamples_ = false;
    channelCount_ = 0;
    centerHz_ = 0.0;
    receiveSampleRateHz_ = 0.0;
    previousReceiveTimestamp_.reset();
    previousReceiveSamples_ = 0;
    if (streamer_) a.streamFree(&streamer_);
    if (metadata_) a.metadataFree(&metadata_);
    if (device_) a.freeDevice(&device_);
    if (!deviceAddress_.isEmpty()) {
        QMutexLocker lock(&rxDevicesMutex);
        openRxDevices.remove(deviceAddress_);
        deviceAddress_.clear();
    }
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
    ownsDevice_ = true;
    if (a.make(&device_, args.constData()) != 0 || !device_) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX device open failed"));
        close();
        return false;
    }
    if (setRate(channel, sampleRateHz, errorMessage) &&
        tune(channels_[0], centerHz, gainDb, bandwidthHz, antenna, errorMessage) &&
        openStreamer(errorMessage))
        return true;
    close();
    return false;
}

bool UhdTxSession::setRate(int channel, double sampleRateHz, QString *errorMessage, bool dual) {
    Api &a = api();
    std::size_t channels = 0;
    if (a.txChannelCount(device_, &channels) != 0 || channel < 0 ||
        static_cast<std::size_t>(channel) >= channels || (dual && channels < 2)) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX channel unavailable"));
        return false;
    }
    channels_ = {dual ? 0 : static_cast<std::size_t>(channel), 1};
    channelCount_ = dual ? 2 : 1;
    for (std::size_t i = 0; i < channelCount_; ++i) {
        const auto selected = channels_[i];
        if (a.txSetRate(device_, sampleRateHz, selected) != 0) {
            if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX rate rejected"));
            return false;
        }
        double actualRate = 0.0;
        if (a.txGetRate(device_, selected, &actualRate) != 0 || !std::isfinite(actualRate) ||
            std::abs(actualRate - sampleRateHz) > sampleRateHz * 0.001) {
            if (errorMessage)
                *errorMessage = QStringLiteral("UHD TX %1 returned %2 S/s instead of %3 S/s")
                    .arg(selected).arg(actualRate).arg(sampleRateHz);
            return false;
        }
    }
    return true;
}

bool UhdTxSession::tune(std::size_t channel, double centerHz, double gainDb, double bandwidthHz,
                        const QString &antenna, QString *errorMessage) {
    Api &a = api();
    TuneRequest tune{centerHz, 65, 0.0, 65, 0.0, nullptr};
    TuneResult tuned{};
    double actualFrequency = 0.0;
    const QByteArray antennaName = antenna.trimmed().toUtf8();
    if (a.txSetFrequency(device_, &tune, channel, &tuned) != 0 ||
        a.txGetFrequency(device_, channel, &actualFrequency) != 0 ||
        std::abs(actualFrequency - centerHz) > (std::max)(100.0, centerHz * 1e-6) ||
        a.txSetGain(device_, gainDb, channel, "") != 0 ||
        (bandwidthHz > 0.0 && a.txSetBandwidth(device_, bandwidthHz, channel) != 0) ||
        (!antennaName.isEmpty() && a.txSetAntenna(device_, antennaName.constData(), channel) != 0)) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX frequency/gain/bandwidth/antenna rejected"));
        return false;
    }
    return true;
}

bool UhdTxSession::openStreamer(QString *errorMessage) {
    Api &a = api();
    if (a.txStreamMake(&streamer_) != 0 || !streamer_) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX streamer allocation failed"));
        return false;
    }
    char cpu[] = "fc32";
    char wire[] = "sc16";
    char empty[] = "";
    StreamArgs streamArgs{cpu, wire, empty, channels_.data(), static_cast<int>(channelCount_)};
    if (a.txGetStream(device_, &streamArgs, streamer_) != 0) {
        if (errorMessage) *errorMessage = deviceError(device_, QStringLiteral("UHD TX stream creation failed"));
        return false;
    }
    return true;
}

bool UhdTxSession::send(const std::complex<float> *iq, std::size_t count, bool startOfBurst,
                        QString *errorMessage, double timeoutSeconds) {
    return sendBuffers({iq, nullptr}, count, startOfBurst, errorMessage, timeoutSeconds);
}

bool UhdTxSession::sendBuffers(const std::array<const std::complex<float> *, 2> &iq, std::size_t count,
                              bool startOfBurst, QString *errorMessage, double timeoutSeconds) {
    if (!streamer_ || !iq[0] || (channelCount_ == 2 && !iq[1]) || !count) return false;
    Api &a = api();
    void *metadata = nullptr;
    if (a.txMetadataMake(&metadata, false, 0, 0.0, startOfBurst, false) != 0 || !metadata) {
        if (errorMessage) *errorMessage = QStringLiteral("UHD TX metadata allocation failed");
        return false;
    }
    std::size_t offset = 0;
    bool okay = true;
    while (offset < count) {
        const void *buffers[] = {iq[0] + offset, iq[1] ? iq[1] + offset : nullptr};
        std::size_t sent = 0;
        if (a.txSend(streamer_, buffers, count - offset, &metadata, timeoutSeconds, &sent) != 0 ||
            sent == 0 || sent > count - offset) {
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
    const void *buffers[] = {nullptr, nullptr};
    std::size_t sent = 0;
    const bool okay = a.txSend(streamer_, buffers, 0, &metadata, 1.0, &sent) == 0;
    a.txMetadataFree(&metadata);
    if (!okay && errorMessage) *errorMessage = txStreamError(streamer_, QStringLiteral("UHD TX end-of-burst failed"));
    return okay;
}

void UhdTxSession::close() {
    Api &a = api();
    if (streamer_) a.txStreamFree(&streamer_);
    if (device_ && ownsDevice_) a.freeDevice(&device_);
    streamer_ = nullptr;
    device_ = nullptr;
    channelCount_ = 0;
    ownsDevice_ = false;
}

UhdDuplexTxStatus UhdRxSession::transmitStatus() const {
    std::lock_guard<std::mutex> lock(transmitStatusMutex_);
    return transmitStatus_;
}

void UhdRxSession::stopTransmit() {
    requestTransmitStop();
    if (transmitThread_.joinable()) transmitThread_.join();
}

bool UhdRxSession::startTransmitTone(double frequencyHz, double gainDb, int durationMs,
                                    QString *errorMessage, int toneChannel) {
    const int target = toneChannel == -1 ? duplexTxChannel_ : toneChannel;
    if (!streamStarted_ || !sharedTx_.streamer_ || transmitStatus().active ||
        target < 0 || target > 2 ||
        (sharedTx_.channelCount_ == 1 && target != duplexTxChannel_) ||
        !std::isfinite(frequencyHz) || frequencyHz < 70e6 || frequencyHz > 6e9 ||
        !std::isfinite(gainDb) || gainDb < 0 || gainDb > 10 ||
        durationMs < 50 || durationMs > 500) {
        if (errorMessage) *errorMessage = QStringLiteral("Invalid or unavailable shared UHD TX burst");
        return false;
    }
    stopTransmit();
    cancelTransmit_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(transmitStatusMutex_);
        transmitStatus_ = {true, 0, QStringLiteral("sending"), {}, target};
    }
    try {
        transmitThread_ = std::thread([this, frequencyHz, gainDb, durationMs, target] {
            constexpr double turn = 6.283185307179586;
            constexpr std::size_t blockSize = 4096;
            constexpr std::size_t rampSamples = 1920;
            const auto total = static_cast<std::size_t>(DuplexTxRateHz * durationMs / 1000.0);
            std::array<std::array<std::complex<float>, blockSize>, 2> samples{};
            const std::array<const std::complex<float> *, 2> buffers{samples[0].data(), samples[1].data()};
            std::array<double, 2> radiansPerSample{};
            std::array<std::complex<float>, 2> step{};
            std::size_t sent = 0;
            QString error;
            for (std::size_t i = 0; i < sharedTx_.channelCount_; ++i) {
                if (cancelTransmit_.load(std::memory_order_relaxed)) break;
                const auto channel = sharedTx_.channels_[i];
                const bool enabled = target == 2 || target == static_cast<int>(channel);
                {
                    const std::lock_guard<std::mutex> lock(deviceControlMutex_);
                    if (!sharedTx_.tune(channel, frequencyHz, enabled ? gainDb : 0.0, 0.0,
                                        QStringLiteral("TX/RX"), &error)) break;
                }
                if (enabled) {
                    const double toneHz = target == 2 && channel == 1 ? 2000.0 : 1000.0;
                    radiansPerSample[i] = turn * toneHz / DuplexTxRateHz;
                    step[i] = std::polar(1.0f, static_cast<float>(radiansPerSample[i]));
                }
            }
            bool attempted = false;
            while (error.isEmpty() && sent < total && !cancelTransmit_.load(std::memory_order_relaxed)) {
                const auto count = (std::min)(blockSize, total - sent);
                for (std::size_t channel = 0; channel < sharedTx_.channelCount_; ++channel) {
                    if (radiansPerSample[channel] == 0.0) continue;
                    auto phase = std::polar(1.0f, static_cast<float>(std::remainder(
                        radiansPerSample[channel] * sent, turn)));
                    for (std::size_t i = 0; i < count; ++i) {
                        const auto edge = (std::min)(sent + i, total - sent - i - 1);
                        const float envelope = static_cast<float>((std::min)(edge, rampSamples)) / rampSamples;
                        samples[channel][i] = phase * (0.025f * envelope);
                        phase *= step[channel];
                    }
                }
                attempted = true;
                if (!sharedTx_.sendBuffers(buffers, count, sent == 0, &error, 0.1)) break;
                sent += count;
            }
            if (attempted) {
                QString finishError;
                if (!sharedTx_.finish(&finishError) && error.isEmpty()) error = finishError;
            }
            std::lock_guard<std::mutex> lock(transmitStatusMutex_);
            transmitStatus_ = {false, sent, error.isEmpty()
                ? (sent == total ? QStringLiteral("completed") : QStringLiteral("stopped"))
                : QStringLiteral("error"), error, target};
        });
    } catch (const std::system_error &) {
        const QString error = QStringLiteral("Could not start UHD TX worker");
        std::lock_guard<std::mutex> lock(transmitStatusMutex_);
        transmitStatus_ = {false, 0, QStringLiteral("error"), error, target};
        if (errorMessage) *errorMessage = error;
        return false;
    }
    return true;
}
