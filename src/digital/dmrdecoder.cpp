#include "dmrdecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

#include <QDebug>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QStringList>

#include "diagnosticlogging.h"

namespace {

constexpr double DMR_SYMBOL_RATE = 4800.0;
constexpr int DMR_SYNC_SYMBOLS = 24;
constexpr int DMR_TRAFFIC_SYMBOLS = 132;
constexpr int DMR_SYMBOLS_BEFORE_SYNC = 54;
constexpr int DMR_CACH_SYMBOLS = 12;
constexpr int DMR_CANDIDATE_SYNC_SCORE = 23;
constexpr int DMR_CONFIRMED_SYNC_SCORE = 24;
constexpr int DMR_CANDIDATE_OUTER_SCORE = 16;
constexpr int DMR_CONFIRMED_OUTER_SCORE = 16;
constexpr int DMR_REPORTABLE_CANDIDATE_OUTER_SCORE = 20;
constexpr int DMR_TRUSTED_VOICE_OUTER_SCORE = 17;
constexpr int DMR_STRONG_ANCHOR_OUTER_SCORE = 18;
constexpr int DMR_LOCKED_POLARITY_OUTER_MARGIN = 3;
constexpr int DMR_MAX_BUFFER_SYMBOLS = 500;
constexpr double DMR_REPORT_INTERVAL_SECONDS = 1.0;
constexpr double DMR_LOCK_TIMEOUT_SECONDS = 1.5;
constexpr double DMR_SYNC_MIN_GAP_SECONDS = 0.020;
// Voice sync marks burst A of a DMR voice superframe; B-F are recovered by
// cadence and EMB. Accepting voice sync candidates every slot admits too many
// false anchors and breaks AMBE continuity.
constexpr double DMR_VOICE_SYNC_MIN_GAP_SECONDS = 0.345;
constexpr double DMR_VOICE_ANCHOR_MIN_GAP_SECONDS = 0.345;
constexpr int DMR_EMB_VARIANT_COUNT = 8;
constexpr int DMR_INVALID_AMBE_FEC_ERRORS = 99;
constexpr int DMR_AUDIO_CANDIDATE_MAX_FEC_ERRORS = 2;
constexpr int DMR_AUDIO_CANDIDATE_MAX_RAW_FEC_ERRORS = 5;
constexpr int DMR_SAME_TIMESLOT_CADENCE_SYMBOLS = 288;

const DmrDecoder::SyncPattern DMR_SYNC_PATTERNS[] = {
    {"BS Voice", "voice", "base station", "131111333113313313113313"},
    {"BS Data", "data", "base station", "313333111331131131331131"},
    {"MS Voice", "voice", "mobile station", "133313311131311113313331"},
    {"MS Data", "data", "mobile station", "311131133313133331131113"},
    {"RC Sync", "control", "mobile station", "131331111133133133311313"},
    {"Direct TS1 Voice", "voice", "direct mode", "113111131333131311133333"},
    {"Direct TS2 Voice", "voice", "direct mode", "133133333111331111311133"},
    {"Direct TS1 Data", "data", "direct mode", "331333313111313133311111"},
    {"Direct TS2 Data", "data", "direct mode", "311311111333113333133311"},
};

const int DMR_CACH_DEINTERLEAVE[24] = {
    0, 4, 8, 12, 14, 18, 22, 1,
    2, 3, 5, 6, 7, 9, 10, 11,
    13, 15, 16, 17, 19, 20, 21, 23
};

const int DMR_CACH_HAMMING_743[16] = {
    0, 11, 22, 29, 39, 44, 49, 58,
    69, 78, 83, 88, 98, 105, 116, 127
};

const int DMR_QR_1676_CODEWORDS[128] = {
    0x0000, 0x0273, 0x04E5, 0x0696, 0x09C9, 0x0BBA, 0x0D2C, 0x0F5F,
    0x11E2, 0x1391, 0x1507, 0x1774, 0x182B, 0x1A58, 0x1CCE, 0x1EBD,
    0x21B7, 0x23C4, 0x2552, 0x2721, 0x287E, 0x2A0D, 0x2C9B, 0x2EE8,
    0x3055, 0x3226, 0x34B0, 0x36C3, 0x399C, 0x3BEF, 0x3D79, 0x3F0A,
    0x411E, 0x436D, 0x45FB, 0x4788, 0x48D7, 0x4AA4, 0x4C32, 0x4E41,
    0x50FC, 0x528F, 0x5419, 0x566A, 0x5935, 0x5B46, 0x5DD0, 0x5FA3,
    0x60A9, 0x62DA, 0x644C, 0x663F, 0x6960, 0x6B13, 0x6D85, 0x6FF6,
    0x714B, 0x7338, 0x75AE, 0x77DD, 0x7882, 0x7AF1, 0x7C67, 0x7E14,
    0x804F, 0x823C, 0x84AA, 0x86D9, 0x8986, 0x8BF5, 0x8D63, 0x8F10,
    0x91AD, 0x93DE, 0x9548, 0x973B, 0x9864, 0x9A17, 0x9C81, 0x9EF2,
    0xA1F8, 0xA38B, 0xA51D, 0xA76E, 0xA831, 0xAA42, 0xACD4, 0xAEA7,
    0xB01A, 0xB269, 0xB4FF, 0xB68C, 0xB9D3, 0xBBA0, 0xBD36, 0xBF45,
    0xC151, 0xC322, 0xC5B4, 0xC7C7, 0xC898, 0xCAEB, 0xCC7D, 0xCE0E,
    0xD0B3, 0xD2C0, 0xD456, 0xD625, 0xD97A, 0xDB09, 0xDD9F, 0xDFEC,
    0xE0E6, 0xE295, 0xE403, 0xE670, 0xE92F, 0xEB5C, 0xEDCA, 0xEFB9,
    0xF104, 0xF377, 0xF5E1, 0xF792, 0xF8CD, 0xFABE, 0xFC28, 0xFE5B
};

qint16 readPcm16Le(const char *data) {
    qint16 value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

double syncQualityDb(int score) {
    const int errors = DMR_SYNC_SYMBOLS - score;
    const double good = static_cast<double>(score) + 0.5;
    const double bad = static_cast<double>(errors) + 0.5;
    return 10.0 * std::log10(good / bad);
}

float averagedSymbolSample(const std::deque<float> &samples, int centerIndex, int samplesPerSymbol) {
    if (centerIndex < 0 || centerIndex >= static_cast<int>(samples.size())) {
        return 0.0f;
    }

    const int radius = (std::max)(1, (std::min)(2, samplesPerSymbol / 5));
    double sum = 0.0;
    double weightSum = 0.0;
    for (int offset = -radius; offset <= radius; ++offset) {
        const int index = centerIndex + offset;
        if (index < 0 || index >= static_cast<int>(samples.size())) {
            continue;
        }
        const double weight = 1.0 / (1.0 + std::abs(offset));
        sum += weight * samples[static_cast<std::size_t>(index)];
        weightSum += weight;
    }
    return weightSum > 0.0
               ? static_cast<float>(sum / weightSum)
               : samples[static_cast<std::size_t>(centerIndex)];
}

struct DmrDibitSlicer {
    bool valid = false;
    std::array<float, 4> levels = {};
    float range = 0.0f;
    float minSeparation = 0.0f;
};

DmrDibitSlicer buildDmrDibitSlicer(const std::vector<float> &symbolSamples) {
    DmrDibitSlicer slicer;
    std::vector<float> sorted;
    sorted.reserve(symbolSamples.size());
    for (const float sample : symbolSamples) {
        if (std::isfinite(sample)) {
            sorted.push_back(sample);
        }
    }
    if (sorted.size() < 24) {
        return slicer;
    }

    std::sort(sorted.begin(), sorted.end());
    const std::size_t n = sorted.size();
    const auto binMean = [&sorted, n](int bin) {
        std::size_t begin = static_cast<std::size_t>(bin) * n / 4;
        std::size_t end = static_cast<std::size_t>(bin + 1) * n / 4;
        begin = (std::min)(begin, n - 1);
        end = (std::max)(end, begin + 1);
        end = (std::min)(end, n);
        double sum = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            sum += sorted[i];
        }
        return static_cast<float>(sum / static_cast<double>(end - begin));
    };

    std::array<float, 4> levels = {
        binMean(0),
        binMean(1),
        binMean(2),
        binMean(3),
    };
    std::sort(levels.begin(), levels.end());

    for (int iteration = 0; iteration < 8; ++iteration) {
        std::array<double, 4> sums = {};
        std::array<int, 4> counts = {};
        for (const float sample : sorted) {
            int nearest = 0;
            float nearestDistance = std::abs(sample - levels[0]);
            for (int i = 1; i < 4; ++i) {
                const float distance = std::abs(sample - levels[static_cast<std::size_t>(i)]);
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearest = i;
                }
            }
            sums[static_cast<std::size_t>(nearest)] += sample;
            ++counts[static_cast<std::size_t>(nearest)];
        }

        for (int i = 0; i < 4; ++i) {
            if (counts[static_cast<std::size_t>(i)] > 0) {
                levels[static_cast<std::size_t>(i)] =
                    static_cast<float>(sums[static_cast<std::size_t>(i)] /
                                       counts[static_cast<std::size_t>(i)]);
            } else {
                const std::size_t quantile =
                    (std::min)(n - 1, static_cast<std::size_t>((2 * i + 1) * n / 8));
                levels[static_cast<std::size_t>(i)] = sorted[quantile];
            }
        }
        std::sort(levels.begin(), levels.end());
    }

    slicer.levels = levels;
    slicer.range = levels[3] - levels[0];
    slicer.minSeparation = (std::min)(
        (std::min)(levels[1] - levels[0], levels[2] - levels[1]),
        levels[3] - levels[2]);
    slicer.valid = slicer.range >= 0.004f &&
                   slicer.minSeparation >= (std::max)(0.0005f, slicer.range * 0.035f);
    return slicer;
}

int dibitFromDmrLevel(const DmrDibitSlicer &slicer, float sample) {
    int nearest = 0;
    float nearestDistance = std::abs(sample - slicer.levels[0]);
    for (int i = 1; i < 4; ++i) {
        const float distance = std::abs(sample - slicer.levels[static_cast<std::size_t>(i)]);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = i;
        }
    }

    switch (nearest) {
    case 0:
        return 1;
    case 1:
        return 0;
    case 2:
        return 2;
    case 3:
    default:
        return 3;
    }
}

std::uint8_t reliabilityFromDistance(float distance, float unit) {
    if (!std::isfinite(distance) || distance <= 0.0f) {
        return 0;
    }
    const float normalized = distance / (std::max)(unit, 0.0001f);
    return static_cast<std::uint8_t>(
        (std::clamp)(static_cast<int>(std::lround(normalized * 220.0f)), 0, 255));
}

struct DmrDibitDecision {
    int dibit = 0;
    std::uint8_t firstReliability = 0;
    std::uint8_t secondReliability = 0;
};

DmrDibitDecision decideDmrDibit(const DmrDibitSlicer &slicer, float sample) {
    DmrDibitDecision decision;
    int nearest = 0;
    float nearestDistance = std::abs(sample - slicer.levels[0]);
    for (int i = 1; i < 4; ++i) {
        const float distance = std::abs(sample - slicer.levels[static_cast<std::size_t>(i)]);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = i;
        }
    }

    decision.dibit = dibitFromDmrLevel(slicer, sample);
    const float unit = (std::max)(0.5f * slicer.minSeparation, 0.0001f);
    const float signBoundary = 0.5f * (slicer.levels[1] + slicer.levels[2]);
    const float sideBoundary = nearest < 2
                                   ? 0.5f * (slicer.levels[0] + slicer.levels[1])
                                   : 0.5f * (slicer.levels[2] + slicer.levels[3]);
    decision.firstReliability = reliabilityFromDistance(std::abs(sample - signBoundary), unit);
    decision.secondReliability = reliabilityFromDistance(std::abs(sample - sideBoundary), unit);
    return decision;
}

int scoreSyncSymbols(const std::deque<float> &samples,
                     int startIndex,
                     int samplesPerSymbol,
                     const char *pattern,
                     bool inverted,
                     int *outerScore = nullptr) {
    float minLevel = std::numeric_limits<float>::max();
    float maxLevel = std::numeric_limits<float>::lowest();
    float symbolSamples[DMR_SYNC_SYMBOLS] = {};
    for (int i = 0; i < DMR_SYNC_SYMBOLS; ++i) {
        const int sampleIndex = startIndex + i * samplesPerSymbol + samplesPerSymbol / 2;
        if (sampleIndex < 0 || sampleIndex >= static_cast<int>(samples.size())) {
            return 0;
        }
        const float sample = averagedSymbolSample(samples, sampleIndex, samplesPerSymbol);
        const float corrected = inverted ? -sample : sample;
        symbolSamples[i] = corrected;
        minLevel = (std::min)(minLevel, corrected);
        maxLevel = (std::max)(maxLevel, corrected);
    }

    if (maxLevel - minLevel < 0.004f) {
        return 0;
    }

    const float center = 0.5f * (maxLevel + minLevel);
    const float outerRatio = 0.55f;
    const float upperOuter = center + outerRatio * (maxLevel - center);
    const float lowerOuter = center + outerRatio * (minLevel - center);

    int score = 0;
    int outer = 0;
    for (int i = 0; i < DMR_SYNC_SYMBOLS; ++i) {
        if (pattern[i] == '3') {
            if (symbolSamples[i] > center) {
                ++score;
            }
            if (symbolSamples[i] > upperOuter) {
                ++outer;
            }
        } else if (pattern[i] == '1') {
            if (symbolSamples[i] < center) {
                ++score;
            }
            if (symbolSamples[i] < lowerOuter) {
                ++outer;
            }
        }
    }
    if (outerScore) {
        *outerScore = outer;
    }
    return score;
}

QString cachLcssText(int lcss) {
    switch (lcss) {
    case 0:
        return QStringLiteral("first CSBK");
    case 1:
        return QStringLiteral("first LC");
    case 2:
        return QStringLiteral("last LC");
    case 3:
        return QStringLiteral("continuation LC/CSBK");
    default:
        return QStringLiteral("unknown");
    }
}

QString dmrFrequencySuffix(double frequencyHz) {
    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0) {
        return QString();
    }
    if (frequencyHz >= 1000000.0) {
        return QStringLiteral(" @ %1 MHz").arg(frequencyHz / 1000000.0, 0, 'f', 6);
    }
    if (frequencyHz >= 1000.0) {
        return QStringLiteral(" @ %1 kHz").arg(frequencyHz / 1000.0, 0, 'f', 3);
    }
    return QStringLiteral(" @ %1 Hz").arg(frequencyHz, 0, 'f', 0);
}

bool hamming743Ok(int value) {
    return std::find(std::begin(DMR_CACH_HAMMING_743),
                     std::end(DMR_CACH_HAMMING_743),
                     value) != std::end(DMR_CACH_HAMMING_743);
}

std::array<bool, 12> golay208Parity(const std::array<bool, 8> &bits) {
    std::array<bool, 12> p = {};
    p[0] = bits[1] ^ bits[4] ^ bits[5] ^ bits[6] ^ bits[7];
    p[1] = bits[1] ^ bits[2] ^ bits[4];
    p[2] = bits[0] ^ bits[2] ^ bits[3] ^ bits[5];
    p[3] = bits[0] ^ bits[1] ^ bits[3] ^ bits[4] ^ bits[6];
    p[4] = bits[0] ^ bits[1] ^ bits[2] ^ bits[4] ^ bits[5] ^ bits[7];
    p[5] = bits[0] ^ bits[2] ^ bits[3] ^ bits[4] ^ bits[7];
    p[6] = bits[3] ^ bits[6] ^ bits[7];
    p[7] = bits[0] ^ bits[1] ^ bits[5] ^ bits[6];
    p[8] = bits[0] ^ bits[1] ^ bits[2] ^ bits[6] ^ bits[7];
    p[9] = bits[2] ^ bits[3] ^ bits[4] ^ bits[5] ^ bits[6];
    p[10] = bits[0] ^ bits[3] ^ bits[4] ^ bits[5] ^ bits[6] ^ bits[7];
    p[11] = bits[1] ^ bits[2] ^ bits[3] ^ bits[5] ^ bits[7];
    return p;
}

std::array<bool, 20> golay208Codeword(int data) {
    std::array<bool, 20> bits = {};
    for (int i = 0; i < 8; ++i) {
        bits[i] = ((data >> (7 - i)) & 0x01) != 0;
    }
    std::array<bool, 8> dataBits = {};
    std::copy(bits.begin(), bits.begin() + 8, dataBits.begin());
    const std::array<bool, 12> parity = golay208Parity(dataBits);
    for (int i = 0; i < 12; ++i) {
        bits[8 + i] = parity[i];
    }
    return bits;
}

int bitDistance(const std::array<bool, 20> &a, const std::array<bool, 20> &b) {
    int errors = 0;
    for (int i = 0; i < 20; ++i) {
        if (a[i] != b[i]) {
            ++errors;
        }
    }
    return errors;
}

QString dmrDataTypeName(int dataType) {
    switch (dataType) {
    case 0:
        return QStringLiteral("PI Header");
    case 1:
        return QStringLiteral("Voice LC Header");
    case 2:
        return QStringLiteral("Terminator with LC");
    case 3:
        return QStringLiteral("CSBK");
    case 4:
        return QStringLiteral("MBC Header");
    case 5:
        return QStringLiteral("MBC Continuation");
    case 6:
        return QStringLiteral("Data Header");
    case 7:
        return QStringLiteral("Rate 1/2 Data");
    case 8:
        return QStringLiteral("Rate 3/4 Data");
    case 9:
        return QStringLiteral("Idle");
    case 10:
        return QStringLiteral("Rate 1 Data");
    case 11:
        return QStringLiteral("Unified Single Block");
    default:
        return QStringLiteral("Reserved");
    }
}

bool qr1676Decode(const std::array<bool, 16> &receivedBits, int &data, int &errors) {
    const auto popcountInt = [](int value) {
        int count = 0;
        while (value != 0) {
            count += value & 0x01;
            value >>= 1;
        }
        return count;
    };

    int received = 0;
    for (int i = 0; i < 16; ++i) {
        received = (received << 1) | (receivedBits[i] ? 1 : 0);
    }

    int bestData = 0;
    int bestErrors = 17;
    for (int candidateData = 0; candidateData < 128; ++candidateData) {
        const int code16 = DMR_QR_1676_CODEWORDS[candidateData] & 0xffff;
        const int delta = received ^ code16;
        const int distance = popcountInt(delta);
        if (distance < bestErrors) {
            bestErrors = distance;
            bestData = candidateData;
        }
    }

    if (bestErrors > 2) {
        return false;
    }

    data = bestData;
    errors = bestErrors;
    return true;
}

QString embVariantName(int variant) {
    switch (variant) {
    case 0:
        return QStringLiteral("direct");
    case 1:
        return QStringLiteral("reverse16");
    case 2:
        return QStringLiteral("swap8");
    case 3:
        return QStringLiteral("reverse8");
    case 4:
        return QStringLiteral("swap+reverse8");
    case 5:
        return QStringLiteral("swapDibitBits");
    case 6:
        return QStringLiteral("reverseDibits");
    case 7:
        return QStringLiteral("reverseDibits+swapBits");
    default:
        return QStringLiteral("unknown");
    }
}

std::array<bool, 16> embBitsVariant(const std::array<bool, 16> &bits, int variant) {
    std::array<bool, 16> out = {};
    switch (variant) {
    case 1:
        for (int i = 0; i < 16; ++i) {
            out[i] = bits[15 - i];
        }
        break;
    case 2:
        for (int i = 0; i < 16; ++i) {
            out[i] = bits[(i + 8) & 0x0f];
        }
        break;
    case 3:
        for (int i = 0; i < 8; ++i) {
            out[i] = bits[7 - i];
            out[8 + i] = bits[15 - i];
        }
        break;
    case 4:
        for (int i = 0; i < 8; ++i) {
            out[i] = bits[15 - i];
            out[8 + i] = bits[7 - i];
        }
        break;
    case 5:
        for (int i = 0; i < 8; ++i) {
            out[i * 2] = bits[i * 2 + 1];
            out[i * 2 + 1] = bits[i * 2];
        }
        break;
    case 6:
        for (int i = 0; i < 8; ++i) {
            out[i * 2] = bits[(7 - i) * 2];
            out[i * 2 + 1] = bits[(7 - i) * 2 + 1];
        }
        break;
    case 7:
        for (int i = 0; i < 8; ++i) {
            out[i * 2] = bits[(7 - i) * 2 + 1];
            out[i * 2 + 1] = bits[(7 - i) * 2];
        }
        break;
    case 0:
    default:
        out = bits;
        break;
    }
    return out;
}

void appendDibitBits(int dibit, bool etsiMap, bool *rawBits, int bitIndex) {
    bool bit0 = false;
    bool bit1 = false;
    if (etsiMap) {
        switch (dibit) {
        case 3:
            bit0 = false;
            bit1 = true;
            break;
        case 2:
            bit0 = false;
            bit1 = false;
            break;
        case 0:
            bit0 = true;
            bit1 = false;
            break;
        case 1:
        default:
            bit0 = true;
            bit1 = true;
            break;
        }
    } else {
        bit0 = (dibit & 0x02) != 0;
        bit1 = (dibit & 0x01) != 0;
    }
    rawBits[bitIndex] = bit0;
    rawBits[bitIndex + 1] = bit1;
}

QString voicePayloadBitMapName(int variant) {
    switch (variant & 0x07) {
    case 1:
        return QStringLiteral("flip0");
    case 2:
        return QStringLiteral("flip1");
    case 3:
        return QStringLiteral("flip01");
    case 4:
        return QStringLiteral("swap");
    case 5:
        return QStringLiteral("swap+flip0");
    case 6:
        return QStringLiteral("swap+flip1");
    case 7:
        return QStringLiteral("swap+flip01");
    case 0:
    default:
        return QStringLiteral("direct");
    }
}

void appendDibitBitsVariant(int dibit, int variant, bool *rawBits, int bitIndex) {
    bool pair[2] = {};
    appendDibitBits(dibit, true, pair, 0);
    bool bit0 = pair[0];
    bool bit1 = pair[1];
    if ((variant & 0x04) != 0) {
        const bool tmp = bit0;
        bit0 = bit1;
        bit1 = tmp;
    }
    if ((variant & 0x01) != 0) {
        bit0 = !bit0;
    }
    if ((variant & 0x02) != 0) {
        bit1 = !bit1;
    }
    rawBits[bitIndex] = bit0;
    rawBits[bitIndex + 1] = bit1;
}

QString bitsToHex(const bool *bits, int bitCount) {
    QString out;
    out.reserve((bitCount + 3) / 4);
    for (int i = 0; i < bitCount; i += 4) {
        int value = 0;
        for (int bit = 0; bit < 4; ++bit) {
            const int index = i + bit;
            value = (value << 1) | (index < bitCount && bits[index] ? 1 : 0);
        }
        out.append(QLatin1Char(value < 10 ? static_cast<char>('0' + value)
                                           : static_cast<char>('A' + value - 10)));
    }
    return out;
}

quint32 bitsToUInt(const bool *bits, int start, int count) {
    quint32 value = 0;
    for (int i = 0; i < count; ++i) {
        value = (value << 1) | (bits[start + i] ? 1U : 0U);
    }
    return value;
}

int hexNibbleValue(QChar ch) {
    const ushort value = ch.unicode();
    if (value >= '0' && value <= '9') {
        return static_cast<int>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<int>(10 + value - 'A');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<int>(10 + value - 'a');
    }
    return -1;
}

struct AmbeFecDecodeResult {
    bool decoded = false;
    QString payloadHex;
    int correctedErrors = 0;
    int rawCorrectedErrors = 0;
};

int popcountBits(quint32 value) {
    int count = 0;
    while (value != 0) {
        count += static_cast<int>(value & 0x1U);
        value >>= 1;
    }
    return count;
}

quint16 ambeGolayParity(quint16 data) {
    static constexpr quint16 generator[12] = {
        0x63a, 0x31d, 0x7b4, 0x3da, 0x1ed, 0x6cc,
        0x366, 0x1b3, 0x6e3, 0x54b, 0x49f, 0x475
    };

    quint16 parity = 0;
    for (int i = 0; i < 12; ++i) {
        if ((data & (1U << (11 - i))) != 0) {
            parity ^= generator[i];
        }
    }
    return static_cast<quint16>(parity & 0x07ffU);
}

struct AmbeGolayDecodeResult {
    quint16 data = 0;
    int correctedErrors = 0;
    int rawCorrectedErrors = 0;
    bool decoded = false;
};

struct AmbeGolayCorrection {
    quint16 dataMask = 0;
    std::uint8_t errorCount = 0;
    bool valid = false;
};

const std::array<AmbeGolayCorrection, 2048> &ambeGolaySyndromeTable() {
    static const std::array<AmbeGolayCorrection, 2048> table = []() {
        std::array<AmbeGolayCorrection, 2048> out = {};
        const auto record = [&out](quint16 syndrome, quint16 dataMask, std::uint8_t errorCount) {
            AmbeGolayCorrection &entry = out[static_cast<std::size_t>(syndrome)];
            if (!entry.valid || errorCount < entry.errorCount) {
                entry.dataMask = dataMask;
                entry.errorCount = errorCount;
                entry.valid = true;
            }
        };
        record(0, 0, 0);
        for (quint32 bit0 = 0; bit0 < 23; ++bit0) {
            const quint32 e0 = 1U << bit0;
            const quint16 de0 = static_cast<quint16>((e0 >> 11) & 0x0fffU);
            const quint16 pe0 = static_cast<quint16>(e0 & 0x07ffU);
            record(static_cast<quint16>(ambeGolayParity(de0) ^ pe0), de0, 1);
            for (quint32 bit1 = bit0 + 1; bit1 < 23; ++bit1) {
                const quint32 e1 = e0 | (1U << bit1);
                const quint16 de1 = static_cast<quint16>((e1 >> 11) & 0x0fffU);
                const quint16 pe1 = static_cast<quint16>(e1 & 0x07ffU);
                record(static_cast<quint16>(ambeGolayParity(de1) ^ pe1), de1, 2);
                for (quint32 bit2 = bit1 + 1; bit2 < 23; ++bit2) {
                    const quint32 e2 = e1 | (1U << bit2);
                    const quint16 de2 = static_cast<quint16>((e2 >> 11) & 0x0fffU);
                    const quint16 pe2 = static_cast<quint16>(e2 & 0x07ffU);
                    record(static_cast<quint16>(ambeGolayParity(de2) ^ pe2), de2, 3);
                }
            }
        }
        return out;
    }();
    return table;
};

AmbeGolayDecodeResult ambeGolayDecode2312(quint32 codeword) {
    const quint16 data = static_cast<quint16>((codeword >> 11) & 0x0fffU);
    const quint16 parity = static_cast<quint16>(codeword & 0x07ffU);
    const quint16 syndrome = static_cast<quint16>(ambeGolayParity(data) ^ parity);
    const AmbeGolayCorrection &correction = ambeGolaySyndromeTable()[syndrome];
    if (!correction.valid) {
        return {data, 0, 0, false};
    }
    const quint16 correctedData =
        static_cast<quint16>(data ^ correction.dataMask);
    const int correctedDataErrors = popcountBits(correction.dataMask);
    return {correctedData, correctedDataErrors, correction.errorCount, true};
}

std::array<bool, 24> ambeC1Keystream(quint16 c0Data) {
    std::array<bool, 24> stream = {};
    quint32 previous = static_cast<quint32>(16U * (c0Data & 0x0fffU));
    for (int i = 1; i < 24; ++i) {
        previous = (173U * previous + 13849U) & 0xffffU;
        stream[static_cast<std::size_t>(i)] = (previous >> 15) != 0;
    }
    return stream;
}

AmbeFecDecodeResult decodeDmrAmbeFecPayload(const QString &frameHex) {
    AmbeFecDecodeResult result;
    if (frameHex.size() != 18) {
        return result;
    }

    std::array<bool, 72> frameBits = {};
    for (int hexIndex = 0; hexIndex < frameHex.size(); ++hexIndex) {
        const int value = hexNibbleValue(frameHex.at(hexIndex));
        if (value < 0) {
            return result;
        }
        for (int bit = 0; bit < 4; ++bit) {
            frameBits[static_cast<std::size_t>(hexIndex * 4 + bit)] =
                ((value >> (3 - bit)) & 0x1) != 0;
        }
    }

    // AMBE+2 DMR on-air FEC layout follows the ETSI bit placement used by
    // GopherTrunk's AMBE FEC reference.
    static constexpr int rW[36] = {
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2,
        0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2
    };
    static constexpr int rX[36] = {
        23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5,
        17, 4, 16, 3, 15, 2, 14, 1, 13, 0, 12, 10,
        11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4
    };
    static constexpr int rY[36] = {
        0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3,
        1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3,
        1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3
    };
    static constexpr int rZ[36] = {
        5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12,
        22, 11, 21, 10, 20, 9, 19, 8, 18, 7, 17, 6,
        16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0
    };

    std::array<std::array<bool, 24>, 4> frame = {};
    for (int i = 0; i < 36; ++i) {
        frame[static_cast<std::size_t>(rW[i])][static_cast<std::size_t>(rX[i])] =
            frameBits[static_cast<std::size_t>(2 * i)];
        frame[static_cast<std::size_t>(rY[i])][static_cast<std::size_t>(rZ[i])] =
            frameBits[static_cast<std::size_t>(2 * i + 1)];
    }

    quint32 c0Codeword = 0;
    for (int j = 0; j < 23; ++j) {
        c0Codeword |= static_cast<quint32>(frame[0][static_cast<std::size_t>(j + 1)] ? 1U : 0U) << j;
    }
    const AmbeGolayDecodeResult c0 = ambeGolayDecode2312(c0Codeword);
    if (!c0.decoded) {
        return result;
    }

    const std::array<bool, 24> c1Keystream = ambeC1Keystream(c0.data);
    quint32 c1Codeword = 0;
    for (int j = 0; j < 23; ++j) {
        const bool descrambled =
            frame[1][static_cast<std::size_t>(j)] ^ c1Keystream[static_cast<std::size_t>(23 - j)];
        c1Codeword |= static_cast<quint32>(descrambled ? 1U : 0U) << j;
    }
    const AmbeGolayDecodeResult c1 = ambeGolayDecode2312(c1Codeword);
    if (!c1.decoded) {
        return result;
    }

    std::array<bool, 49> payload = {};
    for (int i = 0; i < 12; ++i) {
        payload[static_cast<std::size_t>(i)] = ((c0.data >> (11 - i)) & 0x1U) != 0;
        payload[static_cast<std::size_t>(12 + i)] = ((c1.data >> (11 - i)) & 0x1U) != 0;
    }
    for (int i = 0; i < 11; ++i) {
        payload[static_cast<std::size_t>(24 + i)] =
            frame[2][static_cast<std::size_t>(10 - i)];
    }
    for (int i = 0; i < 14; ++i) {
        payload[static_cast<std::size_t>(35 + i)] =
            frame[3][static_cast<std::size_t>(13 - i)];
    }

    result.decoded = true;
    result.payloadHex = bitsToHex(payload.data(), static_cast<int>(payload.size()));
    result.correctedErrors = c0.correctedErrors + c1.correctedErrors;
    result.rawCorrectedErrors = c0.rawCorrectedErrors + c1.rawCorrectedErrors;
    return result;
}

QString emb32VariantHex(const QString &hex, int variant) {
    if (hex.size() != 8) {
        return hex;
    }

    std::array<bool, 32> bits = {};
    for (int hexIndex = 0; hexIndex < hex.size(); ++hexIndex) {
        const int value = hexNibbleValue(hex.at(hexIndex));
        if (value < 0) {
            return hex;
        }
        for (int bit = 0; bit < 4; ++bit) {
            bits[static_cast<std::size_t>(hexIndex * 4 + bit)] =
                ((value >> (3 - bit)) & 0x1) != 0;
        }
    }

    std::array<bool, 32> out = {};
    switch (variant) {
    case 1:
        for (int i = 0; i < 32; ++i) {
            out[i] = bits[31 - i];
        }
        break;
    case 2:
        for (int i = 0; i < 32; ++i) {
            out[i] = bits[(i + 16) & 0x1f];
        }
        break;
    case 3:
        for (int i = 0; i < 16; ++i) {
            out[i] = bits[15 - i];
            out[16 + i] = bits[31 - i];
        }
        break;
    case 4:
        for (int i = 0; i < 16; ++i) {
            out[i] = bits[31 - i];
            out[16 + i] = bits[15 - i];
        }
        break;
    case 5:
        for (int i = 0; i < 16; ++i) {
            out[i * 2] = bits[i * 2 + 1];
            out[i * 2 + 1] = bits[i * 2];
        }
        break;
    case 6:
        for (int i = 0; i < 16; ++i) {
            out[i * 2] = bits[(15 - i) * 2];
            out[i * 2 + 1] = bits[(15 - i) * 2 + 1];
        }
        break;
    case 7:
        for (int i = 0; i < 16; ++i) {
            out[i * 2] = bits[(15 - i) * 2 + 1];
            out[i * 2 + 1] = bits[(15 - i) * 2];
        }
        break;
    case 0:
    default:
        out = bits;
        break;
    }
    return bitsToHex(out.data(), static_cast<int>(out.size()));
}

std::array<bool, 5> embeddedHamming16114Parity(const bool *row) {
    const auto b = [&](int index) { return row[index]; };
    const auto x = [](bool value) { return value; };
    return {
        x(b(0) ^ b(1) ^ b(2) ^ b(3) ^ b(5) ^ b(7) ^ b(8)),
        x(b(1) ^ b(2) ^ b(3) ^ b(4) ^ b(6) ^ b(8) ^ b(9)),
        x(b(2) ^ b(3) ^ b(4) ^ b(5) ^ b(7) ^ b(9) ^ b(10)),
        x(b(0) ^ b(1) ^ b(2) ^ b(4) ^ b(6) ^ b(7) ^ b(10)),
        x(b(0) ^ b(2) ^ b(5) ^ b(6) ^ b(8) ^ b(9) ^ b(10))
    };
}

int embeddedHamming16114Syndrome(const bool *row) {
    const std::array<bool, 5> parity = embeddedHamming16114Parity(row);
    int syndrome = 0;
    for (int i = 0; i < 5; ++i) {
        if (parity[static_cast<std::size_t>(i)] != row[11 + i]) {
            syndrome |= (1 << i);
        }
    }
    return syndrome;
}

int correctEmbeddedHamming16114(bool *row) {
    if (embeddedHamming16114Syndrome(row) == 0) {
        return 0;
    }
    for (int bit = 0; bit < 16; ++bit) {
        row[bit] = !row[bit];
        if (embeddedHamming16114Syndrome(row) == 0) {
            return 1;
        }
        row[bit] = !row[bit];
    }
    return -1;
}

int embeddedLcFiveBitChecksum(const bool *lcBits) {
    int total = 0;
    for (int i = 0; i < 72; i += 8) {
        total += static_cast<int>(bitsToUInt(lcBits, i, 8));
    }
    return total % 31;
}

struct EmbeddedLcDecodeResult {
    QString lc72;
    QString failureReason;
    int corrected = -1;
    int failedRow = -1;
    bool columnParityOk = false;
    bool checksumOk = false;
    int expectedChecksum = -1;
    int receivedChecksum = -1;
    quint32 target = 0;
    quint32 source = 0;
    int flco = -1;
    int fid = -1;
    int serviceOptions = -1;
    bool protectedFlag = false;
};

EmbeddedLcDecodeResult decodeEmbeddedLc72FromBptc(const QString &emb128) {
    EmbeddedLcDecodeResult result;
    if (emb128.size() != 32) {
        result.failureReason =
            QStringLiteral("bad embedded LC length %1").arg(emb128.size());
        return result;
    }

    std::array<bool, 128> onAirBits = {};
    for (int hexIndex = 0; hexIndex < emb128.size(); ++hexIndex) {
        const int value = hexNibbleValue(emb128.at(hexIndex));
        if (value < 0) {
            result.failureReason =
                QStringLiteral("bad embedded LC hex at %1").arg(hexIndex);
            return result;
        }
        for (int bit = 0; bit < 4; ++bit) {
            onAirBits[static_cast<std::size_t>(hexIndex * 4 + bit)] =
                ((value >> (3 - bit)) & 0x1) != 0;
        }
    }

    std::array<bool, 128> data = {};
    for (int airIndex = 0; airIndex < 127; ++airIndex) {
        const int matrixIndex = (airIndex * 16) % 127;
        data[static_cast<std::size_t>(matrixIndex)] =
            onAirBits[static_cast<std::size_t>(airIndex)];
    }
    data[127] = onAirBits[127];

    int corrected = 0;
    for (int row = 0; row < 7; ++row) {
        const int rowCorrection =
            correctEmbeddedHamming16114(&data[static_cast<std::size_t>(row * 16)]);
        if (rowCorrection < 0) {
            result.corrected = -1;
            result.failedRow = row;
            result.failureReason =
                QStringLiteral("Hamming(16,11,4) row %1 failed").arg(row);
            return result;
        }
        corrected += rowCorrection;
    }

    result.columnParityOk = true;
    for (int column = 0; column < 16; ++column) {
        bool parity = false;
        for (int row = 0; row < 8; ++row) {
            parity ^= data[static_cast<std::size_t>(row * 16 + column)];
        }
        if (parity) {
            result.columnParityOk = false;
            break;
        }
    }

    bool lcBits[72] = {};
    int out = 0;
    const std::array<std::pair<int, int>, 7> ranges = {{
        {0, 11}, {16, 27}, {32, 42}, {48, 58}, {64, 74}, {80, 90}, {96, 106}
    }};
    for (const auto &range : ranges) {
        for (int index = range.first; index < range.second; ++index) {
            lcBits[out++] = data[static_cast<std::size_t>(index)];
        }
    }

    result.receivedChecksum =
        (data[42] ? 16 : 0) |
        (data[58] ? 8 : 0) |
        (data[74] ? 4 : 0) |
        (data[90] ? 2 : 0) |
        (data[106] ? 1 : 0);
    result.expectedChecksum = embeddedLcFiveBitChecksum(lcBits);
    result.checksumOk = result.receivedChecksum == result.expectedChecksum;
    result.corrected = corrected;
    result.lc72 = bitsToHex(lcBits, static_cast<int>(std::size(lcBits)));
    result.protectedFlag = lcBits[0];
    result.flco = static_cast<int>(bitsToUInt(lcBits, 2, 6));
    result.fid = static_cast<int>(bitsToUInt(lcBits, 8, 8));
    result.serviceOptions = static_cast<int>(bitsToUInt(lcBits, 16, 8));
    result.target = bitsToUInt(lcBits, 24, 24);
    result.source = bitsToUInt(lcBits, 48, 24);
    return result;
}

QString embeddedLcInfoText(const EmbeddedLcDecodeResult &decoded) {
    if (decoded.lc72.isEmpty()) {
        return decoded.failureReason.isEmpty()
                   ? QStringLiteral("LC decode failed")
                   : QStringLiteral("LC decode failed: %1").arg(decoded.failureReason);
    }

    QString text = QStringLiteral("LC72 %1, FLC BPTC %2, col %3, crc %4/%5 %6, FLCO 0x%7, FID 0x%8, SO 0x%9, TG %10, SRC %11")
                       .arg(decoded.lc72)
                       .arg(decoded.corrected)
                       .arg(decoded.columnParityOk ? QStringLiteral("ok") : QStringLiteral("bad"))
                       .arg(decoded.receivedChecksum)
                       .arg(decoded.expectedChecksum)
                       .arg(decoded.checksumOk ? QStringLiteral("ok") : QStringLiteral("bad"))
                       .arg(decoded.flco, 2, 16, QLatin1Char('0'))
                       .arg(decoded.fid, 2, 16, QLatin1Char('0'))
                       .arg(decoded.serviceOptions, 2, 16, QLatin1Char('0'))
                       .arg(decoded.target)
                       .arg(decoded.source)
                       .toUpper();
    if (decoded.protectedFlag) {
        text.append(QStringLiteral(", PF 1"));
    }
    return text;
}

} // namespace

DmrDecoder::~DmrDecoder() {
    flushDmrDibitDump();
}

void DmrDecoder::flushDmrDibitDump() {
    if (dmrDibitDumpBuffer.isEmpty()) {
        return;
    }

    const QString dumpPath =
        QCoreApplication::applicationDirPath() + QStringLiteral("/ObriiSDR_dmr_dibit_dump.log");
    QFile dumpFile(dumpPath);
    if (dumpFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&dumpFile);
        out << dmrDibitDumpBuffer;
    } else {
        qDebug() << "[DMR dump] failed to flush voice dibit dump" << dumpPath;
    }
    dmrDibitDumpBuffer.clear();
}

void DmrDecoder::reset() {
    flushDmrDibitDump();
    sampleBuffer.clear();
    pendingVoiceEmb.clear();
    voiceEmbeddedFrames.clear();
    voiceLcSequences.clear();
    voiceLcFragmentCandidates.clear();
    reportedVoiceLcCandidateRaw.clear();
    pendingDecodedMessages.clear();
    pendingAmbeFrames.clear();
    pendingAmbeSoftFrames.clear();
    pendingAmbePayloads.clear();
    queuedVoicePayloadBurstSamples.clear();
    pendingDibitBursts.clear();
    voiceLcRawSinceReport.clear();
    voicePayloadCadenceScore30 = 0;
    voicePayloadCadenceScore60 = 0;
    std::fill(std::begin(voicePayloadColorCodeScore),
              std::end(voicePayloadColorCodeScore),
              0);
    lastVoicePayloadAmbeLayout = DMR_DEFAULT_AMBE_LAYOUT;
    lastVoicePayloadBitMapVariant = 0;
    dmrDibitDumpRemaining = fobosVerboseLoggingEnabled() ? 32 : 0;
    dmrDibitDumpSequence = 0;
    ++dmrDibitDumpSession;
    selectedVoicePayloadCadenceSymbols = 0;
    selectedVoicePayloadColorCode = -1;
    selectedVoicePayloadSample = 0;
    lastObservedTimeslot = 0;
    lastForwardedDataBurstSample = 0;
    lastVoiceService = VoiceServiceInfo();
    lastVoiceLcUserKey.clear();
    lastVoiceLcUserSample = 0;
    dcEstimate = 0.0;
    averageMagnitude = 0.0;
    totalSamples = 0;
    lastReportSample = 0;
    lastAcceptedSyncSample = 0;
    haveAcceptedSync = false;
    haveLock = false;
    lockedPolarityInverted = false;
    lockedPhase = 0;
    lockedRfFrequencyHz = 0.0;
    confirmedSyncsSinceReport = 0;
    candidateSyncsSinceReport = 0;
    polarityFlipsSinceReport = 0;
    bestScoreSinceReport = 0;
    bestPhaseSinceReport = 0;
    bestQualitySinceReport = 0.0;
    bestPolarityInvertedSinceReport = false;
    lastConfirmedSyncSample = 0;
    spacingSamplesCount = 0;
    spacingMsSum = 0.0;
    spacingMsMin = 0.0;
    spacingMsMax = 0.0;
    std::fill(std::begin(phaseHistogram), std::end(phaseHistogram), 0);
    std::fill(std::begin(intervalHistogram), std::end(intervalHistogram), 0);
    cachOkSinceReport = 0;
    cachStrictOkSinceReport = 0;
    cachFailSinceReport = 0;
    std::fill(std::begin(cachChannelHistogram), std::end(cachChannelHistogram), 0);
    std::fill(std::begin(cachLcssHistogram), std::end(cachLcssHistogram), 0);
    std::fill(std::begin(cachTimingOffsetHistogram), std::end(cachTimingOffsetHistogram), 0);
    cachMapEtsiCount = 0;
    cachMapLegacyCount = 0;
    slotTypeOkSinceReport = 0;
    slotTypeFailSinceReport = 0;
    std::fill(std::begin(colorCodeHistogram), std::end(colorCodeHistogram), 0);
    std::fill(std::begin(dataTypeHistogram), std::end(dataTypeHistogram), 0);
    slotTypeCorrectedErrors = 0;
    embOkSinceReport = 0;
    embAnchorsSinceReport = 0;
    embWeakSinceReport = 0;
    embFailSinceReport = 0;
    std::fill(std::begin(embColorCodeHistogram), std::end(embColorCodeHistogram), 0);
    std::fill(std::begin(embLcssHistogram), std::end(embLcssHistogram), 0);
    embPrivacyCount = 0;
    embCorrectedErrors = 0;
    std::fill(std::begin(embVariantHistogram), std::end(embVariantHistogram), 0);
    std::fill(std::begin(embWeakVariantHistogram), std::end(embWeakVariantHistogram), 0);
    std::fill(std::begin(embErrorHistogram), std::end(embErrorHistogram), 0);
    std::fill(std::begin(embWeakErrorHistogram), std::end(embWeakErrorHistogram), 0);
    std::fill(std::begin(embWeakColorCodeHistogram), std::end(embWeakColorCodeHistogram), 0);
    std::fill(std::begin(embTimingOffsetHistogram), std::end(embTimingOffsetHistogram), 0);
    std::fill(std::begin(embWeakTimingOffsetHistogram), std::end(embWeakTimingOffsetHistogram), 0);
    voiceLcAssemblySinceReport = 0;
    voiceLcBptcSinceReport = 0;
    voiceLcStrictSinceReport = 0;
    voiceLcLabTargetMatchSinceReport = 0;
    voiceLcLabSourceMatchSinceReport = 0;
    voiceLcLabFullMatchSinceReport = 0;
    std::fill(std::begin(voiceLcFragmentHistogram),
              std::end(voiceLcFragmentHistogram),
              0);
    std::fill(std::begin(voiceLcUsableFragmentHistogram),
              std::end(voiceLcUsableFragmentHistogram),
              0);
    resetSignalQualityCounters();
    lastVoiceEmbAnchorSample = 0;
    lastCachText.clear();
    lastPatternName.clear();
    lockedPatternName.clear();
    lastStatus.clear();
}

void DmrDecoder::configure(int sampleRate) {
    if (sampleRate <= 0) {
        sampleRate = 48000;
    }
    const int newSamplesPerSymbol =
        (std::max)(1, static_cast<int>(std::lround(sampleRate / DMR_SYMBOL_RATE)));
    if (activeSampleRate == sampleRate && samplesPerSymbol == newSamplesPerSymbol) {
        return;
    }

    activeSampleRate = sampleRate;
    samplesPerSymbol = newSamplesPerSymbol;
    qDebug() << "[DMR slicer] configured"
             << "sampleRate" << activeSampleRate
             << "samplesPerSymbol" << samplesPerSymbol
             << "exactSamplesPerSymbol"
             << (static_cast<double>(activeSampleRate) / DMR_SYMBOL_RATE);
    reset();
}

void DmrDecoder::setLabHints(bool enabled,
                             int expectedColorCode,
                             int expectedTimeslot,
                             int expectedSourceId,
                             int expectedTargetId,
                             bool manualTimingEnabled,
                             int manualTimingOffset,
                             float slicerRatio,
                             bool adaptiveSlicer,
                             int ambeLayout) {
    const int normalizedColorCode =
        enabled && expectedColorCode >= 0 && expectedColorCode <= 15 ? expectedColorCode : -1;
    const int normalizedTimeslot =
        enabled && (expectedTimeslot == 1 || expectedTimeslot == 2) ? expectedTimeslot : 0;
    const int normalizedSourceId = enabled && expectedSourceId > 0 ? expectedSourceId : 0;
    const int normalizedTargetId = enabled && expectedTargetId > 0 ? expectedTargetId : 0;
    const int normalizedTimingOffset = (std::clamp)(manualTimingOffset, -80, 80);
    const float normalizedSlicerRatio = (std::clamp)(slicerRatio, 0.45f, 0.80f);
    const int normalizedAmbeLayout = normalizedDmrAmbeLayout(ambeLayout);
    const bool normalizedEnabled =
        enabled &&
        (normalizedColorCode >= 0 ||
         normalizedTimeslot > 0 ||
         normalizedSourceId > 0 ||
         normalizedTargetId > 0);

    const bool changed =
        labHintsEnabled != normalizedEnabled ||
        labExpectedColorCode != normalizedColorCode ||
        labExpectedTimeslot != normalizedTimeslot ||
        labExpectedSourceId != normalizedSourceId ||
        labExpectedTargetId != normalizedTargetId ||
        labManualTimingEnabled != manualTimingEnabled ||
        labManualTimingOffset != normalizedTimingOffset ||
        std::abs(labSlicerRatio - normalizedSlicerRatio) > 0.0005f ||
        labAdaptiveSlicer != adaptiveSlicer ||
        labAmbeLayout != normalizedAmbeLayout;

    labHintsEnabled = normalizedEnabled;
    labExpectedColorCode = normalizedColorCode;
    labExpectedTimeslot = normalizedTimeslot;
    labExpectedSourceId = normalizedSourceId;
    labExpectedTargetId = normalizedTargetId;
    labManualTimingEnabled = manualTimingEnabled;
    labManualTimingOffset = normalizedTimingOffset;
    labSlicerRatio = normalizedSlicerRatio;
    labAdaptiveSlicer = adaptiveSlicer;
    labAmbeLayout = normalizedAmbeLayout;

    if (changed) {
        flushDmrDibitDump();
        pendingVoiceEmb.clear();
        voiceEmbeddedFrames.clear();
        voiceLcSequences.clear();
        voiceLcFragmentCandidates.clear();
        reportedVoiceLcCandidateRaw.clear();
        pendingAmbeFrames.clear();
        pendingAmbeSoftFrames.clear();
        pendingAmbePayloads.clear();
        queuedVoicePayloadBurstSamples.clear();
        voiceLcRawSinceReport.clear();
        voicePayloadCadenceScore30 = 0;
        voicePayloadCadenceScore60 = 0;
        std::fill(std::begin(voicePayloadColorCodeScore),
                  std::end(voicePayloadColorCodeScore),
                  0);
        lastVoicePayloadAmbeLayout = DMR_DEFAULT_AMBE_LAYOUT;
        lastVoicePayloadBitMapVariant = 0;
        dmrDibitDumpRemaining = fobosVerboseLoggingEnabled() ? 32 : 0;
        dmrDibitDumpSequence = 0;
        ++dmrDibitDumpSession;
        selectedVoicePayloadCadenceSymbols = labHintsEnabled ? 288 : 0;
        selectedVoicePayloadColorCode =
            labHintsEnabled && labExpectedColorCode >= 0 ? labExpectedColorCode : -1;
        selectedVoicePayloadSample = 0;
        lastObservedTimeslot = 0;
        lastForwardedDataBurstSample = 0;
        lastVoiceService = VoiceServiceInfo();
        lastVoiceLcUserKey.clear();
        lastVoiceLcUserSample = 0;
        voiceLcAssemblySinceReport = 0;
        voiceLcBptcSinceReport = 0;
        voiceLcStrictSinceReport = 0;
        voiceLcLabTargetMatchSinceReport = 0;
        voiceLcLabSourceMatchSinceReport = 0;
        voiceLcLabFullMatchSinceReport = 0;
        std::fill(std::begin(voiceLcFragmentHistogram),
                  std::end(voiceLcFragmentHistogram),
                  0);
        std::fill(std::begin(voiceLcUsableFragmentHistogram),
                  std::end(voiceLcUsableFragmentHistogram),
                  0);
        if (labHintsEnabled &&
            labExpectedColorCode >= 0 &&
            labExpectedColorCode < static_cast<int>(std::size(voicePayloadColorCodeScore))) {
            voicePayloadColorCodeScore[labExpectedColorCode] = 20;
        }
        if (labHintsEnabled) {
            voicePayloadCadenceScore60 = 40;
        }
    }

    if (changed && labHintsEnabled) {
        qDebug() << "[DMR lock] hints"
                 << "expectedCC" << labExpectedColorCode
                 << "expectedTS" << labExpectedTimeslot
                 << "expectedSRC" << labExpectedSourceId
                 << "expectedTG" << labExpectedTargetId
                 << "manualTiming" << labManualTimingEnabled
                 << "timingOffset" << labManualTimingOffset
                 << "slicerRatio" << labSlicerRatio
                 << "adaptiveSlicer" << labAdaptiveSlicer
                 << "ambeLayout" << dmrAmbeLayoutName(labAmbeLayout);
    } else if (changed) {
        qDebug() << "[DMR lab] slicer controls"
                 << "manualTiming" << labManualTimingEnabled
                 << "timingOffset" << labManualTimingOffset
                 << "slicerRatio" << labSlicerRatio
                 << "adaptiveSlicer" << labAdaptiveSlicer
                 << "ambeLayout" << dmrAmbeLayoutName(labAmbeLayout);
    }
}

void DmrDecoder::setLiveTimingHint(bool enabled,
                                   double phaseFraction,
                                   double confidence) {
    liveTimingHintEnabled = enabled && std::isfinite(phaseFraction) &&
                            std::isfinite(confidence) && confidence >= 0.35;
    liveTimingPhaseFraction =
        liveTimingHintEnabled ? (std::clamp)(phaseFraction, 0.0, 1.0) : 0.5;
    liveTimingConfidence =
        liveTimingHintEnabled ? (std::clamp)(confidence, 0.0, 1.0) : 0.0;
}

DmrDecoder::Result DmrDecoder::processPcmFrame(const QByteArray &pcmData, int sampleRate, double rfFrequencyHz) {
    configure(sampleRate);
    const QString frequencySuffix = dmrFrequencySuffix(rfFrequencyHz);
    const auto lockedFrequencySuffix = [this, rfFrequencyHz]() {
        return dmrFrequencySuffix(lockedRfFrequencyHz > 0.0 ? lockedRfFrequencyHz : rfFrequencyHz);
    };

    Result result;
    const auto finalizeResult = [this](Result &value) {
        std::vector<QString> frames = takePendingAmbeFrames();
        value.ambeFrames.insert(value.ambeFrames.end(), frames.begin(), frames.end());
        std::vector<DmrAmbeSoftFrame> softFrames = takePendingAmbeSoftFrames();
        value.ambeSoftFrames.insert(value.ambeSoftFrames.end(), softFrames.begin(), softFrames.end());
        int correctedErrors = 0;
        std::vector<DmrAmbePayload> payloads = takePendingAmbePayloads(&correctedErrors);
        value.ambePayloads.insert(value.ambePayloads.end(), payloads.begin(), payloads.end());
        value.ambeFecCorrections += correctedErrors;
        value.dibitBursts.insert(value.dibitBursts.end(),
                                 pendingDibitBursts.begin(),
                                 pendingDibitBursts.end());
        pendingDibitBursts.clear();
        int confidence = 0;
        value.voiceAudioTrusted = hasTrustedVoiceAudio(&confidence);
        value.voiceAudioConfidence = confidence;
        value.serviceStatusText = serviceStatusText();
        const int colorCode =
            lastVoiceService.valid && lastVoiceService.colorCode >= 0
                ? lastVoiceService.colorCode
                : selectedVoicePayloadColorCode;
        const int timeslot = currentServiceTimeslot();
        value.metadataColorCode = colorCode;
        value.metadataTimeslot = timeslot;
        if (lastVoiceService.valid) {
            value.metadataTargetId = lastVoiceService.target;
            value.metadataSourceId = lastVoiceService.source;
            value.metadataFlco = lastVoiceService.flco;
        } else if (labHintsEnabled) {
            if (labExpectedTargetId > 0) {
                value.metadataTargetId = static_cast<quint32>(labExpectedTargetId);
            }
            if (labExpectedSourceId > 0) {
                value.metadataSourceId = static_cast<quint32>(labExpectedSourceId);
            }
        }
        value.metadataValid =
            colorCode >= 0 ||
            timeslot > 0 ||
            value.metadataTargetId > 0 ||
            value.metadataSourceId > 0;
        return value;
    };
    if (pcmData.size() < static_cast<int>(sizeof(qint16))) {
        result.statusText = idleStatus();
        result.statusChanged = result.statusText != lastStatus;
        lastStatus = result.statusText;
        return finalizeResult(result);
    }

    const int appendedSamples = pcmData.size() / static_cast<int>(sizeof(qint16));
    appendSamples(pcmData);
    processPendingVoiceEmb();
    const QString pendingDecodedText = takePendingDecodedMessages();
    SyncHit hit;
    const quint64 syncSpanSamples = static_cast<quint64>(DMR_SYNC_SYMBOLS * samplesPerSymbol);
    quint64 minimumSample = totalSamples > static_cast<quint64>(appendedSamples) + syncSpanSamples
                                ? totalSamples - static_cast<quint64>(appendedSamples) - syncSpanSamples
                                : 0;
    quint64 acceptedSyncGapSamples =
        static_cast<quint64>(std::lround(DMR_SYNC_MIN_GAP_SECONDS * activeSampleRate));
    const bool lockedMobileVoice =
        haveLock &&
        (lockedPatternName == QStringLiteral("MS Voice") ||
         lockedPatternName.startsWith(QStringLiteral("Direct TS")));
    if (lockedMobileVoice) {
        acceptedSyncGapSamples =
            (std::max)(acceptedSyncGapSamples,
                       static_cast<quint64>(std::lround(DMR_VOICE_SYNC_MIN_GAP_SECONDS *
                                                        activeSampleRate)));
    }
    acceptedSyncGapSamples = (std::max)(acceptedSyncGapSamples, syncSpanSamples);
    if (haveAcceptedSync) {
        minimumSample = (std::max)(minimumSample, lastAcceptedSyncSample + acceptedSyncGapSamples);
    }
    if (!findBestSync(hit, minimumSample)) {
        result.statusText = idleStatus();
        result.statusChanged = result.statusText != lastStatus;
        lastStatus = result.statusText;
        const quint64 lockTimeoutSamples =
            static_cast<quint64>(std::lround(DMR_LOCK_TIMEOUT_SECONDS * activeSampleRate));
        if (haveLock &&
            lastConfirmedSyncSample > 0 &&
            totalSamples >= lastConfirmedSyncSample + lockTimeoutSamples) {
            const double ageMs =
                1000.0 * static_cast<double>(totalSamples - lastConfirmedSyncSample) /
                static_cast<double>(activeSampleRate);
            result.decodedText =
                QStringLiteral("[DMR%1] lock lost: %2, last confirmed sync %3 ms ago\n")
                    .arg(lockedFrequencySuffix())
                    .arg(lockedPatternName)
                    .arg(ageMs, 0, 'f', 0);
            result.lockLost = true;
            qDebug().noquote() << result.decodedText.trimmed();
            haveLock = false;
            lockedPatternName.clear();
            lockedRfFrequencyHz = 0.0;
            confirmedSyncsSinceReport = 0;
            candidateSyncsSinceReport = 0;
            polarityFlipsSinceReport = 0;
            bestScoreSinceReport = 0;
            bestPhaseSinceReport = 0;
            bestQualitySinceReport = 0.0;
            spacingSamplesCount = 0;
            spacingMsSum = 0.0;
            spacingMsMin = 0.0;
            spacingMsMax = 0.0;
            std::fill(std::begin(phaseHistogram), std::end(phaseHistogram), 0);
            std::fill(std::begin(intervalHistogram), std::end(intervalHistogram), 0);
            cachOkSinceReport = 0;
            cachStrictOkSinceReport = 0;
            cachFailSinceReport = 0;
            std::fill(std::begin(cachChannelHistogram), std::end(cachChannelHistogram), 0);
            std::fill(std::begin(cachLcssHistogram), std::end(cachLcssHistogram), 0);
            std::fill(std::begin(cachTimingOffsetHistogram), std::end(cachTimingOffsetHistogram), 0);
            cachMapEtsiCount = 0;
            cachMapLegacyCount = 0;
            slotTypeOkSinceReport = 0;
            slotTypeFailSinceReport = 0;
            std::fill(std::begin(colorCodeHistogram), std::end(colorCodeHistogram), 0);
            std::fill(std::begin(dataTypeHistogram), std::end(dataTypeHistogram), 0);
            slotTypeCorrectedErrors = 0;
            embOkSinceReport = 0;
            embAnchorsSinceReport = 0;
            embWeakSinceReport = 0;
            embFailSinceReport = 0;
            std::fill(std::begin(embColorCodeHistogram), std::end(embColorCodeHistogram), 0);
            std::fill(std::begin(embLcssHistogram), std::end(embLcssHistogram), 0);
            embPrivacyCount = 0;
            embCorrectedErrors = 0;
            lastVoiceEmbAnchorSample = 0;
            pendingVoiceEmb.clear();
            voiceEmbeddedFrames.clear();
            voiceLcSequences.clear();
            voiceLcFragmentCandidates.clear();
            reportedVoiceLcCandidateRaw.clear();
            pendingAmbeFrames.clear();
            pendingAmbeSoftFrames.clear();
            pendingAmbePayloads.clear();
            queuedVoicePayloadBurstSamples.clear();
            voiceLcRawSinceReport.clear();
            resetSignalQualityCounters();
            voicePayloadCadenceScore30 = 0;
            voicePayloadCadenceScore60 = 0;
            std::fill(std::begin(voicePayloadColorCodeScore),
                      std::end(voicePayloadColorCodeScore),
                      0);
            selectedVoicePayloadCadenceSymbols = 0;
            selectedVoicePayloadColorCode = -1;
            selectedVoicePayloadSample = 0;
            lastObservedTimeslot = 0;
            lastVoiceService = VoiceServiceInfo();
            lastVoiceLcUserKey.clear();
            lastVoiceLcUserSample = 0;
            lastCachText.clear();
        }
        if (!pendingDecodedText.isEmpty()) {
            result.decodedText = pendingDecodedText + result.decodedText;
            qDebug().noquote() << result.decodedText.trimmed();
        }
        return finalizeResult(result);
    }

    result.statusText = formatHit(hit);
    result.statusChanged = result.statusText != lastStatus;
    lastStatus = result.statusText;

    if (hit.score < DMR_CONFIRMED_SYNC_SCORE ||
        hit.outerScore < DMR_CONFIRMED_OUTER_SCORE) {
        ++candidateSyncsSinceReport;
        const quint64 reportIntervalSamples =
            static_cast<quint64>(std::lround(DMR_REPORT_INTERVAL_SECONDS * activeSampleRate));
        const bool reportDue =
            lastReportSample == 0 ||
            hit.absoluteSample >= lastReportSample + reportIntervalSamples;
        if (reportDue &&
            hit.score >= DMR_CANDIDATE_SYNC_SCORE &&
            hit.outerScore >= DMR_REPORTABLE_CANDIDATE_OUTER_SCORE) {
            lastReportSample = hit.absoluteSample;
            const QString patternName = QString::fromLatin1(hit.pattern->name);
            result.decodedText =
                QStringLiteral("[DMR%1] strong sync candidate: %2, sync %3/24, outer %4/24, phase %5, polarity %6, quality %7 dB\n")
                    .arg(frequencySuffix)
                    .arg(patternName)
                    .arg(hit.score)
                    .arg(hit.outerScore)
                    .arg(hit.phase)
                    .arg(hit.inverted ? QStringLiteral("inverted") : QStringLiteral("normal"))
                    .arg(hit.qualityDb, 0, 'f', 1);
            qDebug().noquote() << result.decodedText.trimmed();
        }
        return finalizeResult(result);
    }

    const QString patternName = QString::fromLatin1(hit.pattern->name);
    const bool currentVoicePattern =
        hit.pattern && std::strcmp(hit.pattern->kind, "voice") == 0;
    const bool lockedVoicePattern =
        lockedPatternName.contains(QStringLiteral("Voice"), Qt::CaseInsensitive);
    const bool lockFrequencyChanged =
        lockedRfFrequencyHz > 0.0 &&
        rfFrequencyHz > 0.0 &&
        std::abs(lockedRfFrequencyHz - rfFrequencyHz) > 100.0;
    const bool polarityChangedWithinLock =
        haveLock &&
        lockedPatternName == patternName &&
        lockedPolarityInverted != hit.inverted &&
        !lockFrequencyChanged;
    if (polarityChangedWithinLock && !currentVoicePattern) {
        ++polarityFlipsSinceReport;
        hit.inverted = lockedPolarityInverted;
    }

    const CachInfo strictCach = decodeCachBeforeSync(hit, false);
    const CachInfo cach = strictCach.decoded ? strictCach : decodeCachBeforeSync(hit, true);
    const bool baseStationVoice =
        hit.pattern &&
        std::strcmp(hit.pattern->source, "base station") == 0 &&
        std::strcmp(hit.pattern->kind, "voice") == 0;
    if (baseStationVoice &&
        hit.outerScore < DMR_TRUSTED_VOICE_OUTER_SCORE &&
        !cach.decoded) {
        ++candidateSyncsSinceReport;
        return finalizeResult(result);
    }

    haveAcceptedSync = true;
    lastAcceptedSyncSample = hit.absoluteSample;

    const bool compatibleVoiceLock =
        haveLock &&
        !lockFrequencyChanged &&
        currentVoicePattern &&
        lockedVoicePattern;
    const bool newLock = !haveLock ||
                         (!compatibleVoiceLock && lockedPatternName != patternName) ||
                         lockFrequencyChanged;
    if (newLock) {
        confirmedSyncsSinceReport = 0;
        candidateSyncsSinceReport = 0;
        polarityFlipsSinceReport = 0;
        bestScoreSinceReport = 0;
        bestPhaseSinceReport = 0;
        bestQualitySinceReport = 0.0;
        spacingSamplesCount = 0;
        spacingMsSum = 0.0;
        spacingMsMin = 0.0;
        spacingMsMax = 0.0;
        std::fill(std::begin(phaseHistogram), std::end(phaseHistogram), 0);
        std::fill(std::begin(intervalHistogram), std::end(intervalHistogram), 0);
        cachOkSinceReport = 0;
        cachStrictOkSinceReport = 0;
        cachFailSinceReport = 0;
        std::fill(std::begin(cachChannelHistogram), std::end(cachChannelHistogram), 0);
        std::fill(std::begin(cachLcssHistogram), std::end(cachLcssHistogram), 0);
        std::fill(std::begin(cachTimingOffsetHistogram), std::end(cachTimingOffsetHistogram), 0);
        cachMapEtsiCount = 0;
        cachMapLegacyCount = 0;
        slotTypeOkSinceReport = 0;
        slotTypeFailSinceReport = 0;
        std::fill(std::begin(colorCodeHistogram), std::end(colorCodeHistogram), 0);
        std::fill(std::begin(dataTypeHistogram), std::end(dataTypeHistogram), 0);
        slotTypeCorrectedErrors = 0;
        embOkSinceReport = 0;
        embAnchorsSinceReport = 0;
        embWeakSinceReport = 0;
        embFailSinceReport = 0;
        std::fill(std::begin(embColorCodeHistogram), std::end(embColorCodeHistogram), 0);
        std::fill(std::begin(embLcssHistogram), std::end(embLcssHistogram), 0);
        embPrivacyCount = 0;
        embCorrectedErrors = 0;
        lastVoiceEmbAnchorSample = 0;
        pendingVoiceEmb.clear();
        voiceEmbeddedFrames.clear();
        voiceLcSequences.clear();
        voiceLcFragmentCandidates.clear();
        reportedVoiceLcCandidateRaw.clear();
        pendingAmbeFrames.clear();
        pendingAmbeSoftFrames.clear();
        pendingAmbePayloads.clear();
        queuedVoicePayloadBurstSamples.clear();
        voiceLcRawSinceReport.clear();
        resetSignalQualityCounters();
        voicePayloadCadenceScore30 = 0;
        voicePayloadCadenceScore60 = 0;
        std::fill(std::begin(voicePayloadColorCodeScore),
                  std::end(voicePayloadColorCodeScore),
                  0);
        selectedVoicePayloadCadenceSymbols = 0;
        selectedVoicePayloadColorCode = -1;
        selectedVoicePayloadSample = 0;
        lastObservedTimeslot = 0;
        lastVoiceService = VoiceServiceInfo();
        lastCachText.clear();
    }
    haveLock = true;
    lockedPatternName = patternName;
    if (newLock) {
        lockedPolarityInverted = hit.inverted;
        lockedRfFrequencyHz = rfFrequencyHz;
    }
    lastPatternName = patternName;

    if (lastConfirmedSyncSample > 0 && hit.absoluteSample > lastConfirmedSyncSample) {
        const double spacingMs =
            1000.0 * static_cast<double>(hit.absoluteSample - lastConfirmedSyncSample) /
            static_cast<double>(activeSampleRate);
        if (spacingMs >= 20.0 && spacingMs <= 400.0) {
            spacingMsSum += spacingMs;
            ++spacingSamplesCount;
            const int gridStep = (std::clamp)(static_cast<int>(std::lround(spacingMs / 30.0)), 1, 12);
            ++intervalHistogram[gridStep - 1];
            if (spacingSamplesCount == 1) {
                spacingMsMin = spacingMs;
                spacingMsMax = spacingMs;
            } else {
                spacingMsMin = (std::min)(spacingMsMin, spacingMs);
                spacingMsMax = (std::max)(spacingMsMax, spacingMs);
            }
        }
    }
    lastConfirmedSyncSample = hit.absoluteSample;

    ++confirmedSyncsSinceReport;
    if (hit.phase >= 0 && hit.phase < static_cast<int>(std::size(phaseHistogram))) {
        ++phaseHistogram[hit.phase];
    }
    if (cach.decoded) {
        ++cachOkSinceReport;
        if (strictCach.decoded) {
            ++cachStrictOkSinceReport;
        }
        if (cach.channel >= 1 && cach.channel <= 2) {
            ++cachChannelHistogram[cach.channel - 1];
            lastObservedTimeslot = cach.channel;
        }
        if (cach.lcss >= 0 && cach.lcss < static_cast<int>(std::size(cachLcssHistogram))) {
            ++cachLcssHistogram[cach.lcss];
        }
        const int timingOffsetIndex = cach.timingOffset + 2;
        if (timingOffsetIndex >= 0 && timingOffsetIndex < static_cast<int>(std::size(cachTimingOffsetHistogram))) {
            ++cachTimingOffsetHistogram[timingOffsetIndex];
        }
        if (cach.mapName == QStringLiteral("ETSI")) {
            ++cachMapEtsiCount;
        } else {
            ++cachMapLegacyCount;
        }
        lastCachText =
            QStringLiteral("CACH AT=%1 Ch %2 %3")
                .arg(cach.accessType ? 1 : 0)
                .arg(cach.channel)
                .arg(cachLcssText(cach.lcss));
    } else if (hit.pattern && std::strcmp(hit.pattern->source, "base station") == 0) {
        ++cachFailSinceReport;
    }

    const SlotTypeInfo slotType = decodeSlotTypeAroundSync(hit);
    if (slotType.decoded) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DMR] slot type"
                     << hit.pattern->name
                     << "cc" << slotType.colorCode
                     << "dataType" << slotType.dataTypeName
                     << "errors" << slotType.correctedErrors
                     << "timing" << slotType.timingOffset
                     << "slicer" << slotType.slicerRatio
                     << "map" << slotType.mapName;
        }
        ++slotTypeOkSinceReport;
        if (slotType.colorCode >= 0 && slotType.colorCode < static_cast<int>(std::size(colorCodeHistogram))) {
            ++colorCodeHistogram[slotType.colorCode];
        }
        if (slotType.dataType >= 0 && slotType.dataType < static_cast<int>(std::size(dataTypeHistogram))) {
            ++dataTypeHistogram[slotType.dataType];
        }
        slotTypeCorrectedErrors += slotType.correctedErrors;
    } else if (hit.pattern && std::strcmp(hit.pattern->kind, "data") == 0) {
        ++slotTypeFailSinceReport;
    }

    if (hit.pattern &&
        std::strcmp(hit.pattern->kind, "data") == 0 &&
        hit.score >= DMR_CONFIRMED_SYNC_SCORE &&
        slotType.decoded &&
        slotType.dataType == 0) {
        queueDataBurstForBackend(hit, slotType, cach);
    }

    if (hit.pattern && std::strcmp(hit.pattern->kind, "voice") == 0) {
        scheduleVoiceEmbBursts(hit, cach);
        processPendingVoiceEmb();
    }
    if (hit.score > bestScoreSinceReport || hit.qualityDb > bestQualitySinceReport) {
        bestScoreSinceReport = hit.score;
        bestPhaseSinceReport = hit.phase;
        bestQualitySinceReport = hit.qualityDb;
        bestPolarityInvertedSinceReport = hit.inverted;
    }

    const quint64 reportIntervalSamples =
        static_cast<quint64>(std::lround(DMR_REPORT_INTERVAL_SECONDS * activeSampleRate));
    const bool reportDue =
        lastReportSample == 0 ||
        hit.absoluteSample >= lastReportSample + reportIntervalSamples;

    if (newLock) {
        lastReportSample = hit.absoluteSample;
        result.lockAcquired = true;
        result.decodedText =
            QStringLiteral("[DMR%1] lock acquired: %2, sync %3/24, outer %4/24, phase %5, polarity %6, quality %7 dB\n")
                .arg(frequencySuffix)
                .arg(patternName)
                .arg(hit.score)
                .arg(hit.outerScore)
                .arg(hit.phase)
                .arg(hit.inverted ? QStringLiteral("inverted") : QStringLiteral("normal"))
                .arg(hit.qualityDb, 0, 'f', 1);
    } else if (reportDue) {
        int dominantPhase = 0;
        int dominantPhaseCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(phaseHistogram)); ++i) {
            if (phaseHistogram[i] > dominantPhaseCount) {
                dominantPhaseCount = phaseHistogram[i];
                dominantPhase = i;
            }
        }
        lockedPhase = dominantPhase;

        int dominantIntervalBin = 0;
        int dominantIntervalCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(intervalHistogram)); ++i) {
            if (intervalHistogram[i] > dominantIntervalCount) {
                dominantIntervalCount = intervalHistogram[i];
                dominantIntervalBin = i + 1;
            }
        }

        int dominantCachChannel = 0;
        int dominantCachChannelCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(cachChannelHistogram)); ++i) {
            if (cachChannelHistogram[i] > dominantCachChannelCount) {
                dominantCachChannelCount = cachChannelHistogram[i];
                dominantCachChannel = i + 1;
            }
        }
        int dominantLcss = -1;
        int dominantLcssCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(cachLcssHistogram)); ++i) {
            if (cachLcssHistogram[i] > dominantLcssCount) {
                dominantLcssCount = cachLcssHistogram[i];
                dominantLcss = i;
            }
        }
        int dominantCachTimingOffset = 0;
        int dominantCachTimingOffsetCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(cachTimingOffsetHistogram)); ++i) {
            if (cachTimingOffsetHistogram[i] > dominantCachTimingOffsetCount) {
                dominantCachTimingOffsetCount = cachTimingOffsetHistogram[i];
                dominantCachTimingOffset = i - 2;
            }
        }
        const QString timingOffsetText =
            dominantCachTimingOffset > 0
                ? QStringLiteral("+%1").arg(dominantCachTimingOffset)
                : QString::number(dominantCachTimingOffset);
        const QString cachMapText =
            cachMapEtsiCount >= cachMapLegacyCount
                ? QStringLiteral("ETSI %1/%2").arg(cachMapEtsiCount).arg(cachOkSinceReport)
                : QStringLiteral("legacy %1/%2").arg(cachMapLegacyCount).arg(cachOkSinceReport);

        int dominantColorCode = -1;
        int dominantColorCodeCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(colorCodeHistogram)); ++i) {
            if (colorCodeHistogram[i] > dominantColorCodeCount) {
                dominantColorCodeCount = colorCodeHistogram[i];
                dominantColorCode = i;
            }
        }
        int dominantDataType = -1;
        int dominantDataTypeCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(dataTypeHistogram)); ++i) {
            if (dataTypeHistogram[i] > dominantDataTypeCount) {
                dominantDataTypeCount = dataTypeHistogram[i];
                dominantDataType = i;
            }
        }
        const QString slotTypeText =
            slotTypeOkSinceReport > 0
                ? QStringLiteral(", SlotType %1/%2 ok, CC %3, %4, Golay corrected %5")
                      .arg(slotTypeOkSinceReport)
                      .arg(slotTypeOkSinceReport + slotTypeFailSinceReport)
                      .arg(dominantColorCode >= 0 ? QString::number(dominantColorCode) : QStringLiteral("?"))
                      .arg(dominantDataType >= 0 ? dmrDataTypeName(dominantDataType) : QStringLiteral("Data Type ?"))
                      .arg(slotTypeCorrectedErrors)
                : (std::strcmp(hit.pattern->kind, "voice") == 0
                       ? QStringLiteral(", SlotType/CC pending on voice sync")
                       : (slotTypeFailSinceReport > 0
                              ? QStringLiteral(", SlotType 0/%1 ok").arg(slotTypeFailSinceReport)
                              : QString()));

        int dominantEmbColorCode = -1;
        int dominantEmbColorCodeCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(embColorCodeHistogram)); ++i) {
            if (embColorCodeHistogram[i] > dominantEmbColorCodeCount) {
                dominantEmbColorCodeCount = embColorCodeHistogram[i];
                dominantEmbColorCode = i;
            }
        }
        int dominantEmbLcss = -1;
        int dominantEmbLcssCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(embLcssHistogram)); ++i) {
            if (embLcssHistogram[i] > dominantEmbLcssCount) {
                dominantEmbLcssCount = embLcssHistogram[i];
                dominantEmbLcss = i;
            }
        }
        int dominantEmbVariant = -1;
        int dominantEmbVariantCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(embVariantHistogram)); ++i) {
            if (embVariantHistogram[i] > dominantEmbVariantCount) {
                dominantEmbVariantCount = embVariantHistogram[i];
                dominantEmbVariant = i;
            }
        }
        int dominantWeakEmbVariant = -1;
        int dominantWeakEmbVariantCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(embWeakVariantHistogram)); ++i) {
            if (embWeakVariantHistogram[i] > dominantWeakEmbVariantCount) {
                dominantWeakEmbVariantCount = embWeakVariantHistogram[i];
                dominantWeakEmbVariant = i;
            }
        }
        int dominantWeakEmbColorCode = -1;
        int dominantWeakEmbColorCodeCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(embWeakColorCodeHistogram)); ++i) {
            if (embWeakColorCodeHistogram[i] > dominantWeakEmbColorCodeCount) {
                dominantWeakEmbColorCodeCount = embWeakColorCodeHistogram[i];
                dominantWeakEmbColorCode = i;
            }
        }
        int dominantEmbTimingOffset = 0;
        int dominantEmbTimingOffsetCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(embTimingOffsetHistogram)); ++i) {
            if (embTimingOffsetHistogram[i] > dominantEmbTimingOffsetCount) {
                dominantEmbTimingOffsetCount = embTimingOffsetHistogram[i];
                dominantEmbTimingOffset = i - 2;
            }
        }
        int dominantWeakEmbTimingOffset = 0;
        int dominantWeakEmbTimingOffsetCount = 0;
        for (int i = 0; i < static_cast<int>(std::size(embWeakTimingOffsetHistogram)); ++i) {
            if (embWeakTimingOffsetHistogram[i] > dominantWeakEmbTimingOffsetCount) {
                dominantWeakEmbTimingOffsetCount = embWeakTimingOffsetHistogram[i];
                dominantWeakEmbTimingOffset = i - 2;
            }
        }
        const auto signedOffsetText = [](int offset) {
            return offset > 0 ? QStringLiteral("+%1").arg(offset) : QString::number(offset);
        };
        const QString embCcText =
            (dominantEmbColorCode >= 0 &&
             dominantEmbColorCodeCount >= 3 &&
             dominantEmbColorCodeCount * 2 >= embOkSinceReport)
                ? QString::number(dominantEmbColorCode)
                : (dominantEmbColorCode >= 0
                       ? QStringLiteral("%1? %2/%3")
                             .arg(dominantEmbColorCode)
                             .arg(dominantEmbColorCodeCount)
                             .arg(embOkSinceReport)
                       : QStringLiteral("?"));
        const QString weakEmbCcText =
            dominantWeakEmbColorCode >= 0
                ? QStringLiteral(", weak CC %1 %2/%3")
                      .arg(dominantWeakEmbColorCode)
                      .arg(dominantWeakEmbColorCodeCount)
                      .arg(embWeakSinceReport)
                : QString();
        const QString embVariantText =
            dominantEmbVariant >= 0
                ? QStringLiteral(", map %1 %2/%3")
                      .arg(embVariantName(dominantEmbVariant))
                      .arg(dominantEmbVariantCount)
                      .arg(embOkSinceReport)
                : QString();
        const QString weakEmbVariantText =
            dominantWeakEmbVariant >= 0
                ? QStringLiteral(", weak map %1 %2/%3")
                      .arg(embVariantName(dominantWeakEmbVariant))
                      .arg(dominantWeakEmbVariantCount)
                      .arg(embWeakSinceReport)
                : QString();
        const QString embErrorText =
            embOkSinceReport > 0
                ? QStringLiteral(", QR e0/e1 %1/%2, timing %3 (%4/%5)")
                      .arg(embErrorHistogram[0])
                      .arg(embErrorHistogram[1])
                      .arg(signedOffsetText(dominantEmbTimingOffset))
                      .arg(dominantEmbTimingOffsetCount)
                      .arg(embOkSinceReport)
                : QString();
        const QString weakEmbErrorText =
            embWeakSinceReport > 0
                ? QStringLiteral(", weak QR e2 %1/%2, timing %3 (%4/%2)")
                      .arg(embWeakErrorHistogram[2])
                      .arg(embWeakSinceReport)
                      .arg(signedOffsetText(dominantWeakEmbTimingOffset))
                      .arg(dominantWeakEmbTimingOffsetCount)
                : QString();
        const QString embText =
            embOkSinceReport > 0
                ? QStringLiteral(", EMB anchors %1, corrected %2/%3, weak %4, CC %5, PI %6/%2, %7, QR corrected %8%9%10")
                      .arg(embAnchorsSinceReport)
                      .arg(embOkSinceReport)
                      .arg(embOkSinceReport + embWeakSinceReport + embFailSinceReport)
                      .arg(embWeakSinceReport)
                      .arg(embCcText)
                      .arg(embPrivacyCount)
                      .arg(dominantEmbLcss >= 0 ? cachLcssText(dominantEmbLcss) : QStringLiteral("LCSS ?"))
                      .arg(embCorrectedErrors)
                      .arg(embVariantText)
                      .arg(embErrorText)
                : (embWeakSinceReport > 0
                       ? QStringLiteral(", EMB anchors %1, corrected 0/%2, weak %3%4%5%6")
                             .arg(embAnchorsSinceReport)
                             .arg(embWeakSinceReport + embFailSinceReport)
                             .arg(embWeakSinceReport)
                             .arg(weakEmbCcText)
                             .arg(weakEmbVariantText)
                             .arg(weakEmbErrorText)
                : (embFailSinceReport > 0
                       ? QStringLiteral(", EMB anchors %1, 0/%2 ok")
                             .arg(embAnchorsSinceReport)
                             .arg(embFailSinceReport)
                       : QString()));
        const QString polarityFlipText =
            polarityFlipsSinceReport > 0
                ? QStringLiteral(", polarity flips %1").arg(polarityFlipsSinceReport)
                : QString();
        const QString voiceLcMetricText =
            (labHintsEnabled ||
             voiceLcAssemblySinceReport > 0 ||
             voiceLcFragmentHistogram[1] > 0 ||
             voiceLcFragmentHistogram[2] > 0 ||
             voiceLcFragmentHistogram[3] > 0)
                ? QStringLiteral(", LC fragments F/C/L %1/%2/%3 usable %4/%5/%6, candidates %7, BPTC %8, strict %9, lock TG/SRC %10/%11/%12")
                      .arg(voiceLcFragmentHistogram[1])
                      .arg(voiceLcFragmentHistogram[3])
                      .arg(voiceLcFragmentHistogram[2])
                      .arg(voiceLcUsableFragmentHistogram[1])
                      .arg(voiceLcUsableFragmentHistogram[3])
                      .arg(voiceLcUsableFragmentHistogram[2])
                      .arg(voiceLcAssemblySinceReport)
                      .arg(voiceLcBptcSinceReport)
                      .arg(voiceLcStrictSinceReport)
                      .arg(voiceLcLabTargetMatchSinceReport)
                      .arg(voiceLcLabSourceMatchSinceReport)
                      .arg(voiceLcLabFullMatchSinceReport)
                : QString();
        const QString voiceLcRawText = voiceLcRawSummaryText() + voiceLcMetricText;
        const QString voiceCadenceText =
            selectedVoicePayloadCadenceSymbols > 0
                ? QStringLiteral(", voice cadence %1 ms CC %2 votes 30/60 %3/%4")
                      .arg(1000.0 * selectedVoicePayloadCadenceSymbols / DMR_SYMBOL_RATE, 0, 'f', 0)
                      .arg(selectedVoicePayloadColorCode >= 0
                               ? QString::number(selectedVoicePayloadColorCode)
                               : QStringLiteral("?"))
                      .arg(voicePayloadCadenceScore30)
                      .arg(voicePayloadCadenceScore60) +
                  voicePayloadColorScoreSummaryText()
                : ((voicePayloadCadenceScore30 > 0 || voicePayloadCadenceScore60 > 0)
                       ? QStringLiteral(", voice cadence pending votes 30/60 %1/%2")
                             .arg(voicePayloadCadenceScore30)
                             .arg(voicePayloadCadenceScore60) +
                         voicePayloadColorScoreSummaryText()
                       : QString());
        const QString signalQualityText = signalQualitySummaryText();

        lastReportSample = hit.absoluteSample;
        const QString intervalText =
            spacingSamplesCount > 0
                ? QStringLiteral(", interval %1 ms avg (%2-%3), burst step ~%4 ms (%5/%6)")
                      .arg(spacingMsSum / static_cast<double>(spacingSamplesCount), 0, 'f', 1)
                      .arg(spacingMsMin, 0, 'f', 1)
                      .arg(spacingMsMax, 0, 'f', 1)
                      .arg(dominantIntervalBin * 30)
                      .arg(dominantIntervalCount)
                      .arg(spacingSamplesCount)
                : QString();
        const QString cachText =
            cachOkSinceReport > 0
                ? QStringLiteral(", CACH strict %1/%3, search %2/%3 ok, Ch %4, %5, timing %6 (%7/%2), map %8")
                      .arg(cachStrictOkSinceReport)
                      .arg(cachOkSinceReport)
                      .arg(cachOkSinceReport + cachFailSinceReport)
                      .arg(dominantCachChannel > 0 ? QString::number(dominantCachChannel) : QStringLiteral("?"))
                      .arg(dominantLcss >= 0 ? cachLcssText(dominantLcss) : QStringLiteral("TACT"))
                      .arg(timingOffsetText)
                      .arg(dominantCachTimingOffsetCount)
                      .arg(cachMapText)
                : (cachFailSinceReport > 0
                       ? QStringLiteral(", CACH 0/%1 ok").arg(cachFailSinceReport)
                       : QString());
        const bool shortBurstReport =
            confirmedSyncsSinceReport <= 1 &&
            spacingSamplesCount == 0 &&
            embOkSinceReport == 0;
        if (shortBurstReport) {
            const QString embBriefText =
                embWeakSinceReport > 0
                    ? QStringLiteral(", EMB weak only %1/%2")
                          .arg(embWeakSinceReport)
                          .arg(embWeakSinceReport + embFailSinceReport)
                    : (embFailSinceReport > 0
                           ? QStringLiteral(", EMB 0/%1 ok").arg(embFailSinceReport)
                           : QString());
            result.decodedText =
                QStringLiteral("[DMR%1] short %2 burst: %3 accepted sync, %4 candidates, phase %5, polarity %6, best %7/24 %8 dB%9%10%11%12%13%14\n")
                    .arg(lockedFrequencySuffix())
                    .arg(patternName)
                    .arg(confirmedSyncsSinceReport)
                    .arg(candidateSyncsSinceReport)
                    .arg(dominantPhase)
                    .arg(bestPolarityInvertedSinceReport ? QStringLiteral("inverted") : QStringLiteral("normal"))
                    .arg(bestScoreSinceReport)
                    .arg(bestQualitySinceReport, 0, 'f', 1)
                    .arg(polarityFlipText)
                    .arg(cachText)
                    .arg(embBriefText)
                    .arg(voiceLcRawText)
                    .arg(voiceCadenceText)
                    .arg(signalQualityText);
        } else {
            result.decodedText =
                QStringLiteral("[DMR%1] %2: %3 accepted syncs, %4 candidates, dominant phase %5, locked phase %6, polarity %7, best %8/24 %9 dB%10%11%12%13%14%15%16%17\n")
                    .arg(lockedFrequencySuffix())
                    .arg(patternName)
                    .arg(confirmedSyncsSinceReport)
                    .arg(candidateSyncsSinceReport)
                    .arg(dominantPhase)
                    .arg(lockedPhase)
                    .arg(bestPolarityInvertedSinceReport ? QStringLiteral("inverted") : QStringLiteral("normal"))
                    .arg(bestScoreSinceReport)
                    .arg(bestQualitySinceReport, 0, 'f', 1)
                    .arg(intervalText)
                    .arg(cachText)
                    .arg(slotTypeText)
                    .arg(embText)
                    .arg(voiceLcRawText)
                    .arg(polarityFlipText)
                    .arg(voiceCadenceText)
                    .arg(signalQualityText);
        }
    }

    if (!pendingDecodedText.isEmpty()) {
        result.decodedText = pendingDecodedText + result.decodedText;
    }

    if (!result.decodedText.isEmpty()) {
        qDebug().noquote() << result.decodedText.trimmed();
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DMR] sync"
                     << hit.pattern->name
                     << "score" << hit.score
                     << "outer" << hit.outerScore
                     << "errors" << hit.errors
                     << "phase" << hit.phase
                     << "inverted" << hit.inverted
                     << "qualityDb" << hit.qualityDb;
        }
        confirmedSyncsSinceReport = 0;
        candidateSyncsSinceReport = 0;
        polarityFlipsSinceReport = 0;
        bestScoreSinceReport = 0;
        bestPhaseSinceReport = 0;
        bestQualitySinceReport = 0.0;
        spacingSamplesCount = 0;
        spacingMsSum = 0.0;
        spacingMsMin = 0.0;
        spacingMsMax = 0.0;
        std::fill(std::begin(phaseHistogram), std::end(phaseHistogram), 0);
        std::fill(std::begin(intervalHistogram), std::end(intervalHistogram), 0);
        cachOkSinceReport = 0;
        cachStrictOkSinceReport = 0;
        cachFailSinceReport = 0;
        std::fill(std::begin(cachChannelHistogram), std::end(cachChannelHistogram), 0);
        std::fill(std::begin(cachLcssHistogram), std::end(cachLcssHistogram), 0);
        std::fill(std::begin(cachTimingOffsetHistogram), std::end(cachTimingOffsetHistogram), 0);
        cachMapEtsiCount = 0;
        cachMapLegacyCount = 0;
        slotTypeOkSinceReport = 0;
        slotTypeFailSinceReport = 0;
        std::fill(std::begin(colorCodeHistogram), std::end(colorCodeHistogram), 0);
        std::fill(std::begin(dataTypeHistogram), std::end(dataTypeHistogram), 0);
        slotTypeCorrectedErrors = 0;
        embOkSinceReport = 0;
        embAnchorsSinceReport = 0;
        embWeakSinceReport = 0;
        embFailSinceReport = 0;
        std::fill(std::begin(embColorCodeHistogram), std::end(embColorCodeHistogram), 0);
        std::fill(std::begin(embLcssHistogram), std::end(embLcssHistogram), 0);
        embPrivacyCount = 0;
        embCorrectedErrors = 0;
        std::fill(std::begin(embVariantHistogram), std::end(embVariantHistogram), 0);
        std::fill(std::begin(embWeakVariantHistogram), std::end(embWeakVariantHistogram), 0);
        std::fill(std::begin(embErrorHistogram), std::end(embErrorHistogram), 0);
        std::fill(std::begin(embWeakErrorHistogram), std::end(embWeakErrorHistogram), 0);
        std::fill(std::begin(embWeakColorCodeHistogram), std::end(embWeakColorCodeHistogram), 0);
        std::fill(std::begin(embTimingOffsetHistogram), std::end(embTimingOffsetHistogram), 0);
        std::fill(std::begin(embWeakTimingOffsetHistogram), std::end(embWeakTimingOffsetHistogram), 0);
        resetSignalQualityCounters();
        voiceLcRawSinceReport.clear();
        voiceLcAssemblySinceReport = 0;
        voiceLcBptcSinceReport = 0;
        voiceLcStrictSinceReport = 0;
        voiceLcLabTargetMatchSinceReport = 0;
        voiceLcLabSourceMatchSinceReport = 0;
        voiceLcLabFullMatchSinceReport = 0;
        std::fill(std::begin(voiceLcFragmentHistogram),
                  std::end(voiceLcFragmentHistogram),
                  0);
        std::fill(std::begin(voiceLcUsableFragmentHistogram),
                  std::end(voiceLcUsableFragmentHistogram),
                  0);
    }

    return finalizeResult(result);
}

QString DmrDecoder::voiceLcRawSummaryText() const {
    if (voiceLcRawSinceReport.empty()) {
        return voiceServiceSummaryText();
    }

    QString dominantRaw;
    int dominantCount = 0;
    for (const QString &candidate : voiceLcRawSinceReport) {
        int count = 0;
        for (const QString &other : voiceLcRawSinceReport) {
            if (other == candidate) {
                ++count;
            }
        }
        if (count > dominantCount) {
            dominantRaw = candidate;
            dominantCount = count;
        }
    }

    const EmbeddedLcDecodeResult decodedLc = decodeEmbeddedLc72FromBptc(dominantRaw);
    if (!fobosVerboseLoggingEnabled() && !decodedLc.lc72.isEmpty()) {
        const auto hexByte = [](int value) {
            return QStringLiteral("0x%1")
                .arg(value < 0 ? 0 : value, 2, 16, QLatin1Char('0'))
                .toUpper();
        };
        return QStringLiteral(", LC TG %1, SRC %2, FLCO %3, SO %4, BPTC %5")
            .arg(decodedLc.target)
            .arg(decodedLc.source)
            .arg(hexByte(decodedLc.flco))
            .arg(hexByte(decodedLc.serviceOptions))
            .arg(decodedLc.corrected);
    }

    return QStringLiteral(", LC raw %1 (%2/%3)")
        .arg(dominantRaw)
        .arg(dominantCount)
        .arg(static_cast<int>(voiceLcRawSinceReport.size())) +
        (decodedLc.lc72.isEmpty()
             ? QString()
             : QStringLiteral(", %1").arg(embeddedLcInfoText(decodedLc)));
}

int DmrDecoder::currentServiceTimeslot() const {
    if (labHintsEnabled &&
        labExpectedTimeslot >= 1 &&
        labExpectedTimeslot <= 2) {
        return labExpectedTimeslot;
    }
    if (lastVoiceService.timeslot >= 1 && lastVoiceService.timeslot <= 2) {
        return lastVoiceService.timeslot;
    }
    if (lastObservedTimeslot >= 1 && lastObservedTimeslot <= 2) {
        return lastObservedTimeslot;
    }
    return 0;
}

QString DmrDecoder::voiceServiceSummaryText() const {
    if (!lastVoiceService.valid || !haveLock) {
        return QString();
    }

    const auto hexByte = [](int value) {
        return QStringLiteral("0x%1")
            .arg(value < 0 ? 0 : value, 2, 16, QLatin1Char('0'))
            .toUpper();
    };

    return QStringLiteral(", last LC CC %1, TS %2, TG %3, SRC %4, FLCO %5, SO %6, BPTC %7")
        .arg(lastVoiceService.colorCode >= 0
                 ? QString::number(lastVoiceService.colorCode)
                 : QStringLiteral("?"))
        .arg(currentServiceTimeslot() > 0
                 ? QString::number(currentServiceTimeslot())
                 : QStringLiteral("?"))
        .arg(lastVoiceService.target)
        .arg(lastVoiceService.source)
        .arg(hexByte(lastVoiceService.flco))
        .arg(hexByte(lastVoiceService.serviceOptions))
        .arg(lastVoiceService.bptcCorrections);
}

QString DmrDecoder::serviceStatusText() const {
    if (!haveLock) {
        return QString();
    }

    const int colorCode =
        lastVoiceService.valid && lastVoiceService.colorCode >= 0
            ? lastVoiceService.colorCode
            : (selectedVoicePayloadColorCode >= 0
                   ? selectedVoicePayloadColorCode
                   : (labHintsEnabled ? labExpectedColorCode : -1));
    const int timeslot = currentServiceTimeslot();
    const QString base =
        QStringLiteral("DMR: CC %1 TS %2")
            .arg(colorCode >= 0 ? QString::number(colorCode) : QStringLiteral("?"))
            .arg(timeslot > 0 ? QString::number(timeslot) : QStringLiteral("?"));

    if (lastVoiceService.valid) {
        const auto hexByte = [](int value) {
            return QStringLiteral("0x%1")
                .arg(value < 0 ? 0 : value, 2, 16, QLatin1Char('0'))
                .toUpper();
        };
        return QStringLiteral("%1, LC confirmed, TG %2, SRC %3, FLCO %4, SO %5, %6, BPTC %7")
            .arg(base)
            .arg(lastVoiceService.target)
            .arg(lastVoiceService.source)
            .arg(hexByte(lastVoiceService.flco))
            .arg(hexByte(lastVoiceService.serviceOptions))
            .arg(lastVoiceService.method)
            .arg(lastVoiceService.bptcCorrections);
    }

    return QStringLiteral("%1, sync locked, LC/TG/SRC pending").arg(base);
}

void DmrDecoder::rememberVoiceServiceInfo(int colorCode,
                                          quint32 target,
                                          quint32 source,
                                          int flco,
                                          int fid,
                                          int serviceOptions,
                                          int bptcCorrections,
                                          quint64 absoluteSample,
                                          const QString &method) {
    if (target == 0 || source == 0) {
        return;
    }

    lastVoiceService.valid = true;
    lastVoiceService.colorCode = colorCode;
    lastVoiceService.timeslot = currentServiceTimeslot();
    lastVoiceService.target = target;
    lastVoiceService.source = source;
    lastVoiceService.flco = flco;
    lastVoiceService.fid = fid;
    lastVoiceService.serviceOptions = serviceOptions;
    lastVoiceService.bptcCorrections = bptcCorrections;
    lastVoiceService.method = method;
    lastVoiceService.absoluteSample = absoluteSample;

}

void DmrDecoder::appendSamples(const QByteArray &pcmData) {
    const int sampleCount = pcmData.size() / static_cast<int>(sizeof(qint16));
    const char *raw = pcmData.constData();
    sampleBuffer.resize(sampleBuffer.size() + static_cast<std::size_t>(sampleCount));

    for (int i = 0; i < sampleCount; ++i) {
        const int rawSample = readPcm16Le(raw + i * static_cast<int>(sizeof(qint16)));
        const float sample = static_cast<float>(rawSample) / 32768.0f;
        ++pcmSamplesSinceReport;
        pcmSumSinceReport += sample;
        pcmSquareSumSinceReport += static_cast<double>(sample) * static_cast<double>(sample);
        const double absSample = std::abs(static_cast<double>(sample));
        if (absSample > pcmAbsPeakSinceReport) {
            pcmAbsPeakSinceReport = absSample;
        }
        if (rawSample <= -32760 || rawSample >= 32760) {
            ++pcmClippedSinceReport;
        }
        dcEstimate += 0.00035 * (sample - dcEstimate);
        const float centered = static_cast<float>(sample - dcEstimate);
        averageMagnitude += 0.0006 * (std::abs(centered) - averageMagnitude);
        sampleBuffer[static_cast<std::size_t>(sampleBuffer.size() - sampleCount + i)] = centered;
    }

    totalSamples += static_cast<quint64>(sampleCount);
    const std::size_t maxSamples =
        static_cast<std::size_t>(DMR_MAX_BUFFER_SYMBOLS * samplesPerSymbol + DMR_SYNC_SYMBOLS * samplesPerSymbol);
    while (sampleBuffer.size() > maxSamples) {
        sampleBuffer.pop_front();
    }
}

QString DmrDecoder::signalQualitySummaryText() const {
    QString text;
    if (pcmSamplesSinceReport > 0) {
        const double count = static_cast<double>(pcmSamplesSinceReport);
        const double dc = pcmSumSinceReport / count;
        const double rms = std::sqrt((std::max)(0.0, pcmSquareSumSinceReport / count));
        const double clipPercent =
            100.0 * static_cast<double>(pcmClippedSinceReport) / count;
        text += QStringLiteral(", PCM pk %1 rms %2 dc %3 clip %4%")
                    .arg(pcmAbsPeakSinceReport, 0, 'f', 2)
                    .arg(rms, 0, 'f', 2)
                    .arg(dc, 0, 'f', 2)
                    .arg(clipPercent, 0, 'f', 3);
    }

    if (ambePayloadsSinceReport > 0 || ambeFecDecodeFailSinceReport > 0) {
        const double averageCorrections =
            ambePayloadsSinceReport > 0
                ? static_cast<double>(ambeFecCorrectionsSinceReport) /
                      static_cast<double>(ambePayloadsSinceReport)
                : 0.0;
        text += QStringLiteral(", AMBE FEC %1 avg %2 hist0-6 %3/%4/%5/%6/%7/%8/%9")
                    .arg(ambePayloadsSinceReport)
                    .arg(averageCorrections, 0, 'f', 2)
                    .arg(ambeFecCorrectionHistogram[0])
                    .arg(ambeFecCorrectionHistogram[1])
                    .arg(ambeFecCorrectionHistogram[2])
                    .arg(ambeFecCorrectionHistogram[3])
                    .arg(ambeFecCorrectionHistogram[4])
                    .arg(ambeFecCorrectionHistogram[5])
                    .arg(ambeFecCorrectionHistogram[6]);
        text += QStringLiteral(", AMBE layout %1")
                    .arg(QString::fromLatin1(dmrAmbeLayoutName(lastVoicePayloadAmbeLayout)));
        text += QStringLiteral(", AMBE map %1")
                    .arg(voicePayloadBitMapName(lastVoicePayloadBitMapVariant));
        if (ambeFecDecodeFailSinceReport > 0) {
            text += QStringLiteral(", AMBE FEC fail %1").arg(ambeFecDecodeFailSinceReport);
        }
    }
    if (voicePayloadFallbackSinceReport > 0) {
        text += QStringLiteral(", voice fallback %1").arg(voicePayloadFallbackSinceReport);
    }

    return text;
}

QString DmrDecoder::voicePayloadColorScoreSummaryText() const {
    if (!labLoggingEnabled() &&
        selectedVoicePayloadColorCode < 0 &&
        (!labHintsEnabled || labExpectedColorCode < 0)) {
        return QString();
    }

    std::array<int, 3> topColor = {-1, -1, -1};
    std::array<int, 3> topScore = {0, 0, 0};
    for (int colorCode = 0;
         colorCode < static_cast<int>(std::size(voicePayloadColorCodeScore));
         ++colorCode) {
        const int score = voicePayloadColorCodeScore[colorCode];
        for (int slot = 0; slot < static_cast<int>(topScore.size()); ++slot) {
            if (score > topScore[slot]) {
                for (int move = static_cast<int>(topScore.size()) - 1; move > slot; --move) {
                    topScore[move] = topScore[move - 1];
                    topColor[move] = topColor[move - 1];
                }
                topScore[slot] = score;
                topColor[slot] = colorCode;
                break;
            }
        }
    }

    QStringList parts;
    for (int index = 0; index < static_cast<int>(topScore.size()); ++index) {
        if (topColor[index] >= 0 && topScore[index] > 0) {
            parts << QStringLiteral("%1:%2").arg(topColor[index]).arg(topScore[index]);
        }
    }
    if (parts.isEmpty()) {
        return QString();
    }

    QString text = QStringLiteral(", voice CC scores %1").arg(parts.join(QLatin1Char('/')));
    if (labHintsEnabled) {
        QStringList hints;
        if (labExpectedColorCode >= 0) {
            hints << QStringLiteral("CC%1").arg(labExpectedColorCode);
        }
        if (labExpectedTimeslot > 0) {
            hints << QStringLiteral("TS%1").arg(labExpectedTimeslot);
        }
        if (labExpectedTargetId > 0) {
            hints << QStringLiteral("TG%1").arg(labExpectedTargetId);
        }
        if (labExpectedSourceId > 0) {
            hints << QStringLiteral("SRC%1").arg(labExpectedSourceId);
        }
        if (!hints.isEmpty()) {
            text += QStringLiteral(", lock %1").arg(hints.join(QLatin1Char('/')));
        }
    }
    return text;
}

bool DmrDecoder::labLoggingEnabled() const {
    return fobosVerboseLoggingEnabled();
}

void DmrDecoder::resetSignalQualityCounters() {
    pcmSamplesSinceReport = 0;
    pcmSumSinceReport = 0.0;
    pcmSquareSumSinceReport = 0.0;
    pcmAbsPeakSinceReport = 0.0;
    pcmClippedSinceReport = 0;
    ambePayloadsSinceReport = 0;
    ambeFecCorrectionsSinceReport = 0;
    ambeFecDecodeFailSinceReport = 0;
    voicePayloadFallbackSinceReport = 0;
    std::fill(std::begin(ambeFecCorrectionHistogram),
              std::end(ambeFecCorrectionHistogram),
              0);
}

bool DmrDecoder::findBestSync(SyncHit &hit, quint64 minimumAbsoluteSample) const {
    if (sampleBuffer.size() < static_cast<std::size_t>(DMR_SYNC_SYMBOLS * samplesPerSymbol) ||
        averageMagnitude < 0.002) {
        return false;
    }

    SyncHit best;
    const int symbolCapacity = static_cast<int>(sampleBuffer.size()) / samplesPerSymbol;
    if (symbolCapacity < DMR_SYNC_SYMBOLS) {
        return false;
    }

    std::vector<int> phaseCandidates;
    phaseCandidates.reserve(samplesPerSymbol);
    const auto addPhaseCandidate = [this, &phaseCandidates](int phase) {
        if (samplesPerSymbol <= 0) {
            return;
        }
        phase %= samplesPerSymbol;
        if (phase < 0) {
            phase += samplesPerSymbol;
        }
        if (std::find(phaseCandidates.begin(), phaseCandidates.end(), phase) ==
            phaseCandidates.end()) {
            phaseCandidates.push_back(phase);
        }
    };

    std::vector<const SyncPattern *> patternCandidates;
    patternCandidates.reserve(std::size(DMR_SYNC_PATTERNS));
    const bool recentLock =
        haveLock &&
        lastConfirmedSyncSample > 0 &&
        totalSamples < lastConfirmedSyncSample +
                           static_cast<quint64>(std::lround(0.36 * activeSampleRate));
    const bool recentVoiceLock =
        recentLock &&
        lockedPatternName.contains(QStringLiteral("Voice"), Qt::CaseInsensitive);
    if (recentLock && !recentVoiceLock && lockedPhase >= 0) {
        addPhaseCandidate(lockedPhase);
        addPhaseCandidate(lockedPhase - 1);
        addPhaseCandidate(lockedPhase + 1);
        addPhaseCandidate(lockedPhase - 2);
        addPhaseCandidate(lockedPhase + 2);
    } else {
        const int phaseStep = (std::max)(1, (std::min)(4, samplesPerSymbol / 10));
        for (int phase = 0; phase < samplesPerSymbol; phase += phaseStep) {
            addPhaseCandidate(phase);
        }
    }

    if (recentVoiceLock) {
        for (const SyncPattern &pattern : DMR_SYNC_PATTERNS) {
            if (std::strcmp(pattern.kind, "voice") == 0) {
                patternCandidates.push_back(&pattern);
            }
        }
    } else if (recentLock && !lockedPatternName.isEmpty()) {
        for (const SyncPattern &pattern : DMR_SYNC_PATTERNS) {
            if (lockedPatternName == QString::fromLatin1(pattern.name)) {
                patternCandidates.push_back(&pattern);
                break;
            }
        }
    }
    if (patternCandidates.empty()) {
        for (const SyncPattern &pattern : DMR_SYNC_PATTERNS) {
            patternCandidates.push_back(&pattern);
        }
    }

    std::vector<bool> polarityCandidates;
    polarityCandidates.reserve(2);
    if (recentLock && !recentVoiceLock) {
        polarityCandidates.push_back(lockedPolarityInverted);
    } else {
        polarityCandidates.push_back(false);
        polarityCandidates.push_back(true);
    }

    const quint64 bufferStartSample = totalSamples > sampleBuffer.size()
                                          ? totalSamples - static_cast<quint64>(sampleBuffer.size())
                                          : 0;
    for (const int phase : phaseCandidates) {
        const int maxStart = (static_cast<int>(sampleBuffer.size()) - phase) / samplesPerSymbol - DMR_SYNC_SYMBOLS;
        if (maxStart < 0) {
            continue;
        }
        for (int symbolStart = 0; symbolStart <= maxStart; ++symbolStart) {
            const quint64 absoluteSample =
                bufferStartSample + static_cast<quint64>(symbolStart * samplesPerSymbol + phase);
            if (absoluteSample < minimumAbsoluteSample) {
                continue;
            }
            for (const SyncPattern *patternPtr : patternCandidates) {
                const SyncPattern &pattern = *patternPtr;
                for (const bool inverted : polarityCandidates) {
                    const int startIndex = phase + symbolStart * samplesPerSymbol;
                    int outerScore = 0;
                    const int score = scoreSyncSymbols(sampleBuffer,
                                                       startIndex,
                                                       samplesPerSymbol,
                                                       pattern.symbols,
                                                       inverted,
                                                       &outerScore);
                    if (score < DMR_CANDIDATE_SYNC_SCORE ||
                        outerScore < DMR_CANDIDATE_OUTER_SCORE) {
                        continue;
                    }

                    SyncHit candidate;
                    candidate.pattern = &pattern;
                    candidate.score = score;
                    candidate.errors = DMR_SYNC_SYMBOLS - score;
                    candidate.outerScore = outerScore;
                    candidate.phase = phase;
                    candidate.symbolIndex = symbolStart;
                    candidate.inverted = inverted;
                    candidate.qualityDb = syncQualityDb(score);
                    candidate.absoluteSample = absoluteSample;
                    const bool candidateVoice = std::strcmp(candidate.pattern->kind, "voice") == 0;
                    const bool bestVoice = best.pattern && std::strcmp(best.pattern->kind, "voice") == 0;
                    const bool candidateMatchesLockedPolarity =
                        haveLock &&
                        lockedPatternName == QString::fromLatin1(candidate.pattern->name) &&
                        candidate.inverted == lockedPolarityInverted;
                    const bool bestMatchesLockedPolarity =
                        haveLock &&
                        best.pattern &&
                        lockedPatternName == QString::fromLatin1(best.pattern->name) &&
                        best.inverted == lockedPolarityInverted;
                    const bool lockedPolarityWithinMargin =
                        best.pattern &&
                        candidate.score == best.score &&
                        candidateVoice == bestVoice &&
                        candidateMatchesLockedPolarity &&
                        !bestMatchesLockedPolarity &&
                        candidate.outerScore + DMR_LOCKED_POLARITY_OUTER_MARGIN >= best.outerScore;
                    const bool outerScoreWins =
                        best.pattern &&
                        candidate.score == best.score &&
                        candidate.outerScore > best.outerScore &&
                        !(bestMatchesLockedPolarity &&
                          !candidateMatchesLockedPolarity &&
                          best.outerScore + DMR_LOCKED_POLARITY_OUTER_MARGIN >= candidate.outerScore);
                    const bool preferCandidate =
                        !best.pattern ||
                        candidate.score > best.score ||
                        lockedPolarityWithinMargin ||
                        outerScoreWins ||
                        (candidate.score == best.score &&
                         candidate.outerScore == best.outerScore &&
                        candidateVoice &&
                        !bestVoice) ||
                        (candidate.score == best.score &&
                         candidate.outerScore == best.outerScore &&
                         candidateVoice == bestVoice &&
                         candidateMatchesLockedPolarity &&
                         !bestMatchesLockedPolarity) ||
                        (candidate.score == best.score &&
                         candidate.outerScore == best.outerScore &&
                         candidateVoice == bestVoice &&
                         !candidate.inverted &&
                         best.inverted) ||
                        (candidate.score == best.score &&
                         candidate.outerScore == best.outerScore &&
                         candidateVoice == bestVoice &&
                         candidate.inverted == best.inverted &&
                         candidate.absoluteSample > best.absoluteSample);

                    if (preferCandidate) {
                        best = candidate;
                    }
                }
            }
        }
    }

    if (!best.pattern || best.score < DMR_CANDIDATE_SYNC_SCORE) {
        return false;
    }

    hit = best;
    return true;
}

DmrDecoder::CachInfo DmrDecoder::decodeCachBeforeSync(const SyncHit &hit, bool allowTimingSearch) const {
    CachInfo info;
    if (!hit.pattern || std::strcmp(hit.pattern->source, "base station") != 0) {
        return info;
    }

    const int syncSampleIndex = hit.phase + hit.symbolIndex * samplesPerSymbol;
    const int cachStartSampleIndex =
        syncSampleIndex - (DMR_SYMBOLS_BEFORE_SYNC + DMR_CACH_SYMBOLS) * samplesPerSymbol;
    const int cachEndSampleIndex =
        cachStartSampleIndex + (DMR_CACH_SYMBOLS - 1) * samplesPerSymbol + samplesPerSymbol / 2;
    if (cachStartSampleIndex < 0 ||
        cachEndSampleIndex < 0 ||
        cachEndSampleIndex >= static_cast<int>(sampleBuffer.size())) {
        return info;
    }

    float minSync = std::numeric_limits<float>::max();
    float maxSync = std::numeric_limits<float>::lowest();
    for (int i = 0; i < DMR_SYNC_SYMBOLS; ++i) {
        const int sampleIndex =
            syncSampleIndex + i * samplesPerSymbol + samplesPerSymbol / 2;
        if (sampleIndex < 0 || sampleIndex >= static_cast<int>(sampleBuffer.size())) {
            return info;
        }
        const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
        minSync = (std::min)(minSync, sample);
        maxSync = (std::max)(maxSync, sample);
    }
    if (maxSync - minSync < 0.004f) {
        return info;
    }

    const float center = 0.5f * (maxSync + minSync);
    const auto sampleToDibit = [&](float sample, float thresholdRatio) -> int {
        const float upperMid = center + thresholdRatio * (maxSync - center);
        const float lowerMid = center + thresholdRatio * (minSync - center);
        const float corrected = hit.inverted ? -sample : sample;
        const float correctedCenter = hit.inverted ? -center : center;
        const float correctedUpper = hit.inverted ? -lowerMid : upperMid;
        const float correctedLower = hit.inverted ? -upperMid : lowerMid;
        if (corrected > correctedCenter) {
            return corrected > correctedUpper ? 3 : 2;
        }
        return corrected < correctedLower ? 1 : 0;
    };

    std::vector<int> timingOffsets;
    const int centerTimingOffset = samplesPerSymbol / 2;
    const auto addTimingOffset = [&](int value) {
        value = (std::clamp)(value, 0, (std::max)(0, samplesPerSymbol - 1));
        if (std::find(timingOffsets.begin(), timingOffsets.end(), value) == timingOffsets.end()) {
            timingOffsets.push_back(value);
        }
    };
    if (allowTimingSearch && liveTimingHintEnabled) {
        addTimingOffset(static_cast<int>(std::lround(
            liveTimingPhaseFraction * (std::max)(0, samplesPerSymbol - 1))));
    }
    addTimingOffset(centerTimingOffset);
    if (allowTimingSearch) {
        for (int distance = 1; distance <= 2; ++distance) {
            addTimingOffset(centerTimingOffset - distance);
            addTimingOffset(centerTimingOffset + distance);
        }
    }

    const std::array<float, 3> slicerRatios = {0.625f, 0.55f, 0.70f};
    const std::array<bool, 2> mapVariants = {true, false};

    for (int timingOffset : timingOffsets) {
        for (float slicerRatio : slicerRatios) {
            for (bool etsiMap : mapVariants) {
                bool rawBits[24] = {};
                for (int i = 0; i < DMR_CACH_SYMBOLS; ++i) {
                    const int sampleIndex =
                        cachStartSampleIndex + i * samplesPerSymbol + timingOffset;
                    if (sampleIndex < 0 || sampleIndex >= static_cast<int>(sampleBuffer.size())) {
                        return info;
                    }
                    const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
                    const int dibit =
                        (std::clamp)(sampleToDibit(sample, slicerRatio),
                                     0,
                                     3);
                    appendDibitBits(dibit, etsiMap, rawBits, i * 2);
                }

                bool dataBits[24] = {};
                for (int i = 0; i < 24; ++i) {
                    dataBits[i] = rawBits[DMR_CACH_DEINTERLEAVE[i]];
                }

                int tact = 0;
                for (int i = 0; i < 7; ++i) {
                    tact = (tact << 1) | (dataBits[i] ? 1 : 0);
                }
                if (!hamming743Ok(tact)) {
                    continue;
                }

                info.decoded = true;
                info.accessType = dataBits[0];
                info.channel = dataBits[1] ? 2 : 1;
                info.lcss = (dataBits[2] ? 2 : 0) | (dataBits[3] ? 1 : 0);
                info.timingOffset = timingOffset - samplesPerSymbol / 2;
                info.slicerRatio = slicerRatio;
                info.mapName = etsiMap ? QStringLiteral("ETSI") : QStringLiteral("legacy");
                QString payload;
                payload.reserve(17);
                for (int i = 7; i < 24; ++i) {
                    payload.append(dataBits[i] ? QLatin1Char('1') : QLatin1Char('0'));
                }
                info.payloadBits = payload;
                return info;
            }
        }
    }
    return info;
}

DmrDecoder::SlotTypeInfo DmrDecoder::decodeSlotTypeAroundSync(const SyncHit &hit) const {
    SlotTypeInfo info;
    if (!hit.pattern || std::strcmp(hit.pattern->kind, "data") != 0) {
        return info;
    }

    const int syncSampleIndex = hit.phase + hit.symbolIndex * samplesPerSymbol;
    const int firstSlotTypeSampleIndex = syncSampleIndex - 5 * samplesPerSymbol;
    const int secondSlotTypeSampleIndex = syncSampleIndex + DMR_SYNC_SYMBOLS * samplesPerSymbol;
    const int lastSampleIndex = secondSlotTypeSampleIndex + 4 * samplesPerSymbol + samplesPerSymbol / 2;
    if (firstSlotTypeSampleIndex < 0 ||
        secondSlotTypeSampleIndex < 0 ||
        lastSampleIndex >= static_cast<int>(sampleBuffer.size())) {
        return info;
    }

    float minSync = std::numeric_limits<float>::max();
    float maxSync = std::numeric_limits<float>::lowest();
    for (int i = 0; i < DMR_SYNC_SYMBOLS; ++i) {
        const int sampleIndex =
            syncSampleIndex + i * samplesPerSymbol + samplesPerSymbol / 2;
        if (sampleIndex < 0 || sampleIndex >= static_cast<int>(sampleBuffer.size())) {
            return info;
        }
        const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
        minSync = (std::min)(minSync, sample);
        maxSync = (std::max)(maxSync, sample);
    }
    if (maxSync - minSync < 0.004f) {
        return info;
    }

    const float center = 0.5f * (maxSync + minSync);
    const auto sampleToDibit = [&](float sample, float thresholdRatio) -> int {
        const float upperMid = center + thresholdRatio * (maxSync - center);
        const float lowerMid = center + thresholdRatio * (minSync - center);
        const float corrected = hit.inverted ? -sample : sample;
        const float correctedCenter = hit.inverted ? -center : center;
        const float correctedUpper = hit.inverted ? -lowerMid : upperMid;
        const float correctedLower = hit.inverted ? -upperMid : lowerMid;
        if (corrected > correctedCenter) {
            return corrected > correctedUpper ? 3 : 2;
        }
        return corrected < correctedLower ? 1 : 0;
    };

    int bestData = 0;
    int bestErrors = 21;
    int bestTimingOffset = 0;
    int bestTimingDistance = 99;
    int bestMapPenalty = 99;
    int bestSlicerPenalty = 99;
    float bestSlicerRatio = 0.0f;
    QString bestMapName;
    std::vector<int> timingOffsets;
    timingOffsets.reserve(static_cast<std::size_t>(samplesPerSymbol));
    const auto addTimingOffset = [&](int value) {
        value = (std::clamp)(value, 0, (std::max)(0, samplesPerSymbol - 1));
        if (std::find(timingOffsets.begin(), timingOffsets.end(), value) ==
            timingOffsets.end()) {
            timingOffsets.push_back(value);
        }
    };
    const int centerTimingOffset = samplesPerSymbol / 2;
    const int preferredTimingOffset = liveTimingHintEnabled
        ? static_cast<int>(std::lround(
              liveTimingPhaseFraction * (std::max)(0, samplesPerSymbol - 1)))
        : centerTimingOffset;
    addTimingOffset(preferredTimingOffset);
    addTimingOffset(centerTimingOffset);
    for (int distance = 1; distance <= samplesPerSymbol / 2; ++distance) {
        addTimingOffset(centerTimingOffset - distance);
        addTimingOffset(centerTimingOffset + distance);
    }
    const std::array<float, 3> slicerRatios = {0.625f, 0.55f, 0.70f};
    const std::array<bool, 2> mapVariants = {true, false};

    for (int timingOffset : timingOffsets) {
        const int relativeTimingOffset = timingOffset - samplesPerSymbol / 2;
        const int timingDistance = std::abs(timingOffset - preferredTimingOffset);
        for (int slicerIndex = 0; slicerIndex < static_cast<int>(slicerRatios.size()); ++slicerIndex) {
            const float slicerRatio = slicerRatios[static_cast<std::size_t>(slicerIndex)];
            for (bool etsiMap : mapVariants) {
                bool slotBitsRaw[20] = {};
                bool inRange = true;
                for (int i = 0; i < 5; ++i) {
                    const int sampleIndex = firstSlotTypeSampleIndex + i * samplesPerSymbol + timingOffset;
                    if (sampleIndex < 0 || sampleIndex >= static_cast<int>(sampleBuffer.size())) {
                        inRange = false;
                        break;
                    }
                    const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
                    const int dibit =
                        (std::clamp)(sampleToDibit(sample, slicerRatio),
                                     0,
                                     3);
                    appendDibitBits(dibit, etsiMap, slotBitsRaw, i * 2);
                }
                if (!inRange) {
                    continue;
                }
                for (int i = 0; i < 5; ++i) {
                    const int sampleIndex = secondSlotTypeSampleIndex + i * samplesPerSymbol + timingOffset;
                    if (sampleIndex < 0 || sampleIndex >= static_cast<int>(sampleBuffer.size())) {
                        inRange = false;
                        break;
                    }
                    const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
                    const int dibit =
                        (std::clamp)(sampleToDibit(sample, slicerRatio),
                                     0,
                                     3);
                    appendDibitBits(dibit, etsiMap, slotBitsRaw, 10 + i * 2);
                }
                if (!inRange) {
                    continue;
                }

                std::array<bool, 20> received = {};
                std::copy(std::begin(slotBitsRaw), std::end(slotBitsRaw), received.begin());

                int localBestData = 0;
                int localBestErrors = 21;
                for (int data = 0; data < 256; ++data) {
                    const std::array<bool, 20> candidate = golay208Codeword(data);
                    const int errors = bitDistance(received, candidate);
                    if (errors < localBestErrors) {
                        localBestErrors = errors;
                        localBestData = data;
                    }
                }

                const int mapPenalty = etsiMap ? 0 : 1;
                const int slicerPenalty = slicerIndex == 0 ? 0 : 1;
                const bool prefer =
                    localBestErrors < bestErrors ||
                    (localBestErrors == bestErrors && timingDistance < bestTimingDistance) ||
                    (localBestErrors == bestErrors &&
                     timingDistance == bestTimingDistance &&
                     mapPenalty < bestMapPenalty) ||
                    (localBestErrors == bestErrors &&
                     timingDistance == bestTimingDistance &&
                     mapPenalty == bestMapPenalty &&
                     slicerPenalty < bestSlicerPenalty);
                if (prefer) {
                    bestErrors = localBestErrors;
                    bestData = localBestData;
                    bestTimingOffset = relativeTimingOffset;
                    bestTimingDistance = timingDistance;
                    bestMapPenalty = mapPenalty;
                    bestSlicerPenalty = slicerPenalty;
                    bestSlicerRatio = slicerRatio;
                    bestMapName = etsiMap ? QStringLiteral("ETSI") : QStringLiteral("legacy");
                }
            }
        }
    }

    if (bestErrors > 3) {
        return info;
    }

    info.decoded = true;
    info.colorCode = (bestData >> 4) & 0x0f;
    info.dataType = bestData & 0x0f;
    info.correctedErrors = bestErrors;
    info.dataTypeName = dmrDataTypeName(info.dataType);
    info.timingOffset = bestTimingOffset;
    info.slicerRatio = bestSlicerRatio;
    info.mapName = bestMapName;
    return info;
}

void DmrDecoder::scanAndForwardDataBursts() {
    if (sampleBuffer.size() < static_cast<std::size_t>(DMR_TRAFFIC_SYMBOLS * samplesPerSymbol) ||
        averageMagnitude < 0.002) {
        return;
    }

    const quint64 bufferStartSample = totalSamples > sampleBuffer.size()
                                          ? totalSamples - static_cast<quint64>(sampleBuffer.size())
                                          : 0;
    quint64 minimumSample = bufferStartSample;
    const quint64 duplicateGap =
        static_cast<quint64>(std::lround(0.020 * activeSampleRate));
    if (lastForwardedDataBurstSample > 0) {
        minimumSample = (std::max)(minimumSample, lastForwardedDataBurstSample + duplicateGap);
    }
    const int phaseStep = (std::max)(1, (std::min)(4, samplesPerSymbol / 10));
    std::vector<int> phaseCandidates;
    phaseCandidates.reserve(samplesPerSymbol / phaseStep + 1);
    for (int phase = 0; phase < samplesPerSymbol; phase += phaseStep) {
        phaseCandidates.push_back(phase);
    }

    for (const int phase : phaseCandidates) {
        const int maxStart = (static_cast<int>(sampleBuffer.size()) - phase) / samplesPerSymbol - DMR_SYNC_SYMBOLS;
        if (maxStart < 0) {
            continue;
        }
        for (int symbolStart = 0; symbolStart <= maxStart; ++symbolStart) {
            const quint64 absoluteSample =
                bufferStartSample + static_cast<quint64>(symbolStart * samplesPerSymbol + phase);
            if (absoluteSample < minimumSample) {
                continue;
            }
            for (const SyncPattern &pattern : DMR_SYNC_PATTERNS) {
                if (std::strcmp(pattern.kind, "data") != 0) {
                    continue;
                }
                for (const bool inverted : {false, true}) {
                    const int startIndex = phase + symbolStart * samplesPerSymbol;
                    int outerScore = 0;
                    const int score = scoreSyncSymbols(sampleBuffer,
                                                       startIndex,
                                                       samplesPerSymbol,
                                                       pattern.symbols,
                                                       inverted,
                                                       &outerScore);
                    if (score < DMR_CONFIRMED_SYNC_SCORE ||
                        outerScore < DMR_CONFIRMED_OUTER_SCORE) {
                        continue;
                    }

                    SyncHit hit;
                    hit.pattern = &pattern;
                    hit.score = score;
                    hit.errors = DMR_SYNC_SYMBOLS - score;
                    hit.outerScore = outerScore;
                    hit.phase = phase;
                    hit.symbolIndex = symbolStart;
                    hit.inverted = inverted;
                    hit.qualityDb = syncQualityDb(score);
                    hit.absoluteSample = absoluteSample;
                    const SlotTypeInfo slotType = decodeSlotTypeAroundSync(hit);
                    if (!slotType.decoded || slotType.dataType != 0) {
                        continue;
                    }
                    const CachInfo strictCach = decodeCachBeforeSync(hit, false);
                    const CachInfo cach = strictCach.decoded ? strictCach : decodeCachBeforeSync(hit, true);
                    if (queueDataBurstForBackend(hit, slotType, cach)) {
                        minimumSample = lastForwardedDataBurstSample + duplicateGap;
                    }
                }
            }
        }
    }
}

bool DmrDecoder::queueDataBurstForBackend(const SyncHit &hit,
                                          const SlotTypeInfo &slotType,
                                          const CachInfo &cach) {
    if (!hit.pattern ||
        std::strcmp(hit.pattern->kind, "data") != 0 ||
        hit.score < DMR_CONFIRMED_SYNC_SCORE ||
        !slotType.decoded ||
        slotType.dataType != 0) {
        return false;
    }
    const quint64 duplicateGap =
        static_cast<quint64>(std::lround(0.020 * activeSampleRate));
    if (lastForwardedDataBurstSample > 0 &&
        hit.absoluteSample < lastForwardedDataBurstSample + duplicateGap) {
        return false;
    }

    PendingEmb dataPayload;
    dataPayload.anchorSample = hit.absoluteSample;
    dataPayload.absoluteSample = hit.absoluteSample;
    dataPayload.burstIndex = -1;
    dataPayload.cadenceSymbols = DMR_SAME_TIMESLOT_CADENCE_SYMBOLS;
    dataPayload.inverted = hit.inverted;
    dataPayload.cachDecoded = cach.decoded;
    dataPayload.cachChannel = cach.channel;
    dataPayload.cachLcss = cach.lcss;
    dataPayload.cachMapName = cach.mapName;
    dataPayload.cachPayloadBits = cach.payloadBits;
    if (!measureSyncLevels(hit, dataPayload.minLevel, dataPayload.maxLevel)) {
        return false;
    }

    const VoicePayloadBits rawDataBurst =
        decodeVoicePayloadAt(dataPayload,
                             hit.inverted,
                             slotType.timingOffset,
                             static_cast<float>(slotType.slicerRatio),
                             true,
                             DMR_AMBE_LAYOUT_SPLIT36,
                             0,
                             true);
    const QString dataDibits =
        rawDataBurst.leftDibits +
        rawDataBurst.syncDibits +
        rawDataBurst.rightDibits;
    if (dataDibits.size() != DMR_TRAFFIC_SYMBOLS) {
        return false;
    }

    const quint64 centralDibit =
        samplesPerSymbol > 0
            ? hit.absoluteSample / static_cast<quint64>(samplesPerSymbol)
            : hit.absoluteSample;
    const quint64 baseDibit =
        centralDibit > static_cast<quint64>(DMR_SYMBOLS_BEFORE_SYNC)
            ? centralDibit - static_cast<quint64>(DMR_SYMBOLS_BEFORE_SYNC)
            : quint64(0);
    const bool expectedColorCodeKnown =
        labHintsEnabled && labExpectedColorCode >= 0 && labExpectedColorCode <= 15;
    if (expectedColorCodeKnown && slotType.colorCode != labExpectedColorCode) {
        if (labLoggingEnabled()) {
            qDebug() << "[DMR] drop data burst for GopherTrunk: color code mismatch"
                     << "type" << slotType.dataTypeName
                     << "cc" << slotType.colorCode
                     << "expectedCC" << labExpectedColorCode
                     << "errors" << slotType.correctedErrors
                     << "sample" << static_cast<qulonglong>(hit.absoluteSample);
        }
        return false;
    }
    if (slotType.correctedErrors > 1) {
        if (labLoggingEnabled()) {
            qDebug() << "[DMR] drop data burst for GopherTrunk: weak slot type"
                     << "type" << slotType.dataTypeName
                     << "cc" << slotType.colorCode
                     << "errors" << slotType.correctedErrors
                     << "sample" << static_cast<qulonglong>(hit.absoluteSample);
        }
        return false;
    }
    int timeslot = 0;
    QString timeslotSource = QStringLiteral("unknown");
    if (labHintsEnabled &&
        labExpectedTimeslot >= 1 &&
        labExpectedTimeslot <= 2) {
        timeslot = labExpectedTimeslot;
        timeslotSource = QStringLiteral("manual");
    } else if (cach.decoded &&
               cach.channel >= 1 &&
               cach.channel <= 2) {
        timeslot = cach.channel;
        timeslotSource = QStringLiteral("cach");
    } else if (lastObservedTimeslot >= 1 && lastObservedTimeslot <= 2) {
        timeslot = lastObservedTimeslot;
        timeslotSource = QStringLiteral("cach-last");
    }
    DibitBurst burst;
    burst.dibits = dataDibits;
    burst.burstKind = QStringLiteral("data");
    burst.dataType = slotType.dataType;
    burst.dataTypeName = slotType.dataTypeName;
    burst.slotTypeErrors = slotType.correctedErrors;
    burst.sample = hit.absoluteSample;
    burst.baseDibit = baseDibit;
    burst.burstIndex = -1;
    burst.cadenceSymbols = DMR_SAME_TIMESLOT_CADENCE_SYMBOLS;
    burst.colorCode = slotType.colorCode;
    burst.timeslot = timeslot;
    burst.timeslotSource = timeslotSource;
    burst.cachDecoded = cach.decoded;
    burst.cachChannel = cach.channel;
    burst.cachLcss = cach.lcss;
    burst.cachMapName = cach.mapName;
    burst.cachPayloadBits = cach.payloadBits;
    pendingDibitBursts.push_back(burst);
    if (pendingDibitBursts.size() > 64) {
        pendingDibitBursts.erase(pendingDibitBursts.begin(),
                                 pendingDibitBursts.begin() +
                                     static_cast<std::ptrdiff_t>(pendingDibitBursts.size() - 64));
    }
    lastForwardedDataBurstSample = hit.absoluteSample;
    if (labLoggingEnabled()) {
        qDebug() << "[DMR] forwarding data burst to GopherTrunk"
                 << "pattern" << hit.pattern->name
                 << "type" << slotType.dataTypeName
                 << "cc" << slotType.colorCode
                 << "errors" << slotType.correctedErrors
                 << "sample" << static_cast<qulonglong>(hit.absoluteSample)
                 << "ts" << timeslot
                 << "tsSource" << timeslotSource;
    }
    return true;
}

bool DmrDecoder::measureSyncLevels(const SyncHit &hit, float &minLevel, float &maxLevel) const {
    const int syncSampleIndex = hit.phase + hit.symbolIndex * samplesPerSymbol;
    minLevel = std::numeric_limits<float>::max();
    maxLevel = std::numeric_limits<float>::lowest();
    for (int i = 0; i < DMR_SYNC_SYMBOLS; ++i) {
        const int sampleIndex =
            syncSampleIndex + i * samplesPerSymbol + samplesPerSymbol / 2;
        if (sampleIndex < 0 || sampleIndex >= static_cast<int>(sampleBuffer.size())) {
            return false;
        }
        const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
        minLevel = (std::min)(minLevel, sample);
        maxLevel = (std::max)(maxLevel, sample);
    }
    return maxLevel - minLevel >= 0.004f;
}

void DmrDecoder::scheduleVoiceEmbBursts(const SyncHit &hit, const CachInfo &anchorCach) {
    if (!hit.pattern ||
        std::strcmp(hit.pattern->kind, "voice") != 0 ||
        hit.score < DMR_CONFIRMED_SYNC_SCORE) {
        return;
    }
    const bool baseStation = std::strcmp(hit.pattern->source, "base station") == 0;
    if (hit.outerScore < DMR_STRONG_ANCHOR_OUTER_SCORE && !(baseStation && anchorCach.decoded)) {
        return;
    }

    const quint64 minAnchorIntervalSamples =
        static_cast<quint64>(std::lround(DMR_VOICE_ANCHOR_MIN_GAP_SECONDS *
                                         activeSampleRate));
    if (lastVoiceEmbAnchorSample > 0 &&
        hit.absoluteSample < lastVoiceEmbAnchorSample + minAnchorIntervalSamples) {
        return;
    }

    float minLevel = 0.0f;
    float maxLevel = 0.0f;
    if (!measureSyncLevels(hit, minLevel, maxLevel)) {
        return;
    }

    lastVoiceEmbAnchorSample = hit.absoluteSample;
    ++embAnchorsSinceReport;

    constexpr int voiceEmbBurstsAfterSync = 5;
    const std::array<int, 1> cadenceCandidates = {DMR_SAME_TIMESLOT_CADENCE_SYMBOLS};

    for (int cadenceSymbols : cadenceCandidates) {
        // A's trailing payload may arrive in the next PCM block, just like B-F.
        for (int i = 0; i <= voiceEmbBurstsAfterSync; ++i) {
            PendingEmb pending;
            pending.anchorSample = hit.absoluteSample;
            pending.absoluteSample =
                hit.absoluteSample +
                static_cast<quint64>(i * cadenceSymbols * samplesPerSymbol);
            pending.burstIndex = i;
            pending.cadenceSymbols = cadenceSymbols;
            pending.inverted = hit.inverted;
            pending.minLevel = minLevel;
            pending.maxLevel = maxLevel;
            pending.cachDecoded = anchorCach.decoded;
            pending.cachChannel = anchorCach.channel;
            pending.cachLcss = anchorCach.lcss;
            pending.cachMapName = anchorCach.mapName;
            pending.cachPayloadBits = anchorCach.payloadBits;

            bool duplicate = false;
            for (const PendingEmb &existing : pendingVoiceEmb) {
                const quint64 distance =
                    existing.absoluteSample > pending.absoluteSample
                        ? existing.absoluteSample - pending.absoluteSample
                        : pending.absoluteSample - existing.absoluteSample;
                if (distance < static_cast<quint64>(samplesPerSymbol * 4) &&
                    existing.burstIndex == pending.burstIndex &&
                    existing.cadenceSymbols == pending.cadenceSymbols) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                pendingVoiceEmb.push_back(pending);
            }
        }
    }

    while (pendingVoiceEmb.size() > 48) {
        pendingVoiceEmb.pop_front();
    }
}

void DmrDecoder::processPendingVoiceEmb() {
    const quint64 payloadReadySpanSamples =
        static_cast<quint64>((DMR_SYNC_SYMBOLS + DMR_SYMBOLS_BEFORE_SYNC) *
                             samplesPerSymbol +
                             samplesPerSymbol);
    const quint64 bufferStartSample = totalSamples > sampleBuffer.size()
                                          ? totalSamples - static_cast<quint64>(sampleBuffer.size())
                                          : 0;

    while (!pendingVoiceEmb.empty()) {
        const PendingEmb pending = pendingVoiceEmb.front();
        if (totalSamples < pending.absoluteSample + payloadReadySpanSamples) {
            break;
        }
        pendingVoiceEmb.pop_front();

        if (pending.absoluteSample < bufferStartSample) {
            ++embFailSinceReport;
            continue;
        }

        if (pending.burstIndex == 0) {
            // A contains voice sync, not EMB. The validated anchor supplies timing.
            queueVoicePayloadFrames(pending, pending.inverted, 0, 0.625f);
            continue;
        }
        const EmbInfo emb = decodeVoiceEmbAt(pending);
        if (!emb.decoded) {
            ++embFailSinceReport;
            const bool fallbackAccepted = acceptsKnownVoicePayloadCadence(pending);
            if (fallbackAccepted) {
                queueVoicePayloadFrames(pending, pending.inverted, 0, 0.625f);
                ++voicePayloadFallbackSinceReport;
            }
            if (labLoggingEnabled()) {
                qDebug() << "[DMR lock] EMB"
                         << "burst" << pending.burstIndex
                         << "cadenceMs" << (1000.0 * pending.cadenceSymbols / DMR_SYMBOL_RATE)
                         << "decoded" << false
                         << "selectedCadenceMs"
                         << (selectedVoicePayloadCadenceSymbols > 0
                                 ? 1000.0 * selectedVoicePayloadCadenceSymbols / DMR_SYMBOL_RATE
                                 : 0.0)
                         << "selectedCC" << selectedVoicePayloadColorCode
                         << "expectedCC" << (labHintsEnabled ? labExpectedColorCode : -1)
                         << "fallback" << fallbackAccepted;
            }
            continue;
        }
        const VoiceEmbeddedBits fragment =
            decodeVoiceEmbeddedFragmentAt(pending,
                                          emb.inverted,
                                          emb.timingOffset,
                                          emb.slicerRatio);
        const bool reliableEmbForLc =
            fragment.decoded &&
            emb.colorCode >= 0 &&
            emb.correctedErrors <= 2;
        if (fragment.decoded && emb.colorCode >= 0 && emb.correctedErrors <= 2) {
            int voteWeight = 1;
            if (emb.correctedErrors == 0) {
                voteWeight = 3;
            } else if (emb.correctedErrors == 1) {
                voteWeight = 2;
            }
            if (emb.lcss >= 1 && emb.lcss <= 3) {
                ++voteWeight;
            }
            voteVoicePayloadCadence(pending.cadenceSymbols,
                                    emb.colorCode,
                                    voteWeight,
                                    pending.absoluteSample,
                                    QStringLiteral("EMB"));
        }
        if (reliableEmbForLc) {
            recordVoiceEmbeddedFragment(pending, emb, fragment);
        }
        const bool embCadenceAccepted = acceptsVoicePayloadCadence(pending, emb);
        const bool labEmbColorMismatch =
            labHintsEnabled &&
            labExpectedColorCode >= 0 &&
            emb.colorCode >= 0 &&
            emb.colorCode != labExpectedColorCode;
        if (embCadenceAccepted) {
            queueVoicePayloadFrames(pending, emb.inverted, emb.timingOffset, emb.slicerRatio);
        } else if (acceptsKnownVoicePayloadCadence(pending)) {
            queueVoicePayloadFrames(pending, pending.inverted, 0, 0.625f);
            ++voicePayloadFallbackSinceReport;
        }
        if (labLoggingEnabled()) {
            qDebug() << "[DMR lock] EMB"
                     << "burst" << pending.burstIndex
                     << "cadenceMs" << (1000.0 * pending.cadenceSymbols / DMR_SYMBOL_RATE)
                     << "decoded" << emb.decoded
                     << "cc" << emb.colorCode
                     << "expectedCC" << (labHintsEnabled ? labExpectedColorCode : -1)
                     << "ccMatch"
                     << (!labHintsEnabled ||
                         labExpectedColorCode < 0 ||
                         emb.colorCode == labExpectedColorCode)
                     << "lcss" << cachLcssText(emb.lcss)
                     << "qrErrors" << emb.correctedErrors
                     << "polarity" << (emb.inverted ? "inverted" : "normal")
                     << "timing" << emb.timingOffset
                     << "slicer" << emb.slicerRatio
                     << "variant" << embVariantName(emb.variantIndex)
                     << "fragment" << fragment.decoded
                     << "accepted" << embCadenceAccepted
                     << "selectedCadenceMs"
                     << (selectedVoicePayloadCadenceSymbols > 0
                             ? 1000.0 * selectedVoicePayloadCadenceSymbols / DMR_SYMBOL_RATE
                             : 0.0)
                     << "selectedCC" << selectedVoicePayloadColorCode;
        }

        if (emb.correctedErrors <= 1) {
            ++embOkSinceReport;
            if (emb.colorCode >= 0 && emb.colorCode < static_cast<int>(std::size(embColorCodeHistogram))) {
                ++embColorCodeHistogram[emb.colorCode];
            }
            if (emb.lcss >= 0 && emb.lcss < static_cast<int>(std::size(embLcssHistogram))) {
                ++embLcssHistogram[emb.lcss];
            }
            if (emb.privacyIndicator) {
                ++embPrivacyCount;
            }
            embCorrectedErrors += emb.correctedErrors;
            if (emb.variantIndex >= 0 && emb.variantIndex < static_cast<int>(std::size(embVariantHistogram))) {
                ++embVariantHistogram[emb.variantIndex];
            }
            if (emb.correctedErrors >= 0 && emb.correctedErrors < static_cast<int>(std::size(embErrorHistogram))) {
                ++embErrorHistogram[emb.correctedErrors];
            }
            const int timingIndex = emb.timingOffset + 2;
            if (timingIndex >= 0 && timingIndex < static_cast<int>(std::size(embTimingOffsetHistogram))) {
                ++embTimingOffsetHistogram[timingIndex];
            }
        } else {
            ++embWeakSinceReport;
            if (emb.variantIndex >= 0 && emb.variantIndex < static_cast<int>(std::size(embWeakVariantHistogram))) {
                ++embWeakVariantHistogram[emb.variantIndex];
            }
            if (emb.correctedErrors >= 0 && emb.correctedErrors < static_cast<int>(std::size(embWeakErrorHistogram))) {
                ++embWeakErrorHistogram[emb.correctedErrors];
            }
            if (emb.colorCode >= 0 && emb.colorCode < static_cast<int>(std::size(embWeakColorCodeHistogram))) {
                ++embWeakColorCodeHistogram[emb.colorCode];
            }
            const int timingIndex = emb.timingOffset + 2;
            if (timingIndex >= 0 && timingIndex < static_cast<int>(std::size(embWeakTimingOffsetHistogram))) {
                ++embWeakTimingOffsetHistogram[timingIndex];
            }
        }
    }
}

void DmrDecoder::recordVoiceEmbeddedFragment(const PendingEmb &pending,
                                             const EmbInfo &emb,
                                             const VoiceEmbeddedBits &fragment) {
    if (!fragment.decoded || pending.burstIndex < 1 || pending.burstIndex > 5) {
        return;
    }

    const double anchorMs = activeSampleRate > 0
                                ? static_cast<double>(pending.anchorSample) * 1000.0 /
                                      static_cast<double>(activeSampleRate)
                                : 0.0;
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DMR] voice emb"
                 << "anchorMs" << anchorMs
                 << "burst" << pending.burstIndex
                 << "cadenceMs" << (1000.0 * pending.cadenceSymbols / DMR_SYMBOL_RATE)
                 << "cc" << emb.colorCode
                 << "lcss" << cachLcssText(emb.lcss)
                 << "qrErrors" << emb.correctedErrors
                 << "polarity" << (emb.inverted ? "inverted" : "normal")
                 << "timing" << emb.timingOffset
                 << "slicer" << emb.slicerRatio
                 << "variant" << embVariantName(emb.variantIndex)
                 << "emb32" << fragment.hex;
    }

    recordVoiceLcSequenceFragment(pending, emb, fragment);
    recordVoiceLcFragmentCandidate(pending, emb, fragment);

    auto frameIt = std::find_if(voiceEmbeddedFrames.begin(),
                                voiceEmbeddedFrames.end(),
                                [&](const VoiceEmbeddedFrame &frame) {
                                    return frame.anchorSample == pending.anchorSample &&
                                           frame.cadenceSymbols == pending.cadenceSymbols;
                                });
    if (frameIt == voiceEmbeddedFrames.end()) {
        VoiceEmbeddedFrame frame;
        frame.anchorSample = pending.anchorSample;
        frame.cadenceSymbols = pending.cadenceSymbols;
        voiceEmbeddedFrames.push_back(frame);
        frameIt = std::prev(voiceEmbeddedFrames.end());
    }

    VoiceEmbeddedFrame &frame = *frameIt;
    const int burst = pending.burstIndex;
    frame.present[static_cast<std::size_t>(burst)] = true;
    frame.colorCode[static_cast<std::size_t>(burst)] = emb.colorCode;
    frame.correctedErrors[static_cast<std::size_t>(burst)] = emb.correctedErrors;
    frame.lcss[static_cast<std::size_t>(burst)] = emb.lcss;
    frame.timing[static_cast<std::size_t>(burst)] = emb.timingOffset;
    frame.emb32[static_cast<std::size_t>(burst)] = fragment.hex;

    const bool hasEmbeddedLc =
        frame.present[1] &&
        frame.present[2] &&
        frame.present[3] &&
        frame.present[4] &&
        frame.lcss[1] == 1 &&
        frame.lcss[2] == 3 &&
        frame.lcss[3] == 3 &&
        frame.lcss[4] == 2;
    const bool embeddedLcColorCodeConsistent =
        frame.colorCode[1] == frame.colorCode[2] &&
        frame.colorCode[1] == frame.colorCode[3] &&
        frame.colorCode[1] == frame.colorCode[4];
    if (hasEmbeddedLc && !frame.reportedLc && embeddedLcColorCodeConsistent) {
        const QString emb128 =
            frame.emb32[1] +
            frame.emb32[2] +
            frame.emb32[3] +
            frame.emb32[4];
        const EmbeddedLcDecodeResult decodedLc = decodeEmbeddedLc72FromBptc(emb128);
        const bool strictLc = decodedLc.columnParityOk && decodedLc.checksumOk;
        ++voiceLcAssemblySinceReport;
        if (!decodedLc.lc72.isEmpty()) {
            ++voiceLcBptcSinceReport;
        }
        if (strictLc) {
            ++voiceLcStrictSinceReport;
            const bool targetMatches =
                labExpectedTargetId <= 0 ||
                decodedLc.target == static_cast<quint32>(labExpectedTargetId);
            const bool sourceMatches =
                labExpectedSourceId <= 0 ||
                decodedLc.source == static_cast<quint32>(labExpectedSourceId);
            if (labHintsEnabled && labExpectedTargetId > 0 && targetMatches) {
                ++voiceLcLabTargetMatchSinceReport;
            }
            if (labHintsEnabled && labExpectedSourceId > 0 && sourceMatches) {
                ++voiceLcLabSourceMatchSinceReport;
            }
            if (labHintsEnabled && targetMatches && sourceMatches) {
                ++voiceLcLabFullMatchSinceReport;
            }
        }
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DMR] voice lc raw"
                     << "anchorMs" << anchorMs
                     << "b1" << frame.emb32[1]
                     << "b2" << frame.emb32[2]
                     << "b3" << frame.emb32[3]
                     << "b4" << frame.emb32[4]
                     << "cc"
                     << QStringLiteral("%1/%2/%3/%4")
                            .arg(frame.colorCode[1])
                            .arg(frame.colorCode[2])
                            .arg(frame.colorCode[3])
                            .arg(frame.colorCode[4])
                     << "qrErrors"
                     << QStringLiteral("%1/%2/%3/%4")
                            .arg(frame.correctedErrors[1])
                            .arg(frame.correctedErrors[2])
                            .arg(frame.correctedErrors[3])
                            .arg(frame.correctedErrors[4])
                     << "timing"
                     << QStringLiteral("%1/%2/%3/%4")
                            .arg(frame.timing[1])
                            .arg(frame.timing[2])
                            .arg(frame.timing[3])
                            .arg(frame.timing[4])
                     << "emb128" << emb128
                     << embeddedLcInfoText(decodedLc);
        }
        if (strictLc) {
            rememberVoicePayloadCadence(pending.cadenceSymbols,
                                        frame.colorCode[1],
                                        pending.absoluteSample,
                                        QStringLiteral("frame"));
            rememberVoiceLcRaw(emb128);
            queueVoiceLcDecode(emb128, frame.colorCode[1], pending.anchorSample, QStringLiteral("frame"));
        }
        frame.reportedLc = true;
    } else if (hasEmbeddedLc && !frame.reportedLc) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DMR] voice lc candidate rejected"
                     << "anchorMs" << anchorMs
                     << "reason" << "mixed color code"
                     << "cc"
                     << QStringLiteral("%1/%2/%3/%4")
                            .arg(frame.colorCode[1])
                            .arg(frame.colorCode[2])
                            .arg(frame.colorCode[3])
                            .arg(frame.colorCode[4])
                     << "qrErrors"
                     << QStringLiteral("%1/%2/%3/%4")
                            .arg(frame.correctedErrors[1])
                            .arg(frame.correctedErrors[2])
                            .arg(frame.correctedErrors[3])
                            .arg(frame.correctedErrors[4])
                     << "timing"
                     << QStringLiteral("%1/%2/%3/%4")
                            .arg(frame.timing[1])
                            .arg(frame.timing[2])
                            .arg(frame.timing[3])
                            .arg(frame.timing[4]);
        }
        frame.reportedLc = true;
    }

    while (voiceEmbeddedFrames.size() > 16) {
        voiceEmbeddedFrames.pop_front();
    }
}

void DmrDecoder::recordVoiceLcSequenceFragment(const PendingEmb &pending,
                                               const EmbInfo &emb,
                                               const VoiceEmbeddedBits &fragment) {
    if (!fragment.decoded || emb.colorCode < 0 || emb.lcss < 1 || emb.lcss > 3) {
        return;
    }

    const quint64 minFragmentGapSamples =
        (std::max)(static_cast<quint64>(2 * samplesPerSymbol),
                   static_cast<quint64>(std::lround(0.018 * activeSampleRate)));
    const quint64 maxFragmentGapSamples =
        static_cast<quint64>(std::lround(0.62 * activeSampleRate));
    const quint64 maxSequenceSpanSamples =
        static_cast<quint64>(std::lround(1.55 * activeSampleRate));
    const auto fragmentGapMatches = [&](quint64 fragmentGap) {
        return fragmentGap >= minFragmentGapSamples &&
               fragmentGap <= maxFragmentGapSamples;
    };

    voiceLcSequences.erase(
        std::remove_if(voiceLcSequences.begin(),
                       voiceLcSequences.end(),
                       [&](const VoiceLcSequence &sequence) {
                           return pending.absoluteSample >
                                  sequence.startSample + maxSequenceSpanSamples;
                       }),
        voiceLcSequences.end());

    const auto storeStage = [&](VoiceLcSequence &sequence, int stage) {
        sequence.emb32[static_cast<std::size_t>(stage)] = fragment.hex;
        sequence.burstIndex[static_cast<std::size_t>(stage)] = pending.burstIndex;
        sequence.correctedErrors[static_cast<std::size_t>(stage)] = emb.correctedErrors;
        sequence.lcss[static_cast<std::size_t>(stage)] = emb.lcss;
        sequence.timing[static_cast<std::size_t>(stage)] = emb.timingOffset;
        sequence.lastSample = pending.absoluteSample;
    };

    if (emb.lcss == 1) {
        VoiceLcSequence sequence;
        sequence.colorCode = emb.colorCode;
        sequence.nextStage = 1;
        sequence.cadenceSymbols = pending.cadenceSymbols;
        sequence.startSample = pending.absoluteSample;
        sequence.lastSample = pending.absoluteSample;
        storeStage(sequence, 0);
        voiceLcSequences.push_back(sequence);
        while (voiceLcSequences.size() > 32) {
            voiceLcSequences.pop_front();
        }
        return;
    }

    bool completedSequence = false;
    if (emb.lcss == 2) {
        for (auto it = voiceLcSequences.begin(); it != voiceLcSequences.end();) {
            VoiceLcSequence &sequence = *it;
            const bool sampleAfterLast = pending.absoluteSample > sequence.lastSample;
            const quint64 fragmentGap =
                sampleAfterLast ? pending.absoluteSample - sequence.lastSample : 0;
            if (sequence.colorCode == emb.colorCode &&
                sequence.cadenceSymbols == pending.cadenceSymbols &&
                sequence.nextStage == 3 &&
                sampleAfterLast &&
                fragmentGapMatches(fragmentGap) &&
                pending.absoluteSample <=
                    sequence.startSample + maxSequenceSpanSamples) {
                storeStage(sequence, 3);
                reportVoiceLcSequence(sequence);
                it = voiceLcSequences.erase(it);
                completedSequence = true;
            } else {
                ++it;
            }
        }
    }
    if (completedSequence) {
        return;
    }

    if (emb.lcss == 3) {
        for (VoiceLcSequence &sequence : voiceLcSequences) {
            const bool sampleAfterLast = pending.absoluteSample > sequence.lastSample;
            const quint64 fragmentGap =
                sampleAfterLast ? pending.absoluteSample - sequence.lastSample : 0;
            if (sequence.colorCode != emb.colorCode ||
                sequence.cadenceSymbols != pending.cadenceSymbols ||
                sequence.nextStage < 1 ||
                sequence.nextStage > 2 ||
                !sampleAfterLast ||
                !fragmentGapMatches(fragmentGap) ||
                pending.absoluteSample >
                    sequence.startSample + maxSequenceSpanSamples) {
                continue;
            }
            storeStage(sequence, sequence.nextStage);
            ++sequence.nextStage;
        }
        return;
    }
}

void DmrDecoder::recordVoiceLcFragmentCandidate(const PendingEmb &pending,
                                                const EmbInfo &emb,
                                                const VoiceEmbeddedBits &fragment) {
    if (!fragment.decoded || emb.colorCode < 0 || emb.lcss < 1 || emb.lcss > 3) {
        return;
    }
    ++voiceLcFragmentHistogram[emb.lcss];
    const bool labColorMatches =
        !labHintsEnabled ||
        labExpectedColorCode < 0 ||
        emb.colorCode == labExpectedColorCode;
    if (labColorMatches && !shouldRejectVoicePayloadCadence(pending.cadenceSymbols)) {
        ++voiceLcUsableFragmentHistogram[emb.lcss];
    }

    const auto pushCandidate = [&](const QString &emb32, int fragmentVariantIndex) {
        if (emb32.size() != 8) {
            return;
        }

        const bool duplicate =
            std::any_of(voiceLcFragmentCandidates.begin(),
                        voiceLcFragmentCandidates.end(),
                        [&](const VoiceLcFragmentCandidate &stored) {
                            return stored.absoluteSample == pending.absoluteSample &&
                                   stored.colorCode == emb.colorCode &&
                                   stored.lcss == emb.lcss &&
                                   stored.emb32 == emb32;
                        });
        if (duplicate) {
            return;
        }

        VoiceLcFragmentCandidate candidate;
        candidate.absoluteSample = pending.absoluteSample;
        candidate.colorCode = emb.colorCode;
        candidate.lcss = emb.lcss;
        candidate.burstIndex = pending.burstIndex;
        candidate.correctedErrors = emb.correctedErrors;
        candidate.timing = emb.timingOffset;
        candidate.cadenceSymbols = pending.cadenceSymbols;
        candidate.variantIndex = emb.variantIndex;
        candidate.fragmentVariantIndex = fragmentVariantIndex;
        candidate.slicerRatio = emb.slicerRatio;
        candidate.emb32 = emb32;
        voiceLcFragmentCandidates.push_back(candidate);
        tryReportVoiceLcCandidateMatches(candidate);
    };

    pushCandidate(fragment.hex, 0);

    if (emb.variantIndex > 0) {
        const QString variantHex = emb32VariantHex(fragment.hex, emb.variantIndex);
        if (variantHex != fragment.hex) {
            pushCandidate(variantHex, emb.variantIndex);
        }
    }

    const quint64 maxAgeSamples =
        static_cast<quint64>(std::lround(1.85 * activeSampleRate));
    voiceLcFragmentCandidates.erase(
        std::remove_if(voiceLcFragmentCandidates.begin(),
                       voiceLcFragmentCandidates.end(),
                       [&](const VoiceLcFragmentCandidate &stored) {
                           return pending.absoluteSample >
                                  stored.absoluteSample + maxAgeSamples;
                       }),
        voiceLcFragmentCandidates.end());
    while (voiceLcFragmentCandidates.size() > 128) {
        voiceLcFragmentCandidates.pop_front();
    }
}

void DmrDecoder::tryReportVoiceLcCandidateMatches(const VoiceLcFragmentCandidate &latest) {
    if (latest.lcss != 2 || latest.emb32.size() != 8) {
        return;
    }

    const quint64 maxSequenceSpanSamples =
        static_cast<quint64>(std::lround(1.55 * activeSampleRate));
    const quint64 minFragmentGapSamples =
        (std::max)(static_cast<quint64>(2 * samplesPerSymbol),
                   static_cast<quint64>(std::lround(0.018 * activeSampleRate)));
    const auto orderedAfter = [&](const VoiceLcFragmentCandidate &right,
                                  const VoiceLcFragmentCandidate &left) {
        return right.absoluteSample > left.absoluteSample &&
               right.absoluteSample - left.absoluteSample >= minFragmentGapSamples;
    };

    std::vector<const VoiceLcFragmentCandidate *> pool;
    pool.reserve(voiceLcFragmentCandidates.size());
    for (const VoiceLcFragmentCandidate &candidate : voiceLcFragmentCandidates) {
        if (candidate.colorCode != latest.colorCode ||
            candidate.cadenceSymbols != latest.cadenceSymbols ||
            candidate.emb32.size() != 8 ||
            candidate.absoluteSample > latest.absoluteSample ||
            latest.absoluteSample > candidate.absoluteSample + maxSequenceSpanSamples) {
            continue;
        }
        pool.push_back(&candidate);
    }

    for (const VoiceLcFragmentCandidate *first : pool) {
        if (first->lcss != 1 || !orderedAfter(latest, *first)) {
            continue;
        }
        for (const VoiceLcFragmentCandidate *cont1 : pool) {
            if (cont1->lcss != 3 || !orderedAfter(*cont1, *first)) {
                continue;
            }
            for (const VoiceLcFragmentCandidate *cont2 : pool) {
                if (cont2->lcss != 3 ||
                    !orderedAfter(*cont2, *cont1) ||
                    !orderedAfter(latest, *cont2)) {
                    continue;
                }

                const QString emb128 =
                    first->emb32 + cont1->emb32 + cont2->emb32 + latest.emb32;
                if (std::find(reportedVoiceLcCandidateRaw.begin(),
                              reportedVoiceLcCandidateRaw.end(),
                              emb128) != reportedVoiceLcCandidateRaw.end()) {
                    continue;
                }

                const EmbeddedLcDecodeResult decodedLc = decodeEmbeddedLc72FromBptc(emb128);
                const bool strictLc = decodedLc.columnParityOk && decodedLc.checksumOk;
                ++voiceLcAssemblySinceReport;
                if (!decodedLc.lc72.isEmpty()) {
                    ++voiceLcBptcSinceReport;
                }
                if (strictLc) {
                    ++voiceLcStrictSinceReport;
                    const bool targetMatches =
                        labExpectedTargetId <= 0 ||
                        decodedLc.target == static_cast<quint32>(labExpectedTargetId);
                    const bool sourceMatches =
                        labExpectedSourceId <= 0 ||
                        decodedLc.source == static_cast<quint32>(labExpectedSourceId);
                    if (labHintsEnabled && labExpectedTargetId > 0 && targetMatches) {
                        ++voiceLcLabTargetMatchSinceReport;
                    }
                    if (labHintsEnabled && labExpectedSourceId > 0 && sourceMatches) {
                        ++voiceLcLabSourceMatchSinceReport;
                    }
                    if (labHintsEnabled && targetMatches && sourceMatches) {
                        ++voiceLcLabFullMatchSinceReport;
                    }
                }
                if (decodedLc.lc72.isEmpty()) {
                    continue;
                }

                reportedVoiceLcCandidateRaw.push_back(emb128);
                while (reportedVoiceLcCandidateRaw.size() > 64) {
                    reportedVoiceLcCandidateRaw.pop_front();
                }

                const double startMs = activeSampleRate > 0
                                           ? static_cast<double>(first->absoluteSample) * 1000.0 /
                                                 static_cast<double>(activeSampleRate)
                                           : 0.0;
                const double spanMs = activeSampleRate > 0
                                          ? static_cast<double>(latest.absoluteSample -
                                                                first->absoluteSample) *
                                                1000.0 /
                                                static_cast<double>(activeSampleRate)
                                          : 0.0;

                if (fobosVerboseLoggingEnabled()) {
                    qDebug() << (strictLc ? "[DMR] voice lc pooled"
                                          : "[DMR] voice lc pooled weak")
                             << "startMs" << startMs
                             << "spanMs" << spanMs
                             << "cc" << latest.colorCode
                             << "bursts"
                             << QStringLiteral("%1/%2/%3/%4")
                                    .arg(first->burstIndex)
                                    .arg(cont1->burstIndex)
                                    .arg(cont2->burstIndex)
                                    .arg(latest.burstIndex)
                             << "cadenceMs"
                             << QStringLiteral("%1/%2/%3/%4")
                                    .arg(1000.0 * first->cadenceSymbols / DMR_SYMBOL_RATE, 0, 'f', 0)
                                    .arg(1000.0 * cont1->cadenceSymbols / DMR_SYMBOL_RATE, 0, 'f', 0)
                                    .arg(1000.0 * cont2->cadenceSymbols / DMR_SYMBOL_RATE, 0, 'f', 0)
                                    .arg(1000.0 * latest.cadenceSymbols / DMR_SYMBOL_RATE, 0, 'f', 0)
                             << "qrErrors"
                             << QStringLiteral("%1/%2/%3/%4")
                                    .arg(first->correctedErrors)
                                    .arg(cont1->correctedErrors)
                                    .arg(cont2->correctedErrors)
                                    .arg(latest.correctedErrors)
                             << "timing"
                             << QStringLiteral("%1/%2/%3/%4")
                                    .arg(first->timing)
                                    .arg(cont1->timing)
                                    .arg(cont2->timing)
                                    .arg(latest.timing)
                             << "variant"
                             << QStringLiteral("%1/%2/%3/%4")
                                    .arg(embVariantName(first->variantIndex))
                                    .arg(embVariantName(cont1->variantIndex))
                                    .arg(embVariantName(cont2->variantIndex))
                                    .arg(embVariantName(latest.variantIndex))
                             << "fragVariant"
                             << QStringLiteral("%1/%2/%3/%4")
                                    .arg(embVariantName(first->fragmentVariantIndex))
                                    .arg(embVariantName(cont1->fragmentVariantIndex))
                                    .arg(embVariantName(cont2->fragmentVariantIndex))
                                    .arg(embVariantName(latest.fragmentVariantIndex))
                             << "emb128" << emb128
                             << embeddedLcInfoText(decodedLc);
                }

                if (strictLc) {
                    rememberVoicePayloadCadence(latest.cadenceSymbols,
                                                latest.colorCode,
                                                latest.absoluteSample,
                                                QStringLiteral("pooled"));
                    rememberVoiceLcRaw(emb128);
                    queueVoiceLcDecode(emb128,
                                       latest.colorCode,
                                       latest.absoluteSample,
                                       QStringLiteral("pooled"));
                }
            }
        }
    }
}

void DmrDecoder::rememberVoiceLcRaw(const QString &emb128) {
    if (emb128.size() != 32) {
        return;
    }
    const EmbeddedLcDecodeResult decodedLc = decodeEmbeddedLc72FromBptc(emb128);
    if (decodedLc.lc72.isEmpty() || !decodedLc.columnParityOk || !decodedLc.checksumOk) {
        return;
    }
    if (std::find(voiceLcRawSinceReport.begin(),
                  voiceLcRawSinceReport.end(),
                  emb128) != voiceLcRawSinceReport.end()) {
        return;
    }
    voiceLcRawSinceReport.push_back(emb128);
    if (voiceLcRawSinceReport.size() > 64) {
        voiceLcRawSinceReport.erase(voiceLcRawSinceReport.begin());
    }
}

void DmrDecoder::queueVoiceLcDecode(const QString &emb128,
                                    int colorCode,
                                    quint64 absoluteSample,
                                    const QString &method) {
    if (emb128.size() != 32) {
        return;
    }

    const EmbeddedLcDecodeResult decodedLc = decodeEmbeddedLc72FromBptc(emb128);
    if (decodedLc.lc72.isEmpty() || !decodedLc.columnParityOk || !decodedLc.checksumOk) {
        return;
    }
    if (labHintsEnabled) {
        if (labExpectedColorCode >= 0 &&
            colorCode >= 0 &&
            colorCode != labExpectedColorCode) {
            return;
        }
        if (labExpectedTimeslot > 0 &&
            lastObservedTimeslot > 0 &&
            lastObservedTimeslot != labExpectedTimeslot) {
            return;
        }
        if (labExpectedTargetId > 0 &&
            decodedLc.target != static_cast<quint32>(labExpectedTargetId)) {
            return;
        }
        if (labExpectedSourceId > 0 &&
            decodedLc.source != static_cast<quint32>(labExpectedSourceId)) {
            return;
        }
    }

    rememberVoiceServiceInfo(colorCode,
                             decodedLc.target,
                             decodedLc.source,
                             decodedLc.flco,
                             decodedLc.fid,
                             decodedLc.serviceOptions,
                             decodedLc.corrected,
                             absoluteSample,
                             method);

    const QString colorCodeText =
        colorCode >= 0 ? QString::number(colorCode) : QStringLiteral("?");
    const QString timeslotText =
        currentServiceTimeslot() > 0
            ? QString::number(currentServiceTimeslot())
            : QStringLiteral("?");
    const QString key =
        QStringLiteral("%1|%2|%3|%4|%5|%6")
            .arg(colorCodeText)
            .arg(decodedLc.target)
            .arg(decodedLc.source)
            .arg(decodedLc.flco)
            .arg(decodedLc.fid)
            .arg(decodedLc.serviceOptions);

    const quint64 duplicateWindowSamples =
        static_cast<quint64>(std::lround(60.0 * activeSampleRate));
    const quint64 sampleDistance =
        absoluteSample >= lastVoiceLcUserSample
            ? absoluteSample - lastVoiceLcUserSample
            : lastVoiceLcUserSample - absoluteSample;
    if (lastVoiceLcUserKey == key && sampleDistance < duplicateWindowSamples) {
        return;
    }

    lastVoiceLcUserKey = key;
    lastVoiceLcUserSample = absoluteSample;

    const auto hexByte = [](int value) {
        return QStringLiteral("0x%1")
            .arg(value < 0 ? 0 : value, 2, 16, QLatin1Char('0'))
            .toUpper();
    };

    QString callText =
        QStringLiteral("[DMR%1] Voice LC: CC %2, TS %3, TG %4, SRC %5, FLCO %6, FID %7, SO %8, %9, BPTC %10%11\n")
            .arg(dmrFrequencySuffix(lockedRfFrequencyHz))
            .arg(colorCodeText)
            .arg(timeslotText)
            .arg(decodedLc.target)
            .arg(decodedLc.source)
            .arg(hexByte(decodedLc.flco))
            .arg(hexByte(decodedLc.fid))
            .arg(hexByte(decodedLc.serviceOptions))
            .arg(method)
            .arg(decodedLc.corrected)
            .arg(decodedLc.protectedFlag ? QStringLiteral(", PF 1") : QString());

    pendingDecodedMessages.push_back(callText);
    while (pendingDecodedMessages.size() > 12) {
        pendingDecodedMessages.pop_front();
    }
}

void DmrDecoder::reportVoiceLcSequence(const VoiceLcSequence &sequence) {
    const QString emb128 =
        sequence.emb32[0] +
        sequence.emb32[1] +
        sequence.emb32[2] +
        sequence.emb32[3];
    if (emb128.size() != 32) {
        return;
    }

    const EmbeddedLcDecodeResult decodedLc = decodeEmbeddedLc72FromBptc(emb128);
    const double startMs = activeSampleRate > 0
                               ? static_cast<double>(sequence.startSample) * 1000.0 /
                                     static_cast<double>(activeSampleRate)
                               : 0.0;
    const double spanMs = activeSampleRate > 0
                              ? static_cast<double>(sequence.lastSample - sequence.startSample) *
                                    1000.0 / static_cast<double>(activeSampleRate)
                              : 0.0;

    const bool strictLc = decodedLc.columnParityOk && decodedLc.checksumOk;
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << (strictLc ? "[DMR] voice lc stream" : "[DMR] voice lc stream weak")
                 << "startMs" << startMs
                 << "spanMs" << spanMs
                 << "cadenceMs" << (1000.0 * sequence.cadenceSymbols / DMR_SYMBOL_RATE)
                 << "cc" << sequence.colorCode
                 << "bursts"
                 << QStringLiteral("%1/%2/%3/%4")
                        .arg(sequence.burstIndex[0])
                        .arg(sequence.burstIndex[1])
                        .arg(sequence.burstIndex[2])
                        .arg(sequence.burstIndex[3])
                 << "qrErrors"
                 << QStringLiteral("%1/%2/%3/%4")
                        .arg(sequence.correctedErrors[0])
                        .arg(sequence.correctedErrors[1])
                        .arg(sequence.correctedErrors[2])
                        .arg(sequence.correctedErrors[3])
                 << "lcss"
                 << QStringLiteral("%1/%2/%3/%4")
                        .arg(sequence.lcss[0])
                        .arg(sequence.lcss[1])
                        .arg(sequence.lcss[2])
                        .arg(sequence.lcss[3])
                 << "timing"
                 << QStringLiteral("%1/%2/%3/%4")
                        .arg(sequence.timing[0])
                        .arg(sequence.timing[1])
                        .arg(sequence.timing[2])
                        .arg(sequence.timing[3])
                 << "emb128" << emb128
                 << embeddedLcInfoText(decodedLc);
    }

    if (strictLc) {
        rememberVoicePayloadCadence(sequence.cadenceSymbols,
                                    sequence.colorCode,
                                    sequence.lastSample,
                                    QStringLiteral("stream"));
        rememberVoiceLcRaw(emb128);
        queueVoiceLcDecode(emb128,
                           sequence.colorCode,
                           sequence.lastSample,
                           QStringLiteral("stream"));
    }
}

QString DmrDecoder::takePendingDecodedMessages() {
    if (pendingDecodedMessages.empty()) {
        return QString();
    }

    QString text;
    while (!pendingDecodedMessages.empty()) {
        text += pendingDecodedMessages.front();
        pendingDecodedMessages.pop_front();
    }
    return text;
}

void DmrDecoder::voteVoicePayloadCadence(int cadenceSymbols,
                                         int colorCode,
                                         int weight,
                                         quint64 absoluteSample,
                                         const QString &source) {
    if (cadenceSymbols <= 0 || weight <= 0) {
        return;
    }
    if (shouldRejectVoicePayloadCadence(cadenceSymbols)) {
        return;
    }

    int *score = nullptr;
    int *otherScore = nullptr;
    if (cadenceSymbols == 144) {
        score = &voicePayloadCadenceScore30;
        otherScore = &voicePayloadCadenceScore60;
    } else if (cadenceSymbols == 288) {
        score = &voicePayloadCadenceScore60;
        otherScore = &voicePayloadCadenceScore30;
    } else {
        return;
    }

    *score = (std::min)(100, *score + weight);
    if (*otherScore > 0 && weight >= 2) {
        *otherScore = (std::max)(0, *otherScore - 1);
    }
    const bool labColorPinned = labHintsEnabled && labExpectedColorCode >= 0;
    if (colorCode >= 0 &&
        colorCode < static_cast<int>(std::size(voicePayloadColorCodeScore))) {
        if (!labColorPinned || colorCode == labExpectedColorCode) {
            const int colorWeight =
                weight +
                (labColorPinned && labExpectedColorCode == colorCode ? 3 : 0);
            voicePayloadColorCodeScore[colorCode] =
                (std::min)(100, voicePayloadColorCodeScore[colorCode] + colorWeight);
            if (colorWeight >= 2) {
                for (int index = 0;
                     index < static_cast<int>(std::size(voicePayloadColorCodeScore));
                     ++index) {
                    if (index != colorCode && voicePayloadColorCodeScore[index] > 0) {
                        voicePayloadColorCodeScore[index] =
                            (std::max)(0, voicePayloadColorCodeScore[index] - 1);
                    }
                }
            }
        } else if (labExpectedColorCode < static_cast<int>(std::size(voicePayloadColorCodeScore))) {
            voicePayloadColorCodeScore[labExpectedColorCode] =
                (std::max)(voicePayloadColorCodeScore[labExpectedColorCode], 20);
        }
    }

    const int scoreLead = *score - *otherScore;
    const bool noCadenceSelected = selectedVoicePayloadCadenceSymbols <= 0;
    const bool sameCadenceSelected = selectedVoicePayloadCadenceSymbols == cadenceSymbols;
    const bool strongEnoughForInitialLock = *score >= 4 && scoreLead >= 1;
    const bool strongEnoughForSwitch = *score >= 9 && scoreLead >= 4;

    if (noCadenceSelected && strongEnoughForInitialLock) {
        rememberVoicePayloadCadence(cadenceSymbols,
                                    colorCode,
                                    absoluteSample,
                                    source + QStringLiteral(" vote"));
        return;
    }

    if (sameCadenceSelected) {
        rememberVoicePayloadCadence(cadenceSymbols,
                                    colorCode,
                                    absoluteSample,
                                    source + QStringLiteral(" keepalive"));
        return;
    }

    if (strongEnoughForSwitch) {
        rememberVoicePayloadCadence(cadenceSymbols,
                                    colorCode,
                                    absoluteSample,
                                    source + QStringLiteral(" switch"));
    }
}

bool DmrDecoder::shouldRejectVoicePayloadCadence(int cadenceSymbols) const {
    return cadenceSymbols == 144;
}

void DmrDecoder::rememberVoicePayloadCadence(int cadenceSymbols,
                                             int colorCode,
                                             quint64 absoluteSample,
                                             const QString &source) {
    if (cadenceSymbols <= 0) {
        return;
    }
    if (shouldRejectVoicePayloadCadence(cadenceSymbols)) {
        return;
    }

    const quint64 cadenceSwitchGuardSamples =
        activeSampleRate > 0
            ? static_cast<quint64>(std::lround(0.45 * activeSampleRate))
            : 0;
    const bool haveSelectedCadence = selectedVoicePayloadCadenceSymbols > 0;
    const bool sameCadence = selectedVoicePayloadCadenceSymbols == cadenceSymbols;
    const bool switchGuardExpired =
        selectedVoicePayloadSample == 0 ||
        absoluteSample >= selectedVoicePayloadSample + cadenceSwitchGuardSamples;
    const bool trustedSource =
        source == QStringLiteral("frame") ||
        source == QStringLiteral("stream") ||
        source == QStringLiteral("pooled");
    const bool candidateColorValid =
        colorCode >= 0 &&
        colorCode < static_cast<int>(std::size(voicePayloadColorCodeScore));
    const bool labColorPinned = labHintsEnabled && labExpectedColorCode >= 0;
    const bool candidateMatchesLabColor =
        !labColorPinned || (candidateColorValid && colorCode == labExpectedColorCode);
    if (labColorPinned && selectedVoicePayloadColorCode != labExpectedColorCode) {
        selectedVoicePayloadColorCode = labExpectedColorCode;
        if (labExpectedColorCode < static_cast<int>(std::size(voicePayloadColorCodeScore))) {
            voicePayloadColorCodeScore[labExpectedColorCode] =
                (std::max)(voicePayloadColorCodeScore[labExpectedColorCode], 20);
        }
    }
    if (candidateColorValid && trustedSource && candidateMatchesLabColor) {
        voicePayloadColorCodeScore[colorCode] =
            (std::max)(voicePayloadColorCodeScore[colorCode], 16);
    }
    if (candidateColorValid &&
        labHintsEnabled &&
        labExpectedColorCode == colorCode) {
        voicePayloadColorCodeScore[colorCode] =
            (std::max)(voicePayloadColorCodeScore[colorCode], 10);
    }
    const int candidateColorScore =
        candidateColorValid ? voicePayloadColorCodeScore[colorCode] : 0;
    const int selectedColorScore =
        selectedVoicePayloadColorCode >= 0 &&
                selectedVoicePayloadColorCode <
                    static_cast<int>(std::size(voicePayloadColorCodeScore))
            ? voicePayloadColorCodeScore[selectedVoicePayloadColorCode]
            : 0;
    int bestOtherColorScore = 0;
    if (candidateColorValid) {
        for (int index = 0;
             index < static_cast<int>(std::size(voicePayloadColorCodeScore));
             ++index) {
            if (index != colorCode) {
                bestOtherColorScore =
                    (std::max)(bestOtherColorScore, voicePayloadColorCodeScore[index]);
            }
        }
    }
    const bool strongInitialColor =
        candidateColorValid &&
        candidateMatchesLabColor &&
        ((candidateColorScore >= 4 &&
          candidateColorScore >= bestOtherColorScore + 1) ||
         (labHintsEnabled && labExpectedColorCode == colorCode));
    const bool strongColorSwitch =
        candidateColorValid &&
        candidateMatchesLabColor &&
        ((candidateColorScore >= 12 &&
          candidateColorScore >= selectedColorScore + 6) ||
         (labHintsEnabled &&
          labExpectedColorCode == colorCode &&
          candidateColorScore >= selectedColorScore + 2));
    const bool acceptColorCode =
        candidateColorValid &&
        candidateMatchesLabColor &&
        (trustedSource ||
         selectedVoicePayloadColorCode == colorCode ||
         (selectedVoicePayloadColorCode < 0 && strongInitialColor) ||
         (selectedVoicePayloadColorCode >= 0 && strongColorSwitch));
    const bool colorChanged =
        selectedVoicePayloadColorCode >= 0 &&
        acceptColorCode &&
        selectedVoicePayloadColorCode != colorCode;
    const bool forcedCadenceSwitch =
        trustedSource || source.contains(QStringLiteral("switch"));
    const bool stableSameTimeslotCadence =
        haveSelectedCadence &&
        selectedVoicePayloadCadenceSymbols == 288 &&
        voicePayloadCadenceScore60 >= 40 &&
        voicePayloadCadenceScore60 >= voicePayloadCadenceScore30;

    if (stableSameTimeslotCadence &&
        cadenceSymbols == 144 &&
        !colorChanged) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DMR] ignoring half-slot voice cadence while 60 ms cadence is stable"
                     << "source" << source
                     << "scores30/60"
                     << QStringLiteral("%1/%2")
                            .arg(voicePayloadCadenceScore30)
                            .arg(voicePayloadCadenceScore60);
        }
        return;
    }

    if (haveSelectedCadence &&
        !sameCadence &&
        !colorChanged &&
        !switchGuardExpired &&
        !forcedCadenceSwitch) {
        return;
    }

    const bool changed =
        !haveSelectedCadence ||
        selectedVoicePayloadCadenceSymbols != cadenceSymbols ||
        (acceptColorCode && selectedVoicePayloadColorCode != colorCode);

    selectedVoicePayloadCadenceSymbols = cadenceSymbols;
    if (acceptColorCode) {
        selectedVoicePayloadColorCode = colorCode;
    }
    selectedVoicePayloadSample = absoluteSample;

    if (changed && fobosVerboseLoggingEnabled()) {
        qDebug() << "[DMR] voice payload cadence selected"
                 << "cadenceMs" << (1000.0 * cadenceSymbols / DMR_SYMBOL_RATE)
                 << "cc" << colorCode
                 << "source" << source;
    }
}

bool DmrDecoder::acceptsVoicePayloadCadence(const PendingEmb &pending, const EmbInfo &emb) const {
    if (pending.cadenceSymbols <= 0) {
        return true;
    }
    if (shouldRejectVoicePayloadCadence(pending.cadenceSymbols)) {
        return false;
    }
    if (selectedVoicePayloadCadenceSymbols <= 0) {
        return false;
    }
    if (pending.cadenceSymbols != selectedVoicePayloadCadenceSymbols) {
        return false;
    }
    if (selectedVoicePayloadColorCode >= 0 &&
        emb.colorCode >= 0 &&
        emb.colorCode != selectedVoicePayloadColorCode) {
        return false;
    }
    if (labHintsEnabled &&
        labExpectedColorCode >= 0 &&
        emb.colorCode >= 0 &&
        emb.colorCode != labExpectedColorCode) {
        return false;
    }
    return true;
}

bool DmrDecoder::acceptsKnownVoicePayloadCadence(const PendingEmb &pending) const {
    if (pending.cadenceSymbols <= 0) {
        return true;
    }
    if (shouldRejectVoicePayloadCadence(pending.cadenceSymbols)) {
        return false;
    }
    if (labHintsEnabled &&
        labExpectedColorCode >= 0 &&
        selectedVoicePayloadColorCode >= 0 &&
        selectedVoicePayloadColorCode != labExpectedColorCode) {
        return false;
    }
    return selectedVoicePayloadCadenceSymbols > 0 &&
           pending.cadenceSymbols == selectedVoicePayloadCadenceSymbols;
}

bool DmrDecoder::hasTrustedVoiceAudio(int *confidence) const {
    int score = 0;
    if (haveLock) {
        score += 2;
    }
    if (selectedVoicePayloadCadenceSymbols == 144) {
        score += (std::min)(voicePayloadCadenceScore30, 30);
    } else if (selectedVoicePayloadCadenceSymbols == 288) {
        score += (std::min)(voicePayloadCadenceScore60, 30);
    }
    if (selectedVoicePayloadColorCode >= 0) {
        score += 8;
    }
    if (!lastVoiceLcUserKey.isEmpty()) {
        score += 8;
    }
    const bool labColorOk =
        !labHintsEnabled ||
        labExpectedColorCode < 0 ||
        selectedVoicePayloadColorCode == labExpectedColorCode;
    const bool labTimeslotOk =
        !labHintsEnabled ||
        labExpectedTimeslot <= 0 ||
        lastObservedTimeslot == 0 ||
        lastObservedTimeslot == labExpectedTimeslot;
    const bool labTargetOk =
        !labHintsEnabled ||
        labExpectedTargetId <= 0 ||
        !lastVoiceService.valid ||
        lastVoiceService.target == static_cast<quint32>(labExpectedTargetId);
    const bool labSourceOk =
        !labHintsEnabled ||
        labExpectedSourceId <= 0 ||
        !lastVoiceService.valid ||
        lastVoiceService.source == static_cast<quint32>(labExpectedSourceId);
    const bool labLockOk = labColorOk && labTimeslotOk && labTargetOk && labSourceOk;
    if (!labLockOk) {
        score = (std::min)(score, 10);
    }
    if (confidence) {
        *confidence = score;
    }

    return haveLock &&
           selectedVoicePayloadCadenceSymbols > 0 &&
           selectedVoicePayloadColorCode >= 0 &&
           labLockOk &&
           score >= 18;
}

void DmrDecoder::queueVoicePayloadFrames(const PendingEmb &pending,
                                         bool inverted,
                                         int timingOffset,
                                         float slicerRatio) {
    if (!voicePayloadEnabled) return;
    if (shouldRejectVoicePayloadCadence(pending.cadenceSymbols)) {
        return;
    }

    const quint64 duplicateWindow =
        static_cast<quint64>((std::max)(samplesPerSymbol * 8, 1));
    for (const quint64 sample : queuedVoicePayloadBurstSamples) {
        const quint64 distance =
            sample > pending.absoluteSample
                ? sample - pending.absoluteSample
                : pending.absoluteSample - sample;
        if (distance <= duplicateWindow) {
            return;
        }
    }

    struct VoicePayloadCandidate {
        VoicePayloadBits payload;
        std::array<AmbeFecDecodeResult, 3> fec = {};
        int decodedCount = -1;
        int playableCount = 0;
        int playableCorrectedErrors = 999;
        int playableRawCorrectedErrors = 999;
        int correctedErrors = 999;
        int rawCorrectedErrors = 999;
        int qualityScore = 999;
        int timingOffset = 0;
        float slicerRatio = 0.625f;
        bool inverted = false;
        bool useAdaptiveSlicer = false;
        int ambeLayout = DMR_DEFAULT_AMBE_LAYOUT;
        int bitMapVariant = 0;
    };
    const auto candidateSummary = [](const VoicePayloadCandidate &candidate) {
        if (candidate.decodedCount < 0) {
            return QStringLiteral("none");
        }
        QStringList fecErrors;
        for (const AmbeFecDecodeResult &fec : candidate.fec) {
            fecErrors << (fec.decoded
                              ? QStringLiteral("%1/%2")
                                    .arg(fec.correctedErrors)
                                    .arg(fec.rawCorrectedErrors)
                              : QStringLiteral("x"));
        }
        return QStringLiteral("%1 t%2 s%3 %4 %5 map%6 r%7 sep%8 play%9 corr%10 raw%11 dec%12 corr%13 raw%14 q%15 fec[%16]")
            .arg(candidate.inverted ? QStringLiteral("inv") : QStringLiteral("norm"))
            .arg(candidate.timingOffset)
            .arg(candidate.slicerRatio, 0, 'f', 3)
            .arg(candidate.payload.adaptiveSlicer ? QStringLiteral("local4")
                                                  : QStringLiteral("sync"))
            .arg(QString::fromLatin1(dmrAmbeLayoutName(candidate.ambeLayout)))
            .arg(voicePayloadBitMapName(candidate.bitMapVariant))
            .arg(candidate.payload.slicerRange, 0, 'f', 4)
            .arg(candidate.payload.slicerMinSeparation, 0, 'f', 4)
            .arg(candidate.playableCount)
            .arg(candidate.playableCorrectedErrors)
            .arg(candidate.playableRawCorrectedErrors)
            .arg(candidate.decodedCount)
            .arg(candidate.correctedErrors)
            .arg(candidate.rawCorrectedErrors)
            .arg(candidate.qualityScore)
            .arg(fecErrors.join(QLatin1Char('/')));
    };

    std::vector<int> timingCandidates;
    const int minTimingOffset = -(std::max)(1, samplesPerSymbol / 2);
    const int maxTimingOffset =
        (std::max)(1, samplesPerSymbol - 1 - samplesPerSymbol / 2);
    const int livePreferredTimingOffset = (std::clamp)(
        static_cast<int>(std::lround(
            (liveTimingPhaseFraction - 0.5) * (std::max)(1, samplesPerSymbol - 1))),
        minTimingOffset,
        maxTimingOffset);
    const int preferredTimingOffset =
        labManualTimingEnabled
            ? (std::clamp)(labManualTimingOffset, minTimingOffset, maxTimingOffset)
            : (liveTimingHintEnabled ? livePreferredTimingOffset : timingOffset);
    const float preferredSlicerRatio =
        (std::clamp)(labSlicerRatio, 0.45f, 0.80f);
    int voiceAudioConfidence = 0;
    const bool trustedVoiceAudio = hasTrustedVoiceAudio(&voiceAudioConfidence);
    const bool wideTimingSearch =
        !trustedVoiceAudio ||
        selectedVoicePayloadCadenceSymbols <= 0 ||
        selectedVoicePayloadColorCode < 0;
    const auto addTimingCandidate = [&timingCandidates, minTimingOffset, maxTimingOffset](int value) {
        value = (std::clamp)(value, minTimingOffset, maxTimingOffset);
        if (std::find(timingCandidates.begin(), timingCandidates.end(), value) ==
            timingCandidates.end()) {
            timingCandidates.push_back(value);
        }
    };
    addTimingCandidate(preferredTimingOffset);
    if (!labManualTimingEnabled) {
        addTimingCandidate(timingOffset);
        addTimingCandidate(preferredTimingOffset - 1);
        addTimingCandidate(preferredTimingOffset + 1);
        addTimingCandidate(timingOffset - 1);
        addTimingCandidate(timingOffset + 1);
        addTimingCandidate(timingOffset - 2);
        addTimingCandidate(timingOffset + 2);
        if (wideTimingSearch) {
            const int liveSearchRadius =
                (std::clamp)(samplesPerSymbol / 4, 3, 8);
            const int firstOffset =
                (std::max)(minTimingOffset, preferredTimingOffset - liveSearchRadius);
            const int lastOffset =
                (std::min)(maxTimingOffset, preferredTimingOffset + liveSearchRadius);
            for (int offset = firstOffset; offset <= lastOffset; ++offset) {
                addTimingCandidate(offset);
            }
        }
    }

    std::vector<float> slicerCandidates;
    const auto addSlicerCandidate = [&slicerCandidates](float value) {
        value = (std::clamp)(value, 0.45f, 0.80f);
        for (const float existing : slicerCandidates) {
            if (std::abs(existing - value) < 0.001f) {
                return;
            }
        }
        slicerCandidates.push_back(value);
    };
    addSlicerCandidate(preferredSlicerRatio);
    if (labAdaptiveSlicer) {
        addSlicerCandidate(slicerRatio);
        addSlicerCandidate(0.55f);
        addSlicerCandidate(0.625f);
        addSlicerCandidate(0.70f);
    }

    VoicePayloadCandidate bestCandidate;
    VoicePayloadCandidate runnerUpCandidate;
    int candidateCount = 0;
    const std::array<bool, 2> polarityCandidates = {inverted, !inverted};
    std::vector<bool> adaptiveSlicerCandidates;
    if (labAdaptiveSlicer) {
        adaptiveSlicerCandidates.push_back(true);
        adaptiveSlicerCandidates.push_back(false);
    } else {
        adaptiveSlicerCandidates.push_back(false);
    }
    std::vector<int> ambeLayoutCandidates;
    const auto addAmbeLayoutCandidate = [&ambeLayoutCandidates](int value) {
        value = normalizedDmrAmbeLayout(value);
        if (value == DMR_AMBE_LAYOUT_AUTO) {
            return;
        }
        if (std::find(ambeLayoutCandidates.begin(), ambeLayoutCandidates.end(), value) ==
            ambeLayoutCandidates.end()) {
            ambeLayoutCandidates.push_back(value);
        }
    };
    if (labAmbeLayout == DMR_AMBE_LAYOUT_AUTO) {
        addAmbeLayoutCandidate(DMR_AMBE_LAYOUT_SPLIT36);
    } else {
        addAmbeLayoutCandidate(labAmbeLayout);
    }
    std::vector<int> bitMapCandidates;
    const auto addBitMapCandidate = [&bitMapCandidates](int value) {
        value &= 0x07;
        if (std::find(bitMapCandidates.begin(), bitMapCandidates.end(), value) ==
            bitMapCandidates.end()) {
            bitMapCandidates.push_back(value);
        }
    };
    addBitMapCandidate(lastVoicePayloadBitMapVariant);
    addBitMapCandidate(0);
    if (wideTimingSearch || voiceAudioConfidence < 40) {
        for (int variant = 1; variant < 8; ++variant) {
            addBitMapCandidate(variant);
        }
    }
    const auto isBetterCandidate = [inverted, preferredTimingOffset, preferredSlicerRatio](
                                       const VoicePayloadCandidate &candidate,
                                       const VoicePayloadCandidate &current) {
        if (current.decodedCount < 0) {
            return true;
        }
        const int candidatePolarityPenalty = candidate.inverted == inverted ? 0 : 1;
        const int currentPolarityPenalty = current.inverted == inverted ? 0 : 1;
        if (candidate.decodedCount != current.decodedCount) {
            return candidate.decodedCount > current.decodedCount;
        }
        if (candidate.playableCount != current.playableCount) {
            return candidate.playableCount > current.playableCount;
        }
        if (candidate.playableCorrectedErrors != current.playableCorrectedErrors) {
            return candidate.playableCorrectedErrors < current.playableCorrectedErrors;
        }
        if (candidate.playableRawCorrectedErrors != current.playableRawCorrectedErrors) {
            return candidate.playableRawCorrectedErrors < current.playableRawCorrectedErrors;
        }
        if (candidate.correctedErrors != current.correctedErrors) {
            return candidate.correctedErrors < current.correctedErrors;
        }
        if (candidate.rawCorrectedErrors != current.rawCorrectedErrors) {
            return candidate.rawCorrectedErrors < current.rawCorrectedErrors;
        }
        if (candidate.qualityScore != current.qualityScore) {
            return candidate.qualityScore < current.qualityScore;
        }
        if (candidatePolarityPenalty != currentPolarityPenalty) {
            return candidatePolarityPenalty < currentPolarityPenalty;
        }
        const int candidateTimingDistance = std::abs(candidate.timingOffset - preferredTimingOffset);
        const int currentTimingDistance = std::abs(current.timingOffset - preferredTimingOffset);
        if (candidateTimingDistance != currentTimingDistance) {
            return candidateTimingDistance < currentTimingDistance;
        }
        return std::abs(candidate.slicerRatio - preferredSlicerRatio) <
               std::abs(current.slicerRatio - preferredSlicerRatio);
    };
    for (const bool candidateInverted : polarityCandidates) {
        for (const int candidateTiming : timingCandidates) {
            for (const bool useAdaptiveSlicer : adaptiveSlicerCandidates) {
                for (int slicerIndex = 0; slicerIndex < static_cast<int>(slicerCandidates.size()); ++slicerIndex) {
                    if (useAdaptiveSlicer && slicerIndex > 0) {
                        continue;
                    }
                    const float candidateSlicer = slicerCandidates[static_cast<std::size_t>(slicerIndex)];
                    for (const int candidateAmbeLayout : ambeLayoutCandidates) {
                        for (const int candidateBitMap : bitMapCandidates) {
                            VoicePayloadCandidate candidate;
                            candidate.timingOffset = candidateTiming;
                            candidate.slicerRatio = candidateSlicer;
                            candidate.inverted = candidateInverted;
                            candidate.useAdaptiveSlicer = useAdaptiveSlicer;
                            candidate.ambeLayout = candidateAmbeLayout;
                            candidate.bitMapVariant = candidateBitMap;
                            candidate.payload =
                                decodeVoicePayloadAt(pending,
                                                     candidateInverted,
                                                     candidateTiming,
                                                     candidateSlicer,
                                                     useAdaptiveSlicer,
                                                     candidateAmbeLayout,
                                                     candidateBitMap);
                            if (!candidate.payload.decoded || candidate.payload.combinedHex.size() != 54) {
                                continue;
                            }

                            candidate.decodedCount = 0;
                            candidate.playableCount = 0;
                            candidate.playableCorrectedErrors = 0;
                            candidate.playableRawCorrectedErrors = 0;
                            candidate.correctedErrors = 0;
                            candidate.rawCorrectedErrors = 0;
                            for (int i = 0; i < 3; ++i) {
                                const QString frameHex = candidate.payload.combinedHex.mid(i * 18, 18);
                                candidate.fec[static_cast<std::size_t>(i)] =
                                    decodeDmrAmbeFecPayload(frameHex);
                                if (candidate.fec[static_cast<std::size_t>(i)].decoded) {
                                    ++candidate.decodedCount;
                                    candidate.correctedErrors +=
                                        candidate.fec[static_cast<std::size_t>(i)].correctedErrors;
                                    candidate.rawCorrectedErrors +=
                                        candidate.fec[static_cast<std::size_t>(i)].rawCorrectedErrors;
                                    if (candidate.fec[static_cast<std::size_t>(i)].correctedErrors <=
                                            DMR_AUDIO_CANDIDATE_MAX_FEC_ERRORS &&
                                        candidate.fec[static_cast<std::size_t>(i)].rawCorrectedErrors <=
                                            DMR_AUDIO_CANDIDATE_MAX_RAW_FEC_ERRORS) {
                                        ++candidate.playableCount;
                                        candidate.playableCorrectedErrors +=
                                            candidate.fec[static_cast<std::size_t>(i)].correctedErrors;
                                        candidate.playableRawCorrectedErrors +=
                                            candidate.fec[static_cast<std::size_t>(i)].rawCorrectedErrors;
                                    }
                                } else {
                                    candidate.correctedErrors += 8;
                                    candidate.rawCorrectedErrors += 8;
                                }
                            }
                            const int timingDistance =
                                std::abs(candidate.timingOffset - preferredTimingOffset);
                            const int timingPenalty = timingDistance * 6;
                            const int polarityPenalty = candidate.inverted == inverted ? 0 : 1;
                            const int slicerPenalty =
                                std::abs(candidate.slicerRatio - preferredSlicerRatio) < 0.001f
                                    ? 0
                                    : 2;
                            const int bitMapPenalty =
                                candidate.bitMapVariant == lastVoicePayloadBitMapVariant ? 0 : 1;
                            candidate.qualityScore =
                                candidate.correctedErrors + timingPenalty + polarityPenalty +
                                slicerPenalty + bitMapPenalty;
                            ++candidateCount;

                            if (isBetterCandidate(candidate, bestCandidate)) {
                                runnerUpCandidate = bestCandidate;
                                bestCandidate = candidate;
                            } else if (isBetterCandidate(candidate, runnerUpCandidate)) {
                                runnerUpCandidate = candidate;
                            }
                        }
                    }
                }
            }
        }
    }

    if (bestCandidate.decodedCount < 0) {
        return;
    }
    if (labLoggingEnabled()) {
        QStringList timingSummary;
        for (const int candidateTiming : timingCandidates) {
            timingSummary << QString::number(candidateTiming);
        }
        qDebug() << "[DMR lock] voice payload candidates"
                 << "burst" << pending.burstIndex
                 << "cadenceMs" << (1000.0 * pending.cadenceSymbols / DMR_SYMBOL_RATE)
                 << "selectedCadenceMs"
                 << (selectedVoicePayloadCadenceSymbols > 0
                         ? 1000.0 * selectedVoicePayloadCadenceSymbols / DMR_SYMBOL_RATE
                         : 0.0)
                 << "selectedCC" << selectedVoicePayloadColorCode
                 << "expectedCC" << (labHintsEnabled ? labExpectedColorCode : -1)
                 << "input" << QStringLiteral("%1 t%2 s%3")
                                .arg(inverted ? QStringLiteral("inv") : QStringLiteral("norm"))
                                .arg(preferredTimingOffset)
                                .arg(preferredSlicerRatio, 0, 'f', 3)
                 << "manualTiming" << labManualTimingEnabled
                 << "adaptiveSlicer" << labAdaptiveSlicer
                 << "ambeLayout" << dmrAmbeLayoutName(labAmbeLayout)
                 << "lastBitMap" << voicePayloadBitMapName(lastVoicePayloadBitMapVariant)
                 << "wideTimingSearch" << wideTimingSearch
                 << "confidence" << voiceAudioConfidence
                 << "timings" << timingSummary.join(QLatin1Char(','))
                 << "count" << candidateCount
                 << "best" << candidateSummary(bestCandidate)
                 << "runner" << candidateSummary(runnerUpCandidate);
    }

    queuedVoicePayloadBurstSamples.push_back(pending.absoluteSample);
    while (queuedVoicePayloadBurstSamples.size() > 96) {
        queuedVoicePayloadBurstSamples.pop_front();
    }
    lastVoicePayloadAmbeLayout = bestCandidate.ambeLayout;
    lastVoicePayloadBitMapVariant = bestCandidate.bitMapVariant;
    VoicePayloadBits gopherPayload = bestCandidate.payload;
    if (gopherPayload.leftDibits.isEmpty() ||
        gopherPayload.syncDibits.isEmpty() ||
        gopherPayload.rightDibits.isEmpty()) {
        gopherPayload = decodeVoicePayloadAt(pending,
                                             bestCandidate.inverted,
                                             bestCandidate.timingOffset,
                                             bestCandidate.slicerRatio,
                                             bestCandidate.useAdaptiveSlicer,
                                             bestCandidate.ambeLayout,
                                             bestCandidate.bitMapVariant,
                                             true);
    }
    const QString gopherDibits =
        gopherPayload.leftDibits +
        gopherPayload.syncDibits +
        gopherPayload.rightDibits;
    if (gopherDibits.size() == 132) {
        const quint64 centralDibit =
            samplesPerSymbol > 0
                ? pending.absoluteSample / static_cast<quint64>(samplesPerSymbol)
                : pending.absoluteSample;
        const quint64 baseDibit =
            centralDibit > static_cast<quint64>(DMR_SYMBOLS_BEFORE_SYNC)
                ? centralDibit - static_cast<quint64>(DMR_SYMBOLS_BEFORE_SYNC)
                : quint64(0);
        int timeslot = 0;
        QString timeslotSource = QStringLiteral("unknown");
        if (labHintsEnabled &&
            labExpectedTimeslot >= 1 &&
            labExpectedTimeslot <= 2) {
            timeslot = labExpectedTimeslot;
            timeslotSource = QStringLiteral("manual");
        } else if (lastVoiceService.timeslot >= 1 && lastVoiceService.timeslot <= 2) {
            timeslot = lastVoiceService.timeslot;
            timeslotSource = QStringLiteral("lc");
        } else if (pending.cachDecoded &&
                   pending.cachChannel >= 1 &&
                   pending.cachChannel <= 2) {
            timeslot = pending.cachChannel;
            timeslotSource = QStringLiteral("cach");
        } else if (lastObservedTimeslot >= 1 && lastObservedTimeslot <= 2) {
            timeslot = lastObservedTimeslot;
            timeslotSource = QStringLiteral("cach-last");
        }
        DibitBurst burst;
        burst.dibits = gopherDibits;
        burst.burstKind = QStringLiteral("voice");
        burst.sample = pending.absoluteSample;
        burst.baseDibit = baseDibit;
        burst.burstIndex = pending.burstIndex;
        burst.cadenceSymbols = pending.cadenceSymbols;
        burst.colorCode = selectedVoicePayloadColorCode;
        burst.timeslot = timeslot;
        burst.timeslotSource = timeslotSource;
        burst.cachDecoded = pending.cachDecoded;
        burst.cachChannel = pending.cachChannel;
        burst.cachLcss = pending.cachLcss;
        burst.cachMapName = pending.cachMapName;
        burst.cachPayloadBits = pending.cachPayloadBits;
        const bool forwardCorrectedAmbe49ToGopher = true;
        const bool cleanCorrectedPayload =
            forwardCorrectedAmbe49ToGopher &&
            bestCandidate.playableCount == 3 &&
            bestCandidate.correctedErrors == 0 &&
            bestCandidate.rawCorrectedErrors == 0;
        if (cleanCorrectedPayload) {
            for (int i = 0; i < 3; ++i) {
                const AmbeFecDecodeResult &fec = bestCandidate.fec[static_cast<std::size_t>(i)];
                if (fec.decoded && !fec.payloadHex.isEmpty()) {
                    burst.ambe49Payloads << fec.payloadHex;
                }
            }
            if (burst.ambe49Payloads.size() != 3) {
                burst.ambe49Payloads.clear();
            }
        }
        pendingDibitBursts.push_back(burst);
        if (pendingDibitBursts.size() > 64) {
            pendingDibitBursts.erase(pendingDibitBursts.begin(),
                                     pendingDibitBursts.begin() +
                                         static_cast<std::ptrdiff_t>(pendingDibitBursts.size() - 64));
        }
    }

    if (dmrDibitDumpRemaining > 0 && fobosVerboseLoggingEnabled()) {
        VoicePayloadBits debugPayload =
            decodeVoicePayloadAt(pending,
                                 bestCandidate.inverted,
                                 bestCandidate.timingOffset,
                                 bestCandidate.slicerRatio,
                                 bestCandidate.useAdaptiveSlicer,
                                 bestCandidate.ambeLayout,
                                 bestCandidate.bitMapVariant,
                                 true);
        if (!debugPayload.decoded) {
            debugPayload = bestCandidate.payload;
        }

        QStringList fecErrors;
        QStringList fecPayloads;
        QStringList ambeFrames;
        for (int i = 0; i < 3; ++i) {
            const AmbeFecDecodeResult &fec = bestCandidate.fec[static_cast<std::size_t>(i)];
            fecErrors << (fec.decoded
                              ? QStringLiteral("%1/%2")
                                    .arg(fec.correctedErrors)
                                    .arg(fec.rawCorrectedErrors)
                              : QStringLiteral("x"));
            fecPayloads << (fec.decoded ? fec.payloadHex : QStringLiteral("x"));
            ambeFrames << debugPayload.combinedHex.mid(i * 18, 18);
        }

        QString record;
        QTextStream out(&record);
        if (dmrDibitDumpSequence == 0) {
            out << "\n=== DMR dibit dump session " << dmrDibitDumpSession
                << " at " << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                << " ===\n";
        }
        out << "seq=" << dmrDibitDumpSequence
            << " burst=" << pending.burstIndex
            << " sample=" << pending.absoluteSample
            << " cadenceSymbols=" << pending.cadenceSymbols
            << " cadenceMs=" << (1000.0 * pending.cadenceSymbols / DMR_SYMBOL_RATE)
            << " selectedCadenceSymbols=" << selectedVoicePayloadCadenceSymbols
            << " selectedCC=" << selectedVoicePayloadColorCode
            << " rfHz=" << QString::number(lockedRfFrequencyHz, 'f', 0)
            << " pattern=" << lockedPatternName
            << " best=" << candidateSummary(bestCandidate)
            << " runner=" << candidateSummary(runnerUpCandidate)
            << "\n";
        out << "leftDibits=" << debugPayload.leftDibits << "\n";
        out << "centerDibits=" << debugPayload.syncDibits << "\n";
        out << "rightDibits=" << debugPayload.rightDibits << "\n";
        out << "streamDibits=" << debugPayload.leftDibits << debugPayload.rightDibits << "\n";
        out << "leftHex=" << debugPayload.leftHex
            << " rightHex=" << debugPayload.rightHex
            << " combinedHex=" << debugPayload.combinedHex
            << "\n";
        out << "ambeFrames=" << ambeFrames.join(QLatin1Char(','))
            << " fec=" << fecErrors.join(QLatin1Char(','))
            << " fecPayloads=" << fecPayloads.join(QLatin1Char(','))
            << "\n";
        out << "---\n";
        dmrDibitDumpBuffer += record;
        ++dmrDibitDumpSequence;
        --dmrDibitDumpRemaining;
        if (dmrDibitDumpSequence == 1) {
            qDebug() << "[DMR dump] buffering voice dibit dump"
                     << (QCoreApplication::applicationDirPath() +
                         QStringLiteral("/ObriiSDR_dmr_dibit_dump.log"))
                     << "session" << dmrDibitDumpSession
                     << "limit" << dmrDibitDumpRemaining + 1;
        }
        if ((dmrDibitDumpSequence % 4) == 0 ||
            dmrDibitDumpRemaining <= 0 ||
            dmrDibitDumpBuffer.size() > 16384) {
            flushDmrDibitDump();
        }
    }

    for (int i = 0; i < 3; ++i) {
        const QString frameHex = bestCandidate.payload.combinedHex.mid(i * 18, 18);
        pendingAmbeFrames.push_back(frameHex);
        if (bestCandidate.payload.hasSoftBits) {
            DmrAmbeSoftFrame softFrame;
            softFrame.hex = frameHex;
            const int firstBit = i * 72;
            for (int bit = 0; bit < 72; ++bit) {
                softFrame.reliability[static_cast<std::size_t>(bit)] =
                    bestCandidate.payload.softReliability[static_cast<std::size_t>(firstBit + bit)];
            }
            pendingAmbeSoftFrames.push_back(softFrame);
        }
        const AmbeFecDecodeResult &fec = bestCandidate.fec[static_cast<std::size_t>(i)];
        if (fec.decoded) {
            pendingAmbePayloads.push_back({fec.payloadHex, fec.correctedErrors, fec.rawCorrectedErrors});
            ++ambePayloadsSinceReport;
            ambeFecCorrectionsSinceReport += fec.correctedErrors;
            const int correctionBucket = (std::clamp)(fec.correctedErrors, 0, 6);
            ++ambeFecCorrectionHistogram[correctionBucket];
        } else {
            pendingAmbePayloads.push_back({QString(),
                                           DMR_INVALID_AMBE_FEC_ERRORS,
                                           DMR_INVALID_AMBE_FEC_ERRORS});
            ++ambeFecDecodeFailSinceReport;
        }
        DmrAmbePayload &payload = pendingAmbePayloads.back();
        payload.burstIndex = pending.burstIndex;
        payload.frameIndex = i;
        payload.superframeSample = pending.anchorSample;
        payload.superframeSamples = quint64(6 * pending.cadenceSymbols * samplesPerSymbol);
        // Use trusted Voice LC service options. EMB PI is not a reliable
        // clear/encrypted discriminator for Motorola enhanced privacy.
        if (lastVoiceService.valid && pending.absoluteSample >= lastVoiceService.absoluteSample &&
            pending.absoluteSample - lastVoiceService.absoluteSample <= payload.superframeSamples * 2)
            payload.privacyIndicator = (lastVoiceService.serviceOptions & 0x40) != 0 ? 1 : 0;
        payload.timeslot = pending.cachDecoded ? pending.cachChannel : currentServiceTimeslot();
        if (labHintsEnabled && labExpectedTimeslot >= 1 && labExpectedTimeslot <= 2)
            payload.timeslot = labExpectedTimeslot;
        if (payload.timeslot < 1 || payload.timeslot > 2) payload.timeslot = 0;
    }
    while (pendingAmbeFrames.size() > 360) {
        pendingAmbeFrames.pop_front();
    }
    while (pendingAmbeSoftFrames.size() > 360) {
        pendingAmbeSoftFrames.pop_front();
    }
    while (pendingAmbePayloads.size() > 360) {
        pendingAmbePayloads.pop_front();
    }

    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DMR] voice payload"
                 << "burst" << pending.burstIndex
                 << "cadenceSymbols" << pending.cadenceSymbols
                 << "timing" << bestCandidate.timingOffset
                 << "slicer" << bestCandidate.slicerRatio
                 << "polarity" << (bestCandidate.inverted ? "inverted" : "normal")
                 << "ambeLayout" << dmrAmbeLayoutName(bestCandidate.ambeLayout)
                 << "bitMap" << voicePayloadBitMapName(bestCandidate.bitMapVariant)
                 << "fecDecoded" << bestCandidate.decodedCount
                 << "fecCorrections" << bestCandidate.correctedErrors
                 << "frames"
                 << bestCandidate.payload.combinedHex.mid(0, 18)
                 << bestCandidate.payload.combinedHex.mid(18, 18)
                 << bestCandidate.payload.combinedHex.mid(36, 18)
                 << "fecPayloads" << pendingAmbePayloads.size();
    }
}

std::vector<QString> DmrDecoder::takePendingAmbeFrames() {
    std::vector<QString> frames;
    frames.reserve(pendingAmbeFrames.size());
    while (!pendingAmbeFrames.empty()) {
        frames.push_back(pendingAmbeFrames.front());
        pendingAmbeFrames.pop_front();
    }
    return frames;
}

std::vector<DmrAmbeSoftFrame> DmrDecoder::takePendingAmbeSoftFrames() {
    std::vector<DmrAmbeSoftFrame> frames;
    frames.reserve(pendingAmbeSoftFrames.size());
    while (!pendingAmbeSoftFrames.empty()) {
        frames.push_back(pendingAmbeSoftFrames.front());
        pendingAmbeSoftFrames.pop_front();
    }
    return frames;
}

std::vector<DmrAmbePayload> DmrDecoder::takePendingAmbePayloads(int *correctedErrors) {
    std::vector<DmrAmbePayload> payloads;
    payloads.reserve(pendingAmbePayloads.size());
    int corrections = 0;
    while (!pendingAmbePayloads.empty()) {
        const DmrAmbePayload payload = pendingAmbePayloads.front();
        payloads.push_back(payload);
        if (!payload.hex.isEmpty()) {
            corrections += payload.correctedErrors;
        }
        pendingAmbePayloads.pop_front();
    }
    if (correctedErrors) {
        *correctedErrors = corrections;
    }
    return payloads;
}

DmrDecoder::VoicePayloadBits DmrDecoder::decodeVoicePayloadAt(const PendingEmb &pending,
                                                              bool inverted,
                                                              int timingOffset,
                                                              float slicerRatio,
                                                              bool useAdaptiveSlicer,
                                                              int ambeLayout,
                                                              int bitMapVariant,
                                                              bool includeDebug) const {
    VoicePayloadBits payload;
    const quint64 bufferStartSample = totalSamples > sampleBuffer.size()
                                          ? totalSamples - static_cast<quint64>(sampleBuffer.size())
                                          : 0;
    if (pending.absoluteSample < bufferStartSample) {
        return payload;
    }

    const int centralStartIndex =
        static_cast<int>(pending.absoluteSample - bufferStartSample);
    const int sampleOffset = (std::clamp)(samplesPerSymbol / 2 + timingOffset,
                                          0,
                                          (std::max)(0, samplesPerSymbol - 1));
    const int firstPayloadStart = centralStartIndex - DMR_SYMBOLS_BEFORE_SYNC * samplesPerSymbol;
    const int secondPayloadStart = centralStartIndex + DMR_SYNC_SYMBOLS * samplesPerSymbol;
    const int lastPayloadSample =
        secondPayloadStart + (DMR_SYMBOLS_BEFORE_SYNC - 1) * samplesPerSymbol + sampleOffset;
    if (firstPayloadStart + sampleOffset < 0 ||
        lastPayloadSample >= static_cast<int>(sampleBuffer.size())) {
        return payload;
    }

    std::array<float, DMR_SYMBOLS_BEFORE_SYNC> leftSamples = {};
    std::array<float, DMR_SYMBOLS_BEFORE_SYNC> rightSamples = {};
    std::vector<float> payloadSamples;
    payloadSamples.reserve(DMR_SYMBOLS_BEFORE_SYNC * 2);
    for (int i = 0; i < DMR_SYMBOLS_BEFORE_SYNC; ++i) {
        const int sampleIndex = firstPayloadStart + i * samplesPerSymbol + sampleOffset;
        const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
        const float corrected = inverted ? -sample : sample;
        leftSamples[static_cast<std::size_t>(i)] = corrected;
        payloadSamples.push_back(corrected);
    }
    for (int i = 0; i < DMR_SYMBOLS_BEFORE_SYNC; ++i) {
        const int sampleIndex = secondPayloadStart + i * samplesPerSymbol + sampleOffset;
        const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
        const float corrected = inverted ? -sample : sample;
        rightSamples[static_cast<std::size_t>(i)] = corrected;
        payloadSamples.push_back(corrected);
    }
    std::array<float, DMR_SYNC_SYMBOLS> syncSamples = {};
    if (includeDebug) {
        for (int i = 0; i < DMR_SYNC_SYMBOLS; ++i) {
            const int sampleIndex = centralStartIndex + i * samplesPerSymbol + sampleOffset;
            const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
            syncSamples[static_cast<std::size_t>(i)] = inverted ? -sample : sample;
        }
    }

    const DmrDibitSlicer localSlicer =
        useAdaptiveSlicer ? buildDmrDibitSlicer(payloadSamples) : DmrDibitSlicer();
    const float correctedMinLevel = inverted ? -pending.maxLevel : pending.minLevel;
    const float correctedMaxLevel = inverted ? -pending.minLevel : pending.maxLevel;
    const float syncMinLevel = (std::min)(correctedMinLevel, correctedMaxLevel);
    const float syncMaxLevel = (std::max)(correctedMinLevel, correctedMaxLevel);
    const float center = 0.5f * (syncMaxLevel + syncMinLevel);
    const float thresholdRatio = (std::clamp)(slicerRatio, 0.45f, 0.80f);
    const float upperMid = center + thresholdRatio * (syncMaxLevel - center);
    const float lowerMid = center + thresholdRatio * (syncMinLevel - center);
    const auto sampleToDecision = [&](float correctedSample) -> DmrDibitDecision {
        if (localSlicer.valid) {
            return decideDmrDibit(localSlicer, correctedSample);
        }
        DmrDibitDecision decision;
        if (correctedSample > center) {
            decision.dibit = correctedSample > upperMid ? 3 : 2;
            decision.firstReliability =
                reliabilityFromDistance(std::abs(correctedSample - center),
                                        (std::max)(0.0001f, 0.5f * (syncMaxLevel - center)));
            decision.secondReliability =
                reliabilityFromDistance(std::abs(correctedSample - upperMid),
                                        (std::max)(0.0001f, 0.5f * (syncMaxLevel - upperMid)));
            return decision;
        }
        decision.dibit = correctedSample < lowerMid ? 1 : 0;
        decision.firstReliability =
            reliabilityFromDistance(std::abs(correctedSample - center),
                                    (std::max)(0.0001f, 0.5f * (center - syncMinLevel)));
        decision.secondReliability =
            reliabilityFromDistance(std::abs(correctedSample - lowerMid),
                                    (std::max)(0.0001f, 0.5f * (lowerMid - syncMinLevel)));
        return decision;
    };

    std::array<bool, DMR_SYMBOLS_BEFORE_SYNC * 2> leftBits = {};
    std::array<bool, DMR_SYMBOLS_BEFORE_SYNC * 2> rightBits = {};
    std::array<std::uint8_t, DMR_SYMBOLS_BEFORE_SYNC * 2> leftReliability = {};
    std::array<std::uint8_t, DMR_SYMBOLS_BEFORE_SYNC * 2> rightReliability = {};
    std::array<bool, DMR_SYMBOLS_BEFORE_SYNC * 4> combinedBits = {};
    if (includeDebug) {
        payload.leftDibits.reserve(DMR_SYMBOLS_BEFORE_SYNC);
        payload.syncDibits.reserve(DMR_SYNC_SYMBOLS);
        payload.rightDibits.reserve(DMR_SYMBOLS_BEFORE_SYNC);
    }
    for (int i = 0; i < DMR_SYMBOLS_BEFORE_SYNC; ++i) {
        const DmrDibitDecision decision =
            sampleToDecision(leftSamples[static_cast<std::size_t>(i)]);
        const int dibit = (std::clamp)(decision.dibit, 0, 3);
        if (includeDebug) {
            payload.leftDibits.append(QLatin1Char(static_cast<char>('0' + dibit)));
        }
        appendDibitBitsVariant(dibit, bitMapVariant, leftBits.data(), i * 2);
        leftReliability[static_cast<std::size_t>(i * 2)] = decision.firstReliability;
        leftReliability[static_cast<std::size_t>(i * 2 + 1)] = decision.secondReliability;
    }
    for (int i = 0; i < DMR_SYMBOLS_BEFORE_SYNC; ++i) {
        const DmrDibitDecision decision =
            sampleToDecision(rightSamples[static_cast<std::size_t>(i)]);
        const int dibit = (std::clamp)(decision.dibit, 0, 3);
        if (includeDebug) {
            payload.rightDibits.append(QLatin1Char(static_cast<char>('0' + dibit)));
        }
        appendDibitBitsVariant(dibit, bitMapVariant, rightBits.data(), i * 2);
        rightReliability[static_cast<std::size_t>(i * 2)] = decision.firstReliability;
        rightReliability[static_cast<std::size_t>(i * 2 + 1)] = decision.secondReliability;
    }
    if (includeDebug) {
        for (int i = 0; i < DMR_SYNC_SYMBOLS; ++i) {
            const DmrDibitDecision decision =
                sampleToDecision(syncSamples[static_cast<std::size_t>(i)]);
            const int dibit = (std::clamp)(decision.dibit, 0, 3);
            payload.syncDibits.append(QLatin1Char(static_cast<char>('0' + dibit)));
        }
    }

    constexpr int ambeFramesPerBurst = 3;
    constexpr int ambeFrameBits = 72;
    constexpr int ambeHalfFrameBits = ambeFrameBits / 2;
    const int selectedAmbeLayout = normalizedDmrAmbeLayout(ambeLayout);
    const auto sourceBit = [&](int streamBit, bool *bitValue, std::uint8_t *reliability) {
        if (streamBit < DMR_SYMBOLS_BEFORE_SYNC * 2) {
            *bitValue = leftBits[static_cast<std::size_t>(streamBit)];
            *reliability = leftReliability[static_cast<std::size_t>(streamBit)];
        } else {
            const int rightBit = streamBit - DMR_SYMBOLS_BEFORE_SYNC * 2;
            *bitValue = rightBits[static_cast<std::size_t>(rightBit)];
            *reliability = rightReliability[static_cast<std::size_t>(rightBit)];
        }
    };
    const auto copySourceBit = [&](int streamBit, int outputBit) {
        bool bitValue = false;
        std::uint8_t reliability = 0;
        sourceBit(streamBit, &bitValue, &reliability);
        combinedBits[static_cast<std::size_t>(outputBit)] = bitValue;
        payload.softReliability[static_cast<std::size_t>(outputBit)] = reliability;
    };

    switch (selectedAmbeLayout) {
    case DMR_AMBE_LAYOUT_AUTO:
    case DMR_AMBE_LAYOUT_SPLIT36:
        // DMR voice bursts carry AMBE as 36 dibits before the center field,
        // then 18 dibits before + 18 after it, then 36 dibits after it.
        // leftBits and rightBits already exclude the center sync/EMB field, so
        // the DSD/mbelib on-air 36/18+18/36 order is linear in this bit stream.
        for (int bit = 0; bit < DMR_SYMBOLS_BEFORE_SYNC * 4; ++bit) {
            copySourceBit(bit, bit);
        }
        break;
    case DMR_AMBE_LAYOUT_DIBIT_STRIPE:
        for (int sourceDibit = 0; sourceDibit < DMR_SYMBOLS_BEFORE_SYNC * 2; ++sourceDibit) {
            const int frame = sourceDibit % ambeFramesPerBurst;
            const int frameDibit = sourceDibit / ambeFramesPerBurst;
            const int sourceBitStart = sourceDibit * 2;
            const int outputBitStart = frame * ambeFrameBits + frameDibit * 2;
            copySourceBit(sourceBitStart, outputBitStart);
            copySourceBit(sourceBitStart + 1, outputBitStart + 1);
        }
        break;
    case DMR_AMBE_LAYOUT_BIT_STRIPE:
        for (int streamBit = 0; streamBit < DMR_SYMBOLS_BEFORE_SYNC * 4; ++streamBit) {
            const int frame = streamBit % ambeFramesPerBurst;
            const int frameBit = streamBit / ambeFramesPerBurst;
            copySourceBit(streamBit, frame * ambeFrameBits + frameBit);
        }
        break;
    case DMR_AMBE_LAYOUT_LINEAR72:
    default:
        for (int bit = 0; bit < DMR_SYMBOLS_BEFORE_SYNC * 4; ++bit) {
            copySourceBit(bit, bit);
        }
        break;
    }

    payload.decoded = true;
    payload.adaptiveSlicer = localSlicer.valid;
    payload.hasSoftBits = true;
    payload.slicerRange = localSlicer.valid ? localSlicer.range : (syncMaxLevel - syncMinLevel);
    payload.slicerMinSeparation =
        localSlicer.valid
            ? localSlicer.minSeparation
            : (std::min)((std::min)(upperMid - center, center - lowerMid),
                         (std::min)(syncMaxLevel - upperMid, lowerMid - syncMinLevel));
    payload.leftHex = bitsToHex(leftBits.data(), DMR_SYMBOLS_BEFORE_SYNC * 2);
    payload.rightHex = bitsToHex(rightBits.data(), DMR_SYMBOLS_BEFORE_SYNC * 2);
    payload.combinedHex = bitsToHex(combinedBits.data(), DMR_SYMBOLS_BEFORE_SYNC * 4);
    return payload;
}

DmrDecoder::VoiceEmbeddedBits DmrDecoder::decodeVoiceEmbeddedFragmentAt(const PendingEmb &pending,
                                                                        bool inverted,
                                                                        int timingOffset,
                                                                        float slicerRatio) const {
    VoiceEmbeddedBits fragment;
    const quint64 bufferStartSample = totalSamples > sampleBuffer.size()
                                          ? totalSamples - static_cast<quint64>(sampleBuffer.size())
                                          : 0;
    if (pending.absoluteSample < bufferStartSample) {
        return fragment;
    }

    const int centralStartIndex =
        static_cast<int>(pending.absoluteSample - bufferStartSample);
    const int sampleOffset = (std::clamp)(samplesPerSymbol / 2 + timingOffset,
                                          0,
                                          (std::max)(0, samplesPerSymbol - 1));
    const int firstFragmentSymbol = 4;
    const int fragmentSymbols = 16;
    const int lastFragmentSample =
        centralStartIndex + (firstFragmentSymbol + fragmentSymbols - 1) * samplesPerSymbol +
        sampleOffset;
    if (centralStartIndex + firstFragmentSymbol * samplesPerSymbol + sampleOffset < 0 ||
        lastFragmentSample >= static_cast<int>(sampleBuffer.size())) {
        return fragment;
    }

    const float center = 0.5f * (pending.maxLevel + pending.minLevel);
    const float thresholdRatio = (std::clamp)(slicerRatio, 0.45f, 0.80f);
    const float upperMid = center + thresholdRatio * (pending.maxLevel - center);
    const float lowerMid = center + thresholdRatio * (pending.minLevel - center);
    const auto sampleToDibit = [&](float sample) -> int {
        const float corrected = inverted ? -sample : sample;
        const float correctedCenter = inverted ? -center : center;
        const float correctedUpper = inverted ? -lowerMid : upperMid;
        const float correctedLower = inverted ? -upperMid : lowerMid;
        if (corrected > correctedCenter) {
            return corrected > correctedUpper ? 3 : 2;
        }
        return corrected < correctedLower ? 1 : 0;
    };

    bool bits[fragmentSymbols * 2] = {};
    for (int i = 0; i < fragmentSymbols; ++i) {
        const int symbol = firstFragmentSymbol + i;
        const int sampleIndex = centralStartIndex + symbol * samplesPerSymbol + sampleOffset;
        const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
        const int dibit = (std::clamp)(sampleToDibit(sample), 0, 3);
        appendDibitBits(dibit, true, bits, i * 2);
    }

    fragment.decoded = true;
    fragment.hex = bitsToHex(bits, fragmentSymbols * 2);
    return fragment;
}

DmrDecoder::EmbInfo DmrDecoder::decodeVoiceEmbAt(const PendingEmb &pending) const {
    EmbInfo info;
    const quint64 bufferStartSample = totalSamples > sampleBuffer.size()
                                          ? totalSamples - static_cast<quint64>(sampleBuffer.size())
                                          : 0;
    if (pending.absoluteSample < bufferStartSample) {
        return info;
    }

    const int centralStartIndex =
        static_cast<int>(pending.absoluteSample - bufferStartSample);
    const int lastEmbSampleIndex =
        centralStartIndex + 23 * samplesPerSymbol + samplesPerSymbol / 2;
    if (centralStartIndex < 0 || lastEmbSampleIndex >= static_cast<int>(sampleBuffer.size())) {
        return info;
    }

    const float center = 0.5f * (pending.maxLevel + pending.minLevel);
    const auto sampleToDibit = [&](float sample, float thresholdRatio, bool inverted) -> int {
        const float upperMid = center + thresholdRatio * (pending.maxLevel - center);
        const float lowerMid = center + thresholdRatio * (pending.minLevel - center);
        const float corrected = inverted ? -sample : sample;
        const float correctedCenter = inverted ? -center : center;
        const float correctedUpper = inverted ? -lowerMid : upperMid;
        const float correctedLower = inverted ? -upperMid : lowerMid;
        if (corrected > correctedCenter) {
            return corrected > correctedUpper ? 3 : 2;
        }
        return corrected < correctedLower ? 1 : 0;
    };

    int bestData = 0;
    int bestErrors = 17;
    int bestVariant = -1;
    int bestTimingOffset = 0;
    int bestTimingDistance = 99;
    int bestPolarityPenalty = 99;
    float bestSlicerRatio = 0.625f;
    bool bestInverted = pending.inverted;
    int fallbackData = 0;
    int fallbackErrors = 17;
    int fallbackVariant = -1;
    int fallbackTimingOffset = 0;
    int fallbackTimingDistance = 99;
    int fallbackPolarityPenalty = 99;
    int fallbackVariantPenalty = 99;
    float fallbackSlicerRatio = 0.625f;
    bool fallbackInverted = pending.inverted;
    std::vector<int> timingOffsets;
    timingOffsets.reserve(static_cast<std::size_t>(samplesPerSymbol));
    const auto addTimingOffset = [&](int value) {
        value = (std::clamp)(value, 0, (std::max)(0, samplesPerSymbol - 1));
        if (std::find(timingOffsets.begin(), timingOffsets.end(), value) ==
            timingOffsets.end()) {
            timingOffsets.push_back(value);
        }
    };
    const int centerTimingOffset = samplesPerSymbol / 2;
    const int preferredTimingOffset = liveTimingHintEnabled
        ? static_cast<int>(std::lround(
              liveTimingPhaseFraction * (std::max)(0, samplesPerSymbol - 1)))
        : centerTimingOffset;
    addTimingOffset(preferredTimingOffset);
    addTimingOffset(centerTimingOffset);
    for (int distance = 1; distance <= samplesPerSymbol / 2; ++distance) {
        addTimingOffset(centerTimingOffset - distance);
        addTimingOffset(centerTimingOffset + distance);
    }
    const std::array<float, 3> slicerRatios = {0.625f, 0.55f, 0.70f};
    const std::array<bool, 2> polarityVariants = {pending.inverted, !pending.inverted};
    for (bool candidateInverted : polarityVariants) {
        const int polarityPenalty = candidateInverted == pending.inverted ? 0 : 1;
        for (int timingOffset : timingOffsets) {
            const int relativeTimingOffset = timingOffset - samplesPerSymbol / 2;
            const int timingDistance = std::abs(timingOffset - preferredTimingOffset);
            for (float slicerRatio : slicerRatios) {
                bool embRawBits[16] = {};
                bool inRange = true;
                for (int i = 0; i < 4; ++i) {
                    const int sampleIndex = centralStartIndex + i * samplesPerSymbol + timingOffset;
                    if (sampleIndex < 0 || sampleIndex >= static_cast<int>(sampleBuffer.size())) {
                        inRange = false;
                        break;
                    }
                    const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
                    const int dibit =
                        (std::clamp)(sampleToDibit(sample, slicerRatio, candidateInverted),
                                     0,
                                     3);
                    appendDibitBits(dibit, true, embRawBits, i * 2);
                }
                if (!inRange) {
                    continue;
                }
                for (int i = 0; i < 4; ++i) {
                    const int sampleIndex = centralStartIndex + (20 + i) * samplesPerSymbol + timingOffset;
                    if (sampleIndex < 0 || sampleIndex >= static_cast<int>(sampleBuffer.size())) {
                        inRange = false;
                        break;
                    }
                    const float sample = averagedSymbolSample(sampleBuffer, sampleIndex, samplesPerSymbol);
                    const int dibit =
                        (std::clamp)(sampleToDibit(sample, slicerRatio, candidateInverted),
                                     0,
                                     3);
                    appendDibitBits(dibit, true, embRawBits, 8 + i * 2);
                }
                if (!inRange) {
                    continue;
                }

                std::array<bool, 16> embBits = {};
                std::copy(std::begin(embRawBits), std::end(embRawBits), embBits.begin());

                for (int variant = 0; variant < DMR_EMB_VARIANT_COUNT; ++variant) {
                    int data = 0;
                    int errors = 0;
                    const std::array<bool, 16> candidateBits = embBitsVariant(embBits, variant);
                    if (!qr1676Decode(candidateBits, data, errors)) {
                        continue;
                    }
                    const int variantPenalty = variant == 0 ? 0 : (variant == 1 ? 1 : 2);
                    const bool preferFallback =
                        errors < fallbackErrors ||
                        (errors == fallbackErrors && timingDistance < fallbackTimingDistance) ||
                        (errors == fallbackErrors &&
                         timingDistance == fallbackTimingDistance &&
                         polarityPenalty < fallbackPolarityPenalty) ||
                        (errors == fallbackErrors &&
                         timingDistance == fallbackTimingDistance &&
                         polarityPenalty == fallbackPolarityPenalty &&
                         variantPenalty < fallbackVariantPenalty);
                    if (preferFallback) {
                        fallbackErrors = errors;
                        fallbackData = data;
                        fallbackVariant = variant;
                        fallbackTimingOffset = relativeTimingOffset;
                        fallbackTimingDistance = timingDistance;
                        fallbackPolarityPenalty = polarityPenalty;
                        fallbackVariantPenalty = variantPenalty;
                        fallbackSlicerRatio = slicerRatio;
                        fallbackInverted = candidateInverted;
                    }
                    if (variant == 0) {
                        const bool preferDirect =
                        errors < bestErrors ||
                            (errors == bestErrors && timingDistance < bestTimingDistance) ||
                            (errors == bestErrors &&
                             timingDistance == bestTimingDistance &&
                             polarityPenalty < bestPolarityPenalty);
                        if (preferDirect) {
                            bestErrors = errors;
                            bestData = data;
                            bestVariant = variant;
                            bestTimingOffset = relativeTimingOffset;
                            bestTimingDistance = timingDistance;
                            bestPolarityPenalty = polarityPenalty;
                            bestSlicerRatio = slicerRatio;
                            bestInverted = candidateInverted;
                        }
                    }
                }
            }
            if (bestErrors == 0 && bestTimingDistance == 0 && bestPolarityPenalty == 0) {
                break;
            }
        }
        if (bestErrors == 0 && bestTimingDistance == 0 && bestPolarityPenalty == 0) {
            break;
        }
    }

    if (bestVariant < 0) {
        if (fallbackVariant < 0) {
            return info;
        }
        bestData = fallbackData;
        bestErrors = fallbackErrors;
        bestVariant = fallbackVariant;
        bestTimingOffset = fallbackTimingOffset;
        bestTimingDistance = fallbackTimingDistance;
        bestPolarityPenalty = fallbackPolarityPenalty;
        bestSlicerRatio = fallbackSlicerRatio;
        bestInverted = fallbackInverted;
    } else if (bestErrors > 1 && fallbackVariant >= 0 && fallbackErrors < bestErrors) {
        bestData = fallbackData;
        bestErrors = fallbackErrors;
        bestVariant = fallbackVariant;
        bestTimingOffset = fallbackTimingOffset;
        bestTimingDistance = fallbackTimingDistance;
        bestPolarityPenalty = fallbackPolarityPenalty;
        bestSlicerRatio = fallbackSlicerRatio;
        bestInverted = fallbackInverted;
    }

    if (bestVariant != 0 && bestErrors < 2) {
        bestErrors = 2;
    }

    info.decoded = true;
    info.colorCode = (bestData >> 3) & 0x0f;
    info.privacyIndicator = ((bestData >> 2) & 0x01) != 0;
    info.lcss = bestData & 0x03;
    info.correctedErrors = bestErrors;
    info.variantIndex = bestVariant;
    info.timingOffset = bestTimingOffset;
    info.slicerRatio = bestSlicerRatio;
    info.inverted = bestInverted;
    return info;
}

QString DmrDecoder::formatHit(const SyncHit &hit) const {
    if (!hit.pattern) {
        return idleStatus();
    }

    const bool confirmed =
        hit.score >= DMR_CONFIRMED_SYNC_SCORE &&
        hit.outerScore >= DMR_CONFIRMED_OUTER_SCORE;
    return QStringLiteral("DMR %1: %2, %3, sync %4/24, outer %5/24, phase %6, %7, CC/slot pending")
        .arg(confirmed ? QStringLiteral("lock") : QStringLiteral("candidate"))
        .arg(QString::fromLatin1(hit.pattern->source))
        .arg(QString::fromLatin1(hit.pattern->kind))
        .arg(hit.score)
        .arg(hit.outerScore)
        .arg(hit.phase)
        .arg(hit.inverted ? QStringLiteral("inverted") : QStringLiteral("normal"));
}

QString DmrDecoder::idleStatus() const {
    return averageMagnitude < 0.002
               ? QStringLiteral("DMR monitor: waiting for discriminator signal")
               : QStringLiteral("DMR monitor: searching sync");
}
