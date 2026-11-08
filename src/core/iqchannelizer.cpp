#include "iqchannelizer.h"

#include "channelizerutils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#endif

namespace {
constexpr double TWO_PI = 6.28318530717958647692;
constexpr float CHANNEL_IQ_TARGET_LEVEL = 0.45f;

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
void mixAtvRfBlock(const float *input, std::size_t count, double phase, double step,
                    std::vector<std::complex<float>> &mixed) {
    mixed.resize(count);
    __m128 re = _mm_setr_ps(float(std::cos(phase)), float(std::cos(phase+step)),
                            float(std::cos(phase+2*step)), float(std::cos(phase+3*step)));
    __m128 im = _mm_setr_ps(float(std::sin(phase)), float(std::sin(phase+step)),
                            float(std::sin(phase+2*step)), float(std::sin(phase+3*step)));
    const __m128 stepRe = _mm_set1_ps(float(std::cos(4*step)));
    const __m128 stepIm = _mm_set1_ps(float(std::sin(4*step)));
    const __m128 sign = _mm_set1_ps(-0.0f), infinity = _mm_set1_ps(INFINITY);
    float *output = reinterpret_cast<float *>(mixed.data());
    std::size_t n = 0;
    for (; n + 4 <= count; n += 4) {
        __m128 a = _mm_loadu_ps(input+n*2), b = _mm_loadu_ps(input+n*2+4);
        a = _mm_and_ps(a, _mm_cmplt_ps(_mm_andnot_ps(sign,a),infinity));
        b = _mm_and_ps(b, _mm_cmplt_ps(_mm_andnot_ps(sign,b),infinity));
        const __m128 srcRe = _mm_shuffle_ps(a,b,_MM_SHUFFLE(2,0,2,0));
        const __m128 srcIm = _mm_shuffle_ps(a,b,_MM_SHUFFLE(3,1,3,1));
        const __m128 mixedRe = _mm_sub_ps(_mm_mul_ps(srcRe,re),_mm_mul_ps(srcIm,im));
        const __m128 mixedIm = _mm_add_ps(_mm_mul_ps(srcRe,im),_mm_mul_ps(srcIm,re));
        _mm_storeu_ps(output+n*2,_mm_unpacklo_ps(mixedRe,mixedIm));
        _mm_storeu_ps(output+n*2+4,_mm_unpackhi_ps(mixedRe,mixedIm));
        const __m128 nextRe = _mm_sub_ps(_mm_mul_ps(re,stepRe),_mm_mul_ps(im,stepIm));
        im = _mm_add_ps(_mm_mul_ps(re,stepIm),_mm_mul_ps(im,stepRe));
        re = nextRe;
        if ((n & 4095U) == 4092U) {
            // Re-anchor phase as well as amplitude: float oscillator error
            // must not depend on how the receiver partitions its callbacks.
            const double p = phase + double(n + 4) * step;
            re = _mm_setr_ps(float(std::cos(p)), float(std::cos(p+step)),
                             float(std::cos(p+2*step)), float(std::cos(p+3*step)));
            im = _mm_setr_ps(float(std::sin(p)), float(std::sin(p+step)),
                             float(std::sin(p+2*step)), float(std::sin(p+3*step)));
        }
    }
    for (; n < count; ++n) {
        float i = input[n*2], q = input[n*2+1];
        if (!std::isfinite(i)) i=0;
        if (!std::isfinite(q)) q=0;
        const float r=float(std::cos(phase+n*step)), s=float(std::sin(phase+n*step));
        mixed[n]={i*r-q*s,i*s+q*r};
    }
}
// The factor-two, order-three moving-average filter has exactly the taps
// [1, 3, 3, 1]/8. Compute two decimated complex outputs together, retaining
// the same filter response and the odd-sample carry across USB callbacks.
void decimateAtvPairs(const std::vector<std::complex<float>> &input,
                     std::vector<std::complex<float>> &output,
                     std::array<std::complex<float>, 3> &history, bool &pending) {
    output.resize(input.size()/2+1);
    std::size_t n=0,written=0;
    const auto pair=[&](std::complex<float> a,std::complex<float> b) {
        output[written++]=(b+history[0])*0.125f+(a+history[1])*0.375f;
        history[0]=a;history[1]=b;
    };
    if(pending && !input.empty()) {pair(history[2],input[n++]);pending=false;}
    __m128 previous=_mm_loadu_ps(reinterpret_cast<const float *>(history.data()));
    const auto *src=reinterpret_cast<const float *>(input.data());
    auto *dst=reinterpret_cast<float *>(output.data());
    for(;n+4<=input.size();n+=4) {
        const __m128 a=_mm_loadu_ps(src+n*2),b=_mm_loadu_ps(src+n*2+4);
        const __m128 outer=_mm_add_ps(_mm_shuffle_ps(a,b,_MM_SHUFFLE(3,2,3,2)),
                                      _mm_shuffle_ps(previous,a,_MM_SHUFFLE(1,0,1,0)));
        const __m128 inner=_mm_add_ps(_mm_shuffle_ps(a,b,_MM_SHUFFLE(1,0,1,0)),
                                      _mm_shuffle_ps(previous,a,_MM_SHUFFLE(3,2,3,2)));
        _mm_storeu_ps(dst+written*2,_mm_add_ps(_mm_mul_ps(outer,_mm_set1_ps(0.125f)),
                                              _mm_mul_ps(inner,_mm_set1_ps(0.375f))));
        written+=2;previous=b;
    }
    _mm_storeu_ps(reinterpret_cast<float *>(history.data()),previous);
    if(n+2<=input.size()){pair(input[n],input[n+1]);n+=2;}
    if(n<input.size()){history[2]=input[n];pending=true;}
    output.resize(written);
}
#endif

bool finiteIqSample(float value) {
    if constexpr (sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559) {
        // Classify binary32 without per-sample CRT calls or aliasing violations.
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7f800000U) != 0x7f800000U;
    }
    return std::isfinite(value);
}

double normalizedSinc(double value) {
    if (std::abs(value) < 1.0e-12) {
        return 1.0;
    }
    const double argument = (TWO_PI * 0.5) * value;
    return std::sin(argument) / argument;
}

int frontDecimationFactorForRatio(int totalDecimationFactor) {
    int factor = 1;
    while (factor < 64 && totalDecimationFactor / factor > 4) {
        factor *= 2;
    }
    return (std::min)(factor, 64);
}

int firTapCountForOptions(const IqChannelizer::Options *options, double inputRate,
                         double targetRate, double cutoff) {
    if (!options) return 32;
    int taps = 32;
    switch (options->firQuality) {
    case IqChannelizer::FirQuality::Legacy: break;
    case IqChannelizer::FirQuality::Balanced: taps = 64; break;
    case IqChannelizer::FirQuality::High: taps = 128; break;
    default: return 0;
    }
    if (options->firTapCount != 0) {
        if (options->firTapCount < 16 || options->firTapCount > 256 || options->firTapCount % 2)
            return 0;
        taps = options->firTapCount;
    }
    if (!std::isfinite(options->transitionHz) || options->transitionHz < 0.0) return 0;
    if (options->transitionHz > 0.0) {
        if (options->transitionHz > targetRate - 2.0 * cutoff) return 0;
        // Blackman's approximate full transition width is six bins. Refuse
        // unachievable requests instead of silently capping quality at 256 taps.
        const double required = std::ceil(6.0 * (inputRate / options->transitionHz));
        if (!std::isfinite(required) || required > 256.0) return 0;
        const int evenRequired = (static_cast<int>(required) + 1) & ~1;
        if (options->firTapCount && taps < evenRequired) return 0;
        taps = (std::max)(taps, evenRequired);
    }
    return taps;
}
}

void IqChannelizer::reset(double initialNcoPhaseRadians) {
    ncoPhase = std::remainder(initialNcoPhaseRadians, TWO_PI);
    if (ncoPhase < 0.0) {
        ncoPhase += TWO_PI;
    }
    configuredFrontDecimationFactor = 0;
    configuredDirectFront = false;atvFrontPending = false;atvFrontHistory.fill({0,0});
    frontDecimationWriteIndex = 0;
    frontDecimationCount = 0;
    frontDecimationScale = 1.0f;
    for (auto &delay : frontDecimationDelay) {
        delay.fill({0.0f, 0.0f});
    }
    frontDecimationSums.fill({0.0f, 0.0f});
    decimationSum = {0.0f, 0.0f};
    preLowPassStates.fill({0.0f, 0.0f});
    decimationCount = 0;
    agcLevel = 0.01f;
    configuredCoarseRate = 0.0;
    configuredTargetRate = 0.0;
    configuredCutoff = 0.0;
    configuredTapCount = ResamplerTaps;
    resamplerHistory.fill({0.0f, 0.0f});
    resamplerSamplesSeen = 0;
    resamplerWriteIndex = 0;
    nextOutputPosition = static_cast<double>(ResamplerTaps / 2);
}

void IqChannelizer::configureFrontDecimator(int factor, bool directPairs) {
    factor = (std::clamp)(factor, 1, FrontDecimatorMaxFactor);
    if (configuredFrontDecimationFactor == factor && configuredDirectFront == directPairs) {
        return;
    }

    configuredFrontDecimationFactor = factor;
    configuredDirectFront = directPairs;atvFrontPending = false;atvFrontHistory.fill({0,0});
    frontDecimationWriteIndex = 0;
    frontDecimationCount = 0;
    for (auto &delay : frontDecimationDelay) {
        delay.fill({0.0f, 0.0f});
    }
    frontDecimationSums.fill({0.0f, 0.0f});
    const double factorCubed = static_cast<double>(factor) *
                               static_cast<double>(factor) *
                               static_cast<double>(factor);
    frontDecimationScale = static_cast<float>(1.0 / factorCubed);
    decimationSum = {0.0f, 0.0f};
    preLowPassStates.fill({0.0f, 0.0f});
    decimationCount = 0;
    configuredCoarseRate = 0.0;
    configuredTargetRate = 0.0;
    configuredCutoff = 0.0;
}

bool IqChannelizer::pushFrontDecimatorSample(const std::complex<float> &sample,
                                             std::complex<float> &output) {
    if (configuredFrontDecimationFactor <= 1) {
        output = sample;
        return true;
    }
    std::complex<float> stageValue = sample;
    for (int stage = 0; stage < FrontDecimatorOrder; ++stage) {
        auto &delay = frontDecimationDelay[static_cast<std::size_t>(stage)];
        auto &sum = frontDecimationSums[static_cast<std::size_t>(stage)];
        const std::complex<float> oldest = delay[static_cast<std::size_t>(frontDecimationWriteIndex)];
        delay[static_cast<std::size_t>(frontDecimationWriteIndex)] = stageValue;
        sum += stageValue - oldest;
        stageValue = sum;
    }
    if (++frontDecimationWriteIndex == configuredFrontDecimationFactor)
        frontDecimationWriteIndex = 0;
    ++frontDecimationCount;
    if (frontDecimationCount < configuredFrontDecimationFactor) return false;
    frontDecimationCount = 0;
    output = stageValue * frontDecimationScale;
    return true;
}

void IqChannelizer::configureResampler(double inputRate,
                                       double targetRate,
                                       double cutoff,
                                       int tapCount) {
    if (std::abs(configuredCoarseRate - inputRate) < 0.5 &&
        std::abs(configuredTargetRate - targetRate) < 0.5 &&
        std::abs(configuredCutoff - cutoff) < 0.5 && configuredTapCount == tapCount) {
        return;
    }

    configuredCoarseRate = inputRate;
    configuredTargetRate = targetRate;
    configuredOutputStep = inputRate / targetRate;
    configuredCutoff = cutoff;
    configuredTapCount = tapCount;
    resamplerHistory.fill({0.0f, 0.0f});
    resamplerSamplesSeen = 0;
    resamplerWriteIndex = 0;
    nextOutputPosition = static_cast<double>(tapCount / 2);

    const double normalizedCutoff = (std::clamp)(
        (std::min)(cutoff, targetRate * 0.45) / inputRate,
        0.000001,
        0.495);
    const int leftTaps = tapCount / 2 - 1;
    for (int phase = 0; phase < ResamplerPhases; ++phase) {
        const double fraction = static_cast<double>(phase) /
                                static_cast<double>(ResamplerPhases);
        double sum = 0.0;
        for (int tap = 0; tap < tapCount; ++tap) {
            const double distance = static_cast<double>(leftTaps - tap) + fraction;
            const double windowPosition = static_cast<double>(tap) /
                                          static_cast<double>(tapCount - 1);
            const double blackman = 0.42 - 0.5 * std::cos(TWO_PI * windowPosition) +
                                    0.08 * std::cos(2.0 * TWO_PI * windowPosition);
            const double coefficient =
                2.0 * normalizedCutoff *
                normalizedSinc(2.0 * normalizedCutoff * distance) * blackman;
            resamplerTaps[static_cast<std::size_t>(phase)]
                         [static_cast<std::size_t>(tap)] = static_cast<float>(coefficient);
            sum += coefficient;
        }
        if (std::abs(sum) > 1.0e-12) {
            const float scale = static_cast<float>(1.0 / sum);
            for (int tap = 0; tap < tapCount; ++tap) {
                resamplerTaps[static_cast<std::size_t>(phase)][static_cast<std::size_t>(tap)] *= scale;
            }
        }
    }
}

std::complex<float> IqChannelizer::historySample(std::int64_t sampleIndex) const {
    if (sampleIndex < 0 ||
        static_cast<std::uint64_t>(sampleIndex) >= resamplerSamplesSeen) {
        return {0.0f, 0.0f};
    }
    const std::uint64_t age = resamplerSamplesSeen - 1U -
                              static_cast<std::uint64_t>(sampleIndex);
    if (age >= static_cast<std::uint64_t>(ResamplerHistory)) {
        return {0.0f, 0.0f};
    }
    int index = resamplerWriteIndex - 1 - static_cast<int>(age);
    while (index < 0) {
        index += ResamplerHistory;
    }
    return resamplerHistory[static_cast<std::size_t>(index)];
}

#if defined(_MSC_VER)
__forceinline
#endif
void IqChannelizer::pushResamplerSample(const std::complex<float> &sample,
                                        float *output, std::size_t &written,
                                        bool applyAgc) {
    resamplerHistory[static_cast<std::size_t>(resamplerWriteIndex)] = sample;
    resamplerHistory[static_cast<std::size_t>(resamplerWriteIndex + ResamplerHistory)] = sample;
    resamplerWriteIndex = (resamplerWriteIndex + 1) % ResamplerHistory;
    const double currentPosition = static_cast<double>(resamplerSamplesSeen);
    ++resamplerSamplesSeen;

    const double outputStep = configuredOutputStep;
    const int leftTaps = configuredTapCount / 2 - 1;
    const double filterDelay = static_cast<double>(configuredTapCount / 2);
    while (nextOutputPosition <= currentPosition + 1.0e-9) {
        const double sourcePosition = nextOutputPosition - filterDelay;
        // The first output position equals the filter delay, and advances
        // monotonically; sourcePosition is nonnegative throughout the stream.
        const std::int64_t center = static_cast<std::int64_t>(sourcePosition);
        const double fraction = sourcePosition - static_cast<double>(center);
        const int phase = (std::clamp)(
            static_cast<int>(fraction * static_cast<double>(ResamplerPhases - 1) + 0.5),
            0,
            ResamplerPhases - 1);

        std::complex<float> filtered(0.0f, 0.0f);
        const auto &taps = resamplerTaps[static_cast<std::size_t>(phase)];
        const std::int64_t first = center - leftTaps;
        const std::int64_t last = first + configuredTapCount - 1;
        if (first >= 0 && last < static_cast<std::int64_t>(resamplerSamplesSeen) &&
            resamplerSamplesSeen - 1 - static_cast<std::uint64_t>(first) < ResamplerHistory) {
            const int age = int(resamplerSamplesSeen - 1 - static_cast<std::uint64_t>(first));
            int index = resamplerWriteIndex - 1 - age;
            if (index < 0) index += ResamplerHistory;
            const auto *window = resamplerHistory.data() + index;
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
            const float *iq = reinterpret_cast<const float *>(window);
            __m128 sumLo = _mm_setzero_ps(), sumHi = _mm_setzero_ps();
            int tap = 0;
            for (; tap + 4 <= configuredTapCount; tap += 4) {
                const __m128 coefficients = _mm_loadu_ps(taps.data() + tap);
                sumLo = _mm_add_ps(sumLo, _mm_mul_ps(_mm_loadu_ps(iq + tap * 2),
                                                _mm_unpacklo_ps(coefficients, coefficients)));
                sumHi = _mm_add_ps(sumHi, _mm_mul_ps(_mm_loadu_ps(iq + tap * 2 + 4),
                                                _mm_unpackhi_ps(coefficients, coefficients)));
            }
            alignas(16) float lanes[4];
            _mm_store_ps(lanes, _mm_add_ps(sumLo,sumHi));
            filtered = {lanes[0] + lanes[2], lanes[1] + lanes[3]};
            for (; tap < configuredTapCount; ++tap)
                filtered += window[tap] * taps[static_cast<std::size_t>(tap)];
#else
            for (int tap = 0; tap < configuredTapCount; ++tap)
                filtered += window[tap] * taps[static_cast<std::size_t>(tap)];
#endif
        } else {
            for (int tap = 0; tap < configuredTapCount; ++tap)
                filtered += historySample(first + tap) * taps[static_cast<std::size_t>(tap)];
        }

        float gain = 1.0f;
        if (applyAgc) {
            const float magnitude = std::abs(filtered);
            const float agcCoeff = magnitude > agcLevel ? 0.01f : 0.0002f;
            agcLevel += agcCoeff * (magnitude - agcLevel);
            agcLevel = (std::max)(agcLevel, 0.00001f);
            gain = CHANNEL_IQ_TARGET_LEVEL / agcLevel;
        }
        output[written++] = (std::clamp)(std::real(filtered) * gain, -1.0f, 1.0f);
        output[written++] = (std::clamp)(std::imag(filtered) * gain, -1.0f, 1.0f);
        nextOutputPosition += outputStep;
    }
}

IqChannelizer::Result IqChannelizer::processFloatIq(const float *samples,
                                                    std::size_t floatCount,
                                                    const RadioSettings &settings,
                                                    std::vector<float> &output,
                                                    bool applyAgc,
                                                    const Options *options) {
    output.clear();

    Result result;
    result.inputRate = settings.sampleRate;
    result.centerFrequency = settings.centerFrequency;
    result.listeningFrequency = settings.listeningFrequency;
    result.frequencyShift = settings.listeningFrequency - settings.centerFrequency;

    if (!samples || floatCount < 2 || settings.sampleRate <= 0.0 ||
        !std::isfinite(settings.sampleRate)) {
        return result;
    }

    const std::size_t iqSamples = floatCount / 2U;
    if (iqSamples > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        !std::isfinite(result.frequencyShift)) return result;
    const double targetRate = options ? options->outputRateHz : channelizerTargetRate(settings);
    if (!std::isfinite(targetRate) || targetRate <= 0.0 ||
        (options && (targetRate > settings.sampleRate || !std::isfinite(options->cutoffHz) ||
                     options->cutoffHz <= 0.0 || options->cutoffHz >= targetRate * 0.45))) return result;
    const double ratio = settings.sampleRate / targetRate;
    if (!std::isfinite(ratio) || ratio > (std::numeric_limits<int>::max)()) return result;
    const int totalDecimationFactor = (std::max)(1, static_cast<int>(std::floor(ratio)));
    int frontDecimationFactor =
        frontDecimationFactorForRatio(totalDecimationFactor);
    const int requestedDecimation = options ? options->decimationFactor : 0;
    if (requestedDecimation < 0 || requestedDecimation > 4096 ||
        (requestedDecimation > 0 && settings.sampleRate / requestedDecimation < targetRate)) return result;
    if (requestedDecimation > 0) {
        frontDecimationFactor = 1;
        while (frontDecimationFactor < 64 && requestedDecimation % (frontDecimationFactor * 2) == 0)
            frontDecimationFactor *= 2;
    }
    const double frontOutputRate =
        settings.sampleRate / static_cast<double>(frontDecimationFactor);
    const int coarseDecimationFactor =
        requestedDecimation > 0 ? requestedDecimation / frontDecimationFactor :
        (std::max)(1, static_cast<int>(std::floor(frontOutputRate / targetRate)));
    const double coarseRate =
        frontOutputRate / static_cast<double>(coarseDecimationFactor);
    const double outputRate = (std::min)(targetRate, coarseRate);
    const double cutoff = options ? options->cutoffHz : channelizerCutoff(settings, outputRate);
    const int tapCount = firTapCountForOptions(options, coarseRate, outputRate, cutoff);
    if (tapCount == 0) return result;
    bool directAtvFront = false;
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    directAtvFront = settings.modulationType == MOD_ATV && settings.inputMode == INPUT_RF &&
                     frontDecimationFactor == 2;
#endif
    configureFrontDecimator(frontDecimationFactor, directAtvFront);
    configureResampler(coarseRate, outputRate, cutoff, tapCount);
    const float preLowPassAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI * cutoff / frontOutputRate),
        0.000001,
        1.0));

    const double fShift = result.frequencyShift;
    const double phaseIncrement = -TWO_PI * fShift / settings.sampleRate;
    const bool noFrequencyShift = std::abs(fShift) < 0.5;
    float rotI = 1.0f;
    float rotQ = 0.0f;
    float rotStepI = 1.0f;
    float rotStepQ = 0.0f;
    if (!noFrequencyShift) {
        rotI = static_cast<float>(std::cos(ncoPhase));
        rotQ = static_cast<float>(std::sin(ncoPhase));
        rotStepI = static_cast<float>(std::cos(phaseIncrement));
        rotStepQ = static_cast<float>(std::sin(phaseIncrement));
    }

    const int effectiveDecimationFactor =
        frontDecimationFactor * coarseDecimationFactor;
    const float coarseScale = 1.0f / static_cast<float>(coarseDecimationFactor);
    output.resize((iqSamples / static_cast<std::size_t>(effectiveDecimationFactor) + 8U) * 2U);
    std::size_t written = 0;
    bool vectorMixed = false;
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    if (settings.modulationType == MOD_ATV && settings.inputMode == INPUT_RF && (!noFrequencyShift || directAtvFront)) {
        mixAtvRfBlock(samples, iqSamples, noFrequencyShift ? 0.0 : ncoPhase,
                      noFrequencyShift ? 0.0 : phaseIncrement, mixedAtvInput);
        vectorMixed = true;
    }
    if (directAtvFront) decimateAtvPairs(mixedAtvInput, frontAtvInput, atvFrontHistory, atvFrontPending);
#endif

    const std::size_t processingSamples = directAtvFront ? frontAtvInput.size() : iqSamples;
    for (std::size_t n = 0; n < processingSamples; ++n) {
        std::complex<float> frontDecimatedSample;
        if (directAtvFront) frontDecimatedSample = frontAtvInput[n];
        else {
        std::complex<float> mixedSample;
        if (vectorMixed) mixedSample = mixedAtvInput[n];
        else {
        float iSample = samples[2U * n];
        float qSample = samples[2U * n + 1U];
        if (!finiteIqSample(iSample)) {
            iSample = 0.0f;
        }
        if (!finiteIqSample(qSample)) {
            qSample = 0.0f;
        }

        if (settings.inputMode == INPUT_HF_COMBINED) {
            if (fShift < 0.0) {
                qSample = 0.0f;
            } else {
                iSample = qSample;
                qSample = 0.0f;
            }
        } else if (settings.inputMode == INPUT_HF1) {
            qSample = 0.0f;
        } else if (settings.inputMode == INPUT_HF2) {
            iSample = qSample;
            qSample = 0.0f;
        }

        if (noFrequencyShift) {
            mixedSample = {iSample, qSample};
        } else {
            const float mixedI = iSample * rotI - qSample * rotQ;
            const float mixedQ = iSample * rotQ + qSample * rotI;
            mixedSample = {mixedI, mixedQ};
        }

        if (!noFrequencyShift) {
            const float nextRotI = rotI * rotStepI - rotQ * rotStepQ;
            const float nextRotQ = rotI * rotStepQ + rotQ * rotStepI;
            rotI = nextRotI;
            rotQ = nextRotQ;
            if ((n & 4095U) == 4095U) {
                const float norm = std::sqrt(rotI * rotI + rotQ * rotQ);
                if (norm > 0.0f) {
                    rotI /= norm;
                    rotQ /= norm;
                }
            }
        }

        }
        if (!pushFrontDecimatorSample(mixedSample, frontDecimatedSample)) {
            continue;
        }
        }

        if (coarseDecimationFactor > 1) {
            preLowPassStates[0] +=
                preLowPassAlpha * (frontDecimatedSample - preLowPassStates[0]);
            preLowPassStates[1] +=
                preLowPassAlpha * (preLowPassStates[0] - preLowPassStates[1]);
            preLowPassStates[2] +=
                preLowPassAlpha * (preLowPassStates[1] - preLowPassStates[2]);
            frontDecimatedSample = preLowPassStates[2];
        }

        decimationSum += frontDecimatedSample;
        ++decimationCount;
        if (decimationCount < coarseDecimationFactor) {
            continue;
        }

        std::complex<float> channelSample = decimationSum * coarseScale;
        decimationSum = {0.0f, 0.0f};
        decimationCount = 0;

        pushResamplerSample(channelSample, output.data(), written, applyAgc);
    }
    output.resize(written);

    ncoPhase = noFrequencyShift
                   ? 0.0
                   : std::remainder(ncoPhase + phaseIncrement * static_cast<double>(iqSamples),
                                    TWO_PI);
    if (ncoPhase < 0.0) {
        ncoPhase += TWO_PI;
    }

    result.valid = true;
    result.outputRate = outputRate;
    result.cutoff = cutoff;
    result.targetRate = targetRate;
    result.decimationFactor = effectiveDecimationFactor;
    result.frontDecimationFactor = frontDecimationFactor;
    result.coarseDecimationFactor = coarseDecimationFactor;
    result.inputSamples = static_cast<int>(iqSamples);
    result.outputSamples = static_cast<int>(output.size() / 2U);
    result.agcLevel = agcLevel;
    result.firTapCount = configuredTapCount;
    result.firDelayCoarseSamples = configuredTapCount / 2.0;
    return result;
}
