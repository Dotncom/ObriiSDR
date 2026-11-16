#include "m17adapter.h"
#include "m17codec2library.h"

extern "C" {
#include "m17.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

namespace {
void setError(QString *error, const QString &value) {
    if (error) *error = value;
}

void appendSymbols(QVector<qint8> *target, const int8_t *symbols, int count) {
    if (!target || !symbols || count <= 0) return;
    target->reserve(target->size() + count);
    for (int i = 0; i < count; ++i) target->push_back(static_cast<qint8>(symbols[i]));
}

QByteArray serializeLsf(const lsf_t &lsf) {
    QByteArray bytes;
    bytes.reserve(30);
    bytes.append(reinterpret_cast<const char *>(lsf.dst), 6);
    bytes.append(reinterpret_cast<const char *>(lsf.src), 6);
    bytes.append(reinterpret_cast<const char *>(lsf.type), 2);
    bytes.append(reinterpret_cast<const char *>(lsf.meta), 14);
    bytes.append(reinterpret_cast<const char *>(lsf.crc), 2);
    return bytes;
}

bool prepareLsf(lsf_t &lsf, const QString &source, const QString &destination, int can,
                uint16_t type, QString *error) {
    if (can < 0 || can > 15) {
        setError(error, QStringLiteral("M17 CAN must be in the range 0..15"));
        return false;
    }
    const QString from = source.trimmed().toUpper();
    const QString to = destination.trimmed().toUpper();
    if (from.isEmpty() || from.size() > 9 || to.isEmpty() || to.size() > 9 ||
        from.contains(QChar(0)) || to.contains(QChar(0))) {
        setError(error, QStringLiteral("M17 source and destination must contain 1..9 characters"));
        return false;
    }
    if (set_LSF(&lsf, from.toLatin1().constData(), to.toLatin1().constData(),
                static_cast<uint16_t>(type | M17_TYPE_CAN(can)), nullptr) != 0) {
        setError(error, QStringLiteral("libm17 rejected the source or destination callsign"));
        return false;
    }
    return true;
}

bool appendLinkSetup(QVector<qint8> &out, const lsf_t &lsf, QString *error) {
    std::array<int8_t, SYM_PER_FRA> symbols{};
    uint32_t count = 0;
    gen_preamble_i8(symbols.data(), &count, PREAM_LSF);
    appendSymbols(&out, symbols.data(), static_cast<int>(count));
    if (gen_frame_i8(symbols.data(), nullptr, FRAME_LSF, &lsf, 0, 0) != 0) {
        setError(error, QStringLiteral("libm17 failed to generate the link setup frame"));
        return false;
    }
    appendSymbols(&out, symbols.data(), SYM_PER_FRA);
    return true;
}

void appendEndOfTransmission(QVector<qint8> &out) {
    std::array<int8_t, SYM_PER_FRA> symbols{};
    uint32_t count = 0;
    gen_eot_i8(symbols.data(), &count);
    appendSymbols(&out, symbols.data(), static_cast<int>(count));
}

constexpr int ShapingRate = M17VoiceStream::IqRate;
constexpr int SamplesPerSymbol = 10;
constexpr int TapCount = 81;
constexpr int CodecFrameSamples = 160;
constexpr int VoiceFrameSamples = M17VoiceStream::FrameSamples;
constexpr qint64 MaximumIqSamples = 20000000;
constexpr double Tau = 6.2831853071795864769;

bool validModulation(float level, int sampleRate) {
    return std::isfinite(level) && level > 0 && level <= 1 &&
           sampleRate >= ShapingRate && sampleRate <= 2000000;
}

qint64 iqSampleCount(int symbols, int sampleRate) {
    const qint64 numerator = qint64(symbols * SamplesPerSymbol + TapCount - 1) * sampleRate;
    return (numerator + ShapingRate - 1) / ShapingRate;
}

bool addShapedSymbols(const QVector<qint8> &symbols, QVector<float> &shaped) {
    for (int i = 0; i < symbols.size(); ++i) {
        const int symbol = symbols[i];
        if (symbol != -3 && symbol != -1 && symbol != 1 && symbol != 3) return false;
        for (int tap = 0; tap < TapCount; ++tap)
            shaped[i * SamplesPerSymbol + tap] += symbol * rrc_taps_10[tap];
    }
    return true;
}

QVector<std::complex<float>> integrateFm(const QVector<float> &shaped, int count,
    float level, int sampleRate, double &phase, bool rampIn, bool rampOut) {
    // Restore the nominal 800 Hz per level after unity-energy RRC shaping.
    const double phasePerLevel = Tau * 800.0 * std::sqrt(SamplesPerSymbol) / sampleRate;
    const double step = double(ShapingRate) / sampleRate;
    const int ramp = sampleRate / 1000;
    QVector<std::complex<float>> iq(count);
    for (int n = 0; n < iq.size(); ++n) {
        const double position = n * step;
        const int index = qMin(int(position), shaped.size() - 1);
        const double fraction = position - index;
        const float next = index + 1 < shaped.size() ? shaped[index + 1] : 0.0f;
        const double deviation = (1.0 - fraction) * shaped[index] + fraction * next;
        int edge = ramp;
        if (rampIn) edge = qMin(edge, n);
        if (rampOut) edge = qMin(edge, iq.size() - 1 - n);
        const float amplitude = level * float(edge) / ramp;
        phase += phasePerLevel * deviation;
        if (phase > Tau || phase < -Tau) phase = std::remainder(phase, Tau);
        iq[n] = std::polar(amplitude, float(phase));
    }
    return iq;
}

QVector<std::complex<float>> modulateSymbols(const QVector<qint8> &symbols, float level, int sampleRate) {
    QVector<float> shaped(symbols.size() * SamplesPerSymbol + TapCount - 1, 0);
    if (!addShapedSymbols(symbols, shaped)) return {};
    double phase = 0.0;
    return integrateFm(shaped, int(iqSampleCount(symbols.size(), sampleRate)), level,
                       sampleRate, phase, true, true);
}

struct VoiceEncoder {
    QLibrary library;
    using Destroy = void (*)(void *);
    using Encode = void (*)(void *, unsigned char *, short *);
    std::unique_ptr<void, Destroy> codec{nullptr, nullptr};
    Encode encode = nullptr;
    lsf_t lsf{};
    uint16_t number = 0;
    uint8_t lich = 0;

    bool begin(const QString &source, const QString &destination, int can,
               QVector<qint8> &symbols, QString *error) {
        if (!prepareLsf(lsf, source, destination, can, M17_TYPE_STREAM | M17_TYPE_VOICE, error))
            return false;
        if (!loadM17Codec2(library, {"codec2_create", "codec2_destroy", "codec2_encode",
                                   "codec2_samples_per_frame", "codec2_bits_per_frame"})) {
            setError(error, QStringLiteral("M17 voice TX requires the Codec2 runtime with encoder support."));
            return false;
        }
        const auto create = reinterpret_cast<void *(*)(int)>(library.resolve("codec2_create"));
        const auto destroy = reinterpret_cast<Destroy>(library.resolve("codec2_destroy"));
        encode = reinterpret_cast<Encode>(library.resolve("codec2_encode"));
        const auto samplesPerFrame = reinterpret_cast<int (*)(void *)>(library.resolve("codec2_samples_per_frame"));
        const auto bitsPerFrame = reinterpret_cast<int (*)(void *)>(library.resolve("codec2_bits_per_frame"));
        constexpr int Codec2Mode3200 = 0;
        codec = std::unique_ptr<void, Destroy>(create(Codec2Mode3200), destroy);
        if (!codec || samplesPerFrame(codec.get()) != CodecFrameSamples || bitsPerFrame(codec.get()) != 64) {
            setError(error, QStringLiteral("Codec2 3200 voice encoding is unavailable."));
            return false;
        }
        return appendLinkSetup(symbols, lsf, error);
    }

    bool appendFrame(QVector<qint8> &symbols, const float *pcm, int count, bool last, QString *error) {
        std::array<uint8_t, 16> data{};
        std::array<short, CodecFrameSamples> speech{};
        int position = 0;
        for (int offset = 0; offset < int(data.size()); offset += 8) {
            for (auto &sample : speech) {
                const float value = position < count ? pcm[position++] : 0.0f;
                if (!std::isfinite(value)) {
                    setError(error, QStringLiteral("M17 voice input contains non-finite audio."));
                    return false;
                }
                sample = static_cast<short>(std::lround((std::clamp)(value, -1.0f, 1.0f) * 32767.0f));
            }
            encode(codec.get(), data.data() + offset, speech.data());
        }
        std::array<int8_t, SYM_PER_FRA> frame{};
        const auto sequence = static_cast<uint16_t>(number | (last ? 0x8000 : 0));
        if (gen_frame_i8(frame.data(), data.data(), FRAME_STR, &lsf, lich, sequence) != 0) {
            setError(error, QStringLiteral("libm17 failed to generate a voice frame."));
            return false;
        }
        appendSymbols(&symbols, frame.data(), SYM_PER_FRA);
        number = (number + 1) & 0x7fff;
        lich = (lich + 1) % 6;
        return true;
    }
};
}

bool M17Adapter::available() {
    return true;
}

struct M17VoiceStream::State {
    VoiceEncoder encoder;
    QVector<float> tail = QVector<float>(TapCount - 1, 0.0f);
    QVector<float> pendingPcm;
    double phase = 0.0;
    float level = 0.0f;

    QVector<std::complex<float>> render(const QVector<qint8> &symbols, bool first, bool last) {
        if (symbols.isEmpty()) return {};
        const int frameLength = symbols.size() * SamplesPerSymbol;
        QVector<float> shaped(frameLength + TapCount - 1, 0.0f);
        std::copy(tail.cbegin(), tail.cend(), shaped.begin());
        if (!addShapedSymbols(symbols, shaped)) return {};
        tail = shaped.mid(frameLength);
        return integrateFm(shaped, last ? shaped.size() : frameLength,
                           level, IqRate, phase, first, last);
    }
};

M17VoiceStream::M17VoiceStream() = default;
M17VoiceStream::~M17VoiceStream() = default;

QVector<std::complex<float>> M17VoiceStream::begin(const QString &source, const QString &destination,
    int can, float level, QString *error) {
    reset();
    if (!validModulation(level, IqRate)) {
        setError(error, QStringLiteral("M17 voice level must be above zero and at most 100%."));
        return {};
    }
    auto next = std::make_unique<State>();
    QVector<qint8> symbols;
    if (!next->encoder.begin(source, destination, can, symbols, error)) return {};
    next->level = level;
    auto iq = next->render(symbols, true, false);
    state = std::move(next);
    setError(error, {});
    return iq;
}

QVector<std::complex<float>> M17VoiceStream::pushAudio(const QVector<float> &mono8k, QString *error) {
    setError(error, {});
    if (!state) return {};
    state->pendingPcm += mono8k;
    QVector<qint8> symbols;
    int offset = 0;
    // Retain a complete frame when possible so it can carry the final flag on release.
    while (state->pendingPcm.size() - offset >= 2 * FrameSamples) {
        if (!state->encoder.appendFrame(symbols, state->pendingPcm.constData() + offset,
                                        FrameSamples, false, error)) {
            reset();
            return {};
        }
        offset += FrameSamples;
    }
    state->pendingPcm.remove(0, offset);
    return state->render(symbols, false, false);
}

QVector<std::complex<float>> M17VoiceStream::finish(QString *error) {
    setError(error, {});
    if (!state) return {};
    auto completed = std::move(state);
    QVector<qint8> symbols;
    const auto &pending = completed->pendingPcm;
    const int count = qMin(pending.size(), FrameSamples);
    const bool oneFrame = pending.size() <= FrameSamples;
    if (!completed->encoder.appendFrame(symbols, pending.constData(), count, oneFrame, error))
        return {};
    if (!oneFrame && !completed->encoder.appendFrame(symbols, pending.constData() + count,
                                                      pending.size() - count, true, error))
        return {};
    appendEndOfTransmission(symbols);
    return completed->render(symbols, false, true);
}

void M17VoiceStream::reset() { state.reset(); }
bool M17VoiceStream::isActive() const { return bool(state); }

QString M17Adapter::libraryVersion() {
    return QString::fromLatin1(LIBM17_VERSION);
}

bool M17Adapter::encodeCallsign(const QString &callsign, QByteArray *encoded, QString *error) {
    if (!encoded) {
        setError(error, QStringLiteral("M17 output callsign buffer is null"));
        return false;
    }
    const QByteArray latin = callsign.trimmed().toUpper().toLatin1();
    if (latin.isEmpty() || latin.size() > 9) {
        setError(error, QStringLiteral("M17 callsign must contain 1..9 characters"));
        return false;
    }
    std::array<uint8_t, 6> bytes{};
    if (encode_callsign_bytes(bytes.data(), latin.constData()) != 0) {
        setError(error, QStringLiteral("libm17 rejected the callsign"));
        return false;
    }
    *encoded = QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()));
    if (error) error->clear();
    return true;
}

QString M17Adapter::decodeCallsign(const QByteArray &encoded, QString *error) {
    if (encoded.size() != 6) {
        setError(error, QStringLiteral("An encoded M17 callsign must contain exactly six bytes"));
        return {};
    }
    std::array<char, 16> decoded{};
    decode_callsign_bytes(decoded.data(), reinterpret_cast<const uint8_t *>(encoded.constData()));
    if (!decoded[0]) {
        setError(error, QStringLiteral("M17 callsign is in a reserved address range"));
        return {};
    }
    if (error) error->clear();
    return QString::fromLatin1(decoded.data());
}

M17PacketSymbols M17Adapter::buildPacket(const QString &source,
                                         const QString &destination,
                                         const QByteArray &payload,
                                         int channelAccessNumber,
                                         QString *error) {
    M17PacketSymbols result;
    if (payload.isEmpty() || payload.size() > 823) {
        setError(error, QStringLiteral("M17 packet payload must contain 1..823 bytes"));
        return result;
    }
    lsf_t lsf{};
    if (!prepareLsf(lsf, source, destination, channelAccessNumber, M17_TYPE_PACKET | M17_TYPE_DATA, error))
        return result;
    if (!appendLinkSetup(result.symbols, lsf, error)) {
        result.symbols.clear();
        return result;
    }

    QByteArray packetData = payload;
    const uint16_t packetCrc = CRC_M17(
        reinterpret_cast<const uint8_t *>(packetData.constData()),
        static_cast<uint16_t>(packetData.size()));
    packetData.append(static_cast<char>((packetCrc >> 8U) & 0xFFU));
    packetData.append(static_cast<char>(packetCrc & 0xFFU));
    const int frameCount = (packetData.size() + 24) / 25;
    std::array<int8_t, SYM_PER_FRA> symbols{};
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        std::array<uint8_t, 26> packet{};
        const int offset = frameIndex * 25;
        const int bytes = qMin(25, packetData.size() - offset);
        std::memcpy(packet.data(), packetData.constData() + offset, static_cast<size_t>(bytes));
        const bool last = frameIndex + 1 == frameCount;
        packet[25] = static_cast<uint8_t>(last
            ? (0x80U | ((bytes & 0x1FU) << 2U))
            : ((frameIndex & 0x1FU) << 2U));
        if (gen_frame_i8(symbols.data(), packet.data(), FRAME_PKT, nullptr, 0, 0) != 0) {
            setError(error, QStringLiteral("libm17 failed to generate packet frame %1").arg(frameIndex));
            result.symbols.clear();
            result.linkSetupFrame.clear();
            return result;
        }
        appendSymbols(&result.symbols, symbols.data(), SYM_PER_FRA);
    }
    appendEndOfTransmission(result.symbols);
    result.linkSetupFrame = serializeLsf(lsf);
    result.packetFrameCount = frameCount;
    if (error) error->clear();
    return result;
}

QVector<std::complex<float>> M17Adapter::modulatePacket(const M17PacketSymbols &packet, float level,
                                                       int sampleRate) {
    if (packet.symbolRate != 4800 || packet.symbols.isEmpty() ||
        packet.symbols.size() > 36 * SYM_PER_FRA || !validModulation(level, sampleRate))
        return {};
    return modulateSymbols(packet.symbols, level, sampleRate);
}

QVector<std::complex<float>> M17Adapter::modulateVoice(const QString &source, const QString &destination,
    int channelAccessNumber, const QVector<float> &pcm, float level, int sampleRate, QString *error) {
    if (pcm.isEmpty() || pcm.size() > MaximumVoiceSamples || !validModulation(level, sampleRate)) {
        setError(error, QStringLiteral("M17 voice requires up to 60 seconds of 8 kHz PCM and a 48 kS/s..2 MS/s output rate"));
        return {};
    }
    const int frames = (pcm.size() + VoiceFrameSamples - 1) / VoiceFrameSamples;
    const int symbolCount = (frames + 3) * SYM_PER_FRA;
    if (iqSampleCount(symbolCount, sampleRate) > MaximumIqSamples) {
        setError(error, QStringLiteral("M17 voice exceeds 20 million IQ samples. Shorten the recording or lower the output rate."));
        return {};
    }
    QVector<qint8> symbols;
    symbols.reserve(symbolCount);
    VoiceEncoder encoder;
    if (!encoder.begin(source, destination, channelAccessNumber, symbols, error)) return {};
    for (int i = 0; i < frames; ++i) {
        const int offset = i * VoiceFrameSamples;
        const int count = qMin(VoiceFrameSamples, pcm.size() - offset);
        if (!encoder.appendFrame(symbols, pcm.constData() + offset, count, i + 1 == frames, error))
            return {};
    }
    appendEndOfTransmission(symbols);
    auto iq = modulateSymbols(symbols, level, sampleRate);
    setError(error, iq.isEmpty() ? QStringLiteral("Could not shape the M17 voice waveform.") : QString{});
    return iq;
}

bool M17Adapter::decodePacketSymbols(const QByteArray &symbols,
                                     M17DecodedPacket *packet, QString *error) {
    if (!packet || symbols.size() < SYM_PER_FRA * 4 ||
        symbols.size() % SYM_PER_FRA != 0 || symbols.size() > SYM_PER_FRA * 36) {
        setError(error, QStringLiteral("Invalid M17 symbol-file length"));
        return false;
    }
    const auto syncMatches = [&symbols](int offset, const int8_t *sync) {
        for (int i = 0; i < SYM_PER_SWD; ++i)
            if (static_cast<qint8>(symbols.at(offset + i)) != sync[i]) return false;
        return true;
    };
    if (!syncMatches(SYM_PER_FRA, lsf_sync_symbols)) {
        setError(error, QStringLiteral("M17 LSF sync word is absent"));
        return false;
    }
    std::array<float, SYM_PER_PLD> soft{};
    const auto payloadSymbols = [&symbols, &soft](int frameOffset) {
        for (int i = 0; i < SYM_PER_PLD; ++i) {
            const qint8 symbol = static_cast<qint8>(symbols.at(frameOffset + SYM_PER_SWD + i));
            if (symbol != -3 && symbol != -1 && symbol != 1 && symbol != 3) return false;
            soft[static_cast<std::size_t>(i)] = static_cast<float>(symbol);
        }
        return true;
    };
    if (!payloadSymbols(SYM_PER_FRA)) {
        setError(error, QStringLiteral("M17 LSF contains invalid symbols"));
        return false;
    }
    lsf_t lsf{};
    decode_LSF(&lsf, soft.data());
    if (CRC_M17(reinterpret_cast<const uint8_t *>(&lsf), sizeof(lsf)) != 0) {
        setError(error, QStringLiteral("M17 LSF CRC failed"));
        return false;
    }
    const int type = (static_cast<int>(lsf.type[0]) << 8) | lsf.type[1];
    if ((type & 1) != M17_TYPE_PACKET || (type & (3 << 3)) != M17_TYPE_ENCR_NONE) {
        setError(error, QStringLiteral("Only unencrypted M17 packet symbol files are supported"));
        return false;
    }
    for (int i = 0; i < SYM_PER_FRA; ++i) {
        if (static_cast<qint8>(symbols.at(symbols.size() - SYM_PER_FRA + i)) !=
            eot_symbols[i % SYM_PER_SWD]) {
            setError(error, QStringLiteral("M17 EOT symbols are missing"));
            return false;
        }
    }

    QByteArray assembled;
    int frameCount = 0;
    bool complete = false;
    for (int offset = SYM_PER_FRA * 2;
         offset + SYM_PER_FRA <= symbols.size() - SYM_PER_FRA; offset += SYM_PER_FRA) {
        if (!syncMatches(offset, pkt_sync_symbols) || !payloadSymbols(offset)) {
            setError(error, QStringLiteral("M17 packet sync or symbols invalid at frame %1").arg(frameCount));
            return false;
        }
        std::array<uint8_t, 25> data{};
        uint8_t eof = 0;
        uint8_t frameNumber = 0;
        decode_pkt_frame(data.data(), &eof, &frameNumber, soft.data());
        const int bytes = eof ? frameNumber : 25;
        if (bytes < 1 || bytes > 25 || (!eof && frameNumber != frameCount % 32)) {
            setError(error, QStringLiteral("M17 packet frame number or final length invalid"));
            return false;
        }
        assembled.append(reinterpret_cast<const char *>(data.data()), bytes);
        ++frameCount;
        if (eof) {
            complete = offset + SYM_PER_FRA * 2 == symbols.size();
            break;
        }
    }
    if (!complete || assembled.size() < 3 ||
        CRC_M17(reinterpret_cast<const uint8_t *>(assembled.constData()),
                static_cast<uint16_t>(assembled.size())) != 0) {
        setError(error, QStringLiteral("M17 packet is incomplete or CRC failed"));
        return false;
    }
    assembled.chop(2);
    packet->source = decodeCallsign(serializeLsf(lsf).mid(6, 6));
    packet->destination = decodeCallsign(serializeLsf(lsf).left(6));
    packet->channelAccessNumber = (type >> 7) & 0x0f;
    packet->payload = assembled;
    packet->packetFrameCount = frameCount;
    if (error) error->clear();
    return true;
}
