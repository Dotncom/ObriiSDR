#include "dmrprivacyprocessor.h"
#include "dmrprivacyutils.h"
#include <algorithm>

namespace {
int nibble(QChar ch) {
    const ushort c = ch.unicode();
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
}

bool DmrPrivacyProcessor::configure(int mode, bool enabled, const QString &keyHex, int keyId) {
    if (requestedMode == mode && requestedEnabled == enabled &&
        requestedKey == keyHex && requestedId == keyId) return false;
    const bool previouslyActive = active;
    requestedMode = mode;
    requestedEnabled = enabled;
    requestedKey = keyHex;
    requestedId = keyId;
    active = enabled && (mode == DMR_PRIVACY_HYTERA_BASIC || mode == DMR_PRIVACY_ARC4 || mode == DMR_PRIVACY_AES256);
    configuredKeyBits = 0;
    key.clear();
    masks = {};
    reset();
    if (!active) return previouslyActive;
    if (mode != DMR_PRIVACY_HYTERA_BASIC) {
        QString clean = keyHex.trimmed();
        if (clean.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) clean.remove(0, 2);
        clean.remove(QLatin1Char(' '));
        const int length = mode == DMR_PRIVACY_ARC4 ? 10 : 64;
        if (clean.size() == length && std::all_of(clean.begin(), clean.end(), [](QChar c) { return nibble(c) >= 0; })) {
            key = QByteArray::fromHex(clean.toLatin1());
            configuredKeyBits = length * 4;
        }
        return true;
    }
    if (!isValidHyteraBasicKey(keyHex)) return true;

    const QString key = normalizedDmrPrivacyKeyHex(keyHex);
    configuredKeyBits = key.size() * 4;
    // Basic repeats key bits over 18 * 49 bits, restarting at every A burst.
    // Length is explicit: leading/all-zero bytes must not change the key width.
    for (int frame = 0; frame < 18; ++frame) {
        for (int bit = 0; bit < 49; ++bit) {
            const int keyBit = (frame * 49 + bit) % configuredKeyBits;
            const int value = (nibble(key.at(keyBit / 4)) >> (3 - keyBit % 4)) & 1;
            masks[frame][bit / 4] |= static_cast<unsigned char>(value << (3 - bit % 4));
        }
    }
    return true;
}

void DmrPrivacyProcessor::reset() {
    slotContexts = {};
    pendingHeaders = {};
    counters = {};
}

bool DmrPrivacyProcessor::observePi(const QString &dibits, int timeslot, quint64 sample) {
    if (!active || requestedMode == DMR_PRIVACY_HYTERA_BASIC) return false;
    DmrPrivacyCodec::PiHeader header;
    bool decoded = DmrPrivacyCodec::decodePi(dibits, header);
    if (!decoded && dibits.size() == 132) {
        QString flipped = dibits;
        for (int i = 0; i < flipped.size(); ++i) {
            const int symbol = flipped.at(i).unicode() - '0';
            if (symbol < 0 || symbol > 3) { ++counters.piRejected; return false; }
            flipped[i] = QLatin1Char(char('0' + ((symbol + 2) & 3)));
        }
        decoded = DmrPrivacyCodec::decodePi(flipped, header);
    }
    if (timeslot < 0 || timeslot > 2 || !decoded) {
        ++counters.piRejected;
        return false;
    }
    auto &headers = pendingHeaders[static_cast<std::size_t>(timeslot)];
    const auto position = std::lower_bound(headers.begin(), headers.end(), sample,
        [](const PendingPi &item, quint64 value) { return item.sample < value; });
    if (position != headers.end() && position->sample == sample) return true;
    headers.insert(position, {header, sample});
    if (headers.size() > 8) headers.pop_front();
    ++counters.piAccepted;
    return true;
}

void DmrPrivacyProcessor::beginSuperframe(Slot &slot, const DmrAmbePayload &payload, int ordinal) {
    bool continuous = false;
    quint32 mi = slot.nextMi;
    if (slot.hasAnchor && payload.superframeSamples > 0 && payload.superframeSample > slot.anchor) {
        const quint64 delta = payload.superframeSample - slot.anchor;
        const quint64 count = (delta + payload.superframeSamples / 2) / payload.superframeSamples;
        const quint64 expected = count * payload.superframeSamples;
        const quint64 error = delta > expected ? delta - expected : expected - delta;
        continuous = count >= 1 && count <= 4 && error <= (std::max)(quint64(2), payload.superframeSamples / 600);
        if (continuous && slot.nextMiValid)
            for (quint64 step = 1; step < count; ++step)
                mi = DmrPrivacyCodec::nextMi(mi, requestedMode == DMR_PRIVACY_AES256);
    }
    bool valid = continuous && slot.nextMiValid;
    if (!continuous) { slot.encrypted = -1; slot.blocked = false; }
    slot.anchor = payload.superframeSample;
    slot.hasAnchor = true;
    slot.lastOrdinal = -1;
    slot.aligned = ordinal == 0;
    slot.fragments.fill(-1);
    slot.stream.clear();
    auto &headers = pendingHeaders[static_cast<std::size_t>(payload.timeslot)];
    while (!headers.empty() && headers.front().sample <= slot.anchor) {
        const auto pi = headers.front();
        headers.pop_front();
        if (payload.superframeSamples && slot.anchor - pi.sample > payload.superframeSamples * 2) continue;
        slot.encrypted = 1;
        slot.blocked = pi.header.algorithm != (requestedMode == DMR_PRIVACY_ARC4 ? 1 : 5) ||
                       (requestedId != 0 && pi.header.keyId != requestedId);
        mi = pi.header.mi;
        valid = !slot.blocked;
    }
    slot.nextMiValid = valid;
    if (valid) {
        slot.stream = DmrPrivacyCodec::voiceKeystream(requestedMode, key, mi);
        slot.nextMi = DmrPrivacyCodec::nextMi(mi, requestedMode == DMR_PRIVACY_AES256);
    }
}

void DmrPrivacyProcessor::finishLateEntry(Slot &slot) {
    if (slot.encrypted == 0 || slot.blocked) return;
    quint32 mi = 0;
    if (!DmrPrivacyCodec::decodeLateEntry(slot.fragments, mi)) {
        ++counters.lateEntryRejected;
        return;
    }
    // A-F carries NEXT MI. Commit only after processing F's third voice frame.
    slot.nextMi = mi;
    slot.nextMiValid = true;
    slot.encrypted = 1;
    ++counters.lateEntryAccepted;
}

void DmrPrivacyProcessor::processVoiceFrames(std::vector<DmrAmbePayload> &payloads,
                                           std::vector<QString> &rawFrames,
                                           std::vector<DmrAmbeSoftFrame> &softFrames) {
    if (!active) return;
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        if (index < rawFrames.size())
            payloads[index].lateEntryNibble = DmrPrivacyCodec::lateEntryNibble(rawFrames[index]);
        process(payloads[index]);
    }
    rawFrames.clear();
    softFrames.clear();
}

DmrPrivacyProcessor::Outcome DmrPrivacyProcessor::process(DmrAmbePayload &payload) {
    if (!active) return Outcome::Bypassed;
    const auto mute = [&](Outcome outcome, quint64 &counter) {
        payload.hex.clear();
        ++counter;
        return outcome;
    };
    const bool basic = requestedMode == DMR_PRIVACY_HYTERA_BASIC;
    if (basic && configuredKeyBits == 0) return mute(Outcome::InvalidKey, counters.invalidKey);
    if (payload.timeslot < 0 || payload.timeslot > 2 ||
        payload.burstIndex < 0 || payload.burstIndex > 5 ||
        payload.frameIndex < 0 || payload.frameIndex > 2) {
        return mute(Outcome::Unaligned, counters.unaligned);
    }
    const int ordinal = payload.burstIndex * 3 + payload.frameIndex;
    Slot &slot = slotContexts[static_cast<std::size_t>(payload.timeslot)];
    if (!slot.hasAnchor || payload.superframeSample > slot.anchor) {
        beginSuperframe(slot, payload, ordinal);
    }
    if (payload.superframeSample != slot.anchor || !slot.aligned || ordinal <= slot.lastOrdinal) {
        return mute(Outcome::Unaligned, counters.unaligned);
    }
    slot.lastOrdinal = ordinal;
    if (!basic) {
        slot.fragments[ordinal] = payload.lateEntryNibble;
        if (payload.privacyIndicator == 0) {
            slot.encrypted = 0;
            slot.blocked = false;
            slot.nextMiValid = false;
            slot.stream.clear();
        } else if (payload.privacyIndicator == 1) {
            slot.encrypted = 1;
        }
        if (slot.encrypted == 0) { ++counters.clearFrames; return Outcome::Bypassed; }
        // Even a rejected voice payload contributes its raw late-entry nibble.
        if (ordinal == 17) finishLateEntry(slot);
        if (!configuredKeyBits) return mute(Outcome::InvalidKey, counters.invalidKey);
        if (slot.blocked) return mute(Outcome::KeyMismatch, counters.keyMismatch);
        if (slot.stream.size() != 126) return mute(Outcome::MissingMi, counters.missingMi);
    }

    // Advance by the RF ordinal even for bad/missing FEC frames, never by the
    // number of frames that survive an audio quality gate. Slot 0 is simplex/unknown.
    const int size = payload.hex.size();
    if (size != 13 && size != 14) return mute(Outcome::InvalidPayload, counters.invalidPayload);
    std::array<int, 14> input = {};
    for (int n = 0; n < size; ++n) {
        input[n] = nibble(payload.hex.at(n));
        if (input[n] < 0) return mute(Outcome::InvalidPayload, counters.invalidPayload);
    }
    if ((input[12] & 7) != 0 || (size == 14 && input[13] != 0)) {
        return mute(Outcome::InvalidPayload, counters.invalidPayload);
    }
    if (payload.hex.left(13).compare(QStringLiteral("F801A99F8CE08"), Qt::CaseInsensitive) == 0) {
        ++counters.silence;
        return Outcome::Silence;
    }
    static constexpr char hex[] = "0123456789ABCDEF";
    QString output;
    output.reserve(size);
    for (int n = 0; n < 13; ++n) {
        int mask = masks[ordinal][n];
        if (!basic) {
            const int byte = n / 2;
            mask = (quint8(slot.stream.at(ordinal * 7 + byte)) >> (n % 2 == 0 ? 4 : 0)) & 15;
            if (n == 12) mask &= 8;
        }
        output.append(QLatin1Char(hex[input[n] ^ mask]));
    }
    if (size == 14) output.append(QLatin1Char('0'));
    payload.hex = output;
    ++counters.applied;
    return Outcome::Applied;
}
