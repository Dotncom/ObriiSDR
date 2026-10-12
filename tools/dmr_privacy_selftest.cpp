#include "dmrprivacyprocessor.h"
#include "dmrprivacyutils.h"
#include "dmrdecoder.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QtEndian>
#include <iostream>

namespace {
int failures = 0;
using Outcome = DmrPrivacyProcessor::Outcome;
const QString plain = QStringLiteral("0123456789AB8");
void check(bool success, const char *name) {
    if (!success) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
}

void testCrypto() {
    const QByteArray rcKey = QByteArray::fromHex("0102030405");
    check(DmrPrivacyCodec::rc4(rcKey, 0, 16).toHex() == "b2396305f03dc027ccc3524a0a1118a8", "RFC 6229 RC4 initial bytes");
    check(DmrPrivacyCodec::rc4(rcKey, 256, 16).toHex() == "1cfcf62b03eddb641d77dfcf7f8d8c93", "RFC 6229 drop256");
    const QByteArray aesKey = QByteArray::fromHex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
    const QByteArray iv = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const QByteArray input = QByteArray::fromHex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    QByteArray output = DmrPrivacyCodec::aes256Ofb(aesKey, iv, input.size());
    for (int i = 0; i < output.size(); ++i) output[i] = char(quint8(output.at(i)) ^ quint8(input.at(i)));
    check(output.toHex() == "dc7e84bfda79164b7ecd8486985d38604febdc6740d20b3ac88f6ad82a4fb08d71ab47a086e86eedf39d1c5bba97c4080126141d67f37be8538f5a8be740e484", "NIST SP800-38A AES256 OFB four blocks");
    check(DmrPrivacyCodec::aes256Ofb(QByteArray(31, 'x'), iv, 16).isEmpty(), "AES rejects wrong key width");
}

void testDeferredVoiceAnchor() {
    // Public synthetic PCM: put a block boundary at sync, before A's right half.
    const QString symbols = QString(100, '2') + QString(54, '2') +
        QStringLiteral("133313311131311113313331") + QString(86, '2');
    constexpr qint16 levels[] = {-6000, -18000, 6000, 18000};
    for (int rate : {24000, 48000}) {
        const int samplesPerSymbol = rate / 4800;
        QByteArray pcm(symbols.size() * samplesPerSymbol * 2, '\0');
        int offset = 0;
        for (QChar symbol : symbols)
            for (int sample = 0; sample < samplesPerSymbol; ++sample, offset += 2)
                qToLittleEndian<qint16>(levels[symbol.unicode() - '0'], pcm.data() + offset);
        for (int tailSymbols : {0, 1, 26, 53, 54, 86}) {
            DmrDecoder decoder;
            decoder.configure(rate);
            decoder.setLabHints(true, 6, 1, -1, -1);
            const int boundary = (178 + tailSymbols) * samplesPerSymbol * 2;
            const auto first = decoder.processPcmFrame(pcm.left(boundary), rate);
            const auto second = decoder.processPcmFrame(pcm.mid(boundary), rate);
            int aFrames = 0;
            for (const auto *result : {&first, &second})
                for (const auto &frame : result->ambePayloads)
                    if (frame.burstIndex == 0) {
                        const qint64 anchorError = qint64(frame.superframeSample) - 154 * samplesPerSymbol;
                        check(qAbs(anchorError) <= samplesPerSymbol &&
                              frame.superframeSamples == quint64(1728 * samplesPerSymbol),
                              "deferred A retains RF anchor and superframe span");
                        check(frame.frameIndex == aFrames && frame.timeslot == 1,
                              "deferred A retains frame ordinal and slot");
                        ++aFrames;
                    }
            check(aFrames == 3, "A survives sync/tail PCM boundaries exactly once");
            if (tailSymbols < 54)
                check(first.ambePayloads.empty(), "incomplete A cannot reach privacy/vocoder");
        }
    }
}

void testMotorolaVectors(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { check(false, "open independent Go vectors"); return; }
    const auto vectors = QJsonDocument::fromJson(file.readAll()).array();
    check(vectors.size() == 2, "ARC4 and AES256 reference fixtures");
    for (const auto &value : vectors) {
        const auto v = value.toObject();
        const int mode = v.value("Mode").toInt();
        const QString key = v.value("Key").toString(), pi = v.value("PI").toString();
        DmrPrivacyCodec::PiHeader header;
        check(DmrPrivacyCodec::decodePi(pi, header) && header.keyId == 7 && header.mi == 0x12345678 && header.algorithm == (mode == 1 ? 1 : 5), "independent PI BPTC+CRC");
        for (bool withPI : {false, true}) {
            DmrPrivacyProcessor processor;
            processor.configure(mode, true, key, 7);
            if (withPI) { check(processor.observePi(pi, 1, 900), "accept trusted PI"); processor.resetVoiceAlignment(); }
            const auto superframes = v.value("Frames").toArray();
            for (int sf = 0; sf < superframes.size(); ++sf) {
                const auto record = superframes.at(sf).toObject();
                const auto cipher = record.value("Cipher").toArray(), raw = record.value("Raw").toArray();
                std::vector<DmrAmbePayload> payloads;
                std::vector<QString> rawFrames;
                std::vector<DmrAmbeSoftFrame> soft;
                // Feed individual frames to catch committing next MI too early.
                for (int ordinal = 0; ordinal < 18; ++ordinal) {
                    DmrAmbePayload payload;
                    payload.hex = cipher.at(ordinal).toString().toUpper();
                    payload.burstIndex = ordinal / 3; payload.frameIndex = ordinal % 3;
                    payload.superframeSample = 1000 + sf * 8640;
                    payload.superframeSamples = 8640; payload.timeslot = 1; payload.privacyIndicator = ordinal < 3 ? -1 : 1;
                    payloads = {payload}; rawFrames = {raw.at(ordinal).toString()}; soft = {{rawFrames.front()}};
                    processor.processVoiceFrames(payloads, rawFrames, soft);
                    check(rawFrames.empty() && soft.empty(), "ciphertext fallback disabled");
                    if (!withPI && sf == 0) check(payloads[0].hex.isEmpty(), "late entry waits for NEXT superframe");
                    else check(payloads[0].hex == QStringLiteral("827E93D4857680"), "all 18 Motorola frames match independent plaintext");
                }
            }
            check(processor.statistics().lateEntryAccepted == 3, "three changing next-MI confirmations");
            check(processor.statistics().applied == (withPI ? 54 : 36), "no duplicate/early cipher frames");
        }
        DmrPrivacyProcessor processor;
        processor.configure(mode, true, key, 8);
        processor.observePi(pi, 2, 900);
        DmrAmbePayload payload;
        payload.hex = plain; payload.superframeSample = 1000;
        payload.burstIndex = 0; payload.frameIndex = 0; payload.timeslot = 2; payload.superframeSamples = 8640;
        check(processor.process(payload) == Outcome::KeyMismatch && payload.hex.isEmpty(), "wrong KID muted");
        payload.hex = plain; payload.frameIndex = 1; payload.privacyIndicator = 0;
        check(processor.process(payload) == Outcome::Bypassed && payload.hex == plain, "clear voice bypasses configured cipher");
        processor.configure(mode, true, key, 7);
        auto corrupt = pi; corrupt[0] = corrupt[0] == '0' ? QLatin1Char('1') : QLatin1Char('0');
        check(processor.observePi(corrupt, 1, 900), "one channel bit corrected");
        corrupt = QString(132, '0');
        check(!processor.observePi(corrupt, 1, 900), "PI CRC prevents false key context");
        QString flipped = pi;
        for (int i = 0; i < flipped.size(); ++i) flipped[i] = QLatin1Char(char('0' + ((flipped.at(i).unicode() - '0' + 2) & 3)));
        check(processor.observePi(flipped, 2, 900), "independent PI discriminator polarity");
        const auto superframes = v.value("Frames").toArray();
        const auto first = superframes.at(0).toObject(), second = superframes.at(1).toObject();
        const auto firstCipher = first.value("Cipher").toArray(), firstRaw = first.value("Raw").toArray();
        const auto makePayload = [&](int ordinal, int sf = 0, int slot = 1) {
            DmrAmbePayload p;
            p.hex = (sf ? second.value("Cipher").toArray() : firstCipher).at(ordinal).toString().toUpper();
            p.burstIndex = ordinal / 3; p.frameIndex = ordinal % 3;
            p.superframeSample = 1000 + sf * 8640; p.superframeSamples = 8640;
            p.timeslot = slot; p.privacyIndicator = 1;
            p.lateEntryNibble = DmrPrivacyCodec::lateEntryNibble((sf ? second.value("Raw").toArray() : firstRaw).at(ordinal).toString());
            return p;
        };
        for (bool badFecInsteadOfGap : {false, true}) {
            processor.reset();
            processor.observePi(pi, 1, 900);
            for (int ordinal = 0; ordinal < 18; ++ordinal) {
                if (ordinal == 7 && !badFecInsteadOfGap) continue;
                auto p = makePayload(ordinal);
                if (ordinal == 7) p.hex.clear();
                const auto outcome = processor.process(p);
                check(ordinal == 7 ? outcome == Outcome::InvalidPayload :
                      outcome == Outcome::Applied && p.hex == QStringLiteral("827E93D4857680"),
                      "missing/invalid voice does not shift the key stream");
            }
            auto p = makePayload(0, 1);
            check(processor.process(p) == Outcome::Applied && p.hex == QStringLiteral("827E93D4857680"),
                  "next-MI recovery or LFSR fallback after an incomplete superframe");
        }
        processor.reset(); processor.observePi(pi, 1, 900);
        auto p = makePayload(0); p.hex = QStringLiteral("F801A99F8CE080");
        check(processor.process(p) == Outcome::Silence && p.hex == QStringLiteral("F801A99F8CE080"), "clear silence frame preserved");
        p = makePayload(1);
        check(processor.process(p) == Outcome::Applied && p.hex == QStringLiteral("827E93D4857680"), "silence consumes full seven-byte position");
        p = makePayload(0, 0, 2);
        check(processor.process(p) == Outcome::MissingMi, "slot cannot inherit another slot's PI");
        p = makePayload(1);
        check(processor.process(p) == Outcome::Unaligned, "duplicate native frame is muted");
        p = makePayload(2); p.superframeSample = 1000 + 8640 * 20;
        check(processor.process(p) == Outcome::Unaligned, "long-gap middle entry waits for A");
        processor.reset(); processor.observePi(pi, 1, 10000);
        p = makePayload(0);
        check(processor.process(p) == Outcome::MissingMi, "future PI cannot change earlier voice");
        processor.configure(mode, true, QStringLiteral("xyz"), 7);
        processor.observePi(pi, 1, 900); p = makePayload(0);
        check(processor.process(p) == Outcome::InvalidKey && p.hex.isEmpty(), "invalid native key cannot become plaintext");
        processor.configure(mode, false, key, 7); p = makePayload(0);
        const auto saved = p.hex;
        check(processor.process(p) == Outcome::Bypassed && p.hex == saved, "Motorola master OFF bypasses");
    }
}

void testRecordedTiming(const QString &path, int rate, int chunkMs = 20) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || (rate != 24000 && rate != 48000)) {
        check(false, "private PCM timing fixture"); return;
    }
    DmrDecoder decoder;
    decoder.configure(rate);
    decoder.setLabHints(true, -1, -1, -1, -1);
    DmrPrivacyProcessor processor;
    // A synthetic key tests RF cadence/MI recovery, not this recording's speech.
    processor.configure(DMR_PRIVACY_ARC4, true, QStringLiteral("0102030405"), 0);
    while (!file.atEnd()) {
        auto result = decoder.processPcmFrame(file.read(rate * 2 * chunkMs / 1000), rate);
        if (result.lockAcquired) processor.resetVoiceAlignment();
        for (const auto &burst : result.dibitBursts)
            if (burst.burstKind == QStringLiteral("data") && burst.dataType == 0)
                processor.observePi(burst.dibits, burst.timeslot, burst.sample);
        processor.processVoiceFrames(result.ambePayloads, result.ambeFrames, result.ambeSoftFrames);
    }
    const auto &stats = processor.statistics();
    std::cout << "Recorded RF timing: applied=" << stats.applied << " LE=" << stats.lateEntryAccepted
              << " missingMI=" << stats.missingMi << " unaligned=" << stats.unaligned
              << " clear=" << stats.clearFrames << '\n';
    check(stats.applied > 0 && stats.lateEntryAccepted > 0, "real RF A-F metadata and next-MI recovery");
}
DmrAmbePayload frame(int ordinal, quint64 anchor = 1000, int slot = 1,
                     const QString &hex = plain) {
    DmrAmbePayload payload;
    payload.hex = hex;
    payload.correctedErrors = 1;
    payload.rawCorrectedErrors = 2;
    payload.burstIndex = ordinal / 3;
    payload.frameIndex = ordinal % 3;
    payload.superframeSample = anchor;
    payload.timeslot = slot;
    return payload;
}
QString reference(const QString &key, int ordinal, const QString &input) {
    QString bits;
    for (QChar ch : key) {
        const int value = QString(ch).toInt(nullptr, 16);
        for (int bit = 3; bit >= 0; --bit) bits.append(value & (1 << bit) ? '1' : '0');
    }
    QString result;
    for (int digit = 0; digit < 13; ++digit) {
        int value = QString(input.at(digit)).toInt(nullptr, 16);
        for (int bit = 0; bit < 4 && digit * 4 + bit < 49; ++bit) {
            if (bits.at((ordinal * 49 + digit * 4 + bit) % bits.size()) == QLatin1Char('1'))
                value ^= 1 << (3 - bit);
        }
        result.append(QString::number(value, 16).toUpper());
    }
    return result;
}
void testVectors() {
    struct Vector { const char *key; std::array<const char *, 3> expected; };
    const Vector vectors[] = {
        {"0123456789", {"0000000000AA8", "47A98A758BED0", "132103ED46B98"}},
        {"00112233445566778899AABBCCDDEEFF", {"01326754CDFE8", "CDCC5454DCDC0", "1010101010100"}},
        {"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
         {"012247648DAE8", "0D2D55759DBD8", "1131517191B18"}}
    };
    for (const Vector &vector : vectors) {
        DmrPrivacyProcessor processor;
        const QString key = QString::fromLatin1(vector.key);
        check(processor.configure(DMR_PRIVACY_HYTERA_BASIC, true, key, 7), "enable Basic");
        check(processor.keyBits() == key.size() * 4, "explicit key bit length");
        for (int ordinal = 0; ordinal < 18; ++ordinal) {
            auto payload = frame(ordinal);
            check(processor.process(payload) == Outcome::Applied, "complete superframe applied");
            check(payload.hex == reference(key, ordinal, plain), "bit-string reference for all 18 frames");
            if (ordinal == 0 || ordinal == 1 || ordinal == 17) {
                const int index = ordinal == 17 ? 2 : ordinal;
                check(payload.hex == QString::fromLatin1(vector.expected[index]), "independent fixed vector");
            }
            check(payload.correctedErrors == 1 && payload.rawCorrectedErrors == 2,
                  "FEC quality metadata preserved");
        }
        auto next = frame(0, 5000);
        check(processor.process(next) == Outcome::Applied &&
              next.hex == QString::fromLatin1(vector.expected[0]), "A resets Basic keystream");
        processor.reset();
        for (int ordinal = 0; ordinal < 18; ++ordinal) {
            auto encrypted = frame(ordinal, 10000, 1, reference(key, ordinal, plain));
            check(processor.process(encrypted) == Outcome::Applied && encrypted.hex == plain,
                  "decrypt independent ciphertext");
        }
    }
    for (int length : {10, 32, 64}) {
        DmrPrivacyProcessor processor;
        processor.configure(DMR_PRIVACY_HYTERA_BASIC, true, QString(length, '0'), 0);
        auto payload = frame(0);
        check(processor.keyBits() == length * 4 &&
              processor.process(payload) == Outcome::Applied && payload.hex == plain,
              "all-zero key is supplied, width not inferred from value");
    }
}
void testContinuity() {
    const QString key = QStringLiteral("0123456789");
    DmrPrivacyProcessor processor;
    processor.configure(DMR_PRIVACY_HYTERA_BASIC, true, key, 1);
    auto late = frame(6);
    check(processor.process(late) == Outcome::Unaligned && late.hex.isEmpty(), "late entry waits for A");
    auto a = frame(0, 5000);
    check(processor.process(a) == Outcome::Applied, "A synchronizes");
    auto missingFec = frame(1, 5000, 1, QString());
    check(processor.process(missingFec) == Outcome::InvalidPayload, "failed FEC consumes RF position");
    auto skipped = frame(8, 5000);
    check(processor.process(skipped) == Outcome::Applied && skipped.hex == reference(key, 8, plain),
          "missing bursts do not move key offset");
    auto duplicate = frame(8, 5000);
    check(processor.process(duplicate) == Outcome::Unaligned && duplicate.hex.isEmpty(), "duplicate muted");
    auto reordered = frame(5, 5000);
    check(processor.process(reordered) == Outcome::Unaligned, "out-of-order muted");
    check(!processor.configure(DMR_PRIVACY_HYTERA_BASIC, true, key, 1), "idempotent configure");
    auto continuing = frame(9, 5000);
    check(processor.process(continuing) == Outcome::Applied, "unchanged UI settings preserve alignment");
    auto secondSlot = frame(0, 5100, 2);
    check(processor.process(secondSlot) == Outcome::Applied, "second slot independent A");
    auto firstSlot = frame(10, 5000);
    check(processor.process(firstSlot) == Outcome::Applied && firstSlot.hex == reference(key, 10, plain),
          "second slot does not disturb first");
    auto simplex = frame(0, 6000, 0);
    check(processor.process(simplex) == Outcome::Applied, "simplex/unknown slot context");
    auto noA = frame(3, 9000);
    check(processor.process(noA) == Outcome::Unaligned, "new superframe without A muted");
    auto newA = frame(0, 12000);
    check(processor.process(newA) == Outcome::Applied, "next A recovers");
    auto stale = frame(17, 5000);
    check(processor.process(stale) == Outcome::Unaligned, "old superframe cannot overwrite state");
    auto current = frame(1, 12000);
    check(processor.process(current) == Outcome::Applied, "stale packet does not harm current context");
    processor.configure(DMR_PRIVACY_HYTERA_BASIC, true, "9876543210", 2);
    auto changedKey = frame(2, 12000);
    check(processor.process(changedKey) == Outcome::Unaligned, "live key switch waits for A");
    auto rekeyA = frame(0, 15000);
    check(processor.process(rekeyA) == Outcome::Applied, "live key switch recovers");
    processor.reset();
    auto afterReset = frame(3, 15000);
    check(processor.process(afterReset) == Outcome::Unaligned, "lock loss reset waits for A");
}
void testGates() {
    DmrPrivacyProcessor processor;
    const QString key = QStringLiteral("0123456789");
    for (const QString &bad : {QString(), QStringLiteral("ABC"), QString(10, 'Z'), QString(66, 'A')}) {
        processor.configure(DMR_PRIVACY_HYTERA_BASIC, true, bad, 0);
        auto payload = frame(0);
        check(processor.process(payload) == Outcome::InvalidKey && payload.hex.isEmpty(), "invalid key mutes");
    }
    processor.configure(DMR_PRIVACY_HYTERA_BASIC, true, "0x01:23:45:67:89", 0);
    auto a = frame(0);
    check(processor.process(a) == Outcome::Applied && a.hex == reference(key, 0, plain), "formatted key");
    auto silence = frame(1, 1000, 1, QStringLiteral("F801A99F8CE08"));
    check(processor.process(silence) == Outcome::Silence && silence.hex == "F801A99F8CE08",
          "unciphered silence left intact");
    auto afterSilence = frame(2);
    check(processor.process(afterSilence) == Outcome::Applied && afterSilence.hex == reference(key, 2, plain),
          "silence consumes RF position");
    for (const QString &bad : {QStringLiteral("0123456789AB9"), QStringLiteral("Z123456789AB8"),
                              QStringLiteral("0123456789AB81")}) {
        processor.reset();
        auto payload = frame(0, 1000, 1, bad);
        check(processor.process(payload) == Outcome::InvalidPayload && payload.hex.isEmpty(),
              "invalid hex or padding cannot reach vocoder");
    }
    processor.reset();
    auto padded = frame(0, 1000, 1, plain + '0');
    check(processor.process(padded) == Outcome::Applied && padded.hex == reference(key, 0, plain) + '0',
          "byte-padded 49-bit frame");
    for (int invalidField = 0; invalidField < 3; ++invalidField) {
        auto payload = frame(0);
        if (invalidField == 0) payload.burstIndex = 6;
        if (invalidField == 1) payload.frameIndex = 3;
        if (invalidField == 2) payload.timeslot = -1;
        check(processor.process(payload) == Outcome::Unaligned && payload.hex.isEmpty(), "invalid metadata muted");
    }
    std::vector<DmrAmbePayload> payloads = {frame(0, 2000)};
    const auto encryptedForExternal = payloads;
    std::vector<QString> raw = {QString(18, 'F')};
    std::vector<DmrAmbeSoftFrame> soft = {{QString(18, 'F')}};
    processor.processVoiceFrames(payloads, raw, soft);
    check(raw.empty() && soft.empty() && payloads[0].hex == reference(key, 0, plain),
          "native audio cannot fall back to encrypted 72-bit frames");
    check(encryptedForExternal[0].hex == plain, "external burst snapshot unchanged");
    for (int mode : {DMR_PRIVACY_NONE}) {
        processor.configure(mode, true, key, 0);
        auto payload = frame(0);
        check(processor.process(payload) == Outcome::Bypassed && payload.hex == plain, "other paths untouched");
    }
    processor.configure(DMR_PRIVACY_HYTERA_BASIC, false, key, 0);
    raw = {QString(18, 'F')};
    soft = {{QString(18, 'F')}};
    payloads = {frame(0)};
    processor.processVoiceFrames(payloads, raw, soft);
    check(!processor.isActive() && !raw.empty() && !soft.empty() && payloads[0].hex == plain,
          "master switch OFF preserves existing path");
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    testVectors();
    testCrypto();
    testDeferredVoiceAnchor();
    testContinuity();
    testGates();
    if (app.arguments().size() == 3 && app.arguments().at(1) == QStringLiteral("--reference-json"))
        testMotorolaVectors(app.arguments().at(2));
    else testMotorolaVectors(QStringLiteral(OBRII_TEST_SOURCE_DIR "/tools/fixtures/dmr_motorola_vectors.json"));
    if ((app.arguments().size() == 4 || app.arguments().size() == 5) &&
        app.arguments().at(1) == QStringLiteral("--pcm-timing")) {
        const int chunkMs = app.arguments().size() == 5 ? app.arguments().at(4).toInt() : 20;
        if (chunkMs < 1 || chunkMs > 60) { check(false, "PCM chunk duration 1..60 ms"); }
        else testRecordedTiming(app.arguments().at(2), app.arguments().at(3).toInt(), chunkMs);
    }
    if (failures) return 1;
    std::cout << "Native DMR privacy tests passed (Motorola ARC4/AES256 and Basic, alignment, slots, gates).\n";
    return 0;
}
