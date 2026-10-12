#include "digitaldecoder.h"

#include "diagnosticlogging.h"
#include "dmrprivacyutils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <cstdint>
#include <limits>
#include <numeric>

#include <QDateTime>
#include <QDebug>

#include <fftw3.h>

namespace {

constexpr double TWO_PI = 6.28318530717958647692;
constexpr double RTTY_BAUD = 45.45;
constexpr double RTTY_MARK_HZ = 2125.0;
constexpr double RTTY_SPACE_HZ = 2295.0;
constexpr double RTTY_DETECTOR_BANDWIDTH_HZ = 90.0;
constexpr double RTTY_MIN_TONE_LEVEL = 0.0015;
constexpr double RTTY_MIN_TONE_DOMINANCE = 0.16;
constexpr double FSK_MIN_DISCRIMINATOR_LEVEL = 0.015;
constexpr double FT8_SYMBOL_SECONDS = 0.160;
constexpr int FT8_SYMBOL_COUNT = 79;
constexpr double FT8_TONE_SPACING_HZ = 6.25;
constexpr double FT8_MIN_AUDIO_HZ = 150.0;
constexpr double FT8_MAX_AUDIO_HZ = 3000.0;
constexpr double FT8_SLOT_SECONDS = 15.0;
constexpr double FT8_SLOT_DECODE_DELAY_SECONDS = 1.25;
constexpr double FT8_SLOT_DECODE_WINDOW_SECONDS = 3.5;
constexpr double FT8_BUFFER_SECONDS = 28.0;
constexpr double FT8_MIN_BUFFER_SECONDS = 13.0;
constexpr double FT8_MIN_SYNC_DB = 3.0;
constexpr int FT8_MIN_SYNC_HITS = 11;
constexpr int FT8_MAX_REPORTED_CANDIDATES = 8;
constexpr int DMR_TRUSTED_AUDIO_MAX_FEC_ERRORS = 2;
constexpr int DMR_PROVISIONAL_AUDIO_MAX_FEC_ERRORS = 1;
constexpr int DMR_TRUSTED_AUDIO_MAX_RAW_FEC_ERRORS = 5;
constexpr int DMR_PROVISIONAL_AUDIO_MAX_RAW_FEC_ERRORS = 4;
constexpr int DMR_MIN_QUALITY_PAYLOADS_PER_CHUNK = 1;
constexpr int DMR_MAX_VOCODER_ERRORS_PER_FRAME = 2;
constexpr int DMR_AUDIO_SELECTOR_SWITCH_MARGIN = 2;
constexpr double DMR_AUDIO_ARTIFACT_PEAK = 0.85;
constexpr double DMR_AUDIO_ARTIFACT_RMS = 0.18;
constexpr double DMR_AUDIO_HARD_ARTIFACT_PEAK = 0.90;
constexpr double DMR_AUDIO_HARD_ARTIFACT_RMS = 0.18;
constexpr double DMR_AUDIO_SUSPICIOUS_ARTIFACT_RMS = 0.14;
constexpr double DMR_AUDIO_NEAR_SILENCE_RMS = 0.0008;
constexpr double DMR_VOCODER_COLLAPSE_PEAK = 0.025;
constexpr double DMR_VOCODER_COLLAPSE_RMS = 0.004;
constexpr double DMR_VOCODER_RECOVERY_PEAK = 0.030;
constexpr double DMR_VOCODER_RECOVERY_RMS = 0.003;
constexpr int DMR_VOICE_PCM_SAMPLE_RATE = 48000;
constexpr int DMR_VOICE_PCM_BYTES_PER_SAMPLE = 2;
constexpr int DMR_VOICE_PCM_CHUNK_MS = 60;
constexpr int DMR_VOICE_PCM_CHUNK_BYTES =
    DMR_VOICE_PCM_SAMPLE_RATE * DMR_VOICE_PCM_BYTES_PER_SAMPLE * DMR_VOICE_PCM_CHUNK_MS / 1000;
constexpr int DMR_VOICE_PCM_MAX_BUFFER_BYTES = DMR_VOICE_PCM_CHUNK_BYTES * 4;
constexpr int DMR_AMBE_PCM_FRAME_BYTES =
    160 * (DMR_VOICE_PCM_SAMPLE_RATE / 8000) * DMR_VOICE_PCM_BYTES_PER_SAMPLE;
constexpr double FT8_CANDIDATE_CLUSTER_HZ = 25.0;
constexpr double FT8_CANDIDATE_CLUSTER_SECONDS = 1.2;
constexpr int FT8_CANDIDATE_REPEAT_ANALYSES = 0;
constexpr double FT8_PANEL_MIN_CANDIDATE_SYNC_DB = 8.0;
constexpr int FT8_PANEL_MIN_CANDIDATE_COSTAS = 14;
constexpr int FT8_DATA_SYMBOL_COUNT = 58;
constexpr int FT8_CODE_BITS = FT8_DATA_SYMBOL_COUNT * 3;
constexpr int FT8_PAYLOAD_BITS = 77;
constexpr int FT8_MESSAGE_BITS = 91;
constexpr int FT8_LDPC_CHECK_COUNT = FT8_CODE_BITS - FT8_MESSAGE_BITS;
constexpr int FT8_LDPC_MAX_CHECK_DEGREE = 7;
constexpr int FT8_LDPC_VARIABLE_DEGREE = 3;
constexpr int FT8_LDPC_MAX_ITERATIONS = 30;

constexpr std::array<int, 8> FT8_GRAY_MAP = {{0, 1, 3, 2, 5, 6, 4, 7}};

constexpr std::array<std::array<int, FT8_LDPC_MAX_CHECK_DEGREE>, FT8_LDPC_CHECK_COUNT> FT8_LDPC_CHECKS = {{
    {{  3,  30,  58,  90,  91,  95, 152}},
    {{  4,  31,  59,  92, 114, 145,  -1}},
    {{  5,  23,  60,  93, 121, 150,  -1}},
    {{  6,  32,  61,  94,  95, 142,  -1}},
    {{  7,  24,  62,  82,  92,  95, 147}},
    {{  5,  31,  63,  96, 125, 137,  -1}},
    {{  4,  33,  64,  77,  97, 106, 153}},
    {{  8,  34,  65,  98, 138, 145,  -1}},
    {{  9,  35,  66,  99, 106, 125,  -1}},
    {{ 10,  36,  66,  86, 100, 138, 157}},
    {{ 11,  37,  67, 101, 104, 154,  -1}},
    {{ 12,  38,  68, 102, 148, 161,  -1}},
    {{  7,  39,  69,  81, 103, 113, 144}},
    {{ 13,  40,  70,  87, 101, 122, 155}},
    {{ 14,  41,  58, 105, 122, 158,  -1}},
    {{  0,  32,  71, 105, 106, 156,  -1}},
    {{ 15,  42,  72, 107, 140, 159,  -1}},
    {{ 16,  36,  73,  80, 108, 130, 153}},
    {{ 10,  43,  74, 109, 120, 165,  -1}},
    {{ 44,  54,  63, 110, 129, 160, 172}},
    {{  7,  45,  70, 111, 118, 165,  -1}},
    {{ 17,  35,  75,  88, 112, 113, 142}},
    {{ 18,  37,  76, 103, 115, 162,  -1}},
    {{ 19,  46,  69,  91, 137, 164,  -1}},
    {{  1,  47,  73, 112, 127, 159,  -1}},
    {{ 20,  44,  77,  82, 116, 120, 150}},
    {{ 21,  46,  57, 117, 126, 163,  -1}},
    {{ 15,  38,  61, 111, 133, 157,  -1}},
    {{ 22,  42,  78, 119, 130, 144,  -1}},
    {{ 18,  34,  58,  72, 109, 124, 160}},
    {{ 19,  35,  62,  93, 135, 160,  -1}},
    {{ 13,  30,  78,  97, 131, 163,  -1}},
    {{  2,  43,  79, 123, 126, 168,  -1}},
    {{ 18,  45,  80, 116, 134, 166,  -1}},
    {{  6,  48,  57,  89,  99, 104, 167}},
    {{ 11,  49,  60, 117, 118, 143,  -1}},
    {{ 12,  50,  63, 113, 117, 156,  -1}},
    {{ 23,  51,  75, 128, 147, 148,  -1}},
    {{ 24,  52,  68,  89, 100, 129, 155}},
    {{ 19,  45,  64,  79, 119, 139, 169}},
    {{ 20,  53,  76,  99, 139, 170,  -1}},
    {{ 34,  81, 132, 141, 170, 173,  -1}},
    {{ 13,  29,  82, 112, 124, 169,  -1}},
    {{  3,  28,  67, 119, 133, 172,  -1}},
    {{  0,   3,  51,  56,  85, 135, 151}},
    {{ 25,  50,  55,  90, 121, 136, 167}},
    {{ 51,  83, 109, 114, 144, 167,  -1}},
    {{  6,  49,  80,  98, 131, 172,  -1}},
    {{ 22,  54,  66,  94, 171, 173,  -1}},
    {{ 25,  40,  76, 108, 140, 147,  -1}},
    {{  1,  26,  40,  60,  61, 114, 132}},
    {{ 26,  39,  55, 123, 124, 125,  -1}},
    {{ 17,  48,  54, 123, 140, 166,  -1}},
    {{  5,  32,  84, 107, 115, 155,  -1}},
    {{ 27,  47,  69,  84, 104, 128, 157}},
    {{  8,  53,  62, 130, 146, 154,  -1}},
    {{ 21,  52,  67, 108, 120, 173,  -1}},
    {{  2,  12,  47,  77,  94, 122,  -1}},
    {{ 30,  68, 132, 149, 154, 168,  -1}},
    {{ 11,  42,  65,  88,  96, 134, 158}},
    {{  4,  38,  74, 101, 135, 166,  -1}},
    {{  1,  53,  85, 100, 134, 163,  -1}},
    {{ 14,  55,  86, 107, 118, 170,  -1}},
    {{  9,  43,  81,  90, 110, 143, 148}},
    {{ 22,  33,  70,  93, 126, 152,  -1}},
    {{ 10,  48,  87,  91, 141, 156,  -1}},
    {{ 28,  33,  86,  96, 146, 161,  -1}},
    {{ 29,  49,  59,  85, 136, 141, 161}},
    {{  9,  52,  65,  83, 111, 127, 164}},
    {{ 21,  56,  84,  92, 139, 158,  -1}},
    {{ 27,  31,  71, 102, 131, 165,  -1}},
    {{ 27,  28,  83,  87, 116, 142, 149}},
    {{  0,  25,  44,  79, 127, 146,  -1}},
    {{ 16,  26,  88, 102, 115, 152,  -1}},
    {{ 50,  56,  97, 162, 164, 171,  -1}},
    {{ 20,  36,  72, 137, 151, 168,  -1}},
    {{ 15,  46,  75, 129, 136, 153,  -1}},
    {{  2,  23,  29,  71, 103, 138,  -1}},
    {{  8,  39,  89, 105, 133, 150,  -1}},
    {{ 14,  57,  59,  73, 110, 149, 162}},
    {{ 17,  41,  78, 143, 145, 151,  -1}},
    {{ 24,  37,  64,  98, 121, 159,  -1}},
    {{ 16,  41,  74, 128, 169, 171,  -1}},
}};

bool isSupportedDecoderMode(int modulationType) {
    return modulationType == MOD_CW ||
           modulationType == MOD_PSK ||
           modulationType == MOD_FT8 ||
           modulationType == MOD_RTTY ||
           modulationType == MOD_FSK ||
           modulationType == MOD_DMR;
}

bool isDigitalMode(int modulationType) {
    return modulationType == MOD_CW ||
           modulationType == MOD_FT8 ||
           modulationType == MOD_RTTY ||
           modulationType == MOD_FSK ||
           modulationType == MOD_PSK ||
           modulationType == MOD_DMR;
}

qint16 readPcm16Le(const char *data) {
    qint16 value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

struct PcmStats {
    double peak = 0.0;
    double rms = 0.0;
    double clippedPercent = 0.0;
};

struct DmrVoiceProbeStats {
    bool ran = false;
    int pcmBytes = 0;
    int decodedFrames = 0;
    int errors = 0;
    PcmStats pcm;
};

struct DmrAudioCandidate {
    QString source;
    QByteArray pcm;
    int decodedFrames = 0;
    int vocoderErrors = 0;
    int concealedPayloads = 0;
    int rejectedVocoderFrames = 0;
    PcmStats stats;

    bool valid() const {
        return !pcm.isEmpty() && decodedFrames > 0;
    }
};

PcmStats pcm16Stats(const QByteArray &pcm) {
    PcmStats stats;
    const int sampleCount = pcm.size() / static_cast<int>(sizeof(qint16));
    if (sampleCount <= 0) {
        return stats;
    }

    const char *raw = pcm.constData();
    double squareSum = 0.0;
    int clipped = 0;
    for (int i = 0; i < sampleCount; ++i) {
        const int value = readPcm16Le(raw + i * static_cast<int>(sizeof(qint16)));
        const double normalized = static_cast<double>(value) / 32768.0;
        stats.peak = (std::max)(stats.peak, std::abs(normalized));
        squareSum += normalized * normalized;
        if (value <= -32760 || value >= 32760) {
            ++clipped;
        }
    }
    stats.rms = std::sqrt(squareSum / static_cast<double>(sampleCount));
    stats.clippedPercent =
        100.0 * static_cast<double>(clipped) / static_cast<double>(sampleCount);
    return stats;
}

QString dmrVoiceProbeSummary(const DmrVoiceProbeStats &probe) {
    if (!probe.ran) {
        return QStringLiteral("off");
    }
    return QStringLiteral("b%1/f%2/e%3/pk%4/rms%5")
        .arg(probe.pcmBytes)
        .arg(probe.decodedFrames)
        .arg(probe.errors)
        .arg(probe.pcm.peak, 0, 'f', 3)
        .arg(probe.pcm.rms, 0, 'f', 3);
}

bool dmrAudioLooksLikeArtifact(const DmrAudioCandidate &candidate) {
    if (!candidate.valid()) {
        return false;
    }
    const int frames = (std::max)(1, candidate.decodedFrames);
    return candidate.vocoderErrors > frames * DMR_MAX_VOCODER_ERRORS_PER_FRAME &&
           (candidate.stats.peak >= DMR_AUDIO_ARTIFACT_PEAK ||
            candidate.stats.rms >= DMR_AUDIO_ARTIFACT_RMS);
}

bool dmrAudioShouldConcealArtifact(const DmrAudioCandidate &candidate) {
    if (!candidate.valid()) {
        return false;
    }

    const int frames = (std::max)(1, candidate.decodedFrames);
    const bool impossibleEnergy =
        candidate.stats.peak >= DMR_AUDIO_HARD_ARTIFACT_PEAK &&
        candidate.stats.rms >= DMR_AUDIO_HARD_ARTIFACT_RMS &&
        candidate.vocoderErrors >= 2;
    const bool suspiciousLoudEnergy =
        candidate.stats.peak >= DMR_AUDIO_HARD_ARTIFACT_PEAK &&
        candidate.stats.rms >= DMR_AUDIO_SUSPICIOUS_ARTIFACT_RMS &&
        candidate.vocoderErrors >= frames + 2;
    const bool severeVocoderErrors =
        candidate.vocoderErrors > frames * 3 &&
        candidate.stats.rms >= 0.050;

    return impossibleEnergy || suspiciousLoudEnergy || severeVocoderErrors;
}

int dmrAudioCandidateScore(const DmrAudioCandidate &candidate, int expectedFrames) {
    if (!candidate.valid()) {
        return std::numeric_limits<int>::max() / 4;
    }

    int score = candidate.vocoderErrors * 100;
    if (expectedFrames > 0 && candidate.decodedFrames < expectedFrames) {
        score += (expectedFrames - candidate.decodedFrames) * 220;
    }
    if (candidate.stats.rms < DMR_AUDIO_NEAR_SILENCE_RMS) {
        score += 350;
    }
    if (dmrAudioLooksLikeArtifact(candidate)) {
        score += 10000;
    }
    return score;
}

DmrVoiceProbeStats dmrProbeFromCandidate(const DmrAudioCandidate &candidate) {
    DmrVoiceProbeStats probe;
    if (candidate.pcm.isEmpty() && candidate.decodedFrames <= 0) {
        return probe;
    }
    probe.ran = true;
    probe.pcmBytes = candidate.pcm.size();
    probe.decodedFrames = candidate.decodedFrames;
    probe.errors = candidate.vocoderErrors;
    probe.pcm = candidate.stats;
    return probe;
}

struct Ft8Candidate {
    bool valid = false;
    double audioHz = 0.0;
    double startOffsetSeconds = 0.0;
    double syncDb = -120.0;
    int hits = 0;
    int decodedCostasHits = 0;
    double symbolReliabilityDb = -120.0;
    double softBitMarginDb = -120.0;
    std::array<unsigned char, FT8_DATA_SYMBOL_COUNT> dataSymbols = {};
    std::array<float, FT8_CODE_BITS> softBits = {};
    std::array<unsigned char, FT8_PAYLOAD_BITS> payloadBits = {};
    bool ldpcDecoded = false;
    int ldpcIterations = 0;
    int ldpcUnsatisfiedChecks = FT8_LDPC_CHECK_COUNT;
    int ldpcHardErrors = -1;
    int ldpcPass = 0;
    QString decodedMessage;
    bool hasDataSymbols = false;
};

class FftwPlanScope {
public:
    explicit FftwPlanScope(fftwf_plan plan)
        : plan_(plan) {
    }

    ~FftwPlanScope() {
        if (plan_) {
            fftwf_destroy_plan(plan_);
        }
    }

    FftwPlanScope(const FftwPlanScope &) = delete;
    FftwPlanScope &operator=(const FftwPlanScope &) = delete;

private:
    fftwf_plan plan_ = nullptr;
};

bool isFt8SyncSymbol(int symbolIndex) {
    return (symbolIndex >= 0 && symbolIndex <= 6) ||
           (symbolIndex >= 36 && symbolIndex <= 42) ||
           (symbolIndex >= 72 && symbolIndex <= 78);
}

int ft8CostasToneForSymbol(int symbolIndex) {
    static const std::array<int, 7> costas = {{3, 1, 4, 0, 6, 5, 2}};
    if (symbolIndex >= 0 && symbolIndex <= 6) {
        return costas[static_cast<std::size_t>(symbolIndex)];
    }
    if (symbolIndex >= 36 && symbolIndex <= 42) {
        return costas[static_cast<std::size_t>(symbolIndex - 36)];
    }
    if (symbolIndex >= 72 && symbolIndex <= 78) {
        return costas[static_cast<std::size_t>(symbolIndex - 72)];
    }
    return -1;
}

QString ft8ToneString(const std::array<unsigned char, FT8_DATA_SYMBOL_COUNT> &symbols) {
    QString result;
    result.reserve(FT8_DATA_SYMBOL_COUNT);
    for (unsigned char symbol : symbols) {
        result.append(QChar(QLatin1Char(static_cast<char>('0' + (symbol & 0x07)))));
    }
    return result;
}

QString ft8PayloadBitString(const std::array<unsigned char, FT8_PAYLOAD_BITS> &bits) {
    QString result;
    result.reserve(FT8_PAYLOAD_BITS);
    for (unsigned char bit : bits) {
        result.append(QChar(QLatin1Char(bit ? '1' : '0')));
    }
    return result;
}

QString ft8FrequencyText(double frequencyHz) {
    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0) {
        return QString();
    }
    if (frequencyHz >= 1000000.0) {
        return QStringLiteral("%1 MHz").arg(frequencyHz / 1000000.0, 0, 'f', 6);
    }
    if (frequencyHz >= 1000.0) {
        return QStringLiteral("%1 kHz").arg(frequencyHz / 1000.0, 0, 'f', 3);
    }
    return QStringLiteral("%1 Hz").arg(frequencyHz, 0, 'f', 0);
}

QString ft8SignedSecondsText(double seconds) {
    return QStringLiteral("%1%2 s")
        .arg(seconds >= 0.0 ? QStringLiteral("+") : QString())
        .arg(seconds, 0, 'f', 2);
}

std::uint64_t ft8ReadBits(const std::array<unsigned char, FT8_PAYLOAD_BITS> &bits, int start, int count) {
    std::uint64_t value = 0;
    for (int i = 0; i < count; ++i) {
        const int bitIndex = start + i;
        value = (value << 1) |
                (bitIndex >= 0 && bitIndex < FT8_PAYLOAD_BITS
                     ? static_cast<std::uint64_t>(bits[static_cast<std::size_t>(bitIndex)] & 1)
                     : 0ULL);
    }
    return value;
}

QString ft8TrimCall(QString call) {
    call = call.trimmed();
    if (call.startsWith(QStringLiteral("CQ_"))) {
        call[2] = QLatin1Char(' ');
    }
    return call;
}

QString ft8Unpack28(std::uint64_t packedValue, bool &ok) {
    constexpr int NTOKENS = 2063592;
    constexpr int MAX22 = 4194304;
    static const QString c1 = QStringLiteral(" 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    static const QString c2 = QStringLiteral("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    static const QString c3 = QStringLiteral("0123456789");
    static const QString c4 = QStringLiteral(" ABCDEFGHIJKLMNOPQRSTUVWXYZ");

    ok = true;
    int n28 = static_cast<int>(packedValue & ((1u << 28) - 1u));
    if (n28 < NTOKENS) {
        if (n28 == 0) {
            return QStringLiteral("DE");
        }
        if (n28 == 1) {
            return QStringLiteral("QRZ");
        }
        if (n28 == 2) {
            return QStringLiteral("CQ");
        }
        if (n28 <= 1002) {
            return QStringLiteral("CQ_%1").arg(n28 - 3, 3, 10, QLatin1Char('0'));
        }
        if (n28 <= 532443) {
            int n = n28 - 1003;
            QString suffix;
            for (int i = 0; i < 4; ++i) {
                const int divisor = static_cast<int>(std::pow(27, 3 - i));
                const int symbol = divisor > 0 ? n / divisor : n;
                if (divisor > 0) {
                    n -= symbol * divisor;
                }
                suffix.append(c4.at((std::clamp)(symbol, 0, c4.size() - 1)));
            }
            return QStringLiteral("CQ_%1").arg(suffix.trimmed());
        }
        ok = false;
        return QStringLiteral("QU1RK");
    }

    n28 -= NTOKENS;
    if (n28 < MAX22) {
        return QStringLiteral("<...>");
    }

    int n = n28 - MAX22;
    const int i1 = n / (36 * 10 * 27 * 27 * 27);
    n -= i1 * 36 * 10 * 27 * 27 * 27;
    const int i2 = n / (10 * 27 * 27 * 27);
    n -= i2 * 10 * 27 * 27 * 27;
    const int i3 = n / (27 * 27 * 27);
    n -= i3 * 27 * 27 * 27;
    const int i4 = n / (27 * 27);
    n -= i4 * 27 * 27;
    const int i5 = n / 27;
    const int i6 = n - i5 * 27;

    if (i1 < 0 || i1 >= c1.size() ||
        i2 < 0 || i2 >= c2.size() ||
        i3 < 0 || i3 >= c3.size() ||
        i4 < 0 || i4 >= c4.size() ||
        i5 < 0 || i5 >= c4.size() ||
        i6 < 0 || i6 >= c4.size()) {
        ok = false;
        return QStringLiteral("QU1RK");
    }

    QString call;
    call.append(c1.at(i1));
    call.append(c2.at(i2));
    call.append(c3.at(i3));
    call.append(c4.at(i4));
    call.append(c4.at(i5));
    call.append(c4.at(i6));
    call = call.trimmed();
    if (call.indexOf(QLatin1Char(' ')) >= 0) {
        ok = false;
        return QStringLiteral("QU1RK");
    }
    return call;
}

QString ft8Grid4(int packedGrid, bool &ok) {
    constexpr int GRID4_MAX = 32400;
    ok = packedGrid >= 0 && packedGrid <= GRID4_MAX;
    if (!ok) {
        return QString();
    }
    int n = packedGrid;
    const int j1 = n / (18 * 10 * 10);
    n -= j1 * 18 * 10 * 10;
    const int j2 = n / (10 * 10);
    n -= j2 * 10 * 10;
    const int j3 = n / 10;
    const int j4 = n - j3 * 10;
    if (j1 < 0 || j1 > 17 || j2 < 0 || j2 > 17 || j3 < 0 || j3 > 9 || j4 < 0 || j4 > 9) {
        ok = false;
        return QString();
    }

    QString grid;
    grid.append(QChar(QLatin1Char(static_cast<char>('A' + j1))));
    grid.append(QChar(QLatin1Char(static_cast<char>('A' + j2))));
    grid.append(QChar(QLatin1Char(static_cast<char>('0' + j3))));
    grid.append(QChar(QLatin1Char(static_cast<char>('0' + j4))));
    return grid;
}

QString ft8FreeText(const std::array<unsigned char, FT8_PAYLOAD_BITS> &bits) {
    static const QString alphabet = QStringLiteral(" 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?");
    std::array<unsigned char, 71> value = {};
    for (int i = 0; i < static_cast<int>(value.size()); ++i) {
        value[static_cast<std::size_t>(i)] = bits[static_cast<std::size_t>(i)] & 1;
    }

    QString text(13, QLatin1Char(' '));
    for (int pos = 12; pos >= 0; --pos) {
        int remainder = 0;
        for (unsigned char &bit : value) {
            remainder = remainder * 2 + (bit & 1);
            bit = static_cast<unsigned char>(remainder / 42);
            remainder %= 42;
        }
        text[pos] = alphabet.at((std::clamp)(remainder, 0, alphabet.size() - 1));
    }
    return text.trimmed();
}

QString ft8UnpackPayload(const std::array<unsigned char, FT8_PAYLOAD_BITS> &bits, bool &ok) {
    constexpr int GRID4_MAX = 32400;
    const int n3 = static_cast<int>(ft8ReadBits(bits, 71, 3));
    const int i3 = static_cast<int>(ft8ReadBits(bits, 74, 3));
    ok = true;

    if (i3 == 0 && n3 == 0) {
        const QString text = ft8FreeText(bits);
        ok = !text.isEmpty();
        return text;
    }

    if (i3 == 1 || i3 == 2) {
        bool callOk = true;
        bool secondCallOk = true;
        QString call1 = ft8TrimCall(ft8Unpack28(ft8ReadBits(bits, 0, 28), callOk));
        const int ipa = static_cast<int>(ft8ReadBits(bits, 28, 1));
        QString call2 = ft8TrimCall(ft8Unpack28(ft8ReadBits(bits, 29, 28), secondCallOk));
        const int ipb = static_cast<int>(ft8ReadBits(bits, 57, 1));
        const int ir = static_cast<int>(ft8ReadBits(bits, 58, 1));
        const int igrid4 = static_cast<int>(ft8ReadBits(bits, 59, 15));

        ok = callOk && secondCallOk && !call1.isEmpty() && !call2.isEmpty();
        if (!ok) {
            return QString();
        }

        if (!call1.startsWith(QLatin1Char('<'))) {
            if (ipa == 1 && i3 == 1) {
                call1 += QStringLiteral("/R");
            } else if (ipa == 1 && i3 == 2) {
                call1 += QStringLiteral("/P");
            }
        }
        if (!call2.startsWith(QLatin1Char('<'))) {
            if (ipb == 1 && i3 == 1) {
                call2 += QStringLiteral("/R");
            } else if (ipb == 1 && i3 == 2) {
                call2 += QStringLiteral("/P");
            }
        }

        if (igrid4 <= GRID4_MAX) {
            bool gridOk = false;
            const QString grid = ft8Grid4(igrid4, gridOk);
            ok = gridOk;
            if (!ok) {
                return QString();
            }
            return ir == 0
                       ? QStringLiteral("%1 %2 %3").arg(call1, call2, grid)
                       : QStringLiteral("%1 %2 R %3").arg(call1, call2, grid);
        }

        const int reportCode = igrid4 - GRID4_MAX;
        if (reportCode == 1) {
            return QStringLiteral("%1 %2").arg(call1, call2);
        }
        if (reportCode == 2) {
            return QStringLiteral("%1 %2 RRR").arg(call1, call2);
        }
        if (reportCode == 3) {
            return QStringLiteral("%1 %2 RR73").arg(call1, call2);
        }
        if (reportCode == 4) {
            return QStringLiteral("%1 %2 73").arg(call1, call2);
        }
        if (reportCode >= 5) {
            int snr = reportCode - 35;
            if (snr > 50) {
                snr -= 101;
            }
            const QString report = QStringLiteral("%1%2")
                                       .arg(snr >= 0 ? QStringLiteral("+") : QString())
                                       .arg(snr, 2, 10, QLatin1Char('0'));
            return ir == 0
                       ? QStringLiteral("%1 %2 %3").arg(call1, call2, report)
                       : QStringLiteral("%1 %2 R%3").arg(call1, call2, report);
        }

        ok = false;
        return QString();
    }

    ok = false;
    return QString();
}

void updateSoftBitsFromToneMetrics(const std::array<double, 8> &metrics,
                                   int dataIndex,
                                   Ft8Candidate &candidate) {
    if (dataIndex < 0 || dataIndex >= FT8_DATA_SYMBOL_COUNT) {
        return;
    }

    const int bitBase = dataIndex * 3;
    for (int bit = 0; bit < 3; ++bit) {
        double bestZero = 1e-20;
        double bestOne = 1e-20;
        for (int symbolValue = 0; symbolValue < 8; ++symbolValue) {
            const int tone = FT8_GRAY_MAP[static_cast<std::size_t>(symbolValue)];
            const bool bitSet = ((symbolValue >> (2 - bit)) & 1) != 0;
            if (bitSet) {
                bestOne = (std::max)(bestOne, metrics[static_cast<std::size_t>(tone)]);
            } else {
                bestZero = (std::max)(bestZero, metrics[static_cast<std::size_t>(tone)]);
            }
        }
        const double llr = bestOne - bestZero;
        candidate.softBits[static_cast<std::size_t>(bitBase + bit)] =
            static_cast<float>((std::clamp)(llr, -1.0e6, 1.0e6));
    }
}

void normalizeFt8SoftBits(std::array<float, FT8_CODE_BITS> &softBits) {
    double meanSquare = 0.0;
    double mean = 0.0;
    for (float value : softBits) {
        mean += value;
        meanSquare += static_cast<double>(value) * value;
    }
    mean /= FT8_CODE_BITS;
    meanSquare /= FT8_CODE_BITS;
    const double variance = meanSquare - mean * mean;
    const double sigma = variance > 0.0 ? std::sqrt(variance) : std::sqrt(meanSquare);
    if (!std::isfinite(sigma) || sigma <= 1e-12) {
        return;
    }
    constexpr double scale = 2.83;
    for (float &value : softBits) {
        value = static_cast<float>((std::clamp)(scale * value / sigma, -30.0, 30.0));
    }
}

void updateSoftBitsFromMultiSymbolMetrics(const std::vector<double> &metrics,
                                          int groupStart,
                                          int groupSymbolCount,
                                          std::array<float, FT8_CODE_BITS> &softBits) {
    if (groupStart < 0 || groupStart >= FT8_DATA_SYMBOL_COUNT ||
        groupSymbolCount <= 0 ||
        groupStart + groupSymbolCount > FT8_DATA_SYMBOL_COUNT ||
        metrics.empty()) {
        return;
    }

    const int bitCount = groupSymbolCount * 3;
    const int bitBase = groupStart * 3;
    for (int bit = 0; bit < bitCount; ++bit) {
        double bestZero = -1.0;
        double bestOne = -1.0;
        const int mask = 1 << (bitCount - 1 - bit);
        for (int value = 0; value < static_cast<int>(metrics.size()); ++value) {
            if ((value & mask) != 0) {
                bestOne = (std::max)(bestOne, metrics[static_cast<std::size_t>(value)]);
            } else {
                bestZero = (std::max)(bestZero, metrics[static_cast<std::size_t>(value)]);
            }
        }

        if (bestZero >= 0.0 && bestOne >= 0.0 && bitBase + bit < FT8_CODE_BITS) {
            softBits[static_cast<std::size_t>(bitBase + bit)] =
                static_cast<float>((std::clamp)(bestOne - bestZero, -1.0e6, 1.0e6));
        }
    }
}

std::array<float, FT8_CODE_BITS> buildFt8SoftBits(
    const std::vector<std::array<std::complex<double>, 8>> &symbols,
    int groupSymbolCount) {
    std::array<float, FT8_CODE_BITS> softBits = {};
    if (static_cast<int>(symbols.size()) != FT8_DATA_SYMBOL_COUNT || groupSymbolCount <= 0) {
        return softBits;
    }

    for (int groupStart = 0; groupStart < FT8_DATA_SYMBOL_COUNT; groupStart += groupSymbolCount) {
        const int actualGroupCount = (std::min)(groupSymbolCount, FT8_DATA_SYMBOL_COUNT - groupStart);
        const int candidateCount = 1 << (actualGroupCount * 3);
        std::vector<double> metrics(static_cast<std::size_t>(candidateCount), 0.0);

        for (int value = 0; value < candidateCount; ++value) {
            std::complex<double> coherentSum(0.0, 0.0);
            for (int symbolOffset = 0; symbolOffset < actualGroupCount; ++symbolOffset) {
                const int shift = (actualGroupCount - 1 - symbolOffset) * 3;
                const int symbolValue = (value >> shift) & 0x07;
                const int tone = FT8_GRAY_MAP[static_cast<std::size_t>(symbolValue)];
                coherentSum += symbols[static_cast<std::size_t>(groupStart + symbolOffset)][static_cast<std::size_t>(tone)];
            }
            metrics[static_cast<std::size_t>(value)] = std::abs(coherentSum);
        }

        updateSoftBitsFromMultiSymbolMetrics(metrics, groupStart, actualGroupCount, softBits);
    }

    normalizeFt8SoftBits(softBits);
    return softBits;
}

double ft8ParabolicPeakOffset(double left, double center, double right) {
    const double denominator = left - 2.0 * center + right;
    if (!std::isfinite(denominator) || std::abs(denominator) <= 1e-20) {
        return 0.0;
    }
    return (std::clamp)(0.5 * (left - right) / denominator, -0.5, 0.5);
}

std::array<std::complex<double>, 8> ft8ToneComplexAt(const std::vector<float> &audio,
                                                     int symbolStart,
                                                     int symbolSamples,
                                                     int sampleRate,
                                                     double baseHz) {
    std::array<std::complex<double>, 8> symbols = {};
    if (symbolStart < 0 ||
        symbolSamples <= 0 ||
        sampleRate <= 0 ||
        symbolStart + symbolSamples > static_cast<int>(audio.size())) {
        return symbols;
    }

    for (int tone = 0; tone < 8; ++tone) {
        const double frequencyHz = baseHz + tone * FT8_TONE_SPACING_HZ;
        const double phaseStep = -TWO_PI * frequencyHz / sampleRate;
        const double rotRe = std::cos(phaseStep);
        const double rotIm = std::sin(phaseStep);
        double oscRe = 1.0;
        double oscIm = 0.0;
        double sumRe = 0.0;
        double sumIm = 0.0;

        for (int i = 0; i < symbolSamples; ++i) {
            const double sample = audio[static_cast<std::size_t>(symbolStart + i)];
            sumRe += sample * oscRe;
            sumIm += sample * oscIm;
            const double nextRe = oscRe * rotRe - oscIm * rotIm;
            const double nextIm = oscRe * rotIm + oscIm * rotRe;
            oscRe = nextRe;
            oscIm = nextIm;
        }

        symbols[static_cast<std::size_t>(tone)] = std::complex<double>(sumRe, sumIm);
    }

    return symbols;
}

struct Ft8VariableEdge {
    int check = -1;
    int edge = -1;
};

const std::array<std::array<Ft8VariableEdge, FT8_LDPC_VARIABLE_DEGREE>, FT8_CODE_BITS> &ft8VariableEdges() {
    static const auto edges = [] {
        std::array<std::array<Ft8VariableEdge, FT8_LDPC_VARIABLE_DEGREE>, FT8_CODE_BITS> result = {};
        std::array<int, FT8_CODE_BITS> counts = {};

        for (int check = 0; check < FT8_LDPC_CHECK_COUNT; ++check) {
            for (int edge = 0; edge < FT8_LDPC_MAX_CHECK_DEGREE; ++edge) {
                const int bit = FT8_LDPC_CHECKS[static_cast<std::size_t>(check)][static_cast<std::size_t>(edge)];
                if (bit < 0 || bit >= FT8_CODE_BITS) {
                    continue;
                }
                const int slot = counts[static_cast<std::size_t>(bit)]++;
                if (slot >= 0 && slot < FT8_LDPC_VARIABLE_DEGREE) {
                    result[static_cast<std::size_t>(bit)][static_cast<std::size_t>(slot)] = {check, edge};
                }
            }
        }

        return result;
    }();

    return edges;
}

int ft8Crc14Remainder(const std::array<unsigned char, 96> &message) {
    constexpr std::array<unsigned char, 15> polynomial = {{1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1}};
    std::array<unsigned char, 15> remainder = {};
    std::copy_n(message.begin(), remainder.size(), remainder.begin());

    for (int i = 0; i <= 96 - 15; ++i) {
        remainder[14] = message[static_cast<std::size_t>(i + 14)];
        const unsigned char feedback = remainder[0];
        if (feedback) {
            for (int bit = 0; bit < static_cast<int>(remainder.size()); ++bit) {
                remainder[static_cast<std::size_t>(bit)] =
                    static_cast<unsigned char>((remainder[static_cast<std::size_t>(bit)] +
                                                polynomial[static_cast<std::size_t>(bit)]) & 1);
            }
        }

        const unsigned char first = remainder[0];
        for (int bit = 0; bit < static_cast<int>(remainder.size()) - 1; ++bit) {
            remainder[static_cast<std::size_t>(bit)] = remainder[static_cast<std::size_t>(bit + 1)];
        }
        remainder.back() = first;
    }

    int crc = 0;
    for (int bit = 0; bit < 14; ++bit) {
        crc = (crc << 1) | (remainder[static_cast<std::size_t>(bit)] & 1);
    }
    return crc;
}

bool ft8CrcOk(const std::array<unsigned char, FT8_MESSAGE_BITS> &messageBits) {
    std::array<unsigned char, 96> crcMessage = {};
    for (int bit = 0; bit < FT8_PAYLOAD_BITS; ++bit) {
        crcMessage[static_cast<std::size_t>(bit)] = messageBits[static_cast<std::size_t>(bit)] & 1;
    }
    for (int bit = 0; bit < 14; ++bit) {
        crcMessage[static_cast<std::size_t>(82 + bit)] =
            messageBits[static_cast<std::size_t>(FT8_PAYLOAD_BITS + bit)] & 1;
    }
    return ft8Crc14Remainder(crcMessage) == 0;
}

double ft8SafeAtanh(double value) {
    value = (std::clamp)(value, -0.999999, 0.999999);
    return 0.5 * std::log((1.0 + value) / (1.0 - value));
}

bool decodeFt8Ldpc(const std::array<float, FT8_CODE_BITS> &softBits,
                   std::array<unsigned char, FT8_PAYLOAD_BITS> &payloadBits,
                   int &iterations,
                   int &unsatisfiedChecks,
                   int &hardErrors) {
    constexpr double LLR_SCALE = 1.0;
    constexpr double LLR_LIMIT = 8.0;
    const auto &variableEdges = ft8VariableEdges();

    std::array<double, FT8_CODE_BITS> llr = {};
    std::array<double, FT8_CODE_BITS> posterior = {};
    std::array<unsigned char, FT8_CODE_BITS> codeword = {};
    std::array<std::array<double, FT8_LDPC_MAX_CHECK_DEGREE>, FT8_LDPC_CHECK_COUNT> variableToCheck = {};
    std::array<std::array<double, FT8_LDPC_MAX_CHECK_DEGREE>, FT8_LDPC_CHECK_COUNT> checkToVariable = {};

    for (int bit = 0; bit < FT8_CODE_BITS; ++bit) {
        llr[static_cast<std::size_t>(bit)] =
            (std::clamp)(static_cast<double>(softBits[static_cast<std::size_t>(bit)]) * LLR_SCALE,
                         -LLR_LIMIT,
                         LLR_LIMIT);
    }

    for (int check = 0; check < FT8_LDPC_CHECK_COUNT; ++check) {
        for (int edge = 0; edge < FT8_LDPC_MAX_CHECK_DEGREE; ++edge) {
            const int bit = FT8_LDPC_CHECKS[static_cast<std::size_t>(check)][static_cast<std::size_t>(edge)];
            if (bit >= 0) {
                variableToCheck[static_cast<std::size_t>(check)][static_cast<std::size_t>(edge)] =
                    llr[static_cast<std::size_t>(bit)];
            }
        }
    }

    auto evaluateCodeword = [&] {
        unsatisfiedChecks = 0;
        for (int bit = 0; bit < FT8_CODE_BITS; ++bit) {
            posterior[static_cast<std::size_t>(bit)] = llr[static_cast<std::size_t>(bit)];
            for (const Ft8VariableEdge &edgeRef : variableEdges[static_cast<std::size_t>(bit)]) {
                if (edgeRef.check < 0) {
                    continue;
                }
                posterior[static_cast<std::size_t>(bit)] +=
                    checkToVariable[static_cast<std::size_t>(edgeRef.check)][static_cast<std::size_t>(edgeRef.edge)];
            }
            codeword[static_cast<std::size_t>(bit)] = posterior[static_cast<std::size_t>(bit)] > 0.0 ? 1 : 0;
        }

        for (int check = 0; check < FT8_LDPC_CHECK_COUNT; ++check) {
            int parity = 0;
            for (int edge = 0; edge < FT8_LDPC_MAX_CHECK_DEGREE; ++edge) {
                const int bit = FT8_LDPC_CHECKS[static_cast<std::size_t>(check)][static_cast<std::size_t>(edge)];
                if (bit >= 0) {
                    parity ^= codeword[static_cast<std::size_t>(bit)];
                }
            }
            if (parity != 0) {
                ++unsatisfiedChecks;
            }
        }
    };

    int lastUnsatisfied = FT8_LDPC_CHECK_COUNT;
    int stalledIterations = 0;
    hardErrors = -1;

    for (iterations = 0; iterations <= FT8_LDPC_MAX_ITERATIONS; ++iterations) {
        evaluateCodeword();
        if (unsatisfiedChecks == 0) {
            std::array<unsigned char, FT8_MESSAGE_BITS> messageBits = {};
            std::copy_n(codeword.begin(), FT8_MESSAGE_BITS, messageBits.begin());
            if (ft8CrcOk(messageBits)) {
                std::copy_n(messageBits.begin(), FT8_PAYLOAD_BITS, payloadBits.begin());
                hardErrors = 0;
                for (int bit = 0; bit < FT8_CODE_BITS; ++bit) {
                    const bool hardDecision = llr[static_cast<std::size_t>(bit)] >= 0.0;
                    if (hardDecision != (codeword[static_cast<std::size_t>(bit)] != 0)) {
                        ++hardErrors;
                    }
                }
                return true;
            }
        }

        if (iterations > 0) {
            if (unsatisfiedChecks < lastUnsatisfied) {
                stalledIterations = 0;
            } else {
                ++stalledIterations;
            }
            if (stalledIterations >= 5 && iterations >= 10 && unsatisfiedChecks > 15) {
                return false;
            }
        }
        lastUnsatisfied = unsatisfiedChecks;

        for (int check = 0; check < FT8_LDPC_CHECK_COUNT; ++check) {
            for (int edge = 0; edge < FT8_LDPC_MAX_CHECK_DEGREE; ++edge) {
                const int bit = FT8_LDPC_CHECKS[static_cast<std::size_t>(check)][static_cast<std::size_t>(edge)];
                if (bit < 0) {
                    continue;
                }
                variableToCheck[static_cast<std::size_t>(check)][static_cast<std::size_t>(edge)] =
                    posterior[static_cast<std::size_t>(bit)] -
                    checkToVariable[static_cast<std::size_t>(check)][static_cast<std::size_t>(edge)];
            }
        }

        for (int check = 0; check < FT8_LDPC_CHECK_COUNT; ++check) {
            for (int edge = 0; edge < FT8_LDPC_MAX_CHECK_DEGREE; ++edge) {
                if (FT8_LDPC_CHECKS[static_cast<std::size_t>(check)][static_cast<std::size_t>(edge)] < 0) {
                    continue;
                }
                double product = 1.0;
                for (int otherEdge = 0; otherEdge < FT8_LDPC_MAX_CHECK_DEGREE; ++otherEdge) {
                    if (otherEdge == edge ||
                        FT8_LDPC_CHECKS[static_cast<std::size_t>(check)][static_cast<std::size_t>(otherEdge)] < 0) {
                        continue;
                    }
                    product *= std::tanh(-variableToCheck[static_cast<std::size_t>(check)][static_cast<std::size_t>(otherEdge)] / 2.0);
                }
                checkToVariable[static_cast<std::size_t>(check)][static_cast<std::size_t>(edge)] =
                    2.0 * ft8SafeAtanh(-product);
            }
        }
    }

    return false;
}

} // namespace

DigitalDecoder::DigitalDecoder(QObject *parent)
    : QObject(parent) {
    configureForMode(MOD_AM, 48000);
}

void DigitalDecoder::setEnabled(bool enabled) {
    if (decoderEnabled == enabled) {
        return;
    }
    decoderEnabled = enabled;
    reset();
    updateStatus(enabled ? QStringLiteral("Digital audio decoder ready")
                         : QStringLiteral("Digital audio decoder disabled"));
}

bool DigitalDecoder::isEnabled() const {
    return decoderEnabled;
}

void DigitalDecoder::reset() {
    cwDecoder.reset();
    pskDecoder.reset(); pskTextDecoder.reset(); pskEncoding = -1; pskStatusSamples = 0;
    resetRttyState();
    markPhase = 0.0;
    spacePhase = 0.0;
    markI = 0.0;
    markQ = 0.0;
    spaceI = 0.0;
    spaceQ = 0.0;
    resetFt8State();
    dmrDecoder.reset();
    dmrPrivacy.reset();
    dmrVocoder.reset();
    dmrPayloadProbeVocoder.reset();
    dmrRawProbeVocoder.reset();
    dmrCanonicalProbeVocoder.reset();
    dmrAmbeFrameCount = 0;
    dmrAmbePayloadCount = 0;
    dmrAmbeFecCorrectionCount = 0;
    dmrVocoderFrameCount = 0;
    dmrVocoderErrorCount = 0;
    dmrAudioLogCounter = 0;
    dmrCollapsedVoiceCount = 0;
    dmrVocoderResetRecoveryCount = 0;
    dmrArtifactConcealCount = 0;
    clearDmrVoicePcmBuffer();
}

void DigitalDecoder::clearDmrVoicePcmBuffer() {
    pendingDmrVoicePcm.clear();
}

void DigitalDecoder::queueDmrVoicePcm(const QByteArray &pcmData) {
    if (pcmData.isEmpty()) {
        return;
    }

    pendingDmrVoicePcm.append(pcmData);
    if (pendingDmrVoicePcm.size() > DMR_VOICE_PCM_MAX_BUFFER_BYTES) {
        const int overflow = pendingDmrVoicePcm.size() - DMR_VOICE_PCM_MAX_BUFFER_BYTES;
        const int chunksToDrop =
            (overflow + DMR_VOICE_PCM_CHUNK_BYTES - 1) / DMR_VOICE_PCM_CHUNK_BYTES;
        const int dropBytes =
            (std::min)(pendingDmrVoicePcm.size(), chunksToDrop * DMR_VOICE_PCM_CHUNK_BYTES);
        pendingDmrVoicePcm.remove(0, dropBytes);
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DMR audio]"
                     << "pcmJitterBufferDropBytes" << dropBytes
                     << "remainingBytes" << pendingDmrVoicePcm.size();
        }
    }

    while (pendingDmrVoicePcm.size() >= DMR_VOICE_PCM_CHUNK_BYTES) {
        const QByteArray chunk = pendingDmrVoicePcm.left(DMR_VOICE_PCM_CHUNK_BYTES);
        pendingDmrVoicePcm.remove(0, DMR_VOICE_PCM_CHUNK_BYTES);
        emit voicePcmReady(chunk);
    }
}

void DigitalDecoder::configure(const RadioSettings &settings, int sampleRate) {
    CwAudioDecoder::Settings cwSettings;
    cwSettings.sampleRate = sampleRate;
    cwSettings.toneHz = settings.cwDecoderToneHz;
    cwSettings.initialWpm = settings.cwDecoderWpm;
    cwSettings.adaptiveSpeed = settings.cwDecoderAdaptiveSpeed;
    cwSettings.alphabet = settings.cwDecoderAlphabet;
    cwSettings.selectivity = settings.cwDecoderSelectivity;
    cwDecoder.configure(cwSettings);
    dmrDecoder.setLabHints(settings.dmrLabEnabled,
                           settings.dmrLabColorCode,
                           settings.dmrLabTimeslot,
                           settings.dmrLabSourceId,
                           settings.dmrLabTargetId,
                           settings.dmrManualTimingEnabled,
                           settings.dmrManualTimingOffset,
                           static_cast<float>(settings.dmrSlicerRatio),
                           settings.dmrAdaptiveSlicer,
                           settings.dmrAmbeLayout);
    dmrDecoder.setLiveTimingHint(settings.liveDigitalSyncEnabled,
                                 settings.liveDigitalSyncTimingPhase,
                                 settings.liveDigitalSyncConfidence);
    configureForMode(settings.modulationType, sampleRate);
    configureDmrPrivacy(settings);
}

void DigitalDecoder::configureDmrPrivacy(const RadioSettings &settings) {
    if (!dmrPrivacy.configure(settings.dmrPrivacyMode,
                              settings.modulationType == MOD_DMR && dmrVoiceOutputEnabled,
                              settings.dmrPrivacyKeyHex, settings.dmrPrivacyKeyId)) return;
    dmrVocoder.reset();
    dmrPayloadProbeVocoder.reset();
    dmrRawProbeVocoder.reset();
    dmrCanonicalProbeVocoder.reset();
    clearDmrVoicePcmBuffer();
}

void DigitalDecoder::setDmrVoiceBackendId(const QString &backendId) {
    const QString clean = backendId.trimmed();
    if (preferredDmrVoiceBackendId == clean) {
        return;
    }
    preferredDmrVoiceBackendId = clean;
    dmrVocoder.setPreferredBackendId(clean);
    dmrPayloadProbeVocoder.setPreferredBackendId(clean);
    dmrRawProbeVocoder.setPreferredBackendId(clean);
    dmrCanonicalProbeVocoder.setPreferredBackendId(clean);
    clearDmrVoicePcmBuffer();
}

void DigitalDecoder::setDmrVoicePayloadEnabled(bool enabled) {
    dmrDecoder.setVoicePayloadEnabled(enabled);
}

void DigitalDecoder::setDmrVoiceOutputEnabled(bool enabled) {
    if (dmrVoiceOutputEnabled == enabled) {
        return;
    }
    dmrVoiceOutputEnabled = enabled;
    clearDmrVoicePcmBuffer();
}

void DigitalDecoder::processPcmFrame(const QByteArray &pcmData, const RadioSettings &settings, int sampleRate) {
    if (!decoderEnabled) {
        return;
    }

    currentSettings = settings;
    dmrDecoder.setLabHints(settings.dmrLabEnabled,
                           settings.dmrLabColorCode,
                           settings.dmrLabTimeslot,
                           settings.dmrLabSourceId,
                           settings.dmrLabTargetId,
                           settings.dmrManualTimingEnabled,
                           settings.dmrManualTimingOffset,
                           static_cast<float>(settings.dmrSlicerRatio),
                           settings.dmrAdaptiveSlicer,
                           settings.dmrAmbeLayout);
    dmrDecoder.setLiveTimingHint(settings.liveDigitalSyncEnabled,
                                 settings.liveDigitalSyncTimingPhase,
                                 settings.liveDigitalSyncConfidence);
    configureForMode(settings.modulationType, sampleRate);
    if (pcmData.size() < static_cast<int>(sizeof(qint16))) {
        return;
    }
    if (!isSupportedDecoderMode(settings.modulationType)) {
        return;
    }

    QString decodedText;
    const int sampleCount = pcmData.size() / static_cast<int>(sizeof(qint16));
    const char *raw = pcmData.constData();

    if (settings.modulationType == MOD_PSK) {
        pskDecoder.configure(sampleRate, settings.pskDecoderToneHz, settings.pskDecoderSquelchPercent, settings.pskDecoderAfc);
        if (!pskTextDecoder || pskEncoding != settings.pskDecoderEncoding) {
            pskEncoding = settings.pskDecoderEncoding;
            pskTextDecoder.reset(QTextCodec::codecForName(pskEncoding == 1 ? "UTF-8" : "ISO-8859-1")->makeDecoder());
        }
        const QByteArray text = pskDecoder.process(pcmData);
        if (!text.isEmpty()) emit textDecoded(pskTextDecoder->toUnicode(text));
        pskStatusSamples += sampleCount;
        if (pskStatusSamples >= sampleRate/2) {
            pskStatusSamples = 0;
            updateStatus(QStringLiteral("BPSK31: %1 | %2 Hz | quality %3% | AFC %4 Hz")
                .arg(pskDecoder.isLocked() ? "LOCK" : "SEARCH")
                .arg(settings.pskDecoderToneHz,0,'f',1).arg(pskDecoder.qualityPercent(),0,'f',0)
                .arg(pskDecoder.carrierOffsetHz(),0,'f',2));
        }
        return;
    }

    if (settings.modulationType == MOD_CW) {
        CwAudioDecoder::Settings cwSettings;
        cwSettings.sampleRate = sampleRate;
        cwSettings.toneHz = settings.cwDecoderToneHz;
        cwSettings.initialWpm = settings.cwDecoderWpm;
        cwSettings.adaptiveSpeed = settings.cwDecoderAdaptiveSpeed;
        cwSettings.alphabet = settings.cwDecoderAlphabet;
        cwSettings.selectivity = settings.cwDecoderSelectivity;
        cwDecoder.configure(cwSettings);
        const CwAudioDecoder::Result result = cwDecoder.processPcm16(pcmData);
        if (!result.status.isEmpty()) updateStatus(result.status);
        if (!result.text.isEmpty()) emit textDecoded(result.text);
        return;
    }

    if (settings.modulationType == MOD_DMR) {
        configureDmrPrivacy(settings);
        DmrDecoder::Result result =
            dmrDecoder.processPcmFrame(pcmData, sampleRate, settings.listeningFrequency);
        if (!result.serviceStatusText.isEmpty()) {
            updateStatus(result.serviceStatusText);
        } else if (result.statusChanged && !result.statusText.isEmpty()) {
            updateStatus(result.statusText);
        }
        if (result.metadataValid) {
            emit dmrMetadataDetected(result.metadataColorCode,
                                     result.metadataTimeslot,
                                     result.metadataTargetId,
                                     result.metadataSourceId,
                                     result.metadataFlco);
        }
        for (const DmrDecoder::DibitBurst &burst : result.dibitBursts) {
            emit dmrDibitBurstReady(burst.dibits,
                                    burst.burstKind,
                                    burst.dataType,
                                    burst.dataTypeName,
                                    burst.slotTypeErrors,
                                    burst.sample,
                                    burst.baseDibit,
                                    burst.burstIndex,
                                    burst.cadenceSymbols,
                                    burst.colorCode,
                                    burst.timeslot,
                                    burst.timeslotSource,
                                    burst.cachDecoded,
                                    burst.cachChannel,
                                    burst.cachLcss,
                                    burst.cachMapName,
                                    burst.cachPayloadBits,
                                    burst.ambe49Payloads);
        }
        if (result.lockAcquired) {
            dmrPrivacy.resetVoiceAlignment();
            dmrVocoder.reset();
            dmrPayloadProbeVocoder.reset();
            dmrRawProbeVocoder.reset();
            dmrCanonicalProbeVocoder.reset();
            dmrCollapsedVoiceCount = 0;
            dmrVocoderResetRecoveryCount = 0;
            dmrArtifactConcealCount = 0;
            clearDmrVoicePcmBuffer();
        }
        if (dmrPrivacy.isActive()) {
            for (const DmrDecoder::DibitBurst &burst : result.dibitBursts)
                if (burst.burstKind == QStringLiteral("data") && burst.dataType == 0)
                    dmrPrivacy.observePi(burst.dibits, burst.timeslot, burst.sample);
            // External backends already received the original encrypted burst.
            // Native audio must use only corrected, decrypted 49-bit payloads.
            dmrPrivacy.processVoiceFrames(result.ambePayloads, result.ambeFrames, result.ambeSoftFrames);
        }
        const qint64 previousFrameCount = dmrAmbeFrameCount;
        const qint64 previousPayloadCount = dmrAmbePayloadCount;
        if (!result.ambeFrames.empty()) {
            dmrAmbeFrameCount += static_cast<qint64>(result.ambeFrames.size());
        }
        const int validAmbePayloads = static_cast<int>(
            std::count_if(result.ambePayloads.begin(),
                          result.ambePayloads.end(),
                          [](const DmrAmbePayload &payload) {
                              return !payload.hex.isEmpty();
                          }));
        if (validAmbePayloads > 0) {
            dmrAmbePayloadCount += static_cast<qint64>(validAmbePayloads);
            dmrAmbeFecCorrectionCount += result.ambeFecCorrections;
        }
        if (dmrVoiceOutputEnabled && (!result.ambeFrames.empty() ||
            !result.ambeSoftFrames.empty() ||
            !result.ambePayloads.empty())) {
            int decodedVoiceFrames = 0;
            int vocoderErrors = 0;
            QByteArray voicePcm;
            QString audioSource;
            std::vector<int> qualityPayloadIndexes;
            int filteredPayloads = 0;
            int concealedPayloads = 0;
            int rejectedVocoderFrames = 0;
            QString softFrameErrorsText;
            const auto isQualityPayload = [](const DmrAmbePayload &payload,
                                             int maxCorrectedErrors,
                                             int maxRawCorrectedErrors) {
                return !payload.hex.isEmpty() &&
                       payload.correctedErrors <= maxCorrectedErrors &&
                       payload.rawCorrectedErrors <= maxRawCorrectedErrors;
            };
            const auto collectQualityPayloads = [&](int maxCorrectedErrors,
                                                    int maxRawCorrectedErrors) {
                qualityPayloadIndexes.clear();
                qualityPayloadIndexes.reserve(result.ambePayloads.size());
                filteredPayloads = 0;
                for (int index = 0; index < static_cast<int>(result.ambePayloads.size()); ++index) {
                    const DmrAmbePayload &payload =
                        result.ambePayloads[static_cast<std::size_t>(index)];
                    if (isQualityPayload(payload, maxCorrectedErrors, maxRawCorrectedErrors)) {
                        qualityPayloadIndexes.push_back(index);
                    } else {
                        ++filteredPayloads;
                    }
                }
            };
            const bool payloadAudioCandidate =
                !result.ambePayloads.empty() &&
                (result.voiceAudioTrusted ||
                 (!result.voiceAudioTrusted && result.voiceAudioConfidence >= 12));
            int maxPayloadErrors = result.voiceAudioTrusted
                                       ? DMR_TRUSTED_AUDIO_MAX_FEC_ERRORS
                                       : DMR_PROVISIONAL_AUDIO_MAX_FEC_ERRORS;
            int maxRawPayloadErrors = result.voiceAudioTrusted
                                          ? DMR_TRUSTED_AUDIO_MAX_RAW_FEC_ERRORS
                                          : DMR_PROVISIONAL_AUDIO_MAX_RAW_FEC_ERRORS;
            if (payloadAudioCandidate) {
                collectQualityPayloads(maxPayloadErrors, maxRawPayloadErrors);
            }
            const int minQualityPayloads =
                validAmbePayloads >= DMR_MIN_QUALITY_PAYLOADS_PER_CHUNK
                    ? DMR_MIN_QUALITY_PAYLOADS_PER_CHUNK
                    : 1;
            const bool hasQualityPayloadAudio =
                static_cast<int>(qualityPayloadIndexes.size()) >= minQualityPayloads;
            const bool provisionalVoiceAudio =
                !result.voiceAudioTrusted &&
                hasQualityPayloadAudio;
            const bool softVoiceAudio =
                !result.ambeSoftFrames.empty() &&
                (result.voiceAudioTrusted || result.voiceAudioConfidence >= 12);
            const bool frameVoiceAudio =
                result.ambePayloads.empty() &&
                (!result.ambeSoftFrames.empty() || !result.ambeFrames.empty()) &&
                (result.voiceAudioTrusted || result.voiceAudioConfidence >= 12);
            if (voicePcm.isEmpty() && softVoiceAudio) {
                audioSource = QStringLiteral("softFrames");
                std::vector<int> softFrameErrors;
                voicePcm = dmrVocoder.decodeSoftFrames(result.ambeSoftFrames,
                                                       &decodedVoiceFrames,
                                                       &vocoderErrors,
                                                       &softFrameErrors);
                if (voicePcm.isEmpty()) {
                    audioSource.clear();
                    decodedVoiceFrames = 0;
                    vocoderErrors = 0;
                } else if (!softFrameErrors.empty()) {
                    QStringList softFrameErrorItems;
                    softFrameErrorItems.reserve(static_cast<int>(softFrameErrors.size()));
                    for (int frameError : softFrameErrors) {
                        softFrameErrorItems << QString::number(frameError);
                    }
                    softFrameErrorsText = softFrameErrorItems.join(QLatin1Char('/'));
                }
            }
            if (voicePcm.isEmpty() && payloadAudioCandidate) {
                audioSource = hasQualityPayloadAudio
                                  ? (result.voiceAudioTrusted
                                         ? QStringLiteral("payload49FecGate")
                                         : QStringLiteral("provisionalPayload49FecGate"))
                                  : QStringLiteral("concealedPayloads");
                voicePcm.reserve(static_cast<int>(result.ambePayloads.size()) *
                                 DMR_AMBE_PCM_FRAME_BYTES);
                for (int index = 0; index < static_cast<int>(result.ambePayloads.size()); ++index) {
                    const DmrAmbePayload &payload =
                        result.ambePayloads[static_cast<std::size_t>(index)];
                    if (!hasQualityPayloadAudio ||
                        !isQualityPayload(payload, maxPayloadErrors, maxRawPayloadErrors)) {
                        voicePcm.append(QByteArray(DMR_AMBE_PCM_FRAME_BYTES, '\0'));
                        ++concealedPayloads;
                        continue;
                    }

                    int frameCount = 0;
                    int frameErrors = 0;
                    QByteArray framePcm =
                        dmrVocoder.decodePayloads(std::vector<DmrAmbePayload>{payload},
                                                  &frameCount,
                                                  &frameErrors);
                    if (framePcm.isEmpty() &&
                        index < static_cast<int>(result.ambeSoftFrames.size())) {
                        framePcm = dmrVocoder.decodeSoftFrames(
                            std::vector<DmrAmbeSoftFrame>{
                                result.ambeSoftFrames[static_cast<std::size_t>(index)]},
                            &frameCount,
                            &frameErrors);
                    }
                    if (framePcm.isEmpty() &&
                        index < static_cast<int>(result.ambeFrames.size())) {
                        framePcm = dmrVocoder.decodeFrames(
                            std::vector<QString>{
                                result.ambeFrames[static_cast<std::size_t>(index)]},
                            &frameCount,
                            &frameErrors);
                    }
                    if (framePcm.isEmpty()) {
                        voicePcm.append(QByteArray(DMR_AMBE_PCM_FRAME_BYTES, '\0'));
                        ++concealedPayloads;
                        continue;
                    }
                    const int maxVocoderErrors =
                        DMR_MAX_VOCODER_ERRORS_PER_FRAME * (std::max)(1, frameCount);
                    if (frameErrors > maxVocoderErrors) {
                        voicePcm.append(QByteArray(DMR_AMBE_PCM_FRAME_BYTES, '\0'));
                        ++concealedPayloads;
                        ++rejectedVocoderFrames;
                        continue;
                    }
                    voicePcm.append(framePcm);
                    decodedVoiceFrames += frameCount;
                    vocoderErrors += frameErrors;
                }
            }
            if (voicePcm.isEmpty() && frameVoiceAudio) {
                audioSource = QStringLiteral("rawFrames");
                voicePcm = dmrVocoder.decodeFrames(result.ambeFrames,
                                                   &decodedVoiceFrames,
                                                   &vocoderErrors);
            }
            PcmStats voiceStats = pcm16Stats(voicePcm);
            const int expectedAudioFrames = (std::max)({
                static_cast<int>(result.ambePayloads.size()),
                static_cast<int>(result.ambeSoftFrames.size()),
                static_cast<int>(result.ambeFrames.size())});
            DmrAudioCandidate selectedCandidate;
            selectedCandidate.source = audioSource;
            selectedCandidate.pcm = voicePcm;
            selectedCandidate.decodedFrames = decodedVoiceFrames;
            selectedCandidate.vocoderErrors = vocoderErrors;
            selectedCandidate.concealedPayloads = concealedPayloads;
            selectedCandidate.rejectedVocoderFrames = rejectedVocoderFrames;
            selectedCandidate.stats = voiceStats;

            DmrAudioCandidate payloadCandidate;
            if (payloadAudioCandidate && !result.ambePayloads.empty()) {
                payloadCandidate.source = result.voiceAudioTrusted
                                              ? QStringLiteral("payload49Selector")
                                              : QStringLiteral("provisionalPayload49Selector");
                payloadCandidate.pcm.reserve(static_cast<int>(result.ambePayloads.size()) *
                                             DMR_AMBE_PCM_FRAME_BYTES);
                for (int index = 0; index < static_cast<int>(result.ambePayloads.size()); ++index) {
                    const DmrAmbePayload &payload =
                        result.ambePayloads[static_cast<std::size_t>(index)];
                    if (!hasQualityPayloadAudio ||
                        !isQualityPayload(payload, maxPayloadErrors, maxRawPayloadErrors)) {
                        payloadCandidate.pcm.append(QByteArray(DMR_AMBE_PCM_FRAME_BYTES, '\0'));
                        ++payloadCandidate.concealedPayloads;
                        continue;
                    }

                    int frameCount = 0;
                    int frameErrors = 0;
                    QByteArray framePcm =
                        dmrPayloadProbeVocoder.decodePayloads(std::vector<DmrAmbePayload>{payload},
                                                              &frameCount,
                                                              &frameErrors);
                    if (framePcm.isEmpty()) {
                        payloadCandidate.pcm.append(QByteArray(DMR_AMBE_PCM_FRAME_BYTES, '\0'));
                        ++payloadCandidate.concealedPayloads;
                        continue;
                    }
                    const int maxVocoderErrors =
                        DMR_MAX_VOCODER_ERRORS_PER_FRAME * (std::max)(1, frameCount);
                    if (frameErrors > maxVocoderErrors) {
                        payloadCandidate.pcm.append(QByteArray(DMR_AMBE_PCM_FRAME_BYTES, '\0'));
                        ++payloadCandidate.concealedPayloads;
                        ++payloadCandidate.rejectedVocoderFrames;
                        continue;
                    }
                    payloadCandidate.pcm.append(framePcm);
                    payloadCandidate.decodedFrames += frameCount;
                    payloadCandidate.vocoderErrors += frameErrors;
                }
                payloadCandidate.stats = pcm16Stats(payloadCandidate.pcm);
            }

            DmrAudioCandidate rawCandidate;
            if (!result.ambeFrames.empty()) {
                rawCandidate.source = QStringLiteral("rawFramesSelector");
                rawCandidate.pcm = dmrRawProbeVocoder.decodeFrames(result.ambeFrames,
                                                                   &rawCandidate.decodedFrames,
                                                                   &rawCandidate.vocoderErrors);
                rawCandidate.stats = pcm16Stats(rawCandidate.pcm);
            }

            DmrAudioCandidate canonicalCandidate;
            if (!result.ambePayloads.empty()) {
                canonicalCandidate.source = QStringLiteral("canonicalSelector");
                canonicalCandidate.pcm =
                    dmrCanonicalProbeVocoder.decodeCanonicalPayloads(
                        result.ambePayloads,
                        &canonicalCandidate.decodedFrames,
                        &canonicalCandidate.vocoderErrors);
                canonicalCandidate.stats = pcm16Stats(canonicalCandidate.pcm);
            }

            QString selectorSwitch;
            auto maybeSelectCandidate = [&](const DmrAudioCandidate &candidate,
                                            bool allowNearSilence) {
                if (!candidate.valid()) {
                    return;
                }
                if (!selectedCandidate.valid()) {
                    selectorSwitch = QStringLiteral("none>%1").arg(candidate.source);
                    selectedCandidate = candidate;
                    return;
                }

                const bool currentArtifact = dmrAudioLooksLikeArtifact(selectedCandidate);
                const bool candidateArtifact = dmrAudioLooksLikeArtifact(candidate);
                if (!allowNearSilence &&
                    candidate.stats.rms < DMR_AUDIO_NEAR_SILENCE_RMS &&
                    !currentArtifact) {
                    return;
                }

                const int currentScore =
                    dmrAudioCandidateScore(selectedCandidate, expectedAudioFrames);
                const int candidateScore =
                    dmrAudioCandidateScore(candidate, expectedAudioFrames);
                const bool scoreWin =
                    candidateScore + DMR_AUDIO_SELECTOR_SWITCH_MARGIN * 100 < currentScore;
                const bool clearlyCleaner =
                    !candidateArtifact &&
                    candidate.vocoderErrors + DMR_AUDIO_SELECTOR_SWITCH_MARGIN <
                        selectedCandidate.vocoderErrors &&
                    (candidate.stats.rms >= DMR_AUDIO_NEAR_SILENCE_RMS || currentArtifact);
                if ((currentArtifact && !candidateArtifact) || scoreWin || clearlyCleaner) {
                    const QString previousSource =
                        selectedCandidate.source.isEmpty()
                            ? QStringLiteral("none")
                            : selectedCandidate.source;
                    selectorSwitch =
                        QStringLiteral("%1>%2").arg(previousSource, candidate.source);
                    selectedCandidate = candidate;
                }
            };

            maybeSelectCandidate(payloadCandidate, false);
            maybeSelectCandidate(rawCandidate, false);
            const bool selectedIsNotUseful =
                !selectedCandidate.valid() ||
                dmrAudioLooksLikeArtifact(selectedCandidate) ||
                (selectedCandidate.decodedFrames < expectedAudioFrames &&
                 selectedCandidate.stats.rms < DMR_AUDIO_NEAR_SILENCE_RMS);
            if (selectedIsNotUseful) {
                maybeSelectCandidate(canonicalCandidate, false);
            }
            if (selectedCandidate.source != audioSource) {
                audioSource = selectedCandidate.source;
                voicePcm = selectedCandidate.pcm;
                decodedVoiceFrames = selectedCandidate.decodedFrames;
                vocoderErrors = selectedCandidate.vocoderErrors;
                concealedPayloads = selectedCandidate.concealedPayloads;
                rejectedVocoderFrames = selectedCandidate.rejectedVocoderFrames;
                voiceStats = selectedCandidate.stats;
            }
            PcmStats retryStats;
            bool resetRetryAttempted = false;
            bool resetRetryUsed = false;
            const bool softVocoderCollapseCandidate =
                audioSource == QStringLiteral("softFrames") &&
                !voicePcm.isEmpty() &&
                !result.ambeSoftFrames.empty() &&
                decodedVoiceFrames > 0 &&
                (result.voiceAudioTrusted || result.voiceAudioConfidence >= 30) &&
                vocoderErrors <= decodedVoiceFrames * 4 &&
                voiceStats.peak < DMR_VOCODER_COLLAPSE_PEAK &&
                voiceStats.rms < DMR_VOCODER_COLLAPSE_RMS;
            if (softVocoderCollapseCandidate) {
                ++dmrCollapsedVoiceCount;
                int retryDecodedVoiceFrames = 0;
                int retryVocoderErrors = 0;
                dmrVocoder.reset();
                resetRetryAttempted = true;
                QByteArray retryPcm =
                    dmrVocoder.decodeSoftFrames(result.ambeSoftFrames,
                                                &retryDecodedVoiceFrames,
                                                &retryVocoderErrors);
                retryStats = pcm16Stats(retryPcm);
                const bool retryImproved =
                    !retryPcm.isEmpty() &&
                    retryDecodedVoiceFrames == decodedVoiceFrames &&
                    retryVocoderErrors <= vocoderErrors + 2 &&
                    retryStats.peak >= (std::max)(DMR_VOCODER_RECOVERY_PEAK,
                                                  voiceStats.peak * 2.0) &&
                    retryStats.rms >= (std::max)(DMR_VOCODER_RECOVERY_RMS,
                                                 voiceStats.rms * 2.0);
                if (retryImproved) {
                    voicePcm = retryPcm;
                    decodedVoiceFrames = retryDecodedVoiceFrames;
                    vocoderErrors = retryVocoderErrors;
                    voiceStats = retryStats;
                    audioSource = QStringLiteral("softFramesResetRetry");
                    ++dmrVocoderResetRecoveryCount;
                    dmrCollapsedVoiceCount = 0;
                    resetRetryUsed = true;
                }
            } else if (!voicePcm.isEmpty() &&
                       (voiceStats.peak >= DMR_VOCODER_COLLAPSE_PEAK ||
                        voiceStats.rms >= DMR_VOCODER_COLLAPSE_RMS)) {
                dmrCollapsedVoiceCount = 0;
            }

            bool artifactConcealed = false;
            PcmStats artifactStats = voiceStats;
            const int artifactVocoderErrors = vocoderErrors;
            DmrAudioCandidate finalCandidate;
            finalCandidate.source = audioSource;
            finalCandidate.pcm = voicePcm;
            finalCandidate.decodedFrames = decodedVoiceFrames;
            finalCandidate.vocoderErrors = vocoderErrors;
            finalCandidate.stats = voiceStats;
            if (dmrAudioShouldConcealArtifact(finalCandidate)) {
                const int concealedFrames =
                    (std::max)(1, (std::max)(expectedAudioFrames, decodedVoiceFrames));
                voicePcm = QByteArray(concealedFrames * DMR_AMBE_PCM_FRAME_BYTES, '\0');
                voiceStats = pcm16Stats(voicePcm);
                audioSource =
                    audioSource.isEmpty()
                        ? QStringLiteral("artifactConceal")
                        : QStringLiteral("%1ArtifactConceal").arg(audioSource);
                concealedPayloads += concealedFrames;
                rejectedVocoderFrames += concealedFrames;
                artifactConcealed = true;
                ++dmrArtifactConcealCount;
                dmrVocoder.reset();
                dmrPayloadProbeVocoder.reset();
                dmrRawProbeVocoder.reset();
                dmrCanonicalProbeVocoder.reset();
            }

            const int dmrAudioLogIndex = dmrAudioLogCounter++;
            const bool verboseLogging = fobosVerboseLoggingEnabled();
            bool shouldLogDmrAudio =
                verboseLogging && (dmrAudioLogIndex < 12 || (dmrAudioLogIndex % 50) == 0);
            const bool weakSoftAudio =
                audioSource == QStringLiteral("softFrames") &&
                !voicePcm.isEmpty() &&
                voiceStats.peak < 0.080 &&
                voiceStats.rms < 0.020;
            const bool runVoiceProbe =
                verboseLogging &&
                (shouldLogDmrAudio ||
                 resetRetryAttempted ||
                 (weakSoftAudio &&
                  (result.voiceAudioTrusted || result.voiceAudioConfidence >= 30)));
            DmrVoiceProbeStats payloadProbe;
            DmrVoiceProbeStats rawProbe;
            DmrVoiceProbeStats canonicalProbe;
            if (runVoiceProbe) {
                shouldLogDmrAudio = true;
                if (!result.ambePayloads.empty()) {
                    payloadProbe = dmrProbeFromCandidate(payloadCandidate);
                }
                if (!result.ambeFrames.empty()) {
                    rawProbe = dmrProbeFromCandidate(rawCandidate);
                }
                if (!result.ambePayloads.empty()) {
                    canonicalProbe = dmrProbeFromCandidate(canonicalCandidate);
                }
            }
            if (shouldLogDmrAudio) {
                qDebug() << "[DMR audio]"
                         << "source" << (audioSource.isEmpty() ? QStringLiteral("none") : audioSource)
                         << "frames" << static_cast<int>(result.ambeFrames.size())
                         << "softFrames" << static_cast<int>(result.ambeSoftFrames.size())
                         << "payloads" << static_cast<int>(result.ambePayloads.size())
                         << "qualityPayloads" << static_cast<int>(qualityPayloadIndexes.size())
                         << "filteredPayloads" << filteredPayloads
                         << "concealedPayloads" << concealedPayloads
                         << "rejectedVocoderFrames" << rejectedVocoderFrames
                         << "trusted" << result.voiceAudioTrusted
                         << "provisional" << provisionalVoiceAudio
                         << "confidence" << result.voiceAudioConfidence
                         << "rawGate" << maxRawPayloadErrors
                         << "softErr" << softFrameErrorsText
                         << "selector" << (selectorSwitch.isEmpty()
                                                ? QStringLiteral("keep")
                                                : selectorSwitch)
                         << "pcmBytes" << voicePcm.size()
                         << "voicePk" << QString::number(voiceStats.peak, 'f', 3)
                         << "voiceRms" << QString::number(voiceStats.rms, 'f', 3)
                         << "voiceClip" << QString::number(voiceStats.clippedPercent, 'f', 3)
                         << "collapse" << dmrCollapsedVoiceCount
                         << "resetRetry" << resetRetryAttempted
                         << "retryUsed" << resetRetryUsed
                         << "retryPk" << QString::number(retryStats.peak, 'f', 3)
                         << "retryRms" << QString::number(retryStats.rms, 'f', 3)
                         << "resetRecoveries" << dmrVocoderResetRecoveryCount
                         << "artifactGate" << artifactConcealed
                         << "artifactPk" << QString::number(artifactStats.peak, 'f', 3)
                         << "artifactRms" << QString::number(artifactStats.rms, 'f', 3)
                         << "artifactErrors" << artifactVocoderErrors
                         << "artifactConceals" << dmrArtifactConcealCount
                         << "probePayload" << dmrVoiceProbeSummary(payloadProbe)
                         << "probeRaw" << dmrVoiceProbeSummary(rawProbe)
                         << "probeCanonical" << dmrVoiceProbeSummary(canonicalProbe)
                         << "decoded" << decodedVoiceFrames
                         << "vocoderErrors" << vocoderErrors;
                if (dmrPrivacy.isActive()) {
                    const auto &privacy = dmrPrivacy.statistics();
                    qDebug() << "[DMR native privacy] mode" << dmrPrivacyModeId(settings.dmrPrivacyMode)
                             << "keyBits" << dmrPrivacy.keyBits()
                             << "applied" << privacy.applied << "silence" << privacy.silence
                             << "unaligned" << privacy.unaligned
                             << "invalidKey" << privacy.invalidKey
                             << "invalidPayload" << privacy.invalidPayload
                             << "missingMi" << privacy.missingMi << "keyMismatch" << privacy.keyMismatch
                             << "piAccepted" << privacy.piAccepted << "piRejected" << privacy.piRejected
                             << "lateEntryAccepted" << privacy.lateEntryAccepted
                             << "lateEntryRejected" << privacy.lateEntryRejected
                             << "clearFrames" << privacy.clearFrames;
                }
            }
            if (decodedVoiceFrames > 0) {
                dmrVocoderFrameCount += decodedVoiceFrames;
                dmrVocoderErrorCount += vocoderErrors;
            }
            if (!voicePcm.isEmpty() && dmrVoiceOutputEnabled) {
                queueDmrVoicePcm(voicePcm);
            }
            if (previousPayloadCount == 0 ||
                previousPayloadCount / 18 != dmrAmbePayloadCount / 18 ||
                previousFrameCount / 18 != dmrAmbeFrameCount / 18) {
                const QString vocoderText = dmrVocoder.isAvailable()
                                                ? QStringLiteral("%1 PCM frames, %2 vocoder errors")
                                                      .arg(dmrVocoderFrameCount)
                                                      .arg(dmrVocoderErrorCount)
                                                : QStringLiteral("vocoder unavailable");
                if (result.serviceStatusText.isEmpty()) {
                    updateStatus(QStringLiteral("DMR voice: %1 AMBE frames, %2 FEC payloads, %3 Golay corrections, %4")
                                     .arg(dmrAmbeFrameCount)
                                     .arg(dmrAmbePayloadCount)
                                     .arg(dmrAmbeFecCorrectionCount)
                                     .arg(vocoderText));
                } else {
                    updateStatus(QStringLiteral("%1, %2").arg(result.serviceStatusText, vocoderText));
                }
            }
        }
        if (!result.decodedText.isEmpty()) {
            emit textDecoded(result.decodedText);
        }
        if (result.lockLost) {
            dmrPrivacy.reset();
            dmrVocoder.reset();
            dmrPayloadProbeVocoder.reset();
            dmrRawProbeVocoder.reset();
            dmrCanonicalProbeVocoder.reset();
            dmrCollapsedVoiceCount = 0;
            dmrArtifactConcealCount = 0;
            clearDmrVoicePcmBuffer();
        }
        return;
    }

    if (settings.modulationType == MOD_FT8) {
        processFt8Samples(raw, sampleCount, sampleRate, decodedText);
        if (!decodedText.isEmpty()) {
            emit textDecoded(decodedText);
        }
        return;
    }

    for (int i = 0; i < sampleCount; ++i) {
        const float sample = readPcm16Le(raw + i * static_cast<int>(sizeof(qint16))) / 32768.0f;
        bool markBit = true;
        bool signalPresent = true;

        if (settings.modulationType == MOD_FSK) {
            markBit = detectFskBit(sample, signalPresent);
        } else {
            markBit = detectAfskBit(sample, signalPresent);
        }

        if (!signalPresent) {
            ++rttyNoSignalSamples;
            if (rttyNoSignalSamples > static_cast<int>(samplesPerBit * 3.0)) {
                rttyState = RttyState::Idle;
                previousRttyBit = true;
            }
            continue;
        }
        rttyNoSignalSamples = 0;

        if (rttyInvertPolarity) {
            markBit = !markBit;
        }
        advanceRttyState(markBit, decodedText);
    }

    if (!decodedText.isEmpty()) {
        emit textDecoded(decodedText);
    }
}

void DigitalDecoder::configureForMode(int modulationType, int sampleRate) {
    if (sampleRate <= 0) {
        sampleRate = 48000;
    }

    if (activeMode == modulationType && activeSampleRate == sampleRate) {
        return;
    }

    activeMode = modulationType;
    activeSampleRate = sampleRate;
    samplesPerBit = activeSampleRate / RTTY_BAUD;
    detectorAlpha = 1.0 - std::exp(-TWO_PI * RTTY_DETECTOR_BANDWIDTH_HZ / activeSampleRate);
    detectorAlpha = std::clamp(detectorAlpha, 0.0001, 1.0);
    reset();

    if (!decoderEnabled) {
        updateStatus(QStringLiteral("Digital audio decoder disabled"));
        return;
    }

    if (modulationType == MOD_CW) {
        updateStatus(QStringLiteral("CW decoder: waiting for %1 Hz tone")
                         .arg(currentSettings.cwDecoderToneHz, 0, 'f', 0));
    } else if (modulationType == MOD_RTTY) {
        updateStatus(QStringLiteral("RTTY decoder: Baudot 45.45 baud, 170 Hz shift, mark 2125 Hz"));
    } else if (modulationType == MOD_FSK) {
        updateStatus(QStringLiteral("FSK decoder: Baudot 45.45 baud from discriminator"));
    } else if (modulationType == MOD_FT8) {
        updateStatus(QStringLiteral("FT8 detector: waiting for a 15 s frame"));
    } else if (modulationType == MOD_DMR) {
        const double samplesPerSymbol = static_cast<double>(activeSampleRate) / 4800.0;
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DMR decoder] configured"
                     << "sampleRate" << activeSampleRate
                     << "samplesPerSymbol" << samplesPerSymbol
                     << "vocoderAvailable" << dmrVocoder.isAvailable();
        }
        updateStatus(dmrVocoder.isAvailable()
                         ? QStringLiteral("DMR monitor: %1 kHz 4FSK, %2 sps, AMBE vocoder ready")
                               .arg(activeSampleRate / 1000)
                               .arg(samplesPerSymbol, 0, 'f', 1)
                         : QStringLiteral("DMR monitor: %1 kHz 4FSK, %2 sps, AMBE vocoder unavailable")
                               .arg(activeSampleRate / 1000)
                               .arg(samplesPerSymbol, 0, 'f', 1));
    } else if (modulationType == MOD_PSK) {
        updateStatus(QStringLiteral("BPSK31 decoder: waiting for an audio carrier"));
    } else if (isDigitalMode(modulationType)) {
        updateStatus(QStringLiteral("Digital audio decoder waiting for supported mode"));
    } else {
        updateStatus(QStringLiteral("Digital audio decoder idle"));
    }
}

void DigitalDecoder::resetRttyState() {
    rttyState = RttyState::Idle;
    bitCountdown = 0.0;
    rttyCode = 0;
    rttyBitIndex = 0;
    previousRttyBit = true;
    rttyLettersShift = true;
    rttyLastWasNewline = false;
    rttyBadStopCount = 0;
    rttyDecodedCount = 0;
    rttyNoSignalSamples = 0;
    fskLevel = 0.0;
    signalQuality = 0.0;
}

void DigitalDecoder::resetFt8State() {
    ft8AudioBuffer.clear();
    ft8SamplesSinceAnalysis = 0;
    ft8AnalysisCounter = 0;
    ft8LastAnalyzedSlot = -1;
    recentFt8CandidateHz.clear();
    recentFt8CandidateAnalysis.clear();
}

void DigitalDecoder::processFt8Samples(const char *raw, int sampleCount, int sampleRate, QString &decodedText) {
    if (!raw || sampleCount <= 0 || sampleRate <= 0) {
        return;
    }

    const int maxSamples = static_cast<int>(std::lround(sampleRate * FT8_BUFFER_SECONDS));
    ft8AudioBuffer.reserve(static_cast<std::size_t>(maxSamples));
    for (int i = 0; i < sampleCount; ++i) {
        ft8AudioBuffer.push_back(readPcm16Le(raw + i * static_cast<int>(sizeof(qint16))) / 32768.0f);
    }
    if (static_cast<int>(ft8AudioBuffer.size()) > maxSamples) {
        const int dropCount = static_cast<int>(ft8AudioBuffer.size()) - maxSamples;
        ft8AudioBuffer.erase(ft8AudioBuffer.begin(),
                             ft8AudioBuffer.begin() + static_cast<std::ptrdiff_t>(dropCount));
    }

    ft8SamplesSinceAnalysis += sampleCount;
    const int minSamples = static_cast<int>(std::lround(sampleRate * FT8_MIN_BUFFER_SECONDS));
    if (static_cast<int>(ft8AudioBuffer.size()) < minSamples) {
        updateStatus(QStringLiteral("FT8 detector: buffering audio"));
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 slotMs = static_cast<qint64>(std::lround(FT8_SLOT_SECONDS * 1000.0));
    const int decodeDelayMs = static_cast<int>(std::lround(FT8_SLOT_DECODE_DELAY_SECONDS * 1000.0));
    const int decodeWindowMs = static_cast<int>(std::lround(FT8_SLOT_DECODE_WINDOW_SECONDS * 1000.0));
    if (slotMs <= 0) {
        return;
    }

    const qint64 slotIndex = nowMs / slotMs;
    const int slotPhaseMs = static_cast<int>(nowMs % slotMs);
    if (slotPhaseMs < decodeDelayMs || slotPhaseMs > decodeWindowMs) {
        updateStatus(QStringLiteral("FT8 detector: waiting for next 15 s slot"));
        return;
    }

    const qint64 decodeSlot = slotIndex - 1;
    if (decodeSlot <= ft8LastAnalyzedSlot) {
        return;
    }

    ft8SamplesSinceAnalysis = 0;
    ft8LastAnalyzedSlot = decodeSlot;
    ++ft8AnalysisCounter;
    const qint64 decodeSlotStartMs = decodeSlot * slotMs;
    const qint64 bufferStartMs = nowMs - static_cast<qint64>(
        std::lround(static_cast<double>(ft8AudioBuffer.size()) * 1000.0 / sampleRate));
    qDebug() << "[FT8] slot analysis"
             << "slot" << decodeSlot
             << "phaseMs" << slotPhaseMs
             << "bufferSeconds" << (static_cast<double>(ft8AudioBuffer.size()) / sampleRate);
    analyzeFt8Buffer(decodedText, decodeSlotStartMs, bufferStartMs);
}

void DigitalDecoder::analyzeFt8Buffer(QString &decodedText, qint64 decodeSlotStartMs, qint64 bufferStartMs) {
    if (activeSampleRate <= 0 || ft8AudioBuffer.empty()) {
        return;
    }

    const int decimation = (std::max)(1, static_cast<int>(std::lround(activeSampleRate / 12000.0)));
    const int analysisRate = activeSampleRate / decimation;
    if (analysisRate <= 0) {
        return;
    }

    std::vector<float> audio;
    audio.reserve(ft8AudioBuffer.size() / static_cast<std::size_t>(decimation) + 1);
    for (std::size_t i = 0; i + static_cast<std::size_t>(decimation) <= ft8AudioBuffer.size();
         i += static_cast<std::size_t>(decimation)) {
        double sum = 0.0;
        for (int j = 0; j < decimation; ++j) {
            sum += ft8AudioBuffer[i + static_cast<std::size_t>(j)];
        }
        audio.push_back(static_cast<float>(sum / decimation));
    }

    if (audio.empty()) {
        return;
    }
    double mean = 0.0;
    for (float sample : audio) {
        mean += sample;
    }
    mean /= static_cast<double>(audio.size());
    for (float &sample : audio) {
        sample = static_cast<float>(sample - mean);
    }

    const int symbolSamples = static_cast<int>(std::lround(FT8_SYMBOL_SECONDS * analysisRate));
    const int frameSamples = symbolSamples * FT8_SYMBOL_COUNT;
    if (symbolSamples <= 0 || static_cast<int>(audio.size()) < frameSamples) {
        updateStatus(QStringLiteral("FT8 detector: buffering audio"));
        return;
    }

    const int fftSize = symbolSamples;
    const double binHz = static_cast<double>(analysisRate) / fftSize;
    const int minBin = (std::max)(1, static_cast<int>(std::floor(FT8_MIN_AUDIO_HZ / binHz)));
    const int maxBaseBin = (std::min)(fftSize / 2 - 8,
                                      static_cast<int>(std::ceil((FT8_MAX_AUDIO_HZ - 7.0 * FT8_TONE_SPACING_HZ) / binHz)));
    if (maxBaseBin <= minBin) {
        return;
    }

    const std::array<int, 7> costas = {{3, 1, 4, 0, 6, 5, 2}};
    const std::array<int, 21> syncSymbols = {{
        0, 1, 2, 3, 4, 5, 6,
        36, 37, 38, 39, 40, 41, 42,
        72, 73, 74, 75, 76, 77, 78
    }};

    std::vector<float> window(static_cast<std::size_t>(symbolSamples));
    for (int i = 0; i < symbolSamples; ++i) {
        window[static_cast<std::size_t>(i)] = 1.0f;
    }

    std::vector<float> fftInput(static_cast<std::size_t>(fftSize), 0.0f);
    std::vector<fftwf_complex> fftOutput(static_cast<std::size_t>(fftSize / 2 + 1));
    fftwf_plan plan = fftwf_plan_dft_r2c_1d(fftSize, fftInput.data(), fftOutput.data(), FFTW_ESTIMATE);
    if (!plan) {
        return;
    }
    FftwPlanScope planScope(plan);

    std::vector<Ft8Candidate> rawCandidates;
    const int maxStart = static_cast<int>(audio.size()) - frameSamples;
    const int startStep = (std::max)(1, symbolSamples);
    const int toneBinStep = (std::max)(1, static_cast<int>(std::lround(FT8_TONE_SPACING_HZ / binHz)));

    for (int start = 0; start <= maxStart; start += startStep) {
        std::vector<std::vector<float>> syncMagnitudes(syncSymbols.size());
        for (std::size_t syncIndex = 0; syncIndex < syncSymbols.size(); ++syncIndex) {
            const int symbol = syncSymbols[syncIndex];
            const int symbolStart = start + symbol * symbolSamples;
            std::fill(fftInput.begin(), fftInput.end(), 0.0f);
            for (int i = 0; i < symbolSamples; ++i) {
                fftInput[static_cast<std::size_t>(i)] =
                    audio[static_cast<std::size_t>(symbolStart + i)] * window[static_cast<std::size_t>(i)];
            }
            fftwf_execute(plan);

            syncMagnitudes[syncIndex].resize(static_cast<std::size_t>(fftSize / 2 + 1), 0.0f);
            for (int bin = minBin; bin <= fftSize / 2; ++bin) {
                const float re = fftOutput[static_cast<std::size_t>(bin)][0];
                const float im = fftOutput[static_cast<std::size_t>(bin)][1];
                syncMagnitudes[syncIndex][static_cast<std::size_t>(bin)] = re * re + im * im;
            }
        }

        for (int baseBin = minBin; baseBin <= maxBaseBin; ++baseBin) {
            double signal = 0.0;
            double alternatives = 0.0;
            int hits = 0;

            for (std::size_t syncIndex = 0; syncIndex < syncSymbols.size(); ++syncIndex) {
                const int expectedTone = costas[syncIndex % costas.size()];
                const std::vector<float> &magnitudes = syncMagnitudes[syncIndex];
                const int expectedBin = baseBin + expectedTone * toneBinStep;
                if (expectedBin < 1 || expectedBin >= static_cast<int>(magnitudes.size())) {
                    continue;
                }

                const double expectedPower = magnitudes[static_cast<std::size_t>(expectedBin)];
                double otherPower = 0.0;
                int otherCount = 0;
                for (int tone = 0; tone < 8; ++tone) {
                    if (tone == expectedTone) {
                        continue;
                    }
                    const int toneBin = baseBin + tone * toneBinStep;
                    if (toneBin < 1 || toneBin >= static_cast<int>(magnitudes.size())) {
                        continue;
                    }
                    otherPower += magnitudes[static_cast<std::size_t>(toneBin)];
                    ++otherCount;
                }
                const double averageOther = otherCount > 0 ? otherPower / otherCount : 1e-12;
                signal += expectedPower;
                alternatives += averageOther;
                if (expectedPower > averageOther * 1.6) {
                    ++hits;
                }
            }

            const double ratio = signal / (alternatives + 1e-12);
            const double syncDb = 10.0 * std::log10((std::max)(ratio, 1e-12));
            if (syncDb >= FT8_MIN_SYNC_DB && hits >= FT8_MIN_SYNC_HITS) {
                Ft8Candidate candidate;
                candidate.valid = true;
                candidate.audioHz = baseBin * binHz;
                candidate.startOffsetSeconds = static_cast<double>(start) / analysisRate;
                candidate.syncDb = syncDb;
                candidate.hits = hits;
                rawCandidates.push_back(candidate);
            }
        }
    }

    if (rawCandidates.empty()) {
        updateStatus(QStringLiteral("FT8 detector: no sync candidate"));
        return;
    }

    qDebug() << "[FT8] analysis candidates"
             << "raw" << rawCandidates.size()
             << "sampleRate" << activeSampleRate
             << "analysisRate" << analysisRate
             << "fftSize" << fftSize;

    std::sort(rawCandidates.begin(), rawCandidates.end(), [](const Ft8Candidate &left, const Ft8Candidate &right) {
        if (left.hits != right.hits) {
            return left.hits > right.hits;
        }
        return left.syncDb > right.syncDb;
    });

    std::vector<Ft8Candidate> clustered;
    for (const Ft8Candidate &candidate : rawCandidates) {
        const double candidateCenterHz = candidate.audioHz + 3.5 * FT8_TONE_SPACING_HZ;
        bool duplicate = false;
        for (const Ft8Candidate &existing : clustered) {
            const double existingCenterHz = existing.audioHz + 3.5 * FT8_TONE_SPACING_HZ;
            if (std::abs(candidateCenterHz - existingCenterHz) <= FT8_CANDIDATE_CLUSTER_HZ &&
                std::abs(candidate.startOffsetSeconds - existing.startOffsetSeconds) <= FT8_CANDIDATE_CLUSTER_SECONDS) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        clustered.push_back(candidate);
        if (static_cast<int>(clustered.size()) >= FT8_MAX_REPORTED_CANDIDATES) {
            break;
        }
    }

    if (clustered.empty()) {
        updateStatus(QStringLiteral("FT8 detector: no sync candidate"));
        return;
    }

    auto scoreCandidateSync = [&](int start,
                                  int baseBin,
                                  double &syncDb,
                                  int &hits,
                                  double &fractionalBinOffset) {
        double signal = 0.0;
        double alternatives = 0.0;
        double weightedOffset = 0.0;
        double offsetWeight = 0.0;
        hits = 0;
        fractionalBinOffset = 0.0;
        int validSymbols = 0;

        for (std::size_t syncIndex = 0; syncIndex < syncSymbols.size(); ++syncIndex) {
            const int symbol = syncSymbols[syncIndex];
            const int symbolStart = start + symbol * symbolSamples;
            if (symbolStart < 0 || symbolStart + symbolSamples > static_cast<int>(audio.size())) {
                continue;
            }

            std::fill(fftInput.begin(), fftInput.end(), 0.0f);
            for (int i = 0; i < symbolSamples; ++i) {
                fftInput[static_cast<std::size_t>(i)] =
                    audio[static_cast<std::size_t>(symbolStart + i)] * window[static_cast<std::size_t>(i)];
            }
            fftwf_execute(plan);

            const int expectedTone = costas[syncIndex % costas.size()];
            const int expectedBin = baseBin + expectedTone * toneBinStep;
            if (expectedBin < 2 || expectedBin + 1 >= static_cast<int>(fftOutput.size())) {
                continue;
            }

            const auto binPower = [&](int bin) {
                const float re = fftOutput[static_cast<std::size_t>(bin)][0];
                const float im = fftOutput[static_cast<std::size_t>(bin)][1];
                return static_cast<double>(re) * re + static_cast<double>(im) * im;
            };

            const double expectedPower = binPower(expectedBin);
            const double leftPower = binPower(expectedBin - 1);
            const double rightPower = binPower(expectedBin + 1);
            double otherPower = 0.0;
            int otherCount = 0;
            for (int tone = 0; tone < 8; ++tone) {
                if (tone == expectedTone) {
                    continue;
                }
                const int toneBin = baseBin + tone * toneBinStep;
                if (toneBin < 1 || toneBin >= static_cast<int>(fftOutput.size())) {
                    continue;
                }
                otherPower += binPower(toneBin);
                ++otherCount;
            }

            const double averageOther = otherCount > 0 ? otherPower / otherCount : 1e-12;
            signal += expectedPower;
            alternatives += averageOther;
            if (expectedPower > averageOther * 1.6) {
                ++hits;
            }
            weightedOffset += ft8ParabolicPeakOffset(leftPower, expectedPower, rightPower) *
                              (std::max)(expectedPower, 1e-20);
            offsetWeight += (std::max)(expectedPower, 1e-20);
            ++validSymbols;
        }

        if (validSymbols < 15) {
            syncDb = -120.0;
            return -120.0;
        }

        const double ratio = signal / (alternatives + 1e-12);
        syncDb = 10.0 * std::log10((std::max)(ratio, 1e-12));
        if (offsetWeight > 0.0) {
            fractionalBinOffset = weightedOffset / offsetWeight;
        }
        return syncDb + hits * 0.25;
    };

    auto demodulateCandidateSymbols = [&](Ft8Candidate &candidate) {
        const int initialStart = static_cast<int>(std::lround(candidate.startOffsetSeconds * analysisRate));
        const int baseBin = static_cast<int>(std::lround(candidate.audioHz / binHz));
        const int refineHalfWindow = (std::max)(1, symbolSamples / 2);
        const int refineStep = (std::max)(1, symbolSamples / 16);
        int bestStart = initialStart;
        double bestSyncDb = candidate.syncDb;
        int bestHits = candidate.hits;
        double bestFractionalBinOffset = 0.0;
        double bestSyncScore = -120.0;

        for (int offset = -refineHalfWindow; offset <= refineHalfWindow; offset += refineStep) {
            const int trialStart = initialStart + offset;
            double trialSyncDb = -120.0;
            int trialHits = 0;
            double trialFractionalBinOffset = 0.0;
            const double trialScore = scoreCandidateSync(trialStart,
                                                         baseBin,
                                                         trialSyncDb,
                                                         trialHits,
                                                         trialFractionalBinOffset);
            if (trialScore > bestSyncScore) {
                bestSyncScore = trialScore;
                bestStart = trialStart;
                bestSyncDb = trialSyncDb;
                bestHits = trialHits;
                bestFractionalBinOffset = trialFractionalBinOffset;
            }
        }

        candidate.startOffsetSeconds = static_cast<double>(bestStart) / analysisRate;
        candidate.syncDb = bestSyncDb;
        candidate.hits = bestHits;
        candidate.audioHz = (std::clamp)(baseBin * binHz + bestFractionalBinOffset * binHz,
                                         FT8_MIN_AUDIO_HZ,
                                         FT8_MAX_AUDIO_HZ - 7.0 * FT8_TONE_SPACING_HZ);
        const int start = bestStart;
        const double baseHz = candidate.audioHz;
        int dataIndex = 0;
        int decodedCostasHits = 0;
        double reliabilitySumDb = 0.0;
        int reliabilityCount = 0;
        std::vector<std::array<std::complex<double>, 8>> dataToneSymbols;
        dataToneSymbols.reserve(FT8_DATA_SYMBOL_COUNT);

        for (int symbol = 0; symbol < FT8_SYMBOL_COUNT; ++symbol) {
            const int symbolStart = start + symbol * symbolSamples;
            if (symbolStart < 0 || symbolStart + symbolSamples > static_cast<int>(audio.size())) {
                continue;
            }

            int bestTone = 0;
            double bestPower = -1.0;
            double secondPower = -1.0;
            const std::array<std::complex<double>, 8> toneSymbols = ft8ToneComplexAt(audio,
                                                                                     symbolStart,
                                                                                     symbolSamples,
                                                                                     analysisRate,
                                                                                     baseHz);
            for (int tone = 0; tone < 8; ++tone) {
                const double metric = std::abs(toneSymbols[static_cast<std::size_t>(tone)]);
                const double power = metric * metric;
                if (power > bestPower) {
                    secondPower = bestPower;
                    bestPower = power;
                    bestTone = tone;
                } else if (power > secondPower) {
                    secondPower = power;
                }
            }

            const double reliabilityDb = 10.0 * std::log10((std::max)(bestPower, 1e-20) /
                                                           (std::max)(secondPower, 1e-20));
            if (std::isfinite(reliabilityDb)) {
                reliabilitySumDb += reliabilityDb;
                ++reliabilityCount;
            }

            const int expectedCostasTone = ft8CostasToneForSymbol(symbol);
            if (expectedCostasTone >= 0) {
                if (bestTone == expectedCostasTone) {
                    ++decodedCostasHits;
                }
                continue;
            }

            if (dataIndex < FT8_DATA_SYMBOL_COUNT) {
                candidate.dataSymbols[static_cast<std::size_t>(dataIndex)] = static_cast<unsigned char>(bestTone);
                dataToneSymbols.push_back(toneSymbols);
                ++dataIndex;
            }
        }

        candidate.decodedCostasHits = decodedCostasHits;
        candidate.symbolReliabilityDb = reliabilityCount > 0 ? reliabilitySumDb / reliabilityCount : -120.0;
        candidate.hasDataSymbols = dataIndex == FT8_DATA_SYMBOL_COUNT;
        if (candidate.hasDataSymbols) {
            int bestChecks = FT8_LDPC_CHECK_COUNT;
            int bestIterations = 0;
            int bestHardErrors = -1;
            int bestPass = 0;
            double bestBitMargin = 0.0;
            std::array<float, FT8_CODE_BITS> bestSoftBits = {};

            for (int pass : {1, 2, 3}) {
                std::array<float, FT8_CODE_BITS> passSoftBits = buildFt8SoftBits(dataToneSymbols, pass);
                double bitMarginSum = 0.0;
                for (float value : passSoftBits) {
                    bitMarginSum += std::fabs(value);
                }
                const double bitMargin = bitMarginSum / FT8_CODE_BITS;

                std::array<unsigned char, FT8_PAYLOAD_BITS> passPayload = {};
                int passIterations = 0;
                int passChecks = FT8_LDPC_CHECK_COUNT;
                int passHardErrors = -1;
                const bool passDecoded = decodeFt8Ldpc(passSoftBits,
                                                       passPayload,
                                                       passIterations,
                                                       passChecks,
                                                       passHardErrors);
                if (passDecoded || passChecks < bestChecks || bestPass == 0) {
                    bestChecks = passChecks;
                    bestIterations = passIterations;
                    bestHardErrors = passHardErrors;
                    bestPass = pass;
                    bestBitMargin = bitMargin;
                    bestSoftBits = passSoftBits;
                    candidate.payloadBits = passPayload;
                    candidate.ldpcDecoded = passDecoded;
                }
                if (passDecoded) {
                    break;
                }
            }

            candidate.softBits = bestSoftBits;
            candidate.softBitMarginDb = bestBitMargin;
            candidate.ldpcIterations = bestIterations;
            candidate.ldpcUnsatisfiedChecks = bestChecks;
            candidate.ldpcHardErrors = bestHardErrors;
            candidate.ldpcPass = bestPass;
            if (candidate.ldpcDecoded) {
                bool unpackOk = false;
                candidate.decodedMessage = ft8UnpackPayload(candidate.payloadBits, unpackOk);
                if (!unpackOk) {
                    candidate.decodedMessage.clear();
                }
            }
        }
    };

    for (Ft8Candidate &candidate : clustered) {
        demodulateCandidateSymbols(candidate);
    }

    clustered.erase(std::remove_if(clustered.begin(),
                                   clustered.end(),
                                   [](const Ft8Candidate &candidate) {
                                       return candidate.decodedCostasHits < 11 ||
                                              !candidate.hasDataSymbols;
                                   }),
                    clustered.end());

    if (clustered.empty()) {
        updateStatus(QStringLiteral("FT8 detector: sync rejected by symbol demodulator"));
        return;
    }

    qDebug() << "[FT8] symbol demod candidates"
             << "accepted" << clustered.size()
             << "bestHz" << (clustered.front().audioHz + 3.5 * FT8_TONE_SPACING_HZ)
             << "costas" << clustered.front().decodedCostasHits
             << "relDb" << clustered.front().symbolReliabilityDb;

    std::sort(clustered.begin(), clustered.end(), [](const Ft8Candidate &left, const Ft8Candidate &right) {
        if (left.ldpcDecoded != right.ldpcDecoded) {
            return left.ldpcDecoded;
        }
        if (left.decodedCostasHits != right.decodedCostasHits) {
            return left.decodedCostasHits > right.decodedCostasHits;
        }
        if (std::abs(left.symbolReliabilityDb - right.symbolReliabilityDb) > 0.1) {
            return left.symbolReliabilityDb > right.symbolReliabilityDb;
        }
        if (std::abs(left.softBitMarginDb - right.softBitMarginDb) > 0.1) {
            return left.softBitMarginDb > right.softBitMarginDb;
        }
        return left.syncDb > right.syncDb;
    });

    const Ft8Candidate &best = clustered.front();
    const double bestCenterHz = best.audioHz + 3.5 * FT8_TONE_SPACING_HZ;
    const int decodedCount = static_cast<int>(std::count_if(clustered.begin(),
                                                            clustered.end(),
                                                            [](const Ft8Candidate &candidate) {
                                                                return candidate.ldpcDecoded;
                                                            }));
    updateStatus(QStringLiteral("FT8 detector: %1 candidates, %2 CRC OK, best %3 Hz, sync %4 dB, Costas %5/21, rel %6 dB, soft %7 dB")
                     .arg(clustered.size())
                     .arg(decodedCount)
                     .arg(bestCenterHz, 0, 'f', 0)
                     .arg(best.syncDb, 0, 'f', 1)
                     .arg(best.decodedCostasHits)
                     .arg(best.symbolReliabilityDb, 0, 'f', 1)
                     .arg(best.softBitMarginDb, 0, 'f', 1));

    for (int i = static_cast<int>(recentFt8CandidateAnalysis.size()) - 1; i >= 0; --i) {
        if (ft8AnalysisCounter - recentFt8CandidateAnalysis[static_cast<std::size_t>(i)] >
            FT8_CANDIDATE_REPEAT_ANALYSES * 3) {
            recentFt8CandidateAnalysis.erase(recentFt8CandidateAnalysis.begin() + i);
            recentFt8CandidateHz.erase(recentFt8CandidateHz.begin() + i);
        }
    }

    QString report;
    int reportedCount = 0;
    const bool reportOnlyDecoded = decodedCount > 0;
    const QString decodeTimeText =
        QDateTime::fromMSecsSinceEpoch(decodeSlotStartMs, Qt::UTC).toString(QStringLiteral("HH:mm:ss"));
    for (const Ft8Candidate &candidate : clustered) {
        const double signalCenterHz = candidate.audioHz + 3.5 * FT8_TONE_SPACING_HZ;
        const double dtSeconds =
            (static_cast<double>(bufferStartMs - decodeSlotStartMs) / 1000.0) +
            candidate.startOffsetSeconds;
        const double rfFrequencyHz =
            currentSettings.listeningFrequency > 0.0
                ? currentSettings.listeningFrequency + signalCenterHz
                : 0.0;
        const QString rfText = ft8FrequencyText(rfFrequencyHz);
        bool recent = false;
        for (std::size_t i = 0; i < recentFt8CandidateHz.size(); ++i) {
            if (std::abs(recentFt8CandidateHz[i] - signalCenterHz) <= FT8_CANDIDATE_CLUSTER_HZ &&
                ft8AnalysisCounter - recentFt8CandidateAnalysis[i] <= FT8_CANDIDATE_REPEAT_ANALYSES) {
                recentFt8CandidateAnalysis[i] = ft8AnalysisCounter;
                recent = true;
                break;
            }
        }
        if (recent) {
            continue;
        }

        recentFt8CandidateHz.push_back(signalCenterHz);
        recentFt8CandidateAnalysis.push_back(ft8AnalysisCounter);
        const QString tones = ft8ToneString(candidate.dataSymbols);
        const QString payloadBits = candidate.ldpcDecoded ? ft8PayloadBitString(candidate.payloadBits) : QString();
        qDebug() << "[FT8] payload symbols"
                 << "audioHz" << signalCenterHz
                 << "rfHz" << rfFrequencyHz
                 << "start" << candidate.startOffsetSeconds
                 << "dt" << dtSeconds
                 << "costas" << candidate.decodedCostasHits
                 << "relDb" << candidate.symbolReliabilityDb
                 << "softDb" << candidate.softBitMarginDb
                 << "ldpc" << candidate.ldpcDecoded
                 << "pass" << candidate.ldpcPass
                 << "iter" << candidate.ldpcIterations
                 << "checks" << candidate.ldpcUnsatisfiedChecks
                 << "hardErrors" << candidate.ldpcHardErrors
                 << "tones" << tones;
        if (reportOnlyDecoded && !candidate.ldpcDecoded) {
            continue;
        }
        if (!reportOnlyDecoded &&
            (candidate.syncDb < FT8_PANEL_MIN_CANDIDATE_SYNC_DB ||
             candidate.decodedCostasHits < FT8_PANEL_MIN_CANDIDATE_COSTAS)) {
            continue;
        }
        ++reportedCount;
        if (candidate.ldpcDecoded) {
            qDebug() << "[FT8] LDPC CRC OK"
                     << "audioHz" << signalCenterHz
                     << "rfHz" << rfFrequencyHz
                     << "dt" << dtSeconds
                     << "message" << candidate.decodedMessage
                     << "payload" << payloadBits;
            if (!candidate.decodedMessage.isEmpty()) {
                // FT8 reports S/N in a 2500 Hz reference bandwidth. syncDb is
                // measured in one 6.25 Hz tone bin, so this is explicitly an
                // estimate until the decoder exposes calibrated noise power.
                const int snrEstimate = (std::clamp)(
                    static_cast<int>(std::lround(candidate.syncDb - 26.0)), -50, 49);
                emit ft8Decoded(QDateTime::fromMSecsSinceEpoch(decodeSlotStartMs, Qt::UTC),
                                rfFrequencyHz,
                                signalCenterHz,
                                dtSeconds,
                                snrEstimate,
                                candidate.syncDb,
                                candidate.decodedCostasHits,
                                candidate.decodedMessage);
                report += QStringLiteral("  %1  %2%3  (+%4 Hz, DT %5, sync %6 dB, Costas %7/21, pass %8)\n")
                              .arg(decodeTimeText)
                              .arg(rfText.isEmpty() ? QString() : rfText + QStringLiteral("  "))
                              .arg(candidate.decodedMessage)
                              .arg(signalCenterHz, 0, 'f', 0)
                              .arg(ft8SignedSecondsText(dtSeconds))
                              .arg(candidate.syncDb, 0, 'f', 1)
                              .arg(candidate.decodedCostasHits)
                              .arg(candidate.ldpcPass);
            } else {
                report += QStringLiteral("  %1  %2audio +%3 Hz, DT %4, sync %5 dB, Costas %6/21, CRC OK, pass %7, bits %8\n")
                              .arg(decodeTimeText)
                              .arg(rfText.isEmpty() ? QString() : rfText + QStringLiteral("  "))
                              .arg(signalCenterHz, 0, 'f', 0)
                              .arg(ft8SignedSecondsText(dtSeconds))
                              .arg(candidate.syncDb, 0, 'f', 1)
                              .arg(candidate.decodedCostasHits)
                              .arg(candidate.ldpcPass)
                              .arg(payloadBits);
            }
        } else {
            report += QStringLiteral("  %1  %2audio +%3 Hz, DT %4, sync %5 dB, Costas %6/21, rel %7 dB, soft %8 dB, LDPC checks %9, pass %10\n")
                          .arg(decodeTimeText)
                          .arg(rfText.isEmpty() ? QString() : rfText + QStringLiteral("  "))
                          .arg(signalCenterHz, 0, 'f', 0)
                          .arg(ft8SignedSecondsText(dtSeconds))
                          .arg(candidate.syncDb, 0, 'f', 1)
                          .arg(candidate.decodedCostasHits)
                          .arg(candidate.symbolReliabilityDb, 0, 'f', 1)
                          .arg(candidate.softBitMarginDb, 0, 'f', 1)
                          .arg(candidate.ldpcUnsatisfiedChecks)
                          .arg(candidate.ldpcPass);
        }
    }

    if (!report.isEmpty()) {
        if (reportOnlyDecoded) {
            decodedText += QStringLiteral("\n[FT8] %1 decode%2, slot %3 UTC:\n%4")
                               .arg(reportedCount)
                               .arg(reportedCount == 1 ? QString() : QStringLiteral("s"))
                               .arg(decodeTimeText)
                               .arg(report);
        } else {
            decodedText += QStringLiteral("\n[FT8] %1 strong sync candidate%2, slot %3 UTC:\n%4")
                               .arg(reportedCount)
                               .arg(reportedCount == 1 ? QString() : QStringLiteral("s"))
                               .arg(decodeTimeText)
                               .arg(report);
        }
    }
}

bool DigitalDecoder::detectAfskBit(float sample, bool &signalPresent) {
    const double markInc = TWO_PI * RTTY_MARK_HZ / activeSampleRate;
    const double spaceInc = TWO_PI * RTTY_SPACE_HZ / activeSampleRate;

    markPhase += markInc;
    if (markPhase >= TWO_PI) {
        markPhase -= TWO_PI;
    }
    spacePhase += spaceInc;
    if (spacePhase >= TWO_PI) {
        spacePhase -= TWO_PI;
    }

    const double markCos = std::cos(markPhase);
    const double markSin = std::sin(markPhase);
    const double spaceCos = std::cos(spacePhase);
    const double spaceSin = std::sin(spacePhase);

    markI += detectorAlpha * (sample * markCos - markI);
    markQ += detectorAlpha * (sample * markSin - markQ);
    spaceI += detectorAlpha * (sample * spaceCos - spaceI);
    spaceQ += detectorAlpha * (sample * spaceSin - spaceQ);

    const double markPower = markI * markI + markQ * markQ;
    const double spacePower = spaceI * spaceI + spaceQ * spaceQ;
    const double totalPower = markPower + spacePower;
    const double level = std::sqrt(totalPower);
    const double dominance = std::abs(markPower - spacePower) / (totalPower + 1e-12);
    signalQuality += 0.02 * (dominance - signalQuality);
    signalPresent = level >= RTTY_MIN_TONE_LEVEL && dominance >= RTTY_MIN_TONE_DOMINANCE;
    return markPower >= spacePower;
}

bool DigitalDecoder::detectFskBit(float sample, bool &signalPresent) {
    fskLevel += 0.002 * (std::fabs(sample) - fskLevel);
    signalPresent = fskLevel >= FSK_MIN_DISCRIMINATOR_LEVEL;
    return sample >= 0.0f;
}

void DigitalDecoder::advanceRttyState(bool markBit, QString &decodedText) {
    switch (rttyState) {
    case RttyState::Idle:
        if (previousRttyBit && !markBit) {
            rttyState = RttyState::ValidateStart;
            bitCountdown = samplesPerBit * 0.5;
        }
        break;

    case RttyState::ValidateStart:
        bitCountdown -= 1.0;
        if (bitCountdown <= 0.0) {
            if (!markBit) {
                rttyState = RttyState::Data;
                bitCountdown += samplesPerBit;
                rttyCode = 0;
                rttyBitIndex = 0;
            } else {
                rttyState = RttyState::Idle;
            }
        }
        break;

    case RttyState::Data:
        bitCountdown -= 1.0;
        if (bitCountdown <= 0.0) {
            if (markBit) {
                rttyCode |= (1u << rttyBitIndex);
            }
            ++rttyBitIndex;
            bitCountdown += samplesPerBit;
            if (rttyBitIndex >= 5) {
                rttyState = RttyState::Stop;
            }
        }
        break;

    case RttyState::Stop:
        bitCountdown -= 1.0;
        if (bitCountdown <= 0.0) {
            if (markBit) {
                const QString decoded = decodeBaudotCode(rttyCode);
                if (!decoded.isEmpty()) {
                    decodedText += decoded;
                    ++rttyDecodedCount;
                }
                rttyBadStopCount = 0;
            } else {
                ++rttyBadStopCount;
                if (rttyDecodedCount < 3 && rttyBadStopCount >= 12) {
                    rttyInvertPolarity = !rttyInvertPolarity;
                    resetRttyState();
                    updateStatus(rttyInvertPolarity
                                     ? QStringLiteral("RTTY decoder: inverted polarity auto-selected")
                                     : QStringLiteral("RTTY decoder: normal polarity auto-selected"));
                    previousRttyBit = markBit;
                    return;
                }
            }
            rttyState = RttyState::Idle;
        }
        break;
    }

    previousRttyBit = markBit;
}

QString DigitalDecoder::decodeBaudotCode(unsigned int code) {
    static const std::array<const char *, 32> letters = {{
        "", "E", "\n", "A", " ", "S", "I", "U",
        "\n", "D", "R", "J", "N", "F", "C", "K",
        "T", "Z", "L", "W", "H", "Y", "P", "Q",
        "O", "B", "G", "", "M", "X", "V", ""
    }};

    static const std::array<const char *, 32> figures = {{
        "", "3", "\n", "-", " ", "'", "8", "7",
        "\n", "$", "4", "'", ",", "!", ":", "(",
        "5", "\"", ")", "2", "#", "6", "0", "1",
        "9", "?", "&", "", ".", "/", ";", ""
    }};

    code &= 0x1f;
    if (code == 31) {
        rttyLettersShift = true;
        return QString();
    }
    if (code == 27) {
        rttyLettersShift = false;
        return QString();
    }

    const char *raw = rttyLettersShift ? letters[code] : figures[code];
    QString decoded = QString::fromLatin1(raw);
    if (decoded == QStringLiteral("\n")) {
        if (rttyLastWasNewline) {
            return QString();
        }
        rttyLastWasNewline = true;
        return decoded;
    }
    if (!decoded.isEmpty()) {
        rttyLastWasNewline = false;
    }
    return decoded;
}

void DigitalDecoder::updateStatus(const QString &status) {
    if (lastStatus == status) {
        return;
    }
    lastStatus = status;
    emit statusChanged(status);
}
