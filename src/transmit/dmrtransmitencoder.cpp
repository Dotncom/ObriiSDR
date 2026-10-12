#include "dmrtransmitencoder.h"

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {

constexpr double Pi = 3.14159265358979323846;
constexpr int DmrSymbolRate = 4800;
constexpr int InternalSampleRate = 24000;
constexpr int SamplesPerSymbol = 5;
constexpr int ActiveBurstBytes = 33;
constexpr int FillBytes = 39;
constexpr int VoiceLcHeaderType = 0x01;
constexpr int TerminatorWithLcType = 0x02;
constexpr int HeaderRepeats = 3;
constexpr int TerminatorRepeats = 2;
constexpr int TxPreambleBytes = 240;

const std::array<quint8, 7> DirectTs1VoiceSync = {0x05, 0xD5, 0x77, 0xF7, 0x75, 0x7F, 0xF0};
const std::array<quint8, 7> DirectTs1DataSync = {0x0F, 0x7F, 0xDD, 0x5D, 0xDF, 0xD5, 0x50};
const std::array<quint8, 7> DirectTs2VoiceSync = {0x07, 0xDF, 0xFD, 0x5F, 0x55, 0xD5, 0xF0};
const std::array<quint8, 7> DirectTs2DataSync = {0x0D, 0x75, 0x57, 0xF5, 0xFF, 0x7F, 0x50};
const std::array<quint8, FillBytes> PrFill = {
    0x63, 0xEA, 0x00, 0x76, 0x6C, 0x76, 0xC4, 0x52, 0xC8, 0x78,
    0x09, 0x2D, 0xB8, 0x79, 0x27, 0x57, 0x9B, 0x31, 0xBC, 0x3E,
    0xEA, 0x45, 0xC3, 0x30, 0x49, 0x17, 0x93, 0xAE, 0x8B, 0x6D,
    0xA4, 0xA5, 0xAD, 0xA2, 0xF1, 0x35, 0xB5, 0x3C, 0x1E
};
const std::array<int, 45> Rrc02 = {
    0, 0, 0, 0, 850, 219, -720, -1548, -1795, -1172, 237, 1927, 3120,
    3073, 1447, -1431, -4544, -6442, -5735, -1633, 5651, 14822, 23810,
    30367, 32767, 30367, 23810, 14822, 5651, -1633, -5735, -6442, -4544,
    -1431, 1447, 3073, 3120, 1927, 237, -1172, -1795, -1548, -720, 219, 850
};
const std::array<int, 24> AmbeATable = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44,
    48, 52, 56, 60, 64, 68, 1, 5, 9, 13, 17, 21
};
const std::array<int, 23> AmbeBTable = {
    25, 29, 33, 37, 41, 45, 49, 53, 57, 61, 65, 69,
    2, 6, 10, 14, 18, 22, 26, 30, 34, 38, 42
};
const std::array<int, 25> AmbeCTable = {
    46, 50, 54, 58, 62, 66, 70, 3, 7, 11, 15, 19, 23,
    27, 31, 35, 39, 43, 47, 51, 55, 59, 63, 67, 71
};

bool bitAt(const QByteArray &bytes, int bit) {
    return bit >= 0 && bit < bytes.size() * 8 &&
           (static_cast<quint8>(bytes.at(bit / 8)) & (0x80U >> (bit % 8))) != 0;
}

void setBit(QByteArray &bytes, int bit, bool value) {
    if (bit < 0 || bit >= bytes.size() * 8) return;
    quint8 byte = static_cast<quint8>(bytes.at(bit / 8));
    const quint8 mask = static_cast<quint8>(0x80U >> (bit % 8));
    byte = value ? static_cast<quint8>(byte | mask)
                 : static_cast<quint8>(byte & static_cast<quint8>(~mask));
    bytes[bit / 8] = static_cast<char>(byte);
}

int parity(quint32 value) {
    int result = 0;
    while (value != 0U) {
        result ^= static_cast<int>(value & 1U);
        value >>= 1U;
    }
    return result;
}

quint32 polynomialRemainder(quint32 value, int topBit, int polynomial, int degree) {
    for (int bit = topBit; bit >= degree; --bit) {
        if ((value & (1U << bit)) != 0U) {
            value ^= static_cast<quint32>(polynomial) << (bit - degree);
        }
    }
    return value & ((1U << degree) - 1U);
}

quint32 golay2087(quint8 value) {
    const quint32 shifted = static_cast<quint32>(value) << 11U;
    const quint32 code19 = shifted | polynomialRemainder(shifted, 18, 0x0C75, 11);
    return (code19 << 1U) | static_cast<quint32>(parity(code19));
}

quint16 qr1676(quint8 value) {
    value &= 0x7FU;
    const quint32 shifted = static_cast<quint32>(value) << 8U;
    const quint32 code15 = shifted | polynomialRemainder(shifted, 14, 0x0139, 8);
    return static_cast<quint16>((code15 << 1U) | static_cast<quint32>(parity(code15)));
}

quint8 gfMultiply(quint8 a, quint8 b) {
    quint8 result = 0U;
    for (int bit = 0; bit < 8; ++bit) {
        if ((b & 1U) != 0U) result ^= a;
        const bool high = (a & 0x80U) != 0U;
        a = static_cast<quint8>(a << 1U);
        if (high) a ^= 0x1DU;
        b = static_cast<quint8>(b >> 1U);
    }
    return result;
}

std::array<quint8, 3> rs129(const QByteArray &message) {
    std::array<quint8, 3> p{};
    const int count = (std::min)(9, message.size());
    for (int i = 0; i < count; ++i) {
        const quint8 d = static_cast<quint8>(message.at(i)) ^ p[2];
        p[2] = p[1] ^ gfMultiply(14U, d);
        p[1] = p[0] ^ gfMultiply(56U, d);
        p[0] = gfMultiply(64U, d);
    }
    return p;
}

void hamming15113(std::array<bool, 196> &data, int offset) {
    bool *d = data.data() + offset;
    d[11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    d[12] = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9];
    d[13] = d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10];
    d[14] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10];
}

void hamming1393(std::array<bool, 13> &d) {
    d[9] = d[0] ^ d[1] ^ d[3] ^ d[5] ^ d[6];
    d[10] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7];
    d[11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    d[12] = d[0] ^ d[2] ^ d[4] ^ d[5] ^ d[8];
}

void hamming16114(std::array<bool, 128> &data, int offset) {
    bool *d = data.data() + offset;
    d[11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    d[12] = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9];
    d[13] = d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10];
    d[14] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10];
    d[15] = d[0] ^ d[2] ^ d[5] ^ d[6] ^ d[8] ^ d[9] ^ d[10];
}

std::array<bool, 196> bptc19696(const QByteArray &payload) {
    std::array<bool, 196> data{};
    const std::array<std::pair<int, int>, 9> ranges = {{{4, 11}, {16, 26}, {31, 41},
        {46, 56}, {61, 71}, {76, 86}, {91, 101}, {106, 116}, {121, 131}}};
    int payloadBit = 0;
    for (const auto &range : ranges) {
        for (int bit = range.first; bit <= range.second; ++bit) {
            data[static_cast<std::size_t>(bit)] = bitAt(payload, payloadBit++);
        }
    }
    for (int row = 0; row < 9; ++row) hamming15113(data, row * 15 + 1);
    for (int column = 0; column < 15; ++column) {
        std::array<bool, 13> col{};
        for (int row = 0; row < 13; ++row) col[static_cast<std::size_t>(row)] = data[static_cast<std::size_t>(column + 1 + row * 15)];
        hamming1393(col);
        for (int row = 0; row < 13; ++row) data[static_cast<std::size_t>(column + 1 + row * 15)] = col[static_cast<std::size_t>(row)];
    }
    std::array<bool, 196> raw{};
    for (int bit = 0; bit < 196; ++bit) raw[static_cast<std::size_t>((bit * 181) % 196)] = data[static_cast<std::size_t>(bit)];
    return raw;
}

QByteArray lcPayload(const DmrTxParameters &p) {
    QByteArray lc(9, '\0');
    lc[0] = static_cast<char>(p.privateCall ? 0x03 : 0x00);
    lc[1] = 0x00;
    lc[2] = 0x00;
    lc[3] = static_cast<char>((p.destinationId >> 16U) & 0xFFU);
    lc[4] = static_cast<char>((p.destinationId >> 8U) & 0xFFU);
    lc[5] = static_cast<char>(p.destinationId & 0xFFU);
    lc[6] = static_cast<char>((p.sourceId >> 16U) & 0xFFU);
    lc[7] = static_cast<char>((p.sourceId >> 8U) & 0xFFU);
    lc[8] = static_cast<char>(p.sourceId & 0xFFU);
    return lc;
}

std::array<bool, 128> embeddedLc(const DmrTxParameters &p) {
    const QByteArray lc = lcPayload(p);
    int checksum = 0;
    for (char value : lc) checksum += static_cast<quint8>(value);
    checksum %= 31;
    std::array<bool, 128> data{};
    data[106] = (checksum & 0x01) != 0;
    data[90] = (checksum & 0x02) != 0;
    data[74] = (checksum & 0x04) != 0;
    data[58] = (checksum & 0x08) != 0;
    data[42] = (checksum & 0x10) != 0;
    const std::array<std::pair<int, int>, 7> ranges = {{{0, 10}, {16, 26}, {32, 41},
        {48, 57}, {64, 73}, {80, 89}, {96, 105}}};
    int sourceBit = 0;
    for (const auto &range : ranges) {
        for (int bit = range.first; bit <= range.second; ++bit) data[static_cast<std::size_t>(bit)] = bitAt(lc, sourceBit++);
    }
    for (int row = 0; row < 7; ++row) hamming16114(data, row * 16);
    for (int column = 0; column < 16; ++column) {
        bool value = false;
        for (int row = 0; row < 7; ++row) value ^= data[static_cast<std::size_t>(column + row * 16)];
        data[static_cast<std::size_t>(112 + column)] = value;
    }
    std::array<bool, 128> raw{};
    int position = 0;
    for (int bit = 0; bit < 128; ++bit) {
        raw[static_cast<std::size_t>(bit)] = data[static_cast<std::size_t>(position)];
        position += 16;
        if (position > 127) position -= 127;
    }
    return raw;
}

void addSync(QByteArray &burst, const std::array<quint8, 7> &sync) {
    for (int bit = 0; bit < 48; ++bit) {
        const int sourceBit = bit + 4;
        const bool value = (sync[static_cast<std::size_t>(sourceBit / 8)] & (0x80U >> (sourceBit % 8))) != 0;
        setBit(burst, 108 + bit, value);
    }
}

void addSlotType(QByteArray &burst, const DmrTxParameters &p, int dataType) {
    const quint8 value = static_cast<quint8>(((p.colorCode & 0x0F) << 4) | (dataType & 0x0F));
    const quint32 code = golay2087(value);
    for (int bit = 0; bit < 10; ++bit) setBit(burst, 98 + bit, (code & (1U << (19 - bit))) != 0U);
    for (int bit = 0; bit < 10; ++bit) setBit(burst, 156 + bit, (code & (1U << (9 - bit))) != 0U);
}

void addEmb(QByteArray &burst, int colorCode, int lcss) {
    const quint8 value = static_cast<quint8>(((colorCode & 0x0F) << 3) | (lcss & 0x03));
    const quint16 code = qr1676(value);
    for (int bit = 0; bit < 8; ++bit) setBit(burst, 108 + bit, (code & (1U << (15 - bit))) != 0U);
    for (int bit = 0; bit < 8; ++bit) setBit(burst, 148 + bit, (code & (1U << (7 - bit))) != 0U);
}

QByteArray withFill(const QByteArray &burst) {
    QByteArray result = burst;
    result.reserve(ActiveBurstBytes + FillBytes);
    for (quint8 value : PrFill) result.append(static_cast<char>(value));
    return result;
}

} // namespace

DmrTransmitEncoder::DmrTransmitEncoder() = default;

bool DmrTransmitEncoder::canEncodeVoice() const {
    return vocoder.canEncode();
}

bool DmrTransmitEncoder::configure(const DmrTxParameters &next, QString *error) {
    if (next.outputSampleRate < InternalSampleRate || next.outputSampleRate > 1000000) {
        if (error) *error = QStringLiteral("DMR generator rate must be between 24 kHz and 1 MHz");
        return false;
    }
    if (next.colorCode < 0 || next.colorCode > 15 || next.timeslot < 1 || next.timeslot > 2 ||
        next.sourceId == 0 || next.sourceId > 0xFFFFFFU ||
        next.destinationId == 0 || next.destinationId > 0xFFFFFFU) {
        if (error) *error = QStringLiteral("Invalid DMR Color Code, time slot or 24-bit radio ID");
        return false;
    }
    if (!vocoder.canEncode()) {
        if (error) *error = QStringLiteral("No DMR AMBE+2 encoder backend is available (OpenDMR backend required)");
        return false;
    }
    parameters = next;
    parameters.level = (std::clamp)(parameters.level, 0.0f, 0.95f);
    configured = true;
    resetCallState();
    return true;
}

bool DmrTransmitEncoder::isActive() const { return active; }

void DmrTransmitEncoder::resetCallState() {
    audioInput.clear();
    pcm8k.clear();
    pendingCanonicalFrames.clear();
    audioCursor = 0.0;
    audioLowpass1 = 0.0;
    audioLowpass2 = 0.0;
    symbolHistory.fill(0.0f);
    phase = 0.0;
    voiceBurstIndex = 0;
    active = false;
    vocoder.reset();
}

void DmrTransmitEncoder::reset() {
    resetCallState();
    configured = false;
}

QVector<std::complex<float>> DmrTransmitEncoder::begin(QString *error) {
    if (!configured) {
        if (error) *error = QStringLiteral("DMR encoder is not configured");
        return {};
    }
    resetCallState();
    active = true;
    QVector<std::complex<float>> result;
    appendIq(result, modulateBytes(QByteArray(TxPreambleBytes, static_cast<char>(0x5F))));
    const QByteArray header = buildLinkControlBurst(parameters, false);
    for (int repeat = 0; repeat < HeaderRepeats; ++repeat) appendIq(result, modulateBurst(header));
    return result;
}

QByteArray DmrTransmitEncoder::linkControlPayload() const { return lcPayload(parameters); }

QByteArray DmrTransmitEncoder::buildLinkControlBurst(const DmrTxParameters &p, bool terminator) {
    QByteArray lc = lcPayload(p);
    const std::array<quint8, 3> rs = rs129(lc);
    const quint8 mask = terminator ? 0x99U : 0x96U;
    lc.append(static_cast<char>(rs[2] ^ mask));
    lc.append(static_cast<char>(rs[1] ^ mask));
    lc.append(static_cast<char>(rs[0] ^ mask));
    const std::array<bool, 196> coded = bptc19696(lc);
    QByteArray burst(ActiveBurstBytes, '\0');
    for (int bit = 0; bit < 98; ++bit) setBit(burst, bit, coded[static_cast<std::size_t>(bit)]);
    for (int bit = 98; bit < 196; ++bit) setBit(burst, bit + 68, coded[static_cast<std::size_t>(bit)]);
    addSlotType(burst, p, terminator ? TerminatorWithLcType : VoiceLcHeaderType);
    addSync(burst, p.timeslot == 2 ? DirectTs2DataSync : DirectTs1DataSync);
    return burst;
}

QByteArray DmrTransmitEncoder::canonicalToOnAir(const QByteArray &canonicalFrame) {
    if (canonicalFrame.size() != 9) return {};
    QByteArray result(9, '\0');
    for (int bit = 0; bit < 24; ++bit) setBit(result, AmbeATable[static_cast<std::size_t>(bit)], bitAt(canonicalFrame, bit));
    for (int bit = 0; bit < 23; ++bit) setBit(result, AmbeBTable[static_cast<std::size_t>(bit)], bitAt(canonicalFrame, 24 + bit));
    for (int bit = 0; bit < 25; ++bit) setBit(result, AmbeCTable[static_cast<std::size_t>(bit)], bitAt(canonicalFrame, 47 + bit));
    return result;
}

QByteArray DmrTransmitEncoder::buildVoiceBurst(const DmrTxParameters &p,
                                                int burstIndex,
                                                const std::array<QByteArray, 3> &canonicalFrames) {
    std::array<QByteArray, 3> frames = {canonicalToOnAir(canonicalFrames[0]),
                                        canonicalToOnAir(canonicalFrames[1]),
                                        canonicalToOnAir(canonicalFrames[2])};
    if (frames[0].size() != 9 || frames[1].size() != 9 || frames[2].size() != 9) return {};
    QByteArray burst(ActiveBurstBytes, '\0');
    for (int bit = 0; bit < 72; ++bit) setBit(burst, bit, bitAt(frames[0], bit));
    for (int bit = 0; bit < 36; ++bit) setBit(burst, 72 + bit, bitAt(frames[1], bit));
    for (int bit = 36; bit < 72; ++bit) setBit(burst, 120 + bit, bitAt(frames[1], bit));
    for (int bit = 0; bit < 72; ++bit) setBit(burst, 192 + bit, bitAt(frames[2], bit));

    const int sequence = ((burstIndex % 6) + 6) % 6;
    if (sequence == 0) {
        addSync(burst, p.timeslot == 2 ? DirectTs2VoiceSync : DirectTs1VoiceSync);
    } else {
        int lcss = 0;
        if (sequence >= 1 && sequence <= 4) {
            const std::array<bool, 128> raw = embeddedLc(p);
            for (int bit = 0; bit < 32; ++bit) {
                setBit(burst, 116 + bit, raw[static_cast<std::size_t>((sequence - 1) * 32 + bit)]);
            }
            lcss = sequence == 1 ? 1 : (sequence == 4 ? 2 : 3);
        }
        addEmb(burst, p.colorCode, lcss);
    }
    return burst;
}

QVector<int> DmrTransmitEncoder::burstDibits(const QByteArray &burstWithFill) {
    QVector<int> symbols;
    symbols.reserve(burstWithFill.size() * 4);
    for (char raw : burstWithFill) {
        quint8 value = static_cast<quint8>(raw);
        for (int pair = 0; pair < 4; ++pair) {
            const int bits = (value >> (6 - pair * 2)) & 0x03;
            switch (bits) {
            case 3: symbols.append(3); break;
            case 2: symbols.append(1); break;
            case 0: symbols.append(-1); break;
            default: symbols.append(-3); break;
            }
        }
    }
    return symbols;
}

QVector<std::complex<float>> DmrTransmitEncoder::modulateBytes(const QByteArray &bytes) {
    const QVector<int> symbols = burstDibits(bytes);
    QVector<std::complex<float>> internal;
    internal.reserve(symbols.size() * SamplesPerSymbol);
    for (int symbol : symbols) {
        for (int index = static_cast<int>(symbolHistory.size()) - 1; index > 0; --index) {
            symbolHistory[static_cast<std::size_t>(index)] = symbolHistory[static_cast<std::size_t>(index - 1)];
        }
        symbolHistory[0] = static_cast<float>(symbol);
        for (int sample = 0; sample < SamplesPerSymbol; ++sample) {
            double shaped = 0.0;
            for (int history = 0; history < static_cast<int>(symbolHistory.size()); ++history) {
                shaped += symbolHistory[static_cast<std::size_t>(history)] *
                          static_cast<double>(Rrc02[static_cast<std::size_t>(sample + history * SamplesPerSymbol)]) /
                          32768.0;
            }
            phase = std::remainder(phase + 2.0 * Pi * 648.0 * shaped / InternalSampleRate,
                                   2.0 * Pi);
            internal.append(std::polar(parameters.level, static_cast<float>(phase)));
        }
    }
    if (parameters.outputSampleRate == InternalSampleRate || internal.isEmpty()) return internal;
    const int outputCount = qRound64(static_cast<double>(internal.size()) *
                                      parameters.outputSampleRate / InternalSampleRate);
    QVector<std::complex<float>> output(outputCount);
    const double step = static_cast<double>(InternalSampleRate) / parameters.outputSampleRate;
    for (int index = 0; index < outputCount; ++index) {
        const double position = index * step;
        const int left = (std::min)(internal.size() - 1, static_cast<int>(position));
        const int right = (std::min)(internal.size() - 1, left + 1);
        const float fraction = static_cast<float>(position - left);
        output[index] = internal[left] + (internal[right] - internal[left]) * fraction;
    }
    return output;
}

QVector<std::complex<float>> DmrTransmitEncoder::modulateBurst(const QByteArray &burst) {
    if (burst.size() != ActiveBurstBytes) return {};
    return modulateBytes(withFill(burst));
}

void DmrTransmitEncoder::appendIq(QVector<std::complex<float>> &target,
                                  const QVector<std::complex<float>> &source) const {
    if (source.isEmpty()) return;
    const int oldSize = target.size();
    target.resize(oldSize + source.size());
    std::copy(source.cbegin(), source.cend(), target.begin() + oldSize);
}

bool DmrTransmitEncoder::encodePcmFrame(const std::array<qint16, 160> &pcm, QString *error) {
    const QByteArray canonical = vocoder.encodeCanonicalFrame(pcm.data(), error);
    if (canonical.size() != 9) return false;
    pendingCanonicalFrames.append(canonical);
    return true;
}

QVector<std::complex<float>> DmrTransmitEncoder::encodeReadyAudio(QString *error) {
    QVector<std::complex<float>> result;
    while (pcm8k.size() >= 160) {
        std::array<qint16, 160> frame{};
        std::copy_n(pcm8k.cbegin(), 160, frame.begin());
        pcm8k.remove(0, 160);
        if (!encodePcmFrame(frame, error)) return {};
        if (pendingCanonicalFrames.size() == 3) {
            const std::array<QByteArray, 3> voice = {pendingCanonicalFrames[0],
                                                    pendingCanonicalFrames[1],
                                                    pendingCanonicalFrames[2]};
            appendIq(result, modulateBurst(buildVoiceBurst(parameters, voiceBurstIndex, voice)));
            pendingCanonicalFrames.clear();
            voiceBurstIndex = (voiceBurstIndex + 1) % 6;
        }
    }
    return result;
}

QVector<std::complex<float>> DmrTransmitEncoder::pushAudio(const QVector<float> &audio,
                                                            QString *error) {
    if (!active || audio.isEmpty()) return {};
    const double alpha = 1.0 - std::exp(-2.0 * Pi * 3400.0 / parameters.outputSampleRate);
    audioInput.reserve(audioInput.size() + audio.size());
    for (float sample : audio) {
        const double clamped = (std::clamp)(static_cast<double>(sample), -1.0, 1.0);
        audioLowpass1 += alpha * (clamped - audioLowpass1);
        audioLowpass2 += alpha * (audioLowpass1 - audioLowpass2);
        audioInput.append(static_cast<float>(audioLowpass2));
    }
    const double step = static_cast<double>(parameters.outputSampleRate) / 8000.0;
    while (audioCursor + 1.0 < audioInput.size()) {
        const int left = static_cast<int>(audioCursor);
        const int right = (std::min)(audioInput.size() - 1, left + 1);
        const double fraction = audioCursor - left;
        const double sample = audioInput[left] + (audioInput[right] - audioInput[left]) * fraction;
        pcm8k.append(static_cast<qint16>(std::lround((std::clamp)(sample, -1.0, 1.0) * 32767.0)));
        audioCursor += step;
    }
    const int consumed = (std::min)(audioInput.size(), static_cast<int>(audioCursor));
    if (consumed > 0) {
        audioInput.remove(0, consumed);
        audioCursor -= consumed;
    }
    return encodeReadyAudio(error);
}

QVector<std::complex<float>> DmrTransmitEncoder::finish(QString *error) {
    if (!active) return {};
    QVector<std::complex<float>> result;
    if (!pcm8k.isEmpty()) {
        while (pcm8k.size() < 160) pcm8k.append(0);
        appendIq(result, encodeReadyAudio(error));
        if (error && !error->isEmpty()) { active = false; return {}; }
    }
    while (!pendingCanonicalFrames.isEmpty() && pendingCanonicalFrames.size() < 3) {
        std::array<qint16, 160> silence{};
        if (!encodePcmFrame(silence, error)) { active = false; return {}; }
    }
    if (pendingCanonicalFrames.size() == 3) {
        const std::array<QByteArray, 3> voice = {pendingCanonicalFrames[0],
                                                pendingCanonicalFrames[1],
                                                pendingCanonicalFrames[2]};
        appendIq(result, modulateBurst(buildVoiceBurst(parameters, voiceBurstIndex, voice)));
        pendingCanonicalFrames.clear();
    }
    const QByteArray terminator = buildLinkControlBurst(parameters, true);
    for (int repeat = 0; repeat < TerminatorRepeats; ++repeat) appendIq(result, modulateBurst(terminator));
    active = false;
    audioInput.clear();
    pcm8k.clear();
    return result;
}