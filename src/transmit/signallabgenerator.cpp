#include "signallabgenerator.h"
#include "fft.h"

#include <fftw3.h>
#include <QRegExp>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <random>

namespace {
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr int kMaximumSamples = 20000000;
constexpr int kMaximumSampleRate = 1000000000;

double wrapUnit(double value) {
    value -= std::floor(value);
    return value;
}

bool isPowerOfTwo(int value) {
    return value >= 2 && (value & (value - 1)) == 0;
}

std::complex<double> constellationPoint(SignalLabConstellation constellation,
                                        std::mt19937 &random) {
    switch (constellation) {
    case SignalLabConstellation::Bpsk:
        return (random() & 1U) ? std::complex<double>(1.0, 0.0)
                               : std::complex<double>(-1.0, 0.0);
    case SignalLabConstellation::Qpsk: {
        static const std::complex<double> points[] = {
            {1.0, 1.0}, {-1.0, 1.0}, {-1.0, -1.0}, {1.0, -1.0}};
        return points[random() & 3U] / std::sqrt(2.0);
    }
    case SignalLabConstellation::Psk8: {
        const int index = static_cast<int>(random() & 7U);
        const double phase = (2.0 * kPi * index / 8.0) + kPi / 8.0;
        return std::polar(1.0, phase);
    }
    case SignalLabConstellation::Qam16:
    case SignalLabConstellation::Qam64:
    case SignalLabConstellation::Qam256: {
        const int side = constellation == SignalLabConstellation::Qam16
                             ? 4
                             : (constellation == SignalLabConstellation::Qam64 ? 8 : 16);
        const int i = static_cast<int>(random() % static_cast<unsigned>(side));
        const int q = static_cast<int>(random() % static_cast<unsigned>(side));
        const double x = 2.0 * i - side + 1.0;
        const double y = 2.0 * q - side + 1.0;
        const double normalization = std::sqrt((2.0 / 3.0) * (side * side - 1.0));
        return {x / normalization, y / normalization};
    }
    }
    return {1.0, 0.0};
}

SignalLabConstellation waveformConstellation(SignalLabWaveform waveform) {
    switch (waveform) {
    case SignalLabWaveform::Bpsk: return SignalLabConstellation::Bpsk;
    case SignalLabWaveform::Qpsk: return SignalLabConstellation::Qpsk;
    case SignalLabWaveform::Psk8: return SignalLabConstellation::Psk8;
    case SignalLabWaveform::Qam16: return SignalLabConstellation::Qam16;
    case SignalLabWaveform::Qam64: return SignalLabConstellation::Qam64;
    case SignalLabWaveform::Qam256: return SignalLabConstellation::Qam256;
    default: return SignalLabConstellation::Qpsk;
    }
}

double rootRaisedCosine(double t, double beta) {
    beta = (std::clamp)(beta, 0.01, 1.0);
    const double absT = std::abs(t);
    if (absT < 1.0e-9) {
        return 1.0 + beta * (4.0 / kPi - 1.0);
    }
    const double singular = 1.0 / (4.0 * beta);
    if (std::abs(absT - singular) < 1.0e-7) {
        const double a = (1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * beta));
        const double b = (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * beta));
        return (beta / std::sqrt(2.0)) * (a + b);
    }
    const double numerator = std::sin(kPi * t * (1.0 - beta)) +
                             4.0 * beta * t * std::cos(kPi * t * (1.0 + beta));
    const double denominator = kPi * t * (1.0 - 16.0 * beta * beta * t * t);
    return numerator / denominator;
}

QVector<double> parseToneList(const QString &text, double nyquist) {
    QVector<double> tones;
    const QStringList parts = text.split(QRegExp(QStringLiteral("[,;\\s]+")),
                                         Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        bool ok = false;
        const double value = part.toDouble(&ok);
        if (ok && std::isfinite(value) && std::abs(value) < nyquist) tones.append(value);
    }
    return tones;
}

void applyFrequencyShift(QVector<std::complex<float>> *iq,
                         int sampleRate,
                         double offsetHz) {
    if (!iq || iq->isEmpty() || std::abs(offsetHz) < 1.0e-12) return;
    const double step = 2.0 * kPi * offsetHz / sampleRate;
    double phase = 0.0;
    for (std::complex<float> &sample : *iq) {
        const std::complex<float> oscillator(static_cast<float>(std::cos(phase)),
                                             static_cast<float>(std::sin(phase)));
        sample *= oscillator;
        phase += step;
        if (phase > kPi) phase -= 2.0 * kPi;
        if (phase < -kPi) phase += 2.0 * kPi;
    }
}

void normalizeAndMeasure(SignalLabGenerationResult *result, double requestedPeak) {
    if (!result || result->iq.isEmpty()) return;
    double peak = 0.0;
    double power = 0.0;
    for (const std::complex<float> &sample : result->iq) {
        const double magnitude = std::abs(sample);
        peak = (std::max)(peak, magnitude);
        power += std::norm(sample);
    }
    const double scale = peak > 1.0e-12 ? requestedPeak / peak : 1.0;
    result->normalizationGain = scale;
    if (std::abs(scale - 1.0) > 1.0e-12) {
        for (std::complex<float> &sample : result->iq) {
            sample *= static_cast<float>(scale);
        }
        power *= scale * scale;
        peak *= scale;
    }
    result->peak = peak;
    result->rms = std::sqrt(power / result->iq.size());
    result->paprDb = result->rms > 1.0e-12
                         ? 20.0 * std::log10(result->peak / result->rms)
                         : 0.0;
}

bool generatePulseShaped(const SignalLabConfiguration &configuration,
                         int sampleCount,
                         SignalLabGenerationResult *result) {
    const int sampleRate = configuration.sampleRate;
    const double requestedRate = configuration.symbolRate;
    if (!(requestedRate >= 1.0) || requestedRate > sampleRate * 0.45) {
        result->error = QStringLiteral("Symbol rate must be between 1 Hz and 45% of the sample rate");
        return false;
    }
    if (!std::isfinite(configuration.rolloff)) {
        result->error = QStringLiteral("RRC rolloff must be finite");
        return false;
    }
    const int samplesPerSymbol = (std::max)(2, static_cast<int>(std::llround(sampleRate / requestedRate)));
    result->samplesPerSymbol = samplesPerSymbol;
    result->pulseRolloff = (std::clamp)(configuration.rolloff, 0.01, 1.0);
    const double actualRate = static_cast<double>(sampleRate) / samplesPerSymbol;
    const int symbolCount = sampleCount / samplesPerSymbol + 12;
    std::mt19937 random(configuration.randomSeed);
    QVector<std::complex<double>> symbols(symbolCount);
    if (configuration.waveform == SignalLabWaveform::Pi4Dqpsk) {
        std::complex<double> phase(1.0, 0.0);
        static const double steps[] = {kPi / 4.0, 3.0 * kPi / 4.0,
                                       -3.0 * kPi / 4.0, -kPi / 4.0};
        for (std::complex<double> &symbol : symbols) {
            phase *= std::polar(1.0, steps[random() & 3U]);
            symbol = phase;
        }
    } else {
        const SignalLabConstellation constellation = waveformConstellation(configuration.waveform);
        for (std::complex<double> &symbol : symbols) {
            symbol = constellationPoint(constellation, random);
        }
    }

    result->iq.resize(sampleCount);
    constexpr int span = 4;
    for (int sample = 0; sample < sampleCount; ++sample) {
        const double symbolTime = static_cast<double>(sample) / samplesPerSymbol + span;
        const int center = static_cast<int>(std::floor(symbolTime));
        std::complex<double> value(0.0, 0.0);
        for (int offset = -span; offset <= span; ++offset) {
            const int index = center + offset;
            if (index < 0 || index >= symbols.size()) continue;
            const double pulse = rootRaisedCosine(symbolTime - index, configuration.rolloff);
            value += symbols[index] * pulse;
        }
        result->iq[sample] = {static_cast<float>(value.real()), static_cast<float>(value.imag())};
    }
    result->occupiedBandwidthHz = actualRate * (1.0 + (std::clamp)(configuration.rolloff, 0.01, 1.0));
    return true;
}

bool generateFsk(const SignalLabConfiguration &configuration,
                 int sampleCount,
                 SignalLabGenerationResult *result) {
    if (!(configuration.symbolRate >= 1.0) ||
        configuration.symbolRate > configuration.sampleRate * 0.45) {
        result->error = QStringLiteral("FSK symbol rate is outside the valid range");
        return false;
    }
    const int samplesPerSymbol = (std::max)(2, static_cast<int>(std::llround(
        static_cast<double>(configuration.sampleRate) / configuration.symbolRate)));
    const double actualRate = static_cast<double>(configuration.sampleRate) / samplesPerSymbol;
    std::mt19937 random(configuration.randomSeed);
    double target = (random() & 1U) ? 1.0 : -1.0;
    double shaped = target;
    double phase = 0.0;
    const bool gaussian = configuration.waveform == SignalLabWaveform::Gfsk2;
    const double alpha = gaussian
                             ? 1.0 - std::exp(-2.0 * kPi *
                                              (std::clamp)(configuration.rolloff, 0.05, 1.0) /
                                              samplesPerSymbol)
                             : 1.0;
    result->iq.resize(sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
        if ((sample % samplesPerSymbol) == 0) target = (random() & 1U) ? 1.0 : -1.0;
        shaped += alpha * (target - shaped);
        phase += 2.0 * kPi * configuration.fskDeviationHz * shaped /
                 configuration.sampleRate;
        result->iq[sample] = {static_cast<float>(std::cos(phase)),
                              static_cast<float>(std::sin(phase))};
    }
    result->occupiedBandwidthHz = 2.0 * std::abs(configuration.fskDeviationHz) +
                                  actualRate * (gaussian ? 0.75 : 1.0);
    return true;
}

bool generateOfdm(const SignalLabConfiguration &configuration,
                  SignalLabGenerationResult *result) {
    const int fftSize = configuration.ofdmFftSize;
    if (!isPowerOfTwo(fftSize) || fftSize < 64 || fftSize > 4096) {
        result->error = QStringLiteral("OFDM FFT size must be a power of two from 64 to 4096");
        return false;
    }
    const int maximumActive = configuration.ofdmNullDc ? fftSize - 2 : fftSize - 1;
    const int active = (std::clamp)(configuration.ofdmActiveCarriers, 2, maximumActive);
    const int cp = (std::clamp)(configuration.ofdmCyclicPrefix, 0, fftSize / 2);
    const int symbols = (std::clamp)(configuration.ofdmSymbolCount, 1, 10000);
    const qint64 totalSamples = static_cast<qint64>(symbols) * (fftSize + cp);
    if (totalSamples > kMaximumSamples) {
        result->error = QStringLiteral("OFDM request exceeds the 20 million sample laboratory limit");
        return false;
    }

    fftwf_complex *frequency = fftwf_alloc_complex(fftSize);
    fftwf_complex *time = fftwf_alloc_complex(fftSize);
    if (!frequency || !time) {
        if (frequency) fftwf_free(frequency);
        if (time) fftwf_free(time);
        result->error = QStringLiteral("Unable to allocate OFDM FFT buffers");
        return false;
    }
    fftwf_plan plan = nullptr;
    {
        std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
        plan = fftwf_plan_dft_1d(fftSize, frequency, time, FFTW_BACKWARD, FFTW_ESTIMATE);
    }
    if (!plan) {
        fftwf_free(frequency);
        fftwf_free(time);
        result->error = QStringLiteral("Unable to create OFDM inverse FFT plan");
        return false;
    }

    std::mt19937 random(configuration.randomSeed);
    result->iq.reserve(static_cast<int>(totalSamples));
    const int negative = active / 2;
    const int positive = configuration.ofdmNullDc
                             ? active - negative
                             : active - negative - 1;
    const int pilotSpacing = (std::max)(0, configuration.ofdmPilotSpacing);
    const int edgeSamples = (std::clamp)(
        static_cast<int>(std::llround((fftSize + cp) * configuration.ofdmEdgeWindowPercent / 100.0)),
        0, (fftSize + cp) / 4);

    for (int symbolIndex = 0; symbolIndex < symbols; ++symbolIndex) {
        for (int i = 0; i < fftSize; ++i) {
            frequency[i][0] = 0.0f;
            frequency[i][1] = 0.0f;
        }
        int logicalCarrier = 0;
        for (int carrier = -negative; carrier <= positive; ++carrier) {
            if (carrier == 0 && configuration.ofdmNullDc) continue;
            if (logicalCarrier >= active) continue;
            std::complex<double> point;
            if (pilotSpacing > 0 && (logicalCarrier % pilotSpacing) == 0) {
                point = ((symbolIndex + logicalCarrier / pilotSpacing) & 1)
                            ? std::complex<double>(-1.0, 0.0)
                            : std::complex<double>(1.0, 0.0);
            } else {
                point = constellationPoint(configuration.ofdmConstellation, random);
            }
            const int bin = carrier < 0 ? fftSize + carrier : carrier;
            frequency[bin][0] = static_cast<float>(point.real());
            frequency[bin][1] = static_cast<float>(point.imag());
            ++logicalCarrier;
        }
        fftwf_execute(plan);
        QVector<std::complex<float>> block(fftSize + cp);
        for (int i = 0; i < cp; ++i) {
            const int source = fftSize - cp + i;
            block[i] = {time[source][0] / fftSize, time[source][1] / fftSize};
        }
        for (int i = 0; i < fftSize; ++i) {
            block[cp + i] = {time[i][0] / fftSize, time[i][1] / fftSize};
        }
        if (edgeSamples > 0) {
            for (int i = 0; i < edgeSamples; ++i) {
                const float gain = static_cast<float>(0.5 - 0.5 *
                    std::cos(kPi * static_cast<double>(i + 1) / edgeSamples));
                block[i] *= gain;
                block[block.size() - 1 - i] *= gain;
            }
        }
        result->iq += block;
    }

    {
        std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
        fftwf_destroy_plan(plan);
    }
    fftwf_free(frequency);
    fftwf_free(time);
    result->occupiedBandwidthHz = static_cast<double>(active) * configuration.sampleRate / fftSize;
    return true;
}
}

QVector<std::complex<float>> SignalLabGenerator::matchedSymbols(
    const SignalLabGenerationResult &result, int maximumPoints, QVector<int> *sampleIndices) {
    QVector<std::complex<float>> symbols;
    if (sampleIndices) sampleIndices->clear();
    if (!result.ok() || result.samplesPerSymbol < 2 || result.sampleRate <= 0 ||
        !std::isfinite(result.pulseRolloff) || !std::isfinite(result.centerOffsetHz)) return symbols;
    constexpr qint64 span = 4;
    const qint64 sps = result.samplesPerSymbol;
    const qint64 firstCenter = span * sps;
    const qint64 lastCenter = result.iq.size() - 1 - span * sps;
    if (firstCenter > lastCenter) return symbols;
    maximumPoints = (std::clamp)(maximumPoints, 1, 2500);
    const qint64 count = (lastCenter - firstCenter) / sps + 1;
    const qint64 symbolStep = (std::max<qint64>)(1, (count + maximumPoints - 1) / maximumPoints);
    // Oversampled, derotated baseband is band-limited. Sampling the matched
    // pulse at up to 64 points/symbol bounds preview work even at gigasample rates.
    const qint64 sampleStep = (std::max<qint64>)(1, (sps + 63) / 64);
    const int halfTaps = static_cast<int>(span * sps / sampleStep);
    QVector<std::complex<double>> taps(halfTaps * 2 + 1);
    for (int tap = -halfTaps; tap <= halfTaps; ++tap) {
        const qint64 offset = tap * sampleStep;
        const double pulse = rootRaisedCosine(double(offset) / sps, result.pulseRolloff);
        const double phase = -2.0 * kPi * result.centerOffsetHz * offset / result.sampleRate;
        taps[tap + halfTaps] = (pulse * sampleStep / sps) * std::polar(1.0, phase);
    }
    for (qint64 index = 0; index < count; index += symbolStep) {
        const qint64 center = firstCenter + index * sps;
        std::complex<double> value(0, 0);
        for (int tap = -halfTaps; tap <= halfTaps; ++tap) {
            const auto sample = result.iq.at(static_cast<int>(center + tap * sampleStep));
            value += std::complex<double>(sample.real(), sample.imag()) * taps[tap + halfTaps];
        }
        value *= std::polar(1.0, -2.0 * kPi * result.centerOffsetHz * center / result.sampleRate);
        symbols.append({static_cast<float>(value.real()), static_cast<float>(value.imag())});
        if (sampleIndices) sampleIndices->append(static_cast<int>(center));
    }
    return symbols;
}

QString SignalLabGenerator::waveformName(SignalLabWaveform waveform) {
    switch (waveform) {
    case SignalLabWaveform::Carrier: return QStringLiteral("Carrier");
    case SignalLabWaveform::Sine: return QStringLiteral("Sine / DSB");
    case SignalLabWaveform::Square: return QStringLiteral("Square");
    case SignalLabWaveform::Triangle: return QStringLiteral("Triangle");
    case SignalLabWaveform::Sawtooth: return QStringLiteral("Sawtooth");
    case SignalLabWaveform::Pulse: return QStringLiteral("Pulse train");
    case SignalLabWaveform::MultiTone: return QStringLiteral("Multi-tone");
    case SignalLabWaveform::LinearChirp: return QStringLiteral("Linear chirp");
    case SignalLabWaveform::WhiteNoise: return QStringLiteral("Complex white noise");
    case SignalLabWaveform::Bpsk: return QStringLiteral("BPSK");
    case SignalLabWaveform::Qpsk: return QStringLiteral("QPSK");
    case SignalLabWaveform::Psk8: return QStringLiteral("8PSK");
    case SignalLabWaveform::Pi4Dqpsk: return QStringLiteral("pi/4-DQPSK");
    case SignalLabWaveform::Qam16: return QStringLiteral("16-QAM");
    case SignalLabWaveform::Qam64: return QStringLiteral("64-QAM");
    case SignalLabWaveform::Qam256: return QStringLiteral("256-QAM");
    case SignalLabWaveform::Fsk2: return QStringLiteral("2-FSK");
    case SignalLabWaveform::Gfsk2: return QStringLiteral("2-GFSK");
    case SignalLabWaveform::Ofdm: return QStringLiteral("OFDM");
    }
    return QStringLiteral("Unknown");
}

QString SignalLabGenerator::constellationName(SignalLabConstellation constellation) {
    switch (constellation) {
    case SignalLabConstellation::Bpsk: return QStringLiteral("BPSK");
    case SignalLabConstellation::Qpsk: return QStringLiteral("QPSK");
    case SignalLabConstellation::Psk8: return QStringLiteral("8PSK");
    case SignalLabConstellation::Qam16: return QStringLiteral("16-QAM");
    case SignalLabConstellation::Qam64: return QStringLiteral("64-QAM");
    case SignalLabConstellation::Qam256: return QStringLiteral("256-QAM");
    }
    return QStringLiteral("QPSK");
}

SignalLabGenerationResult SignalLabGenerator::generate(
    const SignalLabConfiguration &configuration) {
    SignalLabGenerationResult result;
    result.sampleRate = configuration.sampleRate;
    result.centerOffsetHz = configuration.centerOffsetHz;
    if (configuration.sampleRate < 8000 || configuration.sampleRate > kMaximumSampleRate) {
        result.error = QStringLiteral("Sample rate must be between 8 ksps and 1 Gsps");
        return result;
    }
    if (!std::isfinite(configuration.amplitude) || configuration.amplitude <= 0.0 ||
        configuration.amplitude > 1.0) {
        result.error = QStringLiteral("Amplitude must be greater than zero and no more than one");
        return result;
    }
    if (!std::isfinite(configuration.centerOffsetHz) ||
        std::abs(configuration.centerOffsetHz) >= configuration.sampleRate * 0.5) {
        result.error = QStringLiteral("Center offset must remain inside the Nyquist range");
        return result;
    }

    if (configuration.waveform == SignalLabWaveform::Ofdm) {
        if (!generateOfdm(configuration, &result)) return result;
    } else {
        if (!(configuration.durationSeconds > 0.0) ||
            !std::isfinite(configuration.durationSeconds)) {
            result.error = QStringLiteral("Duration must be positive");
            return result;
        }
        const double requested = configuration.durationSeconds * configuration.sampleRate;
        if (!std::isfinite(requested) || requested > kMaximumSamples + 0.49) {
            result.error = QStringLiteral("Request exceeds the 20 million sample buffer; shorten the duration");
            return result;
        }
        const qint64 requestedSamples = static_cast<qint64>(std::llround(requested));
        if (requestedSamples < 1 || requestedSamples > kMaximumSamples) {
            result.error = QStringLiteral("Request must contain from 1 to 20 million samples");
            return result;
        }
        const int sampleCount = static_cast<int>(requestedSamples);
        const bool pulseShaped = configuration.waveform == SignalLabWaveform::Bpsk ||
                                 configuration.waveform == SignalLabWaveform::Qpsk ||
                                 configuration.waveform == SignalLabWaveform::Psk8 ||
                                 configuration.waveform == SignalLabWaveform::Pi4Dqpsk ||
                                 configuration.waveform == SignalLabWaveform::Qam16 ||
                                 configuration.waveform == SignalLabWaveform::Qam64 ||
                                 configuration.waveform == SignalLabWaveform::Qam256;
        if (pulseShaped) {
            if (!generatePulseShaped(configuration, sampleCount, &result)) return result;
        } else if (configuration.waveform == SignalLabWaveform::Fsk2 ||
                   configuration.waveform == SignalLabWaveform::Gfsk2) {
            if (!generateFsk(configuration, sampleCount, &result)) return result;
        } else {
            result.iq.resize(sampleCount);
            std::mt19937 random(configuration.randomSeed);
            std::normal_distribution<float> normal(0.0f, 1.0f);
            QVector<double> tones;
            if (configuration.waveform == SignalLabWaveform::MultiTone) {
                tones = parseToneList(configuration.multiToneFrequencies,
                                      configuration.sampleRate * 0.5);
                if (tones.isEmpty()) {
                    result.error = QStringLiteral("Multi-tone list contains no valid frequencies");
                    return result;
                }
            }
            double chirpPhase = 0.0;
            for (int sample = 0; sample < sampleCount; ++sample) {
                const double t = static_cast<double>(sample) / configuration.sampleRate;
                const double phase = 2.0 * kPi * configuration.toneFrequencyHz * t;
                std::complex<double> value(0.0, 0.0);
                switch (configuration.waveform) {
                case SignalLabWaveform::Carrier:
                    value = {1.0, 0.0};
                    result.occupiedBandwidthHz = 0.0;
                    break;
                case SignalLabWaveform::Sine:
                    value = {std::sin(phase), 0.0};
                    result.occupiedBandwidthHz = 2.0 * std::abs(configuration.toneFrequencyHz);
                    break;
                case SignalLabWaveform::Square:
                    value = {std::sin(phase) >= 0.0 ? 1.0 : -1.0, 0.0};
                    result.occupiedBandwidthHz = (std::min)(configuration.sampleRate * 0.9,
                                                           20.0 * std::abs(configuration.toneFrequencyHz));
                    break;
                case SignalLabWaveform::Triangle: {
                    const double cycle = wrapUnit(configuration.toneFrequencyHz * t);
                    value = {4.0 * std::abs(cycle - 0.5) - 1.0, 0.0};
                    result.occupiedBandwidthHz = (std::min)(configuration.sampleRate * 0.9,
                                                           12.0 * std::abs(configuration.toneFrequencyHz));
                    break;
                }
                case SignalLabWaveform::Sawtooth:
                    value = {2.0 * wrapUnit(configuration.toneFrequencyHz * t) - 1.0, 0.0};
                    result.occupiedBandwidthHz = (std::min)(configuration.sampleRate * 0.9,
                                                           20.0 * std::abs(configuration.toneFrequencyHz));
                    break;
                case SignalLabWaveform::Pulse:
                    value = {wrapUnit(configuration.toneFrequencyHz * t) <
                                     (std::clamp)(configuration.dutyCycle, 0.001, 0.999)
                                 ? 1.0 : 0.0, 0.0};
                    result.occupiedBandwidthHz = (std::min)(configuration.sampleRate * 0.9,
                                                           20.0 * std::abs(configuration.toneFrequencyHz));
                    break;
                case SignalLabWaveform::MultiTone:
                    for (double tone : tones) value += std::polar(1.0, 2.0 * kPi * tone * t);
                    value /= static_cast<double>(tones.size());
                    for (double tone : tones) {
                        result.occupiedBandwidthHz = (std::max)(result.occupiedBandwidthHz,
                                                               2.0 * std::abs(tone));
                    }
                    break;
                case SignalLabWaveform::LinearChirp: {
                    const double ratio = sampleCount > 1
                                             ? static_cast<double>(sample) / (sampleCount - 1)
                                             : 0.0;
                    const double instant = configuration.toneFrequencyHz +
                        (configuration.secondFrequencyHz - configuration.toneFrequencyHz) * ratio;
                    chirpPhase += 2.0 * kPi * instant / configuration.sampleRate;
                    value = std::polar(1.0, chirpPhase);
                    result.occupiedBandwidthHz = std::abs(configuration.secondFrequencyHz -
                                                          configuration.toneFrequencyHz);
                    break;
                }
                case SignalLabWaveform::WhiteNoise:
                    value = {normal(random), normal(random)};
                    result.occupiedBandwidthHz = configuration.sampleRate * 0.95;
                    break;
                default:
                    break;
                }
                result.iq[sample] = {static_cast<float>(value.real()),
                                     static_cast<float>(value.imag())};
            }
        }
    }

    double spectralHalfSpanHz = result.occupiedBandwidthHz * 0.5;
    if (configuration.waveform == SignalLabWaveform::LinearChirp) {
        spectralHalfSpanHz = (std::max)(std::abs(configuration.toneFrequencyHz),
                                        std::abs(configuration.secondFrequencyHz));
    }
    const double nyquistGuardHz = configuration.sampleRate * 0.49;
    if (std::abs(configuration.centerOffsetHz) + spectralHalfSpanHz >
        nyquistGuardHz) {
        result.error = QStringLiteral(
            "Signal bandwidth and center offset exceed the Nyquist range");
        result.iq.clear();
        return result;
    }

    applyFrequencyShift(&result.iq, configuration.sampleRate,
                        configuration.centerOffsetHz);
    normalizeAndMeasure(&result, configuration.amplitude);
    result.sampleRate = configuration.sampleRate;
    result.durationSeconds = static_cast<double>(result.iq.size()) /
                             configuration.sampleRate;
    result.occupiedBandwidthHz = (std::min)(
        configuration.sampleRate * 0.98, result.occupiedBandwidthHz);
    result.summary = QStringLiteral("%1 | %2 samples @ %3 Hz | %4 s | BW %5 Hz | peak %6 | RMS %7 | PAPR %8 dB")
                         .arg(waveformName(configuration.waveform))
                         .arg(result.iq.size())
                         .arg(result.sampleRate)
                         .arg(result.durationSeconds, 0, 'f', 4)
                         .arg(result.occupiedBandwidthHz, 0, 'f', 1)
                         .arg(result.peak, 0, 'f', 4)
                         .arg(result.rms, 0, 'f', 4)
                         .arg(result.paprDb, 0, 'f', 2);
    if (result.samplesPerSymbol > 0) {
        result.summary += QStringLiteral(" | %1 sym/s").arg(
            double(result.sampleRate) / result.samplesPerSymbol, 0, 'f', 3);
    }
    return result;
}
