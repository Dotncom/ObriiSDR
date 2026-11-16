#include "m17audiodecoder.h"
#include "../openradio/m17codec2library.h"
#include <QtEndian>
extern "C" {
#include "m17.h"
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>

struct M17AudioDecoder::Impl {
    QLibrary library;
    using Create = void *(*)(int);
    using Destroy = void (*)(void *);
    using Decode = void (*)(void *, short *, const unsigned char *);
    using Samples = int (*)(void *);
    Create create = nullptr; Destroy destroy = nullptr; Decode decode = nullptr; Samples samples = nullptr;
    void *codec = nullptr;
    int codecMode = -1, rate = 0;
    std::array<float, 81> filter{};
    int filterPos = 0;
    std::deque<float> history;
    qint64 tick = 0, candidateEnd = 0, nextSymbol = 0, lastFrameTick = 0;
    int candidateKind = -1, frameKind = -1;
    float candidateError = 1e9f, gain = 1.0f, bias = 0.0f, candidateGain = 1.0f, candidateBias = 0.0f;
    std::array<float, 184> payload{};
    int payloadCount = 0;
    lsf_t lsf{};
    viterbi_ctx_t viterbi{};
    bool haveLsf = false;
    QByteArray packet;
    int packetFrames = 0;
    std::array<unsigned char, 30> lichBytes{};
    unsigned lichMask = 0;
    int lastSequence = -1;
    double inputPosition = 0.0, nextInputPosition = 0.0;
    float previousInput = 0.0f;

    Impl() {
        if (!loadM17Codec2(library, {"codec2_create", "codec2_destroy", "codec2_decode", "codec2_samples_per_frame"}))
            return;
        create = reinterpret_cast<Create>(library.resolve("codec2_create"));
        destroy = reinterpret_cast<Destroy>(library.resolve("codec2_destroy"));
        decode = reinterpret_cast<Decode>(library.resolve("codec2_decode"));
        samples = reinterpret_cast<Samples>(library.resolve("codec2_samples_per_frame"));
    }
    ~Impl() { resetCodec(); }
    void resetCodec() { if (codec && destroy) destroy(codec); codec = nullptr; codecMode = -1; }
    void reset() {
        resetCodec(); filter.fill(0); filterPos = 0; history.clear(); tick = 0;
        candidateKind = frameKind = -1; payloadCount = 0; haveLsf = false;
        packet.clear(); packetFrames = 0; lichMask = 0; lastSequence = -1;
        lastFrameTick = 0; inputPosition = nextInputPosition = 0; previousInput = 0;
    }
    QString callsign(const unsigned char *bytes) {
        char value[16]{}; decode_callsign_bytes(value, bytes); return QString::fromLatin1(value);
    }
    bool acceptLsf(const lsf_t &value, M17AudioResult &result) {
        if (CRC_M17(reinterpret_cast<const uint8_t *>(&value), 30) != 0) return false;
        const bool changed = !haveLsf || std::memcmp(&value, &lsf, 30) != 0;
        lsf = value; haveLsf = true;
        if (changed) {
            resetCodec(); lastSequence = -1;
            result.text << QStringLiteral("[M17] %1 → %2  CAN %3  TYPE 0x%4")
                .arg(callsign(lsf.src), callsign(lsf.dst)).arg(((lsf.type[0] << 8) | lsf.type[1]) >> 7 & 15)
                .arg((lsf.type[0] << 8) | lsf.type[1], 4, 16, QLatin1Char('0'));
            if (((lsf.type[0] << 8) | lsf.type[1]) & 0xf818)
                result.text << QStringLiteral("[M17] Encrypted, signed or newer payload type: metadata only");
        }
        return true;
    }
    void finishFrame(M17AudioResult &result) {
        lastFrameTick = tick;
        if (frameKind == 0) {
            lsf_t value{}; decode_LSF_ctx(&viterbi, &value, payload.data());
            if (acceptLsf(value, result)) { packet.clear(); packetFrames = 0; lichMask = 0; }
            else if (!haveLsf) {
                // LSF and stream sync words are polarity inverses. Before LSF
                // acquisition, also try stream/LICH for late-entry reception.
                for (float &symbol : payload) symbol = -symbol;
                gain = -gain; frameKind = 1; finishFrame(result);
            }
        } else if (frameKind == 2 && haveLsf) {
            const int type = (lsf.type[0] << 8) | lsf.type[1];
            if ((type & 1) || (type & 0xf818)) return;
            unsigned char data[25]{}, eof = 0, number = 0;
            decode_pkt_frame_ctx(&viterbi, data, &eof, &number, payload.data());
            if ((!eof && number != packetFrames) || (eof && (number < 1 || number > 25)) || packetFrames >= 33) {
                packet.clear(); packetFrames = 0; haveLsf = false; return;
            }
            packet.append(reinterpret_cast<const char *>(data), eof ? number : 25); ++packetFrames;
            if (eof) {
                if (packet.size() >= 3 && CRC_M17(reinterpret_cast<const uint8_t *>(packet.constData()), packet.size()) == 0) {
                    packet.chop(2);
                    const QByteArray content = packet.startsWith(char(5)) ? packet.mid(1) : packet;
                    result.text << QStringLiteral("[M17 packet %1 → %2] %3 | hex %4")
                        .arg(callsign(lsf.src), callsign(lsf.dst), QString::fromUtf8(content).replace(QChar(0), QLatin1Char(' ')), QString::fromLatin1(packet.toHex(' ')));
                }
                packet.clear(); packetFrames = 0; haveLsf = false;
            }
        } else if (frameKind == 1) {
            unsigned char data[16]{}, lich[5]{}, part = 0; uint16_t sequence = 0;
            decode_str_frame_ctx(&viterbi, data, lich, &sequence, &part, payload.data());
            // LICH Golay failure is reported through an invalid part index.
            if (part < 6) {
                std::copy_n(lich, 5, lichBytes.begin() + part * 5); lichMask |= 1U << part;
                if (lichMask == 63) {
                    lsf_t value{}; std::memcpy(&value, lichBytes.data(), 30);
                    acceptLsf(value, result); lichMask = 0;
                }
            }
            if (!haveLsf) return;
            const int type = (lsf.type[0] << 8) | lsf.type[1];
            const int content = (type >> 1) & 3;
            if (!(type & 1) || (type & 0xf818) || content < 2) return;
            const int seq = sequence & 0x7fff;
            if (seq == lastSequence) return;
            if (lastSequence >= 0 && seq != ((lastSequence + 1) & 0x7fff)) resetCodec();
            lastSequence = seq;
            const int mode = content == 2 ? 0 : 2; // Codec2 3200 / legacy 1600+data
            if (!create || !destroy || !decode || !samples) return;
            if (codecMode != mode) { resetCodec(); codec = create(mode); codecMode = mode; }
            if (!codec) return;
            const int count = samples(codec);
            if (count != (mode == 0 ? 160 : 320)) return;
            std::array<short, 320> voice{};
            for (int offset = 0; offset < (mode == 0 ? 16 : 8); offset += 8) {
                decode(codec, voice.data(), data + offset);
                // Voice playback consumes 48 kHz; interpolate 8 kHz Codec2 output.
                for (int i = 0; i < count; ++i) {
                    const short a = voice[i], b = voice[(std::min)(i + 1, count - 1)];
                    for (int sub = 0; sub < 6; ++sub) {
                        char bytes[2]; qToLittleEndian<qint16>(static_cast<qint16>(a + (b - a) * sub / 6), reinterpret_cast<uchar *>(bytes));
                        result.voicePcm.append(bytes, 2);
                    }
                }
            }
            if (sequence & 0x8000) { haveLsf = false; lichMask = 0; resetCodec(); }
        }
    }
    float at(qint64 position) const { return history[static_cast<std::size_t>(position - (tick - qint64(history.size()) + 1))]; }
    void consume(float sample, M17AudioResult &result) {
        filter[filterPos] = sample;
        float filtered = 0;
        for (int i = 0; i < 81; ++i) filtered += filter[(filterPos - i + 81) % 81] * rrc_taps_10[i];
        filterPos = (filterPos + 1) % 81;
        ++tick; history.push_back(filtered); if (history.size() > 4096) history.pop_front();
        if (haveLsf && tick - lastFrameTick > 48000) { haveLsf = false; packet.clear(); packetFrames = 0; lichMask = 0; resetCodec(); }
        if (frameKind >= 0) {
            if (tick >= nextSymbol) {
                payload[payloadCount++] = (at(nextSymbol) - bias) / gain; nextSymbol += 10;
                if (payloadCount == 184) { finishFrame(result); frameKind = -1; candidateKind = -1; }
            }
            return;
        }
        if (history.size() < 80) return;
        const int8_t *patterns[] = {lsf_sync_symbols, str_sync_symbols, pkt_sync_symbols};
        const bool stream = lsf.type[1] & M17_TYPE_STREAM;
        for (int kind = 0; kind < 3; ++kind) {
            // A sync-like sequence inside a payload can precede the next frame.
            // Use the active LSF to rule out the other data frame format.
            if (haveLsf && kind == (stream ? 2 : 1)) continue;
            float mx = 0, my = 0;
            for (int i = 0; i < 8; ++i) { mx += patterns[kind][i] / 8.0f; my += at(tick - (7 - i) * 10) / 8.0f; }
            float cov = 0, var = 0, energy = 0;
            for (int i = 0; i < 8; ++i) { float x = patterns[kind][i] - mx, y = at(tick - (7 - i) * 10) - my; cov += x * y; var += x * x; energy += y * y; }
            const float g = cov / var, dc = my - g * mx;
            if (std::abs(g) < 0.002f || energy < 1e-5f) continue;
            if (haveLsf && g * gain < 0.0f) continue;
            const float error = (std::max)(0.0f, 1.0f - cov * cov / (var * energy));
            if (error < 0.035f && (candidateKind < 0 || error < candidateError)) {
                candidateKind = kind; candidateEnd = tick; candidateError = error; candidateGain = g; candidateBias = dc;
            }
        }
        if (candidateKind >= 0 && tick - candidateEnd >= 5) {
            frameKind = candidateKind; gain = candidateGain; bias = candidateBias;
            nextSymbol = candidateEnd + 10; payloadCount = 0;
            candidateKind = -1; candidateError = 1e9f;
        }
    }
};

M17AudioDecoder::M17AudioDecoder() : impl(new Impl) {}
M17AudioDecoder::~M17AudioDecoder() = default;
void M17AudioDecoder::reset() { impl->reset(); }
bool M17AudioDecoder::voiceAvailable() const { return impl->create != nullptr; }
M17AudioResult M17AudioDecoder::process(const QByteArray &pcm, int sampleRate) {
    M17AudioResult result;
    if (sampleRate < 24000 || sampleRate > 192000) return result;
    if (impl->rate != sampleRate) { impl->reset(); impl->rate = sampleRate; }
    for (int offset = 0; offset + 1 < pcm.size(); offset += 2) {
        const float current = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(pcm.constData() + offset)) / 32768.0f;
        const double position = impl->inputPosition++;
        while (impl->nextInputPosition <= position) {
            const float fraction = static_cast<float>(impl->nextInputPosition - (position - 1.0));
            impl->consume(impl->previousInput + (current - impl->previousInput) * (std::clamp)(fraction, 0.0f, 1.0f), result);
            impl->nextInputPosition += sampleRate / 48000.0;
        }
        impl->previousInput = current;
    }
    return result;
}
