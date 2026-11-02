#include "climoduleprotocol.h"
#include "climodulemanager.h"

#include "audioprocessor.h"
#include "digitaldecoder.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSysInfo>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
bool readExact(char *target, std::size_t bytes) {
    std::size_t offset = 0;
    while (offset < bytes) {
        const std::size_t count = std::fread(target + offset, 1, bytes - offset, stdin);
        if (!count) return false;
        offset += count;
    }
    return true;
}

void send(const QJsonObject &event) {
    const QByteArray line = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
    std::fflush(stdout);
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &) {
        if (type == QtDebugMsg || type == QtInfoMsg) return;
        // Backend diagnostics are not trusted to redact key material.
        std::fprintf(stderr, "Digital decoder diagnostic (type %d)\n", static_cast<int>(type));
    });
    if (QSysInfo::ByteOrder != QSysInfo::LittleEndian || argc < 3 ||
        QByteArray(argv[1]) != "rx" ||
        (QByteArray(argv[2]) != "ft8" && QByteArray(argv[2]) != "ft4" && QByteArray(argv[2]) != "m17" && QByteArray(argv[2]) != "rtty" &&
         QByteArray(argv[2]) != "dmr")) {
        std::fprintf(stderr, "Usage: ObriiCliModule rx ft8|ft4|m17|rtty|dmr [--voice-backend ID]\n");
        return 2;
    }
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) < 0 ||
        _setmode(_fileno(stdout), _O_BINARY) < 0) return 2;
#endif
    const QString mode = QString::fromLatin1(argv[2]);
    if (argc != 3 && (argc != 5 || mode != QStringLiteral("dmr") ||
                     QByteArray(argv[3]) != "--voice-backend")) return 2;
    QJsonObject initial{{QStringLiteral("version"), 1}, {QStringLiteral("module"), mode}};
    if (argc == 5) initial.insert(QStringLiteral("dmrVoiceBackendId"), QString::fromLatin1(argv[4]));
    CliDigitalProfile::Profile activeProfile;
    QString profileError;
    if (!CliDigitalProfile::parse(initial, mode, activeProfile, profileError)) {
        std::fprintf(stderr, "Invalid digital module arguments\n");
        return 2;
    }

    DigitalDecoder decoder;
    AudioProcessor dmrDemodulator;
    dmrDemodulator.setLocalPlaybackEnabled(false);
    RadioSettings settings = activeProfile.radio;
    settings.listeningFrequency = 100000000.0;
    const auto applyProfile = [&] {
        const double frequency = settings.listeningFrequency;
        settings = activeProfile.radio;
        settings.inputMode = INPUT_RF;
        settings.listeningFrequency = frequency;
        decoder.setDmrVoiceBackendId(activeProfile.voiceBackend);
        decoder.setDmrVoiceOutputEnabled(activeProfile.voiceOutput);
        decoder.setDmrVoicePayloadEnabled(activeProfile.voicePayload);
        decoder.setDmrPrivacyKeys(activeProfile.keys);
        decoder.configure(settings);
    };
    applyProfile();
    QJsonObject previousDmrMetadata;
    QObject::connect(&decoder, &DigitalDecoder::textDecoded, &app,
                     [&](const QString &text) {
        if (mode == QStringLiteral("dmr") &&
            (text.contains(QStringLiteral("lock acquired")) ||
             text.contains(QStringLiteral("lock lost")) ||
             text.contains(QStringLiteral("accepted syncs")))) return;
        send({{QStringLiteral("event"), QStringLiteral("text")},
              {QStringLiteral("module"), mode}, {QStringLiteral("text"), text}});
    });
    QObject::connect(&decoder, &DigitalDecoder::ft8Decoded, &app,
                     [&](const QDateTime &utc, double rfHz, double audioHz, double dt,
                         int snrDb, double qualityDb, int costasHits, const QString &message) {
        send({{QStringLiteral("event"), QStringLiteral("ft8")},
              {QStringLiteral("module"), mode}, {QStringLiteral("utc"), utc.toString(Qt::ISODateWithMs)},
              {QStringLiteral("rfFrequencyHz"), rfHz},
              {QStringLiteral("audioFrequencyHz"), audioHz},
              {QStringLiteral("dtSeconds"), dt}, {QStringLiteral("snrDb"), snrDb},
              {QStringLiteral("qualityDb"), qualityDb},
              {QStringLiteral("costasHits"), costasHits},
              {QStringLiteral("message"), message}});
    });
    QObject::connect(&decoder, &DigitalDecoder::statusChanged, &app,
                     [&](const QString &text) {
        send({{QStringLiteral("event"), QStringLiteral("status")},
              {QStringLiteral("module"), mode}, {QStringLiteral("text"), text}});
    });
    QObject::connect(&decoder, &DigitalDecoder::voicePcmReady, &app,
                     [&](const QByteArray &pcm) {
        send({{QStringLiteral("event"), QStringLiteral("voicePcm")},
              {QStringLiteral("module"), mode},
              {QStringLiteral("pcmBase64"), QString::fromLatin1(pcm.toBase64())}});
    });
    QObject::connect(&decoder, &DigitalDecoder::dmrMetadataDetected, &app,
                     [&](int colorCode, int timeslot, quint32 targetId, quint32 sourceId, int flco) {
        const QJsonObject metadata{{QStringLiteral("event"), QStringLiteral("dmrMetadata")},
              {QStringLiteral("module"), mode},
              {QStringLiteral("colorCode"), colorCode},
              {QStringLiteral("timeslot"), timeslot},
              {QStringLiteral("targetId"), static_cast<double>(targetId)},
              {QStringLiteral("sourceId"), static_cast<double>(sourceId)},
              {QStringLiteral("flco"), flco}};
        if (metadata == previousDmrMetadata) return;
        previousDmrMetadata = metadata;
        send(metadata);
    });

    bool discontinuity = true;
    quint32 previousRate = 0;
    std::vector<float> iq;
    std::vector<short> baseband;
    for (;;) {
        uchar header[12];
        if (!readExact(reinterpret_cast<char *>(header), sizeof(header)))
            return std::feof(stdin) ? 0 : 1;
        const quint32 kind = qFromLittleEndian<quint32>(header);
        const quint32 rate = qFromLittleEndian<quint32>(header + 4);
        const quint32 size = qFromLittleEndian<quint32>(header + 8);
        if (size > CliModuleProtocol::MaximumPayloadBytes ||
            (kind == CliModuleProtocol::Configure && size > CliDigitalProfile::MaximumBytes + 1024)) {
            std::fprintf(stderr, "Module frame exceeds payload limit: kind=%u size=%u\n", kind, size);
            return 2;
        }
        QByteArray payload(static_cast<int>(size), Qt::Uninitialized);
        if (size && !readExact(payload.data(), size)) return 1;
        if (kind == CliModuleProtocol::Reset) {
            if (size || rate) return 2;
            decoder.reset();
            applyProfile();
            previousDmrMetadata = {};
            discontinuity = true;
            previousRate = 0;
            continue;
        }
        if (kind == CliModuleProtocol::Configure) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
            const QJsonObject configuration = document.object();
            const QJsonValue frequencyValue = configuration.value(QStringLiteral("listeningFrequencyHz"));
            const double frequency = frequencyValue.toDouble();
            bool valid = !rate && parseError.error == QJsonParseError::NoError && document.isObject() &&
                frequencyValue.isDouble() && std::isfinite(frequency) && frequency > 0.0 && frequency <= 1.0e12;
            for (auto it = configuration.begin(); it != configuration.end(); ++it)
                if (it.key() != QStringLiteral("listeningFrequencyHz") &&
                    it.key() != QStringLiteral("digitalProfile")) valid = false;
            CliDigitalProfile::Profile candidate = activeProfile;
            if (configuration.contains(QStringLiteral("digitalProfile"))) {
                const QJsonValue profile = configuration.value(QStringLiteral("digitalProfile"));
                valid = valid && profile.isObject() &&
                    CliDigitalProfile::parse(profile.toObject(), mode, candidate, profileError);
            }
            if (!valid) {
                std::fprintf(stderr, "Invalid digital module configuration\n");
                return 2;
            }
            activeProfile = std::move(candidate);
            settings.listeningFrequency = frequency;
            decoder.reset();
            applyProfile();
            previousDmrMetadata = {};
            discontinuity = true;
            previousRate = 0;
            send({{QStringLiteral("event"), QStringLiteral("configured")},
                  {QStringLiteral("module"), mode}});
            continue;
        }
        if (mode == QStringLiteral("ft8") || mode == QStringLiteral("ft4") || mode == QStringLiteral("rtty")) {
            if (kind != CliModuleProtocol::Pcm16 || rate != 48000 || size % 2) {
                std::fprintf(stderr, "Invalid text-mode PCM frame: kind=%u rate=%u size=%u\n", kind, rate, size);
                return 2;
            }
            decoder.processPcmFrame(payload, settings, 48000);
            continue;
        }
        if (kind != CliModuleProtocol::ChannelIqF32 || size % (2 * sizeof(float)) ||
            rate < 48000 || rate > 2000000) {
            std::fprintf(stderr, "Invalid DMR frame: kind=%u rate=%u size=%u\n", kind, rate, size);
            return 2;
        }
        iq.resize(size / sizeof(float));
        std::memcpy(iq.data(), payload.constData(), size);
        int basebandRate = 0;
        if (mode == QStringLiteral("m17")) {
            RadioSettings demodSettings = settings;
            demodSettings.sampleRate = rate;
            demodSettings.centerFrequency = demodSettings.listeningFrequency = 0;
            demodSettings.bandwidth = 12500;
            if (!dmrDemodulator.processIndependentIq(iq, demodSettings,
                    discontinuity || rate != previousRate, baseband)) return 3;
            basebandRate = 48000;
        } else if (!dmrDemodulator.processIndependentDmrIq(iq, rate,
                                                     discontinuity || rate != previousRate,
                                                     baseband, basebandRate)) return 3;
        discontinuity = false;
        previousRate = rate;
        if (!baseband.empty()) {
            const QByteArray pcm(reinterpret_cast<const char *>(baseband.data()),
                                 static_cast<int>(baseband.size() * sizeof(short)));
            decoder.processPcmFrame(pcm, settings, basebandRate);
        }
    }
}
