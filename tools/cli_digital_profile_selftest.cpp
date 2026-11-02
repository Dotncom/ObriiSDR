#include "climodulemanager.h"
#include "climoduleprotocol.h"
#include "dmrprivacyprocessor.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <cmath>
#include <functional>
#include <iostream>
#include <utility>

namespace {
int failures = 0;
const QString secret = QStringLiteral("0123456789");

void check(bool okay, const char *name) {
    if (!okay) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

bool spin(const std::function<bool()> &done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    QCoreApplication::processEvents();
    return done();
}

QJsonObject base(const QString &module = QStringLiteral("dmr")) {
    return {{QStringLiteral("version"), 1}, {QStringLiteral("module"), module}};
}

QJsonObject fixed(const QString &mode = QStringLiteral("arc4"), const QString &key = secret) {
    QJsonObject json = base();
    json.insert(QStringLiteral("settings"), QJsonObject{
        {QStringLiteral("dmrPrivacyMode"), mode}, {QStringLiteral("dmrPrivacyKeyId"), 7},
        {QStringLiteral("dmrPrivacyKeyHex"), key}});
    return json;
}

bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool write(const QString &path, const QJsonObject &json) {
    return write(path, QJsonDocument(json).toJson(QJsonDocument::Compact));
}

void validation(const QString &directory) {
    CliDigitalProfile::Profile profile;
    QString error;
    const auto good = [&](const QJsonObject &json, const QString &mode = QStringLiteral("dmr")) {
        check(CliDigitalProfile::parse(json, mode, profile, error) && error.isEmpty(), "valid profile");
    };
    const auto bad = [&](const QJsonObject &json, const QString &mode = QStringLiteral("dmr")) {
        const QJsonObject previous = profile.json;
        check(!CliDigitalProfile::parse(json, mode, profile, error) && !error.isEmpty(), "invalid profile rejected");
        check(profile.json == previous, "failed parse is atomic");
        check(!error.contains(secret), "validation errors redact keys");
    };
    good(base());
    good(fixed());
    check(profile.radio.dmrPrivacyMode == DMR_PRIVACY_ARC4 &&
          profile.radio.dmrPrivacyKeyId == 7 && profile.radio.dmrPrivacyKeyHex == secret,
          "fixed ARC4 settings retained");
    good(fixed(QStringLiteral("aes256"), QString(64, QLatin1Char('a'))));
    check(profile.radio.dmrPrivacyKeyHex == QString(64, QLatin1Char('A')), "key case normalized without truncation");
    for (int width : {10, 32, 64}) good(fixed(QStringLiteral("hytera-bp"), QString(width, QLatin1Char('0'))));
    for (const QString &mode : {QStringLiteral("ft8"), QStringLiteral("rtty"), QStringLiteral("ft4"), QStringLiteral("m17")}) {
        good(base(mode), mode);
        const int expected = mode == "ft4" ? MOD_FT4 : mode == "m17" ? MOD_M17 : mode == "rtty" ? MOD_RTTY : MOD_FT8;
        check(profile.radio.modulationType == expected, "module selects its shared decoder mode");
    }
    QJsonObject rtty = base(QStringLiteral("rtty"));
    rtty.insert(QStringLiteral("rtty"), QJsonObject{{QStringLiteral("baud"), 45.45},
        {QStringLiteral("markHz"), 2125}, {QStringLiteral("spaceHz"), 2295}, {QStringLiteral("polarity"), QStringLiteral("auto")}});
    good(rtty, QStringLiteral("rtty"));
    auto tones = rtty.value(QStringLiteral("rtty")).toObject();
    tones.insert(QStringLiteral("baud"), 50);
    rtty.insert(QStringLiteral("rtty"), tones);
    bad(rtty, QStringLiteral("rtty"));

    for (const QString &key : {QString(), QStringLiteral("0x0123456789"), QStringLiteral("01 23 45 67 89"),
                               QStringLiteral("012345678Z"), QString(65, QLatin1Char('A')), secret + QLatin1Char('\n')})
        bad(fixed(QStringLiteral("arc4"), key));
    bad(fixed(QStringLiteral("aes256"), QString(63, QLatin1Char('1'))));
    bad(fixed(QStringLiteral("hytera-bp"), QString(12, QLatin1Char('1'))));
    bad(fixed(QStringLiteral("ARC4")));
    bad(fixed(QStringLiteral("none")));
    bad(fixed(), QStringLiteral("ft8"));
    auto invalid = base();
    invalid.insert(QStringLiteral("version"), 2); bad(invalid);
    invalid = base(); invalid.insert(QStringLiteral("version"), QStringLiteral("1")); bad(invalid);
    invalid = base(); invalid.insert(QStringLiteral("module"), QStringLiteral("ft8")); bad(invalid);
    invalid = base(); invalid.insert(secret, true); bad(invalid);
    invalid = base(); invalid.insert(QStringLiteral("settings"), QJsonArray{}); bad(invalid);
    invalid = base(); invalid.insert(QStringLiteral("dmrVoiceBackendId"), QStringLiteral("bad\n")); bad(invalid);
    invalid = base(); invalid.insert(QStringLiteral("dmrVoiceOutputEnabled"), 1); bad(invalid);
    invalid = base(); invalid.insert(QStringLiteral("dmrVoicePayloadEnabled"), false); bad(invalid);
    for (const auto &entry : {std::make_pair(QStringLiteral("dmrPrivacyKeyId"), QJsonValue(256)),
                              std::make_pair(QStringLiteral("dmrPrivacyKeyId"), QJsonValue(7.5)),
                              std::make_pair(QStringLiteral("dmrPrivacyAutoKeySelection"), QJsonValue("true")),
                              std::make_pair(QStringLiteral("dmrLabColorCode"), QJsonValue(16)),
                              std::make_pair(QStringLiteral("dmrManualTimingOffset"), QJsonValue(-81)),
                              std::make_pair(QStringLiteral("dmrSlicerRatio"), QJsonValue(0.81)),
                              std::make_pair(QStringLiteral("dmrPrivacyVariant"), QJsonValue("drop0")),
                              std::make_pair(QStringLiteral("dmrBasebandSampleRate"), QJsonValue(24000))}) {
        invalid = fixed();
        auto settings = invalid.value(QStringLiteral("settings")).toObject();
        settings.insert(entry.first, entry.second);
        invalid.insert(QStringLiteral("settings"), settings); bad(invalid);
    }
    const QJsonObject key{{QStringLiteral("mode"), QStringLiteral("arc4")},
        {QStringLiteral("keyId"), 7}, {QStringLiteral("keyHex"), secret}};
    QJsonObject automatic = base();
    automatic.insert(QStringLiteral("settings"), QJsonObject{{QStringLiteral("dmrPrivacyAutoKeySelection"), true}});
    bad(automatic);
    automatic.insert(QStringLiteral("dmrPrivacyKeys"), QJsonArray{key}); good(automatic);
    check(profile.keys.size() == 1 && profile.radio.dmrPrivacyAutoKeySelection, "automatic keyring retained");
    automatic.insert(QStringLiteral("dmrPrivacyKeys"), QJsonArray{key, key}); bad(automatic);
    QJsonArray huge;
    for (int i = 0; i <= CliDigitalProfile::MaximumKeys; ++i) huge.append(key);
    automatic.insert(QStringLiteral("dmrPrivacyKeys"), huge); bad(automatic);
    automatic.insert(QStringLiteral("dmrPrivacyKeys"), QJsonArray{key});
    auto settings = automatic.value(QStringLiteral("settings")).toObject();
    settings.insert(QStringLiteral("dmrPrivacyKeyHex"), secret);
    automatic.insert(QStringLiteral("settings"), settings); bad(automatic);
    invalid = fixed();
    settings = invalid.value(QStringLiteral("settings")).toObject();
    settings.remove(QStringLiteral("dmrPrivacyKeyHex"));
    invalid.insert(QStringLiteral("settings"), settings);
    invalid.insert(QStringLiteral("dmrPrivacyKeys"), QJsonArray{key}); good(invalid);
    check(profile.radio.dmrPrivacyKeyHex == secret, "fixed mode resolves selected keyring entry");

    const QString path = directory + QStringLiteral("/validation.json");
    check(write(path, fixed()) && CliDigitalProfile::readFile(path, QStringLiteral("dmr"), profile, error), "read explicit file");
    QByteArray boundary = QJsonDocument(fixed()).toJson(QJsonDocument::Compact);
    boundary += QByteArray(CliDigitalProfile::MaximumBytes - boundary.size(), ' ');
    check(write(path, boundary) && CliDigitalProfile::readFile(path, QStringLiteral("dmr"), profile, error), "exact size limit accepted");
    check(!CliDigitalProfile::readFile(directory, QStringLiteral("dmr"), profile, error), "directory rejected");
    check(!CliDigitalProfile::readFile(path + QStringLiteral(".missing"), QStringLiteral("dmr"), profile, error), "missing/wrong extension rejected");
    for (const QByteArray &bytes : {QByteArray(), QByteArray("[]"), QByteArray("{\"keyHex\":\"0123456789\""),
                                    QByteArray(CliDigitalProfile::MaximumBytes + 1, ' ')}) {
        check(write(path, bytes), "write malformed fixture");
        check(!CliDigitalProfile::readFile(path, QStringLiteral("dmr"), profile, error), "malformed/oversized file rejected");
        check(!error.contains(secret), "file errors redact key material");
    }
}

void realKeys(const QString &fixture) {
    QFile file(fixture);
    check(file.open(QIODevice::ReadOnly), "open public synthetic Motorola vectors");
    const QJsonArray vectors = QJsonDocument::fromJson(file.readAll()).array();
    check(vectors.size() == 2, "ARC4 and AES256 fixtures present");
    QJsonArray ring;
    for (const QJsonValue &value : vectors) {
        const QJsonObject vector = value.toObject();
        ring.append(QJsonObject{{QStringLiteral("mode"), dmrPrivacyModeId(vector.value(QStringLiteral("Mode")).toInt())},
            {QStringLiteral("keyId"), 7}, {QStringLiteral("keyHex"), vector.value(QStringLiteral("Key"))}});
    }
    for (bool automatic : {false, true}) {
        for (const QJsonValue &value : vectors) {
            const QJsonObject vector = value.toObject();
            QJsonObject json = automatic ? base() : fixed(dmrPrivacyModeId(vector.value(QStringLiteral("Mode")).toInt()),
                                                         vector.value(QStringLiteral("Key")).toString());
            if (automatic) {
                json.insert(QStringLiteral("settings"), QJsonObject{{QStringLiteral("dmrPrivacyAutoKeySelection"), true}});
                json.insert(QStringLiteral("dmrPrivacyKeys"), ring);
            }
            CliDigitalProfile::Profile profile;
            QString error;
            check(CliDigitalProfile::parse(json, QStringLiteral("dmr"), profile, error), "parse real key profile");
            DmrPrivacyProcessor processor;
            processor.setKeyring(profile.keys);
            processor.configure(automatic ? DMR_PRIVACY_AUTO : profile.radio.dmrPrivacyMode,
                                profile.voiceOutput, profile.radio.dmrPrivacyKeyHex, profile.radio.dmrPrivacyKeyId);
            for (int restart = 0; restart < 2; ++restart) {
                processor.reset();
                check(processor.observePi(vector.value(QStringLiteral("PI")).toString(), 1, 900), "accept synthetic PI after reset");
                const QJsonArray frames = vector.value(QStringLiteral("Frames")).toArray();
                if (frames.isEmpty()) { check(false, "synthetic frames present"); continue; }
                const QJsonArray cipher = frames.first().toObject().value(QStringLiteral("Cipher")).toArray();
                for (int ordinal = 0; ordinal < cipher.size(); ++ordinal) {
                    DmrAmbePayload payload;
                    payload.hex = cipher.at(ordinal).toString();
                    payload.burstIndex = ordinal / 3; payload.frameIndex = ordinal % 3;
                    payload.superframeSample = 1000; payload.superframeSamples = 8640;
                    payload.timeslot = 1; payload.privacyIndicator = 1;
                    check(processor.process(payload) == DmrPrivacyProcessor::Outcome::Applied &&
                          payload.hex == QStringLiteral("827E93D4857680"),
                          "profile keys decrypt all 18 known frames through shared privacy processor");
                }
            }
        }
    }
    CliDigitalProfile::Profile basic;
    QString error;
    check(CliDigitalProfile::parse(fixed(QStringLiteral("hytera-bp")), QStringLiteral("dmr"), basic, error), "parse Basic key profile");
    DmrPrivacyProcessor processor;
    processor.configure(basic.radio.dmrPrivacyMode, basic.voiceOutput, basic.radio.dmrPrivacyKeyHex, basic.radio.dmrPrivacyKeyId);
    DmrAmbePayload payload;
    payload.hex = QStringLiteral("0000000000AA8");
    payload.burstIndex = 0; payload.frameIndex = 0; payload.superframeSample = 1000;
    payload.timeslot = 1; payload.privacyIndicator = 1;
    check(processor.process(payload) == DmrPrivacyProcessor::Outcome::Applied && payload.hex == QStringLiteral("0123456789AB8"),
          "profile Basic key decrypts known payload through shared processor");
}

QByteArray wire(const QJsonObject &profile) {
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("listeningFrequencyHz"), 435000000},
        {QStringLiteral("digitalProfile"), profile}}).toJson(QJsonDocument::Compact);
    return CliModuleProtocol::frame(CliModuleProtocol::Configure, 0, payload.constData(), payload.size());
}

void workers() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/ObriiCliModule") +
#ifdef _WIN32
        QStringLiteral(".exe");
#else
        QString();
#endif
    for (const QString &mode : {QStringLiteral("ft8"), QStringLiteral("rtty"), QStringLiteral("dmr")}) {
        for (bool valid : {false, true}) {
            QProcess process;
            process.start(executable, {QStringLiteral("rx"), mode});
            check(process.waitForStarted(3000), "start actual digital worker");
            auto profile = mode == QStringLiteral("dmr") ? fixed() : base(mode);
            if (!valid) profile.insert(QStringLiteral("unexpected"), secret);
            QByteArray frames = wire(profile);
            if (valid) {
                frames += CliModuleProtocol::frame(CliModuleProtocol::Reset, 0, nullptr, 0);
                const QByteArray retune = "{\"listeningFrequencyHz\":436000000}";
                frames += CliModuleProtocol::frame(CliModuleProtocol::Configure, 0, retune.constData(), retune.size());
                const QByteArray silence(mode == QStringLiteral("dmr") ? 4800 * 2 * sizeof(float) : 4800 * 2, '\0');
                frames += CliModuleProtocol::frame(mode == QStringLiteral("dmr") ? CliModuleProtocol::ChannelIqF32 : CliModuleProtocol::Pcm16,
                                                  mode == QStringLiteral("dmr") ? 192000 : 48000,
                                                  silence.constData(), silence.size());
            }
            process.write(frames);
            process.closeWriteChannel();
            const bool finished = process.waitForFinished(10000);
            check(finished, "worker terminates within bound");
            if (!finished) { process.kill(); process.waitForFinished(3000); }
            check(process.exitStatus() == QProcess::NormalExit && process.exitCode() == (valid ? 0 : 2), "worker revalidates Configure");
            const QByteArray output = process.readAllStandardOutput();
            const QByteArray errors = process.readAllStandardError();
            check(!output.contains(secret.toLatin1()) && !errors.contains(secret.toLatin1()), "worker does not echo profile secrets");
            if (valid) check(output.count("\"event\":\"configured\"") == 2, "worker accepts full and frequency-only configuration");
        }
    }
    for (const QByteArray &payload : {QByteArray("{\"listeningFrequencyHz\":\"435000000\"}"),
            QByteArray("{\"listeningFrequencyHz\":0}"), QByteArray("{\"listeningFrequencyHz\":1e13}"),
            QByteArray("{\"listeningFrequencyHz\":435000000,\"digitalProfile\":null}"),
            QByteArray("{\"listeningFrequencyHz\":435000000,\"extra\":\"0123456789\"}"),
            QByteArray("[]"), QByteArray("{"), QByteArray(CliDigitalProfile::MaximumBytes + 1025, ' ')}) {
        QProcess process;
        process.start(executable, {QStringLiteral("rx"), QStringLiteral("dmr")});
        check(process.waitForStarted(3000), "start worker for malformed envelope");
        process.write(CliModuleProtocol::frame(CliModuleProtocol::Configure, 0, payload.constData(), payload.size()));
        process.closeWriteChannel();
        const bool finished = process.waitForFinished(10000);
        check(finished, "malformed worker finishes within bound");
        if (!finished) { process.kill(); process.waitForFinished(3000); }
        check(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 2, "worker rejects invalid envelope and oversize before decoding");
        check(!process.readAllStandardOutput().contains(secret.toLatin1()) &&
              !process.readAllStandardError().contains(secret.toLatin1()), "envelope errors redact values");
    }
}

QByteArray rttyPcm() {
    QByteArray pcm;
    double phase = 0.0;
    double samples = 0.0;
    const auto tone = [&](bool mark, double bits) {
        samples += bits * 48000.0 / 45.45;
        while (pcm.size() / 2 < static_cast<int>(std::lround(samples))) {
            const qint16 sample = static_cast<qint16>(16000 * std::sin(phase));
            char bytes[2]; qToLittleEndian(sample, bytes); pcm.append(bytes, 2);
            phase = std::fmod(phase + 6.283185307179586 * (mark ? 2125.0 : 2295.0) / 48000.0, 6.283185307179586);
        }
    };
    tone(true, 8);
    for (int repeat = 0; repeat < 8; ++repeat)
        for (unsigned int code : {10U, 21U}) {
            tone(false, 1);
            for (int bit = 0; bit < 5; ++bit) tone((code >> bit) & 1, 1);
            tone(true, 1.5);
        }
    tone(true, 4);
    return pcm;
}

void manager(const QString &directory) {
    CliModuleManager manager;
    check(!manager.needsAudio() && !manager.needsChannelIq() && !manager.needsSpectrum(), "normal RX starts no digital work");
    manager.submitPcm(QByteArray(960, '\0'));
    manager.reset(); manager.setListeningHz(435000000);
    const QString path = directory + QStringLiteral("/manager.json");
    QJsonObject request{{QStringLiteral("operation"), QStringLiteral("start")},
        {QStringLiteral("name"), QStringLiteral("dmr")}, {QStringLiteral("path"), path}};
    check(write(path, fixed(QStringLiteral("arc4"), QStringLiteral("invalid"))), "write invalid manager fixture");
    check(!manager.command(request).value(QStringLiteral("ok")).toBool() && !manager.needsChannelIq(), "invalid profile never launches worker");
    request.insert(QStringLiteral("path"), 42);
    check(!manager.command(request).value(QStringLiteral("ok")).toBool(), "request path type validated");
    request.insert(QStringLiteral("path"), path);
    int configured = 0;
    QString decoded;
    manager.onEvent = [&](const QJsonObject &event) {
        check(!QJsonDocument(event).toJson().contains(secret.toLatin1()), "manager events redact keys");
        if (event.value(QStringLiteral("event")) == QStringLiteral("configured")) ++configured;
        if (event.value(QStringLiteral("event")) == QStringLiteral("text")) decoded += event.value(QStringLiteral("text")).toString();
    };
    for (const QString &mode : {QStringLiteral("dmr"), QStringLiteral("rtty"), QStringLiteral("ft8")}) {
        request.insert(QStringLiteral("name"), mode);
        const QJsonObject profile = mode == QStringLiteral("dmr") ? fixed() : base(mode);
        check(write(path, profile), "write manager fixture");
        configured = 0;
        auto reply = manager.command(request);
        check(reply.value(QStringLiteral("ok")).toBool(), "start manager with explicit profile");
        check(!QJsonDocument(reply).toJson().contains(secret.toLatin1()), "start/status redacts keys");
        check(spin([&] { return configured == 1; }), "manager sends Configure before data");
        check(mode == QStringLiteral("dmr") ? manager.needsChannelIq() : manager.needsAudio(), "only selected module requests input");
        const QByteArray listed = QJsonDocument(manager.command({{QStringLiteral("operation"), QStringLiteral("list")}})).toJson();
        check(!listed.contains(secret.toLatin1()) && !listed.contains(path.toUtf8()), "list never exposes profile or path");
        manager.reset();
        manager.setListeningHz(mode == QStringLiteral("dmr") ? 436000000 : 437000000);
        if (mode == QStringLiteral("rtty")) {
            decoded.clear();
            // Reject an oversized data frame, then verify reset/configuration replay on recovery.
            manager.submitPcm(QByteArray(CliModuleProtocol::MaximumPayloadBytes + 2, '\0'));
            const QByteArray pcm = rttyPcm();
            for (int offset = 0; offset < pcm.size(); offset += 9600) manager.submitPcm(pcm.mid(offset, 9600));
            check(spin([&] { return decoded.contains(QStringLiteral("RYRY")); }), "shared RTTY decodes synthetic Baudot after reset/retune");
        }
        manager.command({{QStringLiteral("operation"), QStringLiteral("stop")}, {QStringLiteral("name"), mode}});
        check(!manager.needsAudio() && !manager.needsChannelIq(), "stop removes RX demand");
        check(QFile::remove(path), "remove profile file before restart");
        check(!manager.command(request).value(QStringLiteral("ok")).toBool(), "failed replacement leaves saved selection intact");
        configured = 0;
        reply = manager.command({{QStringLiteral("operation"), QStringLiteral("start")}, {QStringLiteral("name"), mode}});
        check(reply.value(QStringLiteral("ok")).toBool() &&
              reply.value(QStringLiteral("module")).toObject().value(QStringLiteral("profileSelected")).toBool(), "restart retains profile in memory without rereading disk");
        check(spin([&] { return configured == 1; }), "restart replays selected profile");
        manager.command({{QStringLiteral("operation"), QStringLiteral("stop")}, {QStringLiteral("name"), mode}});
    }
    check(!manager.command({{QStringLiteral("operation"), QStringLiteral("start")},
        {QStringLiteral("name"), QStringLiteral("rtty")}, {QStringLiteral("backend"), path}}).value(QStringLiteral("ok")).toBool(),
        "profile file is never inferred from a backend argument");
    check(manager.command({{QStringLiteral("operation"), QStringLiteral("start")},
        {QStringLiteral("name"), QStringLiteral("dmr")}, {QStringLiteral("backend"), QStringLiteral("mbelib")}}).value(QStringLiteral("ok")).toBool(),
        "legacy backend ID remains supported alongside retained profile");
    manager.command({{QStringLiteral("operation"), QStringLiteral("stop")}, {QStringLiteral("name"), QStringLiteral("dmr")}});
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) { std::cerr << "Usage: ObriiCliDigitalProfileSelfTest tools/fixtures/dmr_motorola_vectors.json\n"; return 2; }
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    validation(directory.path());
    realKeys(app.arguments().at(1));
    workers();
    manager(directory.path());
    if (failures) return 1;
    std::cout << "CLI digital profile tests passed (validation, fixed/auto ARC4/AES256, worker Configure, RTTY, reset/restart, redaction).\n";
    return 0;
}
