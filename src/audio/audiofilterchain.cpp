#include "audiofilterchain.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <memory>
#include <vector>

namespace {
constexpr double PI = 3.14159265358979323846;

double finiteClamped(double value, double fallback, double low, double high) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return (std::clamp)(value, low, high);
}

AudioFilterStage normalizedStage(AudioFilterStage stage) {
    stage.frequencyHz = finiteClamped(stage.frequencyHz, 1000.0, 1.0, 22000.0);
    stage.frequency2Hz = finiteClamped(stage.frequency2Hz, 5000.0, 1.0, 22000.0);
    stage.q = finiteClamped(stage.q, 0.707, 0.1, 30.0);
    stage.gainDb = finiteClamped(stage.gainDb, 0.0, -36.0, 36.0);
    stage.thresholdDb = finiteClamped(stage.thresholdDb, -12.0, -80.0, 0.0);
    stage.ratio = finiteClamped(stage.ratio, 3.0, 1.0, 100.0);
    stage.attackMs = finiteClamped(stage.attackMs, 10.0, 0.1, 2000.0);
    stage.releaseMs = finiteClamped(stage.releaseMs, 120.0, 1.0, 5000.0);
    stage.timeConstantUs = finiteClamped(stage.timeConstantUs, 50.0, 10.0, 2000.0);
    stage.bandwidthHz = finiteClamped(stage.bandwidthHz, 250.0, 10.0, 20000.0);
    stage.amount = finiteClamped(stage.amount, 0.8, 0.0, 1.0);
    for (AudioFilterPoint &point : stage.curve) {
        point.frequencyHz = finiteClamped(point.frequencyHz, 1000.0, 20.0, 20000.0);
        point.gainDb = finiteClamped(point.gainDb, 0.0, -24.0, 24.0);
    }
    std::sort(stage.curve.begin(), stage.curve.end(),
              [](const AudioFilterPoint &a, const AudioFilterPoint &b) {
                  return a.frequencyHz < b.frequencyHz;
              });
    if (stage.curve.size() > 32) {
        stage.curve.resize(32);
    }
    if (stage.kind == AudioFilterKind::CwFilter) {
        stage.q = finiteClamped(stage.frequencyHz / stage.bandwidthHz, 2.8, 0.1, 30.0);
    }
    if (stage.kind == AudioFilterKind::BandPass && stage.frequencyHz > stage.frequency2Hz) {
        std::swap(stage.frequencyHz, stage.frequency2Hz);
    }
    return stage;
}

struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

BiquadCoefficients makeBiquad(AudioFilterKind kind,
                              double frequencyHz,
                              double q,
                              double sampleRate,
                              double gainDb = 0.0) {
    BiquadCoefficients result;
    const double nyquist = sampleRate * 0.5;
    const double frequency = (std::clamp)(frequencyHz, 1.0, nyquist * 0.98);
    const double w0 = 2.0 * PI * frequency / sampleRate;
    const double cosine = std::cos(w0);
    const double sine = std::sin(w0);
    const double alpha = sine / (2.0 * (std::max)(0.1, q));
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cosine;
    double a2 = 1.0 - alpha;
    const double amplitude = std::pow(10.0, gainDb / 40.0);

    switch (kind) {
    case AudioFilterKind::LowPass:
        b0 = (1.0 - cosine) * 0.5;
        b1 = 1.0 - cosine;
        b2 = b0;
        break;
    case AudioFilterKind::HighPass:
    case AudioFilterKind::DcBlocker:
        b0 = (1.0 + cosine) * 0.5;
        b1 = -(1.0 + cosine);
        b2 = b0;
        break;
    case AudioFilterKind::Notch:
        b0 = 1.0;
        b1 = -2.0 * cosine;
        b2 = 1.0;
        break;
    case AudioFilterKind::CwFilter:
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        break;
    case AudioFilterKind::ParametricEq:
        b0 = 1.0 + alpha * amplitude;
        b1 = -2.0 * cosine;
        b2 = 1.0 - alpha * amplitude;
        a0 = 1.0 + alpha / amplitude;
        a1 = -2.0 * cosine;
        a2 = 1.0 - alpha / amplitude;
        break;
    case AudioFilterKind::LowShelf: {
        const double rootAmplitude = std::sqrt(amplitude);
        const double shelfAlpha = sine / (2.0 * (std::max)(0.1, q));
        const double twoRootAlpha = 2.0 * rootAmplitude * shelfAlpha;
        b0 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine + twoRootAlpha);
        b1 = 2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosine);
        b2 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine - twoRootAlpha);
        a0 = (amplitude + 1.0) + (amplitude - 1.0) * cosine + twoRootAlpha;
        a1 = -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine);
        a2 = (amplitude + 1.0) + (amplitude - 1.0) * cosine - twoRootAlpha;
        break;
    }
    case AudioFilterKind::HighShelf: {
        const double rootAmplitude = std::sqrt(amplitude);
        const double shelfAlpha = sine / (2.0 * (std::max)(0.1, q));
        const double twoRootAlpha = 2.0 * rootAmplitude * shelfAlpha;
        b0 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine + twoRootAlpha);
        b1 = -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosine);
        b2 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine - twoRootAlpha);
        a0 = (amplitude + 1.0) - (amplitude - 1.0) * cosine + twoRootAlpha;
        a1 = 2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosine);
        a2 = (amplitude + 1.0) - (amplitude - 1.0) * cosine - twoRootAlpha;
        break;
    }
    default:
        return result;
    }

    result.b0 = b0 / a0;
    result.b1 = b1 / a0;
    result.b2 = b2 / a0;
    result.a1 = a1 / a0;
    result.a2 = a2 / a0;
    return result;
}

double biquadMagnitude(const BiquadCoefficients &coefficients,
                       double frequencyHz,
                       double sampleRate) {
    const double omega = 2.0 * PI * frequencyHz / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -omega);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> numerator = coefficients.b0 +
                                           coefficients.b1 * z1 +
                                           coefficients.b2 * z2;
    const std::complex<double> denominator = 1.0 +
                                             coefficients.a1 * z1 +
                                             coefficients.a2 * z2;
    const double magnitude = std::abs(numerator / denominator);
    return std::isfinite(magnitude) ? magnitude : 0.0;
}

double customCurveDb(const QVector<AudioFilterPoint> &curve, double frequencyHz) {
    if (curve.isEmpty()) {
        return 0.0;
    }
    if (frequencyHz <= curve.first().frequencyHz) {
        return curve.first().gainDb;
    }
    if (frequencyHz >= curve.last().frequencyHz) {
        return curve.last().gainDb;
    }
    const double logFrequency = std::log(frequencyHz);
    for (int i = 1; i < curve.size(); ++i) {
        if (frequencyHz <= curve.at(i).frequencyHz) {
            const AudioFilterPoint &left = curve.at(i - 1);
            const AudioFilterPoint &right = curve.at(i);
            const double denominator = std::log(right.frequencyHz) - std::log(left.frequencyHz);
            const double ratio = denominator > 1.0e-12
                                     ? (logFrequency - std::log(left.frequencyHz)) / denominator
                                     : 0.0;
            return left.gainDb + ratio * (right.gainDb - left.gainDb);
        }
    }
    return curve.last().gainDb;
}

class Biquad {
public:
    void set(const BiquadCoefficients &value) {
        coefficients = value;
    }

    float process(float sample) {
        const double output = coefficients.b0 * sample + z1;
        z1 = coefficients.b1 * sample - coefficients.a1 * output + z2;
        z2 = coefficients.b2 * sample - coefficients.a2 * output;
        return std::isfinite(output) ? static_cast<float>(output) : 0.0f;
    }

    void reset() {
        z1 = 0.0;
        z2 = 0.0;
    }

private:
    BiquadCoefficients coefficients;
    double z1 = 0.0;
    double z2 = 0.0;
};

class SpectralNoiseReducer {
public:
    SpectralNoiseReducer() {
        timeInput = static_cast<float *>(fftwf_malloc(sizeof(float) * FFT_SIZE));
        timeOutput = static_cast<float *>(fftwf_malloc(sizeof(float) * FFT_SIZE));
        spectrum = static_cast<fftwf_complex *>(
            fftwf_malloc(sizeof(fftwf_complex) * (FFT_SIZE / 2 + 1)));
        if (!timeInput || !timeOutput || !spectrum) {
            return;
        }
        forwardPlan = fftwf_plan_dft_r2c_1d(FFT_SIZE, timeInput, spectrum, FFTW_ESTIMATE);
        inversePlan = fftwf_plan_dft_c2r_1d(FFT_SIZE, spectrum, timeOutput, FFTW_ESTIMATE);
        overlap.assign(FFT_SIZE, 0.0f);
        noiseFloor.assign(FFT_SIZE / 2 + 1, 0.0f);
    }

    ~SpectralNoiseReducer() {
        if (forwardPlan) fftwf_destroy_plan(forwardPlan);
        if (inversePlan) fftwf_destroy_plan(inversePlan);
        if (timeInput) fftwf_free(timeInput);
        if (timeOutput) fftwf_free(timeOutput);
        if (spectrum) fftwf_free(spectrum);
    }

    float process(float sample, const AudioFilterStage &stage, double sampleRate) {
        input.push_back(sample);
        if (input.size() >= FFT_SIZE) {
            processFrame(stage, sampleRate);
            input.erase(input.begin(), input.begin() + HOP_SIZE);
        }
        if (output.empty()) {
            return 0.0f;
        }
        const float value = output.front();
        output.pop_front();
        return value;
    }

    void reset() {
        input.clear();
        output.clear();
        std::fill(overlap.begin(), overlap.end(), 0.0f);
        std::fill(noiseFloor.begin(), noiseFloor.end(), 0.0f);
        initialized = false;
    }

private:
    static constexpr int FFT_SIZE = 512;
    static constexpr int HOP_SIZE = FFT_SIZE / 2;

    void processFrame(const AudioFilterStage &stage, double sampleRate) {
        if (!forwardPlan || !inversePlan) {
            for (int i = 0; i < HOP_SIZE; ++i) {
                output.push_back(input[static_cast<std::size_t>(i)]);
            }
            return;
        }
        for (int i = 0; i < FFT_SIZE; ++i) {
            const float hann = 0.5f - 0.5f * std::cos(
                static_cast<float>(2.0 * PI * i / (FFT_SIZE - 1)));
            timeInput[i] = input[static_cast<std::size_t>(i)] * std::sqrt(hann);
        }
        fftwf_execute(forwardPlan);
        const float fallCoefficient = static_cast<float>(1.0 - std::exp(
            -static_cast<double>(HOP_SIZE) /
            ((std::max)(1.0, stage.attackMs * 0.001 * sampleRate))));
        const float riseCoefficient = static_cast<float>(1.0 - std::exp(
            -static_cast<double>(HOP_SIZE) /
            ((std::max)(1.0, stage.releaseMs * 0.001 * sampleRate))));
        const float minimumGain = static_cast<float>(std::pow(10.0, stage.thresholdDb / 20.0));
        for (int bin = 0; bin <= FFT_SIZE / 2; ++bin) {
            const float real = spectrum[bin][0];
            const float imaginary = spectrum[bin][1];
            const float magnitude = std::sqrt(real * real + imaginary * imaginary);
            if (!initialized) {
                noiseFloor[static_cast<std::size_t>(bin)] = magnitude;
            } else {
                float &noise = noiseFloor[static_cast<std::size_t>(bin)];
                const float coefficient = magnitude < noise ? fallCoefficient : riseCoefficient;
                noise += coefficient * (magnitude - noise);
            }
            const float noise = noiseFloor[static_cast<std::size_t>(bin)];
            const float rawGain = (std::max)(minimumGain,
                1.0f - static_cast<float>(stage.ratio) * noise /
                       (std::max)(magnitude, 1.0e-12f));
            const float gain = static_cast<float>(1.0 - stage.amount) +
                               static_cast<float>(stage.amount) * rawGain;
            spectrum[bin][0] *= gain;
            spectrum[bin][1] *= gain;
        }
        initialized = true;
        fftwf_execute(inversePlan);
        for (int i = 0; i < FFT_SIZE; ++i) {
            const float hann = 0.5f - 0.5f * std::cos(
                static_cast<float>(2.0 * PI * i / (FFT_SIZE - 1)));
            overlap[static_cast<std::size_t>(i)] +=
                (timeOutput[i] / FFT_SIZE) * std::sqrt(hann);
        }
        for (int i = 0; i < HOP_SIZE; ++i) {
            output.push_back(overlap[static_cast<std::size_t>(i)]);
        }
        std::move(overlap.begin() + HOP_SIZE, overlap.end(), overlap.begin());
        std::fill(overlap.end() - HOP_SIZE, overlap.end(), 0.0f);
    }

    float *timeInput = nullptr;
    float *timeOutput = nullptr;
    fftwf_complex *spectrum = nullptr;
    fftwf_plan forwardPlan = nullptr;
    fftwf_plan inversePlan = nullptr;
    std::vector<float> input;
    std::deque<float> output;
    std::vector<float> overlap;
    std::vector<float> noiseFloor;
    bool initialized = false;
};

std::vector<float> designCustomFir(const AudioFilterStage &stage, double sampleRate) {
    constexpr int tapCount = 129;
    constexpr int designSize = 512;
    std::vector<float> coefficients(tapCount, 0.0f);
    const int center = (tapCount - 1) / 2;
    for (int tap = 0; tap < tapCount; ++tap) {
        const int offset = tap - center;
        double value = std::pow(10.0, customCurveDb(stage.curve, 0.1) / 20.0);
        for (int bin = 1; bin < designSize / 2; ++bin) {
            const double frequency = static_cast<double>(bin) * sampleRate / designSize;
            const double magnitude = std::pow(10.0,
                customCurveDb(stage.curve, frequency) / 20.0);
            value += 2.0 * magnitude *
                     std::cos(2.0 * PI * bin * offset / designSize);
        }
        const double nyquistMagnitude = std::pow(10.0,
            customCurveDb(stage.curve, sampleRate * 0.5) / 20.0);
        value += nyquistMagnitude * std::cos(PI * offset);
        const double window = 0.5 - 0.5 * std::cos(2.0 * PI * tap / (tapCount - 1));
        coefficients[static_cast<std::size_t>(tap)] =
            static_cast<float>((value / designSize) * window);
    }
    return coefficients;
}

struct RuntimeStage {
    AudioFilterStage settings;
    Biquad first;
    Biquad second;
    float dynamicGain = 1.0f;
    float envelope = 0.0f;
    float releaseCoefficient = 0.0f;
    float attackCoefficient = 0.0f;
    float onePoleCoefficient = 0.0f;
    float onePoleState = 0.0f;
    float previousSample = 0.0f;
    int blankSamplesRemaining = 0;
    std::vector<float> analysisBuffer;
    int analysisIndex = 0;
    int adaptiveMisses = 0;
    double adaptiveFrequencyHz = 0.0;
    std::shared_ptr<SpectralNoiseReducer> spectralReducer;
    std::vector<float> firCoefficients;
    std::vector<float> firHistory;
    std::size_t firIndex = 0;
};

void updateAdaptiveNotch(RuntimeStage &runtime, double sampleRate) {
    if (runtime.analysisBuffer.empty()) {
        return;
    }
    const AudioFilterStage &stage = runtime.settings;
    const double low = (std::min)(stage.frequencyHz, stage.frequency2Hz);
    const double high = (std::max)(stage.frequencyHz, stage.frequency2Hz);
    const double range = (std::max)(20.0, high - low);
    const double step = (std::max)(10.0, range / 160.0);
    double mean = 0.0;
    for (float value : runtime.analysisBuffer) {
        mean += value;
    }
    mean /= static_cast<double>(runtime.analysisBuffer.size());

    double bestPower = 0.0;
    double summedPower = 0.0;
    double bestFrequency = low;
    int candidates = 0;
    for (double frequency = low; frequency <= high; frequency += step) {
        const double omega = 2.0 * PI * frequency / sampleRate;
        const double coefficient = 2.0 * std::cos(omega);
        double s0 = 0.0;
        double s1 = 0.0;
        double s2 = 0.0;
        for (float value : runtime.analysisBuffer) {
            s0 = (static_cast<double>(value) - mean) + coefficient * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
        summedPower += (std::max)(0.0, power);
        ++candidates;
        if (power > bestPower) {
            bestPower = power;
            bestFrequency = frequency;
        }
    }
    const double averagePower = candidates > 0 ? summedPower / candidates : 0.0;
    if (averagePower > 1.0e-12 && bestPower >= averagePower * stage.ratio) {
        runtime.adaptiveFrequencyHz = bestFrequency;
        runtime.first.set(makeBiquad(AudioFilterKind::Notch,
                                     bestFrequency,
                                     stage.q,
                                     sampleRate));
        runtime.adaptiveMisses = 0;
    } else if (++runtime.adaptiveMisses >= 20) {
        runtime.first.set(BiquadCoefficients{});
        runtime.adaptiveFrequencyHz = 0.0;
        runtime.adaptiveMisses = 20;
    }
}
}

QString audioFilterKindId(AudioFilterKind kind) {
    switch (kind) {
    case AudioFilterKind::LowPass: return QStringLiteral("low_pass");
    case AudioFilterKind::HighPass: return QStringLiteral("high_pass");
    case AudioFilterKind::BandPass: return QStringLiteral("band_pass");
    case AudioFilterKind::Notch: return QStringLiteral("notch");
    case AudioFilterKind::DcBlocker: return QStringLiteral("dc_blocker");
    case AudioFilterKind::Gain: return QStringLiteral("gain");
    case AudioFilterKind::Limiter: return QStringLiteral("limiter");
    case AudioFilterKind::NoiseGate: return QStringLiteral("noise_gate");
    case AudioFilterKind::DeEmphasis: return QStringLiteral("de_emphasis");
    case AudioFilterKind::ParametricEq: return QStringLiteral("parametric_eq");
    case AudioFilterKind::LowShelf: return QStringLiteral("low_shelf");
    case AudioFilterKind::HighShelf: return QStringLiteral("high_shelf");
    case AudioFilterKind::Compressor: return QStringLiteral("compressor");
    case AudioFilterKind::AdaptiveNotch: return QStringLiteral("adaptive_notch");
    case AudioFilterKind::NoiseBlanker: return QStringLiteral("noise_blanker");
    case AudioFilterKind::CwFilter: return QStringLiteral("cw_filter");
    case AudioFilterKind::CtcssSuppressor: return QStringLiteral("ctcss_suppressor");
    case AudioFilterKind::SpectralDenoise: return QStringLiteral("spectral_denoise");
    case AudioFilterKind::CustomFir: return QStringLiteral("custom_fir");
    }
    return QStringLiteral("low_pass");
}

AudioFilterKind audioFilterKindFromId(const QString &id, bool *ok) {
    const QString value = id.trimmed().toLower();
    const struct Entry { const char *id; AudioFilterKind kind; } entries[] = {
        {"low_pass", AudioFilterKind::LowPass},
        {"high_pass", AudioFilterKind::HighPass},
        {"band_pass", AudioFilterKind::BandPass},
        {"notch", AudioFilterKind::Notch},
        {"dc_blocker", AudioFilterKind::DcBlocker},
        {"gain", AudioFilterKind::Gain},
        {"limiter", AudioFilterKind::Limiter},
        {"noise_gate", AudioFilterKind::NoiseGate},
        {"de_emphasis", AudioFilterKind::DeEmphasis},
        {"parametric_eq", AudioFilterKind::ParametricEq},
        {"low_shelf", AudioFilterKind::LowShelf},
        {"high_shelf", AudioFilterKind::HighShelf},
        {"compressor", AudioFilterKind::Compressor},
        {"adaptive_notch", AudioFilterKind::AdaptiveNotch},
        {"noise_blanker", AudioFilterKind::NoiseBlanker},
        {"cw_filter", AudioFilterKind::CwFilter},
        {"ctcss_suppressor", AudioFilterKind::CtcssSuppressor},
        {"spectral_denoise", AudioFilterKind::SpectralDenoise},
        {"custom_fir", AudioFilterKind::CustomFir}
    };
    for (const Entry &entry : entries) {
        if (value == QLatin1String(entry.id)) {
            if (ok) *ok = true;
            return entry.kind;
        }
    }
    if (ok) *ok = false;
    return AudioFilterKind::LowPass;
}

QString audioFilterKindName(AudioFilterKind kind, bool ukrainian) {
    if (ukrainian) {
        switch (kind) {
        case AudioFilterKind::LowPass: return QStringLiteral("ФНЧ");
        case AudioFilterKind::HighPass: return QStringLiteral("ФВЧ");
        case AudioFilterKind::BandPass: return QStringLiteral("Смуговий фільтр");
        case AudioFilterKind::Notch: return QStringLiteral("Режекторний фільтр");
        case AudioFilterKind::DcBlocker: return QStringLiteral("Блокування DC");
        case AudioFilterKind::Gain: return QStringLiteral("Підсилення");
        case AudioFilterKind::Limiter: return QStringLiteral("Лімітер");
        case AudioFilterKind::NoiseGate: return QStringLiteral("Шумовий поріг");
        case AudioFilterKind::DeEmphasis: return QStringLiteral("FM деемфаза");
        case AudioFilterKind::ParametricEq: return QStringLiteral("Параметричний EQ");
        case AudioFilterKind::LowShelf: return QStringLiteral("Низькочастотна полиця");
        case AudioFilterKind::HighShelf: return QStringLiteral("Високочастотна полиця");
        case AudioFilterKind::Compressor: return QStringLiteral("Компресор / AGC");
        case AudioFilterKind::AdaptiveNotch: return QStringLiteral("Адаптивний notch");
        case AudioFilterKind::NoiseBlanker: return QStringLiteral("Імпульсний noise blanker");
        case AudioFilterKind::CwFilter: return QStringLiteral("Вузький CW-фільтр");
        case AudioFilterKind::CtcssSuppressor: return QStringLiteral("Придушення CTCSS/DCS");
        case AudioFilterKind::SpectralDenoise: return QStringLiteral("Спектральне шумозниження");
        case AudioFilterKind::CustomFir: return QStringLiteral("Користувацький FIR/EQ");
        }
    }
    switch (kind) {
    case AudioFilterKind::LowPass: return QStringLiteral("Low-pass");
    case AudioFilterKind::HighPass: return QStringLiteral("High-pass");
    case AudioFilterKind::BandPass: return QStringLiteral("Band-pass");
    case AudioFilterKind::Notch: return QStringLiteral("Notch");
    case AudioFilterKind::DcBlocker: return QStringLiteral("DC blocker");
    case AudioFilterKind::Gain: return QStringLiteral("Gain");
    case AudioFilterKind::Limiter: return QStringLiteral("Limiter");
    case AudioFilterKind::NoiseGate: return QStringLiteral("Noise gate");
    case AudioFilterKind::DeEmphasis: return QStringLiteral("FM de-emphasis");
    case AudioFilterKind::ParametricEq: return QStringLiteral("Parametric EQ");
    case AudioFilterKind::LowShelf: return QStringLiteral("Low shelf");
    case AudioFilterKind::HighShelf: return QStringLiteral("High shelf");
    case AudioFilterKind::Compressor: return QStringLiteral("Compressor / AGC");
    case AudioFilterKind::AdaptiveNotch: return QStringLiteral("Adaptive notch");
    case AudioFilterKind::NoiseBlanker: return QStringLiteral("Impulse noise blanker");
    case AudioFilterKind::CwFilter: return QStringLiteral("Narrow CW filter");
    case AudioFilterKind::CtcssSuppressor: return QStringLiteral("CTCSS/DCS suppression");
    case AudioFilterKind::SpectralDenoise: return QStringLiteral("Spectral denoise");
    case AudioFilterKind::CustomFir: return QStringLiteral("Custom FIR/EQ");
    }
    return QStringLiteral("Filter");
}

AudioFilterStage defaultAudioFilterStage(AudioFilterKind kind) {
    AudioFilterStage stage;
    stage.kind = kind;
    switch (kind) {
    case AudioFilterKind::LowPass:
        stage.frequencyHz = 3200.0;
        break;
    case AudioFilterKind::HighPass:
        stage.frequencyHz = 250.0;
        break;
    case AudioFilterKind::BandPass:
        stage.frequencyHz = 300.0;
        stage.frequency2Hz = 3000.0;
        break;
    case AudioFilterKind::Notch:
        stage.frequencyHz = 1000.0;
        stage.q = 8.0;
        break;
    case AudioFilterKind::DcBlocker:
        stage.frequencyHz = 30.0;
        stage.q = 0.707;
        break;
    case AudioFilterKind::Gain:
        stage.gainDb = 3.0;
        break;
    case AudioFilterKind::Limiter:
        stage.thresholdDb = -3.0;
        stage.releaseMs = 100.0;
        break;
    case AudioFilterKind::NoiseGate:
        stage.thresholdDb = -45.0;
        stage.releaseMs = 180.0;
        break;
    case AudioFilterKind::DeEmphasis:
        stage.timeConstantUs = 50.0;
        break;
    case AudioFilterKind::ParametricEq:
        stage.frequencyHz = 1000.0;
        stage.q = 1.0;
        stage.gainDb = 0.0;
        break;
    case AudioFilterKind::LowShelf:
        stage.frequencyHz = 250.0;
        stage.q = 0.707;
        stage.gainDb = 3.0;
        break;
    case AudioFilterKind::HighShelf:
        stage.frequencyHz = 3000.0;
        stage.q = 0.707;
        stage.gainDb = -3.0;
        break;
    case AudioFilterKind::Compressor:
        stage.thresholdDb = -18.0;
        stage.ratio = 3.0;
        stage.attackMs = 8.0;
        stage.releaseMs = 180.0;
        stage.gainDb = 3.0;
        break;
    case AudioFilterKind::AdaptiveNotch:
        stage.frequencyHz = 300.0;
        stage.frequency2Hz = 3500.0;
        stage.q = 12.0;
        stage.ratio = 6.0;
        break;
    case AudioFilterKind::NoiseBlanker:
        stage.ratio = 6.0;
        stage.attackMs = 0.2;
        stage.releaseMs = 20.0;
        break;
    case AudioFilterKind::CwFilter:
        stage.frequencyHz = 700.0;
        stage.bandwidthHz = 250.0;
        stage.q = stage.frequencyHz / stage.bandwidthHz;
        break;
    case AudioFilterKind::CtcssSuppressor:
        stage.frequencyHz = 250.0;
        stage.q = 0.707;
        break;
    case AudioFilterKind::SpectralDenoise:
        stage.amount = 0.8;
        stage.thresholdDb = -18.0;
        stage.ratio = 1.5;
        stage.attackMs = 60.0;
        stage.releaseMs = 1200.0;
        break;
    case AudioFilterKind::CustomFir:
        for (double frequency : {60.0, 120.0, 250.0, 500.0, 1000.0,
                                 2000.0, 4000.0, 8000.0, 16000.0}) {
            stage.curve.push_back({frequency, 0.0});
        }
        break;
    }
    return stage;
}

QVector<AudioFilterStage> decodeAudioFilterChain(const QString &json, bool *ok) {
    QVector<AudioFilterStage> result;
    if (json.trimmed().isEmpty()) {
        if (ok) *ok = true;
        return result;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        if (ok) *ok = false;
        return result;
    }
    for (const QJsonValue &value : document.array()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        bool kindOk = false;
        AudioFilterStage stage = defaultAudioFilterStage(
            audioFilterKindFromId(object.value(QStringLiteral("type")).toString(), &kindOk));
        if (!kindOk) {
            continue;
        }
        stage.id = object.value(QStringLiteral("id")).toString().trimmed().left(80);
        stage.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        stage.customName = object.value(QStringLiteral("name")).toString().trimmed().left(80);
        stage.frequencyHz = object.value(QStringLiteral("frequencyHz")).toDouble(stage.frequencyHz);
        stage.frequency2Hz = object.value(QStringLiteral("frequency2Hz")).toDouble(stage.frequency2Hz);
        stage.q = object.value(QStringLiteral("q")).toDouble(stage.q);
        stage.gainDb = object.value(QStringLiteral("gainDb")).toDouble(stage.gainDb);
        stage.thresholdDb = object.value(QStringLiteral("thresholdDb")).toDouble(stage.thresholdDb);
        stage.ratio = object.value(QStringLiteral("ratio")).toDouble(stage.ratio);
        stage.attackMs = object.value(QStringLiteral("attackMs")).toDouble(stage.attackMs);
        stage.releaseMs = object.value(QStringLiteral("releaseMs")).toDouble(stage.releaseMs);
        stage.timeConstantUs = object.value(QStringLiteral("timeConstantUs")).toDouble(stage.timeConstantUs);
        stage.bandwidthHz = object.value(QStringLiteral("bandwidthHz")).toDouble(stage.bandwidthHz);
        stage.amount = object.value(QStringLiteral("amount")).toDouble(stage.amount);
        if (object.value(QStringLiteral("curve")).isArray()) {
            stage.curve.clear();
            for (const QJsonValue &pointValue : object.value(QStringLiteral("curve")).toArray()) {
                const QJsonObject pointObject = pointValue.toObject();
                stage.curve.push_back({
                    pointObject.value(QStringLiteral("frequencyHz")).toDouble(1000.0),
                    pointObject.value(QStringLiteral("gainDb")).toDouble(0.0)
                });
            }
        }
        result.push_back(normalizedStage(stage));
        if (result.size() >= 32) {
            break;
        }
    }
    if (ok) *ok = true;
    return result;
}

QString encodeAudioFilterChain(const QVector<AudioFilterStage> &stages) {
    QJsonArray array;
    for (int i = 0; i < stages.size() && i < 32; ++i) {
        const AudioFilterStage stage = normalizedStage(stages.at(i));
        QJsonObject object;
        if (!stage.id.isEmpty()) {
            object.insert(QStringLiteral("id"), stage.id);
        }
        object.insert(QStringLiteral("type"), audioFilterKindId(stage.kind));
        object.insert(QStringLiteral("enabled"), stage.enabled);
        if (!stage.customName.trimmed().isEmpty()) {
            object.insert(QStringLiteral("name"), stage.customName.trimmed().left(80));
        }
        object.insert(QStringLiteral("frequencyHz"), stage.frequencyHz);
        object.insert(QStringLiteral("frequency2Hz"), stage.frequency2Hz);
        object.insert(QStringLiteral("q"), stage.q);
        object.insert(QStringLiteral("gainDb"), stage.gainDb);
        object.insert(QStringLiteral("thresholdDb"), stage.thresholdDb);
        object.insert(QStringLiteral("ratio"), stage.ratio);
        object.insert(QStringLiteral("attackMs"), stage.attackMs);
        object.insert(QStringLiteral("releaseMs"), stage.releaseMs);
        object.insert(QStringLiteral("timeConstantUs"), stage.timeConstantUs);
        object.insert(QStringLiteral("bandwidthHz"), stage.bandwidthHz);
        object.insert(QStringLiteral("amount"), stage.amount);
        if (!stage.curve.isEmpty()) {
            QJsonArray points;
            for (const AudioFilterPoint &point : stage.curve) {
                QJsonObject pointObject;
                pointObject.insert(QStringLiteral("frequencyHz"), point.frequencyHz);
                pointObject.insert(QStringLiteral("gainDb"), point.gainDb);
                points.append(pointObject);
            }
            object.insert(QStringLiteral("curve"), points);
        }
        array.append(object);
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

double audioFilterMagnitudeDb(const AudioFilterStage &sourceStage,
                              double frequencyHz,
                              double sampleRate) {
    const AudioFilterStage stage = normalizedStage(sourceStage);
    const double boundedRate = (std::max)(1000.0, sampleRate);
    const double boundedFrequency = (std::clamp)(frequencyHz, 0.1, boundedRate * 0.499);
    double magnitude = 1.0;
    switch (stage.kind) {
    case AudioFilterKind::LowPass:
    case AudioFilterKind::HighPass:
    case AudioFilterKind::Notch:
    case AudioFilterKind::DcBlocker:
        magnitude = biquadMagnitude(makeBiquad(stage.kind,
                                                stage.frequencyHz,
                                                stage.q,
                                                boundedRate),
                                     boundedFrequency,
                                     boundedRate);
        break;
    case AudioFilterKind::BandPass:
        magnitude = biquadMagnitude(makeBiquad(AudioFilterKind::HighPass,
                                                stage.frequencyHz,
                                                stage.q,
                                                boundedRate),
                                     boundedFrequency,
                                     boundedRate) *
                    biquadMagnitude(makeBiquad(AudioFilterKind::LowPass,
                                                stage.frequency2Hz,
                                                stage.q,
                                                boundedRate),
                                     boundedFrequency,
                                     boundedRate);
        break;
    case AudioFilterKind::ParametricEq:
    case AudioFilterKind::LowShelf:
    case AudioFilterKind::HighShelf:
        magnitude = biquadMagnitude(makeBiquad(stage.kind,
                                                stage.frequencyHz,
                                                stage.q,
                                                boundedRate,
                                                stage.gainDb),
                                     boundedFrequency,
                                     boundedRate);
        break;
    case AudioFilterKind::DeEmphasis: {
        const double tauSeconds = stage.timeConstantUs * 1.0e-6;
        magnitude = 1.0 / std::sqrt(1.0 + std::pow(2.0 * PI * boundedFrequency * tauSeconds, 2.0));
        break;
    }
    case AudioFilterKind::CwFilter:
        magnitude = biquadMagnitude(makeBiquad(AudioFilterKind::CwFilter,
                                                stage.frequencyHz,
                                                stage.q,
                                                boundedRate),
                                     boundedFrequency,
                                     boundedRate);
        break;
    case AudioFilterKind::CtcssSuppressor:
        magnitude = biquadMagnitude(makeBiquad(AudioFilterKind::HighPass,
                                                stage.frequencyHz,
                                                stage.q,
                                                boundedRate),
                                     boundedFrequency,
                                     boundedRate);
        break;
    case AudioFilterKind::CustomFir:
        return customCurveDb(stage.curve, boundedFrequency);
    case AudioFilterKind::Gain:
        return stage.gainDb;
    case AudioFilterKind::Limiter:
    case AudioFilterKind::NoiseGate:
    case AudioFilterKind::Compressor:
    case AudioFilterKind::AdaptiveNotch:
    case AudioFilterKind::NoiseBlanker:
    case AudioFilterKind::SpectralDenoise:
        return 0.0;
    }
    return 20.0 * std::log10((std::max)(1.0e-6, magnitude));
}

class AudioFilterChain::Impl {
public:
    QString configuration;
    double sampleRate = 48000.0;
    std::vector<RuntimeStage> stages;

    void configure(const QString &json, double requestedSampleRate) {
        const double rate = (std::max)(1000.0, requestedSampleRate);
        if (json == configuration && std::abs(rate - sampleRate) < 0.5) {
            return;
        }
        configuration = json;
        sampleRate = rate;
        bool ok = false;
        const QVector<AudioFilterStage> decoded = decodeAudioFilterChain(json, &ok);
        stages.clear();
        if (!ok) {
            return;
        }
        stages.reserve(static_cast<std::size_t>(decoded.size()));
        for (const AudioFilterStage &source : decoded) {
            RuntimeStage runtime;
            runtime.settings = normalizedStage(source);
            const AudioFilterStage &stage = runtime.settings;
            if (stage.kind == AudioFilterKind::BandPass) {
                runtime.first.set(makeBiquad(AudioFilterKind::HighPass,
                                             stage.frequencyHz,
                                             stage.q,
                                             sampleRate));
                runtime.second.set(makeBiquad(AudioFilterKind::LowPass,
                                              stage.frequency2Hz,
                                              stage.q,
                                              sampleRate));
            } else if (stage.kind == AudioFilterKind::LowPass ||
                       stage.kind == AudioFilterKind::HighPass ||
                       stage.kind == AudioFilterKind::Notch ||
                       stage.kind == AudioFilterKind::DcBlocker) {
                runtime.first.set(makeBiquad(stage.kind,
                                             stage.frequencyHz,
                                             stage.q,
                                             sampleRate));
            } else if (stage.kind == AudioFilterKind::ParametricEq ||
                       stage.kind == AudioFilterKind::LowShelf ||
                       stage.kind == AudioFilterKind::HighShelf) {
                runtime.first.set(makeBiquad(stage.kind,
                                             stage.frequencyHz,
                                             stage.q,
                                             sampleRate,
                                             stage.gainDb));
            } else if (stage.kind == AudioFilterKind::CwFilter) {
                runtime.first.set(makeBiquad(AudioFilterKind::CwFilter,
                                             stage.frequencyHz,
                                             stage.q,
                                             sampleRate));
            } else if (stage.kind == AudioFilterKind::CtcssSuppressor) {
                runtime.first.set(makeBiquad(AudioFilterKind::HighPass,
                                             stage.frequencyHz,
                                             stage.q,
                                             sampleRate));
            } else if (stage.kind == AudioFilterKind::AdaptiveNotch) {
                runtime.analysisBuffer.assign(512, 0.0f);
            } else if (stage.kind == AudioFilterKind::SpectralDenoise) {
                runtime.spectralReducer = std::make_shared<SpectralNoiseReducer>();
            } else if (stage.kind == AudioFilterKind::CustomFir) {
                runtime.firCoefficients = designCustomFir(stage, sampleRate);
                runtime.firHistory.assign(runtime.firCoefficients.size(), 0.0f);
            }
            runtime.attackCoefficient = static_cast<float>(
                1.0 - std::exp(-1.0 / ((stage.attackMs * 0.001) * sampleRate)));
            runtime.releaseCoefficient = static_cast<float>(
                1.0 - std::exp(-1.0 / ((stage.releaseMs * 0.001) * sampleRate)));
            if (stage.kind == AudioFilterKind::DeEmphasis) {
                const double tauSeconds = stage.timeConstantUs * 1.0e-6;
                runtime.onePoleCoefficient = static_cast<float>(
                    std::exp(-1.0 / ((std::max)(1.0e-9, tauSeconds) * sampleRate)));
            }
            stages.push_back(runtime);
        }
    }
};

AudioFilterChain::AudioFilterChain()
    : impl(std::make_unique<Impl>()) {
}

AudioFilterChain::~AudioFilterChain() = default;

void AudioFilterChain::configure(const QString &json, double sampleRate) {
    impl->configure(json, sampleRate);
}

void AudioFilterChain::reset() {
    for (RuntimeStage &stage : impl->stages) {
        stage.first.reset();
        stage.second.reset();
        stage.dynamicGain = 1.0f;
        stage.envelope = 0.0f;
        stage.onePoleState = 0.0f;
        stage.previousSample = 0.0f;
        stage.blankSamplesRemaining = 0;
        stage.analysisIndex = 0;
        stage.adaptiveMisses = 0;
        stage.firIndex = 0;
        std::fill(stage.firHistory.begin(), stage.firHistory.end(), 0.0f);
        if (stage.spectralReducer) {
            stage.spectralReducer->reset();
        }
    }
}

void AudioFilterChain::process(std::vector<short> &samples) {
    if (samples.empty() || impl->stages.empty()) {
        return;
    }
    for (short &pcm : samples) {
        float sample = static_cast<float>(pcm) / 32768.0f;
        for (RuntimeStage &runtime : impl->stages) {
            const AudioFilterStage &stage = runtime.settings;
            if (!stage.enabled) {
                continue;
            }
            switch (stage.kind) {
            case AudioFilterKind::LowPass:
            case AudioFilterKind::HighPass:
            case AudioFilterKind::Notch:
            case AudioFilterKind::DcBlocker:
                sample = runtime.first.process(sample);
                break;
            case AudioFilterKind::BandPass:
                sample = runtime.second.process(runtime.first.process(sample));
                break;
            case AudioFilterKind::Gain:
                sample *= static_cast<float>(std::pow(10.0, stage.gainDb / 20.0));
                break;
            case AudioFilterKind::Limiter: {
                const float threshold = static_cast<float>(std::pow(10.0, stage.thresholdDb / 20.0));
                const float absolute = std::fabs(sample);
                const float target = absolute > threshold && absolute > 1.0e-9f
                                         ? threshold / absolute
                                         : 1.0f;
                if (target < runtime.dynamicGain) {
                    runtime.dynamicGain = target;
                } else {
                    runtime.dynamicGain += runtime.releaseCoefficient *
                                           (target - runtime.dynamicGain);
                }
                sample *= runtime.dynamicGain;
                break;
            }
            case AudioFilterKind::NoiseGate: {
                const float threshold = static_cast<float>(std::pow(10.0, stage.thresholdDb / 20.0));
                const float absolute = std::fabs(sample);
                const float envelopeCoefficient = absolute > runtime.envelope ? 0.08f
                                                                               : runtime.releaseCoefficient;
                runtime.envelope += envelopeCoefficient * (absolute - runtime.envelope);
                const float target = runtime.envelope >= threshold ? 1.0f : 0.0f;
                const float coefficient = target > runtime.dynamicGain ? 0.02f
                                                                        : runtime.releaseCoefficient;
                runtime.dynamicGain += coefficient * (target - runtime.dynamicGain);
                sample *= runtime.dynamicGain;
                break;
            }
            case AudioFilterKind::DeEmphasis:
                runtime.onePoleState = (1.0f - runtime.onePoleCoefficient) * sample +
                                       runtime.onePoleCoefficient * runtime.onePoleState;
                sample = runtime.onePoleState;
                break;
            case AudioFilterKind::ParametricEq:
            case AudioFilterKind::LowShelf:
            case AudioFilterKind::HighShelf:
                sample = runtime.first.process(sample);
                break;
            case AudioFilterKind::Compressor: {
                const float absolute = std::fabs(sample);
                const float envelopeCoefficient = absolute > runtime.envelope
                                                      ? runtime.attackCoefficient
                                                      : runtime.releaseCoefficient;
                runtime.envelope += envelopeCoefficient * (absolute - runtime.envelope);
                const float levelDb = 20.0f * std::log10((std::max)(runtime.envelope, 1.0e-9f));
                const float overDb = (std::max)(0.0f,
                                                levelDb - static_cast<float>(stage.thresholdDb));
                const float reductionDb = -overDb *
                    (1.0f - 1.0f / static_cast<float>(stage.ratio));
                const float targetGain = std::pow(10.0f,
                    (reductionDb + static_cast<float>(stage.gainDb)) / 20.0f);
                const float gainCoefficient = targetGain < runtime.dynamicGain
                                                  ? runtime.attackCoefficient
                                                  : runtime.releaseCoefficient;
                runtime.dynamicGain += gainCoefficient * (targetGain - runtime.dynamicGain);
                sample *= runtime.dynamicGain;
                break;
            }
            case AudioFilterKind::AdaptiveNotch: {
                if (!runtime.analysisBuffer.empty()) {
                    runtime.analysisBuffer[static_cast<std::size_t>(runtime.analysisIndex++)] = sample;
                    if (runtime.analysisIndex >= static_cast<int>(runtime.analysisBuffer.size())) {
                        runtime.analysisIndex = 0;
                        updateAdaptiveNotch(runtime, impl->sampleRate);
                    }
                }
                sample = runtime.first.process(sample);
                break;
            }
            case AudioFilterKind::NoiseBlanker: {
                const float absolute = std::fabs(sample);
                const float triggerFloor = (std::max)(0.002f,
                    runtime.envelope * static_cast<float>(stage.ratio));
                const bool impulse = runtime.envelope > 1.0e-5f && absolute > triggerFloor;
                if (impulse) {
                    runtime.blankSamplesRemaining = (std::max)(1,
                        static_cast<int>(stage.attackMs * 0.001 * impl->sampleRate));
                } else {
                    runtime.envelope += runtime.releaseCoefficient *
                                        (absolute - runtime.envelope);
                }
                if (runtime.blankSamplesRemaining > 0) {
                    sample = runtime.previousSample;
                    --runtime.blankSamplesRemaining;
                } else {
                    runtime.previousSample = sample;
                }
                break;
            }
            case AudioFilterKind::CwFilter:
            case AudioFilterKind::CtcssSuppressor:
                sample = runtime.first.process(sample);
                break;
            case AudioFilterKind::SpectralDenoise:
                if (runtime.spectralReducer) {
                    sample = runtime.spectralReducer->process(sample, stage, impl->sampleRate);
                }
                break;
            case AudioFilterKind::CustomFir: {
                if (runtime.firCoefficients.empty()) {
                    break;
                }
                runtime.firHistory[runtime.firIndex] = sample;
                double filtered = 0.0;
                std::size_t historyIndex = runtime.firIndex;
                for (float coefficient : runtime.firCoefficients) {
                    filtered += coefficient * runtime.firHistory[historyIndex];
                    historyIndex = historyIndex == 0
                                       ? runtime.firHistory.size() - 1
                                       : historyIndex - 1;
                }
                runtime.firIndex = (runtime.firIndex + 1) % runtime.firHistory.size();
                sample = static_cast<float>(filtered);
                break;
            }
            }
            if (!std::isfinite(sample)) {
                sample = 0.0f;
            }
        }
        sample = (std::clamp)(sample, -1.0f, 1.0f);
        pcm = static_cast<short>(std::lround(sample * 32767.0f));
    }
}

bool AudioFilterChain::empty() const {
    return impl->stages.empty();
}
