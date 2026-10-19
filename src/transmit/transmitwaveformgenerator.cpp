#include "transmitwaveformgenerator.h"

extern "C" {
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"
}

#include <QHash>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr int HilbertTaps = 63;
constexpr int HilbertDelay = HilbertTaps / 2;

float clampAudio(float value) {
    return (std::clamp)(value, -1.0f, 1.0f);
}

const QHash<QChar, QString> &morseTable() {
    static const QHash<QChar, QString> table = {
        {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."}, {'E', "."},
        {'F', "..-."}, {'G', "--."}, {'H', "...."}, {'I', ".."}, {'J', ".---"},
        {'K', "-.-"}, {'L', ".-.."}, {'M', "--"}, {'N', "-."}, {'O', "---"},
        {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."}, {'S', "..."}, {'T', "-"},
        {'U', "..-"}, {'V', "...-"}, {'W', ".--"}, {'X', "-..-"}, {'Y', "-.--"},
        {'Z', "--.."}, {'0', "-----"}, {'1', ".----"}, {'2', "..---"},
        {'3', "...--"}, {'4', "....-"}, {'5', "....."}, {'6', "-...."},
        {'7', "--..."}, {'8', "---.."}, {'9', "----."}, {'.', ".-.-.-"},
        {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."}, {'=', "-...-"},
        {'+', ".-.-."}, {'-', "-....-"}, {'(', "-.--."}, {')', "-.--.-"},
        {QChar(0x0410), ".-"}, {QChar(0x0411), "-..."}, {QChar(0x0412), ".--"},
        {QChar(0x0413), "...."}, {QChar(0x0490), "--."}, {QChar(0x0414), "-.."},
        {QChar(0x0415), "."}, {QChar(0x0404), "..-.."}, {QChar(0x0416), "...-"},
        {QChar(0x0417), "--.."}, {QChar(0x0418), "-.--"}, {QChar(0x0406), ".."},
        {QChar(0x0407), ".---."}, {QChar(0x0419), ".---"}, {QChar(0x041A), "-.-"},
        {QChar(0x041B), ".-.."}, {QChar(0x041C), "--"}, {QChar(0x041D), "-."},
        {QChar(0x041E), "---"}, {QChar(0x041F), ".--."}, {QChar(0x0420), ".-."},
        {QChar(0x0421), "..."}, {QChar(0x0422), "-"}, {QChar(0x0423), "..-"},
        {QChar(0x0424), "..-."}, {QChar(0x0425), "----"}, {QChar(0x0426), "-.-."},
        {QChar(0x0427), "---."}, {QChar(0x0428), "--.-"}, {QChar(0x0429), "--.--"},
        {QChar(0x042C), "-..-"}, {QChar(0x042E), "..--"}, {QChar(0x042F), ".-.-"}
    };
    return table;
}

double hilbertCoefficient(int tap) {
    const int n = tap - HilbertDelay;
    if (n == 0 || (n & 1) == 0) {
        return 0.0;
    }
    const double ideal = 2.0 / (Pi * static_cast<double>(n));
    const double window = 0.54 + 0.46 * std::cos(Pi * static_cast<double>(n) /
                                                 static_cast<double>(HilbertDelay));
    return ideal * window;
}

float gfskPulse(float bt, float t) {
    constexpr float K = 5.336446f;
    const float arg1 = K * bt * (t + 0.5f);
    const float arg2 = K * bt * (t - 0.5f);
    return 0.5f * (std::erf(arg1) - std::erf(arg2));
}

struct TimedBit {
    int bit = 0;
    double durationSymbols = 1.0;
};

void appendByteBits(QVector<int> *bits, std::uint8_t value, bool invert) {
    for (int shift = 7; shift >= 0; --shift) {
        bits->append(((value >> shift) & 1U) ^ (invert ? 1 : 0));
    }
}

std::uint16_t crc16Ccitt(const QByteArray &data) {
    std::uint16_t crc = 0xFFFFU;
    for (unsigned char byte : data) {
        crc ^= static_cast<std::uint16_t>(byte) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0U
                      ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                      : static_cast<std::uint16_t>(crc << 1U);
        }
    }
    return crc;
}

QVector<int> framedPacketBits(const QByteArray &payload, bool invert) {
    QVector<int> bits;
    bits.reserve((payload.size() + 16) * 8);
    for (int byte = 0; byte < 8; ++byte) appendByteBits(&bits, 0x55U, invert);
    appendByteBits(&bits, 0xD3U, invert);
    appendByteBits(&bits, 0x91U, invert);
    appendByteBits(&bits, static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF), invert);
    appendByteBits(&bits, static_cast<std::uint8_t>(payload.size() & 0xFF), invert);
    for (unsigned char byte : payload) appendByteBits(&bits, byte, invert);
    const std::uint16_t crc = crc16Ccitt(payload);
    appendByteBits(&bits, static_cast<std::uint8_t>(crc >> 8U), invert);
    appendByteBits(&bits, static_cast<std::uint8_t>(crc & 0xFFU), invert);
    for (int bit = 0; bit < 8; ++bit) bits.append(invert ? 1 : 0);
    return bits;
}

bool baudotCode(QChar character, int *code, bool *figures) {
    static const QHash<QChar, int> letters = {
        {'A', 3}, {'B', 25}, {'C', 14}, {'D', 9}, {'E', 1}, {'F', 13},
        {'G', 26}, {'H', 20}, {'I', 6}, {'J', 11}, {'K', 15}, {'L', 18},
        {'M', 28}, {'N', 12}, {'O', 24}, {'P', 22}, {'Q', 23}, {'R', 10},
        {'S', 5}, {'T', 16}, {'U', 7}, {'V', 30}, {'W', 19}, {'X', 29},
        {'Y', 21}, {'Z', 17}, {' ', 4}, {'\r', 8}, {'\n', 2}
    };
    static const QHash<QChar, int> numbers = {
        {'0', 22}, {'1', 23}, {'2', 19}, {'3', 1}, {'4', 10}, {'5', 16},
        {'6', 21}, {'7', 7}, {'8', 6}, {'9', 24}, {'-', 3}, {'?', 25},
        {':', 14}, {'$', 9}, {'!', 13}, {'&', 26}, {'#', 20}, {'\'', 11},
        {'(', 15}, {')', 18}, {'.', 28}, {',', 12}, {'/', 29}, {'+', 17},
        {'=', 30}, {' ', 4}, {'\r', 8}, {'\n', 2}
    };
    const QChar upper = character.toUpper();
    const auto letter = letters.constFind(upper);
    if (letter != letters.cend()) {
        *code = letter.value();
        *figures = false;
        return true;
    }
    const auto number = numbers.constFind(upper);
    if (number != numbers.cend()) {
        *code = number.value();
        *figures = true;
        return true;
    }
    return false;
}

void appendRttyCode(QVector<TimedBit> *symbols, int code, bool invert) {
    const auto append = [symbols, invert](int bit, double duration = 1.0) {
        symbols->append({bit ^ (invert ? 1 : 0), duration});
    };
    append(0);
    for (int bit = 0; bit < 5; ++bit) append((code >> bit) & 1);
    append(1, 1.5);
}

QVector<TimedBit> rttySymbols(const QString &text, bool invert) {
    QVector<TimedBit> symbols;
    const auto append = [&symbols, invert](int bit, double duration = 1.0) {
        symbols.append({bit ^ (invert ? 1 : 0), duration});
    };
    for (int bit = 0; bit < 12; ++bit) append(1);
    bool figuresMode = false;
    for (QChar character : text.toUpper()) {
        int code = 0;
        bool figures = false;
        if (!baudotCode(character, &code, &figures)) continue;
        if (!character.isSpace() && character != QLatin1Char('\r') &&
            character != QLatin1Char('\n') && figures != figuresMode) {
            appendRttyCode(&symbols, figures ? 27 : 31, invert);
            figuresMode = figures;
        }
        appendRttyCode(&symbols, code, invert);
    }
    for (int bit = 0; bit < 12; ++bit) append(1);
    return symbols;
}

double smoothTransition(double fraction) {
    const double position = (std::clamp)(fraction / 0.20, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(Pi * position);
}

double qpskPhase(int first, int second) {
    if (first == 0 && second == 0) return Pi * 0.25;
    if (first == 0 && second == 1) return Pi * 0.75;
    if (first == 1 && second == 1) return -Pi * 0.75;
    return -Pi * 0.25;
}

double gaussianSymbolControl(const QVector<int> &bits, double symbolPosition, double bt) {
    if (bits.isEmpty()) return 0.0;
    const int center = static_cast<int>(std::floor(symbolPosition));
    double weighted = 0.0;
    double weightSum = 0.0;
    for (int offset = -3; offset <= 3; ++offset) {
        const int symbol = (std::clamp)(center + offset, 0, bits.size() - 1);
        const double weight = gfskPulse(static_cast<float>((std::clamp)(bt, 0.20, 1.0)),
                                        static_cast<float>(symbolPosition -
                                                           (center + offset + 0.5)));
        weighted += (bits.at(symbol) != 0 ? 1.0 : -1.0) * weight;
        weightSum += weight;
    }
    return weightSum > 1.0e-9 ? weighted / weightSum : 0.0;
}
}

void TxModulatorState::resetAudioFilter() {
    audioFilterSampleRate = 0;
    audioFilterCutoffHz = 0.0;
    audioFilterZ1.fill(0.0);
    audioFilterZ2.fill(0.0);
}

void TxBandwidthLimiterState::reset() {
    sampleRate = 0;
    bandwidthHz = 0.0;
    centerHz = 0.0;
    shiftOscillator = std::complex<double>(1.0, 0.0);
    iZ1.fill(0.0);
    iZ2.fill(0.0);
    qZ1.fill(0.0);
    qZ2.fill(0.0);
}

bool TransmitWaveformGenerator::limitSignalBandwidth(
    const TxConfiguration &configuration,
    const std::complex<float> *input,
    int count,
    QVector<std::complex<float>> *output,
    TxBandwidthLimiterState *state) {
    if (!input || count <= 0 || !output || !state || configuration.sampleRate <= 0 ||
        !std::isfinite(configuration.signalBandwidthHz) || configuration.signalBandwidthHz <= 0.0) {
        return false;
    }
    const double sampleRate = static_cast<double>(configuration.sampleRate);
    const double bandwidth = (std::min)(configuration.signalBandwidthHz, sampleRate * 0.90);

    const double centerHz = signalCenterHz(configuration);
    if (state->sampleRate != configuration.sampleRate ||
        std::abs(state->bandwidthHz - bandwidth) > 0.01 ||
        std::abs(state->centerHz - centerHz) > 0.01) {
        state->reset();
        state->sampleRate = configuration.sampleRate;
        state->bandwidthHz = bandwidth;
        state->centerHz = centerHz;
    }

    // SSB occupies one side of the complex baseband. Its requested bandwidth
    // is therefore also its one-sided cutoff, unlike AM/FM/DSB where the
    // requested RF bandwidth is split equally around the carrier.
    const bool singleSideband = configuration.modulation == TxModulation::Usb ||
                                configuration.modulation == TxModulation::Lsb;
    const double cutoff = (std::min)(singleSideband ? bandwidth : bandwidth * 0.5,
                                     sampleRate * 0.45);
    const double omega = 2.0 * Pi * cutoff / sampleRate;
    const double cosine = std::cos(omega);
    const double sine = std::sin(omega);
    constexpr std::array<double, 2> ButterworthQ{{0.541196100146197, 1.306562964876377}};
    std::array<double, 2> b0{};
    std::array<double, 2> b1{};
    std::array<double, 2> b2{};
    std::array<double, 2> a1{};
    std::array<double, 2> a2{};
    for (int stage = 0; stage < 2; ++stage) {
        const double alpha = sine / (2.0 * ButterworthQ[stage]);
        const double a0 = 1.0 + alpha;
        b0[stage] = (1.0 - cosine) * 0.5 / a0;
        b1[stage] = (1.0 - cosine) / a0;
        b2[stage] = b0[stage];
        a1[stage] = -2.0 * cosine / a0;
        a2[stage] = (1.0 - alpha) / a0;
    }

    const bool shifted = std::abs(centerHz) > 0.01;
    const std::complex<double> oscillatorStep = shifted
        ? std::polar(1.0, 2.0 * Pi * centerHz / sampleRate)
        : std::complex<double>(1.0, 0.0);
    std::complex<double> oscillator = state->shiftOscillator;
    output->resize(count);
    for (int index = 0; index < count; ++index) {
        const std::complex<double> shiftedInput = shifted
            ? std::complex<double>(input[index].real(), input[index].imag()) * std::conj(oscillator)
            : std::complex<double>(input[index].real(), input[index].imag());
        double iValue = shiftedInput.real();
        double qValue = shiftedInput.imag();
        for (int stage = 0; stage < 2; ++stage) {
            const double filteredI = b0[stage] * iValue + state->iZ1[stage];
            state->iZ1[stage] = b1[stage] * iValue - a1[stage] * filteredI + state->iZ2[stage];
            state->iZ2[stage] = b2[stage] * iValue - a2[stage] * filteredI;
            iValue = filteredI;

            const double filteredQ = b0[stage] * qValue + state->qZ1[stage];
            state->qZ1[stage] = b1[stage] * qValue - a1[stage] * filteredQ + state->qZ2[stage];
            state->qZ2[stage] = b2[stage] * qValue - a2[stage] * filteredQ;
            qValue = filteredQ;
        }
        std::complex<double> filtered(iValue, qValue);
        if (shifted) {
            filtered *= oscillator;
            oscillator *= oscillatorStep;
        }
        (*output)[index] = std::complex<float>(static_cast<float>(filtered.real()),
                                               static_cast<float>(filtered.imag()));
    }
    if (shifted) {
        const double magnitude = std::abs(oscillator);
        state->shiftOscillator = magnitude > 0.0 ? oscillator / magnitude
                                                  : std::complex<double>(1.0, 0.0);
    }
    return true;
}
bool TransmitWaveformGenerator::isTextMode(TxModulation modulation) {
    return modulation == TxModulation::Cw || modulation == TxModulation::Ft8 ||
           isDigitalTextMode(modulation);
}

bool TransmitWaveformGenerator::isDigitalTextMode(TxModulation modulation) {
    switch (modulation) {
    case TxModulation::Ook:
    case TxModulation::Ask:
    case TxModulation::Fsk:
    case TxModulation::Gfsk:
    case TxModulation::Gmsk:
    case TxModulation::Bpsk:
    case TxModulation::Qpsk:
    case TxModulation::Rtty:
        return true;
    default:
        return false;
    }
}

double TransmitWaveformGenerator::effectiveFmDeviationHz(
    const TxConfiguration &configuration) {
    if (configuration.modulation == TxModulation::Wfm) {
        return (std::max)(10000.0, configuration.deviationHz);
    }
    if (configuration.modulation == TxModulation::Nfm) {
        return (std::max)(100.0, configuration.deviationHz);
    }
    return 0.0;
}

double TransmitWaveformGenerator::effectiveFskDeviationHz(
    const TxConfiguration &configuration) {
    if (configuration.modulation == TxModulation::Gmsk) {
        return (std::max)(1.0, configuration.symbolRate * 0.25);
    }
    if (configuration.modulation == TxModulation::Fsk ||
        configuration.modulation == TxModulation::Gfsk ||
        configuration.modulation == TxModulation::Rtty) {
        return (std::max)(1.0, configuration.deviationHz);
    }
    return 0.0;
}

double TransmitWaveformGenerator::signalCenterHz(const TxConfiguration &configuration) {
    if (configuration.modulation == TxModulation::Cw) return configuration.toneHz;
    if (configuration.modulation == TxModulation::Ft8) {
        return configuration.toneHz + 3.5 / FT8_SYMBOL_PERIOD;
    }
    if (isDigitalTextMode(configuration.modulation)) return configuration.toneHz;
    return 0.0;
}

double TransmitWaveformGenerator::audioLowPassCutoffHz(
    const TxConfiguration &configuration) {
    if (!std::isfinite(configuration.signalBandwidthHz) ||
        configuration.signalBandwidthHz <= 0.0) {
        return 0.0;
    }
    switch (configuration.modulation) {
    case TxModulation::Am:
    case TxModulation::Dsb:
        return configuration.signalBandwidthHz * 0.5;
    case TxModulation::Usb:
    case TxModulation::Lsb:
        return configuration.signalBandwidthHz;
    case TxModulation::Nfm:
    case TxModulation::Wfm:
        return (std::max)(0.0, configuration.signalBandwidthHz * 0.5 -
                                  effectiveFmDeviationHz(configuration));
    case TxModulation::Cw:
    case TxModulation::Ft8:
    case TxModulation::Ook:
    case TxModulation::Ask:
    case TxModulation::Fsk:
    case TxModulation::Gfsk:
    case TxModulation::Gmsk:
    case TxModulation::Bpsk:
    case TxModulation::Qpsk:
    case TxModulation::Rtty:
    case TxModulation::Dmr:
        return 0.0;
    }
    return 0.0;
}

QString TransmitWaveformGenerator::modulationName(TxModulation modulation) {
    switch (modulation) {
    case TxModulation::Am: return QStringLiteral("AM");
    case TxModulation::Nfm: return QStringLiteral("NFM");
    case TxModulation::Wfm: return QStringLiteral("WFM");
    case TxModulation::Dsb: return QStringLiteral("DSB-SC");
    case TxModulation::Usb: return QStringLiteral("USB");
    case TxModulation::Lsb: return QStringLiteral("LSB");
    case TxModulation::Cw: return QStringLiteral("CW / Morse");
    case TxModulation::Ft8: return QStringLiteral("FT8");
    case TxModulation::Ook: return QStringLiteral("OOK (raw frame)");
    case TxModulation::Ask: return QStringLiteral("2-ASK (raw frame)");
    case TxModulation::Fsk: return QStringLiteral("2-FSK (raw frame)");
    case TxModulation::Gfsk: return QStringLiteral("GFSK (raw frame)");
    case TxModulation::Gmsk: return QStringLiteral("GMSK (raw frame)");
    case TxModulation::Bpsk: return QStringLiteral("BPSK (raw frame)");
    case TxModulation::Qpsk: return QStringLiteral("QPSK (raw frame)");
    case TxModulation::Rtty: return QStringLiteral("RTTY 45.45 / ITA2");
    case TxModulation::Dmr: return QStringLiteral("DMR voice (Tier II direct)");
    }
    return QStringLiteral("Unknown");
}

TxGenerationResult TransmitWaveformGenerator::generateText(
    const TxConfiguration &configuration,
    const QString &text) {
    if (configuration.modulation == TxModulation::Cw) {
        return generateCw(configuration, text);
    }
    if (configuration.modulation == TxModulation::Ft8) {
        return generateFt8(configuration, text);
    }
    if (isDigitalTextMode(configuration.modulation)) {
        return generateDigitalText(configuration, text);
    }
    TxGenerationResult result;
    result.error = QStringLiteral("The selected modulation requires microphone audio");
    return result;
}

QVector<std::complex<float>> TransmitWaveformGenerator::modulateAudio(
    const TxConfiguration &configuration,
    const QVector<float> &audio,
    TxModulatorState *state) {
    QVector<std::complex<float>> result;
    if (audio.isEmpty() || configuration.sampleRate <= 0) {
        return result;
    }
    result.resize(audio.size());
    TxModulatorState localState;
    TxModulatorState &s = state ? *state : localState;
    const double sampleRate = static_cast<double>(configuration.sampleRate);
    const float level = (std::clamp)(configuration.level, 0.0f, 0.95f);
    const double deviation = effectiveFmDeviationHz(configuration);
    const double requestedAudioCutoff = audioLowPassCutoffHz(configuration);
    const double audioCutoff = (std::min)(requestedAudioCutoff, sampleRate * 0.45);
    const bool filterAudio = audioCutoff >= 20.0 && audioCutoff < sampleRate * 0.445;
    std::array<double, 2> audioB0{};
    std::array<double, 2> audioB1{};
    std::array<double, 2> audioB2{};
    std::array<double, 2> audioA1{};
    std::array<double, 2> audioA2{};
    if (filterAudio) {
        if (s.audioFilterSampleRate != configuration.sampleRate ||
            std::abs(s.audioFilterCutoffHz - audioCutoff) > 0.01) {
            s.resetAudioFilter();
            s.audioFilterSampleRate = configuration.sampleRate;
            s.audioFilterCutoffHz = audioCutoff;
        }
        const double omega = 2.0 * Pi * audioCutoff / sampleRate;
        const double cosine = std::cos(omega);
        const double sine = std::sin(omega);
        constexpr std::array<double, 2> ButterworthQ{{0.541196100146197, 1.306562964876377}};
        for (int stage = 0; stage < 2; ++stage) {
            const double alpha = sine / (2.0 * ButterworthQ[stage]);
            const double a0 = 1.0 + alpha;
            audioB0[stage] = (1.0 - cosine) * 0.5 / a0;
            audioB1[stage] = (1.0 - cosine) / a0;
            audioB2[stage] = audioB0[stage];
            audioA1[stage] = -2.0 * cosine / a0;
            audioA2[stage] = (1.0 - alpha) / a0;
        }
    } else if (s.audioFilterSampleRate != 0) {
        s.resetAudioFilter();
    }

    for (int i = 0; i < audio.size(); ++i) {
        double filteredAudio = clampAudio(audio[i]);
        if (filterAudio) {
            for (int stage = 0; stage < 2; ++stage) {
                const double next = audioB0[stage] * filteredAudio + s.audioFilterZ1[stage];
                s.audioFilterZ1[stage] = audioB1[stage] * filteredAudio -
                                         audioA1[stage] * next + s.audioFilterZ2[stage];
                s.audioFilterZ2[stage] = audioB2[stage] * filteredAudio -
                                         audioA2[stage] * next;
                filteredAudio = next;
            }
        }
        const float x = clampAudio(static_cast<float>(filteredAudio));
        switch (configuration.modulation) {
        case TxModulation::Am: {
            // Keep the RF peak bounded by `level`, while using a useful voice
            // modulation index instead of the old, quiet fixed value.
            const float modulationIndex =
                (std::clamp)(configuration.amDepth, 0.0f, 1.0f);
            result[i] = std::complex<float>(
                level * (1.0f + modulationIndex * x) /
                    (1.0f + modulationIndex),
                0.0f);
            break;
        }
        case TxModulation::Dsb:
            result[i] = std::complex<float>(level * x, 0.0f);
            break;
        case TxModulation::Nfm:
        case TxModulation::Wfm:
            s.phase += 2.0 * Pi * deviation * static_cast<double>(x) / sampleRate;
            result[i] = std::polar(level, static_cast<float>(s.phase));
            break;
        case TxModulation::Usb:
        case TxModulation::Lsb: {
            s.hilbertHistory[s.hilbertWriteIndex] = x;
            double quadrature = 0.0;
            for (int tap = 0; tap < HilbertTaps; ++tap) {
                int historyIndex = s.hilbertWriteIndex - tap;
                if (historyIndex < 0) historyIndex += HilbertTaps;
                quadrature += hilbertCoefficient(tap) * s.hilbertHistory[historyIndex];
            }
            int delayedIndex = s.hilbertWriteIndex - HilbertDelay;
            if (delayedIndex < 0) delayedIndex += HilbertTaps;
            const float delayed = s.hilbertHistory[delayedIndex];
            s.hilbertWriteIndex = (s.hilbertWriteIndex + 1) % HilbertTaps;
            const float q = static_cast<float>(quadrature) *
                            (configuration.modulation == TxModulation::Usb ? 1.0f : -1.0f);
            result[i] = level * std::complex<float>(delayed, q);
            break;
        }
        case TxModulation::Cw:
        case TxModulation::Ft8:
        case TxModulation::Ook:
        case TxModulation::Ask:
        case TxModulation::Fsk:
        case TxModulation::Gfsk:
        case TxModulation::Gmsk:
        case TxModulation::Bpsk:
        case TxModulation::Qpsk:
        case TxModulation::Rtty:
        case TxModulation::Dmr:
            result[i] = std::complex<float>(0.0f, 0.0f);
            break;
        }
        if (std::abs(s.phase) > 2.0 * Pi) {
            s.phase = std::fmod(s.phase, 2.0 * Pi);
        }
    }
    return result;
}

TxGenerationResult TransmitWaveformGenerator::generateDigitalText(
    const TxConfiguration &configuration,
    const QString &text) {
    TxGenerationResult result;
    const QByteArray payload = text.toUtf8();
    if (payload.trimmed().isEmpty()) {
        result.error = QStringLiteral("Enter text or packet payload");
        return result;
    }
    if (payload.size() > 1024) {
        result.error = QStringLiteral("Digital text payload is limited to 1024 UTF-8 bytes");
        return result;
    }
    const int sampleRate = configuration.sampleRate;
    const double symbolRate = configuration.symbolRate;
    if (sampleRate <= 0 || !std::isfinite(symbolRate) || symbolRate <= 0.0 ||
        symbolRate > sampleRate * 0.25) {
        result.error = QStringLiteral("Symbol rate must be positive and no more than one quarter of the generator rate");
        return result;
    }

    const float level = (std::clamp)(configuration.level, 0.0f, 0.95f);
    const double centerHz = signalCenterHz(configuration);
    const double samplesPerSymbol = static_cast<double>(sampleRate) / symbolRate;
    double phase = 0.0;

    if (configuration.modulation == TxModulation::Rtty) {
        const QVector<TimedBit> symbols = rttySymbols(text, configuration.invertData);
        if (symbols.size() <= 24) {
            result.error = QStringLiteral("The RTTY text contains no supported ITA2 characters");
            return result;
        }
        double totalSymbols = 0.0;
        for (const TimedBit &symbol : symbols) totalSymbols += symbol.durationSymbols;
        const qint64 totalSamples = qRound64(totalSymbols * samplesPerSymbol);
        if (totalSamples <= 0 || totalSamples > static_cast<qint64>(sampleRate) * 180 ||
            totalSamples > std::numeric_limits<int>::max()) {
            result.error = QStringLiteral("Generated RTTY waveform is too long");
            return result;
        }
        result.iq.resize(static_cast<int>(totalSamples));
        const double deviation = effectiveFskDeviationHz(configuration);
        double symbolCursor = 0.0;
        for (const TimedBit &symbol : symbols) {
            const int start = qRound(symbolCursor * samplesPerSymbol);
            symbolCursor += symbol.durationSymbols;
            const int end = (std::min)(result.iq.size(), qRound(symbolCursor * samplesPerSymbol));
            const double control = symbol.bit != 0 ? -1.0 : 1.0;
            const double step = 2.0 * Pi * (centerHz + deviation * control) / sampleRate;
            for (int sample = start; sample < end; ++sample) {
                phase += step;
                result.iq[sample] = std::polar(level, static_cast<float>(phase));
                if (std::abs(phase) > 2.0 * Pi) phase = std::fmod(phase, 2.0 * Pi);
            }
        }
        result.durationSeconds = static_cast<double>(result.iq.size()) / sampleRate;
        result.summary = QStringLiteral("RTTY ITA2: %1 symbols, %2 samples, %3 s, %4 Bd, %5 Hz shift")
                             .arg(symbols.size())
                             .arg(result.iq.size())
                             .arg(result.durationSeconds, 0, 'f', 3)
                             .arg(symbolRate, 0, 'f', 2)
                             .arg(2.0 * deviation, 0, 'f', 0);
        return result;
    }

    const QVector<int> bits = framedPacketBits(payload, configuration.invertData);
    const bool qpsk = configuration.modulation == TxModulation::Qpsk;
    const int symbolCount = qpsk ? (bits.size() + 1) / 2 : bits.size();
    const qint64 totalSamples = qRound64(symbolCount * samplesPerSymbol);
    if (totalSamples <= 0 || totalSamples > static_cast<qint64>(sampleRate) * 180 ||
        totalSamples > std::numeric_limits<int>::max()) {
        result.error = QStringLiteral("Generated digital waveform is too long");
        return result;
    }
    result.iq.resize(static_cast<int>(totalSamples));
    const double carrierStep = 2.0 * Pi * centerHz / sampleRate;
    const double deviation = effectiveFskDeviationHz(configuration);
    const double askDepth = (std::clamp)(static_cast<double>(configuration.askDepth), 0.0, 1.0);
    const double bt = (std::clamp)(configuration.gaussianBt, 0.20, 1.0);

    const auto bitAt = [&bits](int index) {
        return bits.at((std::clamp)(index, 0, bits.size() - 1));
    };
    const auto phaseAt = [&bitAt, qpsk](int symbol) {
        if (!qpsk) return bitAt(symbol) != 0 ? Pi : 0.0;
        return qpskPhase(bitAt(symbol * 2), bitAt(symbol * 2 + 1));
    };

    for (int sample = 0; sample < result.iq.size(); ++sample) {
        const double position = static_cast<double>(sample) / samplesPerSymbol;
        const int symbol = (std::min)(symbolCount - 1, static_cast<int>(std::floor(position)));
        const double local = position - std::floor(position);
        if (configuration.modulation == TxModulation::Fsk ||
            configuration.modulation == TxModulation::Gfsk ||
            configuration.modulation == TxModulation::Gmsk) {
            const double control = configuration.modulation == TxModulation::Fsk
                                       ? (bitAt(symbol) != 0 ? 1.0 : -1.0)
                                       : gaussianSymbolControl(bits, position, bt);
            phase += 2.0 * Pi * (centerHz + deviation * control) / sampleRate;
            result.iq[sample] = std::polar(level, static_cast<float>(phase));
        } else if (configuration.modulation == TxModulation::Ook ||
                   configuration.modulation == TxModulation::Ask) {
            const double previousTarget = symbol > 0
                                              ? (bitAt(symbol - 1) != 0 ? 1.0 :
                                                 (configuration.modulation == TxModulation::Ook
                                                      ? 0.0 : 1.0 - askDepth))
                                              : 0.0;
            const double currentTarget = bitAt(symbol) != 0
                                             ? 1.0
                                             : (configuration.modulation == TxModulation::Ook
                                                    ? 0.0 : 1.0 - askDepth);
            const double amplitude = previousTarget +
                                     (currentTarget - previousTarget) * smoothTransition(local);
            phase += carrierStep;
            result.iq[sample] = std::polar(static_cast<float>(level * amplitude),
                                           static_cast<float>(phase));
        } else {
            const double currentPhase = phaseAt(symbol);
            const double previousPhase = symbol > 0 ? phaseAt(symbol - 1) : currentPhase;
            const double dataPhase = previousPhase +
                                     std::remainder(currentPhase - previousPhase, 2.0 * Pi) *
                                         smoothTransition(local);
            phase += carrierStep;
            result.iq[sample] = std::polar(level, static_cast<float>(phase + dataPhase));
        }
        if (std::abs(phase) > 2.0 * Pi) phase = std::fmod(phase, 2.0 * Pi);
    }

    result.durationSeconds = static_cast<double>(result.iq.size()) / sampleRate;
    result.summary = QStringLiteral("%1: %2 payload bytes, %3 symbols, %4 samples, %5 s, %6 Bd")
                         .arg(modulationName(configuration.modulation))
                         .arg(payload.size())
                         .arg(symbolCount)
                         .arg(result.iq.size())
                         .arg(result.durationSeconds, 0, 'f', 3)
                         .arg(symbolRate, 0, 'f', 2);
    return result;
}
TxGenerationResult TransmitWaveformGenerator::generateCw(
    const TxConfiguration &configuration,
    const QString &text) {
    TxGenerationResult result;
    const QString normalized = text.trimmed().toUpper();
    if (normalized.isEmpty()) {
        result.error = QStringLiteral("Enter Morse text");
        return result;
    }
    const int sampleRate = configuration.sampleRate;
    const double dotSeconds = 1.2 / static_cast<double>((std::max)(5, configuration.cwWpm));
    const int unitSamples = (std::max)(1, qRound(dotSeconds * sampleRate));
    const int rampSamples = (std::min)(unitSamples / 3, (std::max)(1, sampleRate / 200));
    const float level = (std::clamp)(configuration.level, 0.0f, 0.95f);
    double phase = 0.0;
    const double phaseStep = 2.0 * Pi * configuration.toneHz / sampleRate;

    auto appendUnits = [&](int units, bool keyed) {
        const int samples = units * unitSamples;
        const int oldSize = result.iq.size();
        result.iq.resize(oldSize + samples);
        for (int i = 0; i < samples; ++i) {
            float envelope = keyed ? 1.0f : 0.0f;
            if (keyed && rampSamples > 0) {
                if (i < rampSamples) {
                    const float p = static_cast<float>(i) / rampSamples;
                    envelope = std::sin(0.5f * static_cast<float>(Pi) * p);
                    envelope *= envelope;
                } else if (i >= samples - rampSamples) {
                    const float p = static_cast<float>(samples - 1 - i) / rampSamples;
                    envelope = std::sin(0.5f * static_cast<float>(Pi) * (std::max)(0.0f, p));
                    envelope *= envelope;
                }
            }
            result.iq[oldSize + i] = keyed
                                         ? std::polar(level * envelope, static_cast<float>(phase))
                                         : std::complex<float>(0.0f, 0.0f);
            phase += phaseStep;
            if (phase > 2.0 * Pi) phase -= 2.0 * Pi;
        }
    };

    bool emittedCharacter = false;
    for (int charIndex = 0; charIndex < normalized.size(); ++charIndex) {
        const QChar ch = normalized.at(charIndex);
        if (ch.isSpace()) {
            if (emittedCharacter) appendUnits(4, false); // Existing 3-unit character gap + 4 = 7.
            emittedCharacter = false;
            continue;
        }
        const QString code = morseTable().value(ch);
        if (code.isEmpty()) {
            continue;
        }
        if (emittedCharacter) appendUnits(3, false);
        for (int symbol = 0; symbol < code.size(); ++symbol) {
            if (symbol > 0) appendUnits(1, false);
            appendUnits(code.at(symbol) == QLatin1Char('-') ? 3 : 1, true);
        }
        emittedCharacter = true;
    }
    if (result.iq.isEmpty()) {
        result.error = QStringLiteral("The text contains no supported Morse characters");
        return result;
    }
    result.durationSeconds = static_cast<double>(result.iq.size()) / sampleRate;
    result.summary = QStringLiteral("CW: %1 samples, %2 s, %3 WPM")
                         .arg(result.iq.size())
                         .arg(result.durationSeconds, 0, 'f', 3)
                         .arg(configuration.cwWpm);
    return result;
}

TxGenerationResult TransmitWaveformGenerator::generateFt8(
    const TxConfiguration &configuration,
    const QString &text) {
    TxGenerationResult result;
    QByteArray messageBytes = text.trimmed().toUpper().toLatin1();
    if (messageBytes.isEmpty()) {
        result.error = QStringLiteral("Enter an FT8 message");
        return result;
    }
    ftx_message_t message;
    ftx_message_init(&message);
    const ftx_message_rc_t rc = ftx_message_encode(&message, nullptr, messageBytes.constData());
    if (rc != FTX_MESSAGE_RC_OK) {
        result.error = QStringLiteral("FT8 message cannot be encoded (code %1)").arg(static_cast<int>(rc));
        return result;
    }

    std::array<std::uint8_t, FT8_NN> tones{};
    ft8_encode(message.payload, tones.data());
    const int sampleRate = configuration.sampleRate;
    const int samplesPerSymbol = (std::max)(1, qRound(FT8_SYMBOL_PERIOD * sampleRate));
    const int waveformSamples = FT8_NN * samplesPerSymbol;
    const int pulseSamples = 3 * samplesPerSymbol;
    QVector<float> pulse(pulseSamples);
    for (int i = 0; i < pulseSamples; ++i) {
        pulse[i] = gfskPulse(2.0f,
                            static_cast<float>(i - samplesPerSymbol) /
                                static_cast<float>(samplesPerSymbol));
    }

    QVector<double> deltaPhase(waveformSamples + 2 * samplesPerSymbol, 0.0);
    const double toneSpacingHz = 1.0 / FT8_SYMBOL_PERIOD;
    const double peakStep = 2.0 * Pi * toneSpacingHz / sampleRate;
    for (int symbol = 0; symbol < FT8_NN; ++symbol) {
        const int base = symbol * samplesPerSymbol;
        for (int j = 0; j < pulseSamples; ++j) {
            deltaPhase[base + j] += peakStep * tones[symbol] * pulse[j];
        }
    }
    for (int j = 0; j < 2 * samplesPerSymbol; ++j) {
        deltaPhase[j] += peakStep * tones.front() * pulse[j + samplesPerSymbol];
        deltaPhase[j + waveformSamples] += peakStep * tones.back() * pulse[j];
    }

    result.iq.resize(waveformSamples);
    const float level = (std::clamp)(configuration.level, 0.0f, 0.95f);
    const int envelopeSamples = samplesPerSymbol / 8;
    double phase = 0.0;
    const double baseStep = 2.0 * Pi * configuration.toneHz / sampleRate;
    for (int i = 0; i < waveformSamples; ++i) {
        phase += baseStep + deltaPhase[i + samplesPerSymbol];
        float envelope = level;
        if (envelopeSamples > 0 && i < envelopeSamples) {
            envelope *= 0.5f * (1.0f - std::cos(static_cast<float>(Pi) * i / envelopeSamples));
        } else if (envelopeSamples > 0 && i >= waveformSamples - envelopeSamples) {
            const int remaining = waveformSamples - 1 - i;
            envelope *= 0.5f * (1.0f - std::cos(static_cast<float>(Pi) * remaining / envelopeSamples));
        }
        result.iq[i] = std::polar(envelope, static_cast<float>(phase));
        if (std::abs(phase) > 2.0 * Pi) phase = std::fmod(phase, 2.0 * Pi);
    }
    result.durationSeconds = static_cast<double>(waveformSamples) / sampleRate;
    result.summary = QStringLiteral("FT8: 79 symbols, %1 samples, %2 s")
                         .arg(waveformSamples)
                         .arg(result.durationSeconds, 0, 'f', 3);
    return result;
}
