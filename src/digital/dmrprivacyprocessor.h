#ifndef DMRPRIVACYPROCESSOR_H
#define DMRPRIVACYPROCESSOR_H

#include "dmrvoicepayload.h"
#include "dmrprivacycodec.h"
#include <array>
#include <deque>
#include <vector>

class DmrPrivacyProcessor {
public:
    enum class Outcome { Bypassed, Applied, Silence, InvalidKey, Unaligned, InvalidPayload, MissingMi, KeyMismatch };
    struct Statistics {
        quint64 applied = 0;
        quint64 silence = 0;
        quint64 invalidKey = 0;
        quint64 unaligned = 0;
        quint64 invalidPayload = 0;
        quint64 missingMi = 0;
        quint64 keyMismatch = 0;
        quint64 piAccepted = 0;
        quint64 piRejected = 0;
        quint64 lateEntryAccepted = 0;
        quint64 lateEntryRejected = 0;
        quint64 clearFrames = 0;
    };

    bool configure(int mode, bool enabled, const QString &keyHex, int keyId);
    void reset();
    void resetVoiceAlignment() { slotContexts = {}; }
    bool isActive() const { return active; }
    int keyBits() const { return configuredKeyBits; }
    bool observePi(const QString &dibits, int timeslot, quint64 sample);
    Outcome process(DmrAmbePayload &payload);
    void processVoiceFrames(std::vector<DmrAmbePayload> &payloads,
                            std::vector<QString> &rawFrames,
                            std::vector<DmrAmbeSoftFrame> &softFrames);
    const Statistics &statistics() const { return counters; }

private:
    struct Slot {
        quint64 anchor = 0;
        int lastOrdinal = -1;
        bool hasAnchor = false;
        bool aligned = false;
        int encrypted = -1;
        bool blocked = false;
        quint32 nextMi = 0;
        bool nextMiValid = false;
        QByteArray stream;
        std::array<int, 18> fragments = {};
    };
    struct PendingPi { DmrPrivacyCodec::PiHeader header; quint64 sample = 0; };
    std::array<Slot, 3> slotContexts = {};
    std::array<std::deque<PendingPi>, 3> pendingHeaders;
    std::array<std::array<unsigned char, 13>, 18> masks = {};
    Statistics counters;
    QString requestedKey;
    QByteArray key;
    int requestedMode = -1;
    int requestedId = -1;
    int configuredKeyBits = 0;
    bool requestedEnabled = false;
    bool active = false;
    void beginSuperframe(Slot &slot, const DmrAmbePayload &payload, int ordinal);
    void finishLateEntry(Slot &slot);
};

#endif // DMRPRIVACYPROCESSOR_H
