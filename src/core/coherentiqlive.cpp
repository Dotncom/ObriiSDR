#include "coherentiqlive.h"

#include "coherentiqaligner.h"
#include "fftplannermutex.h"
#include "ffttransformcore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>

namespace {
using Complex = std::complex<double>;
using SampleIndex = std::uint64_t;
constexpr double TwoPi = 6.28318530717958647693;
constexpr SampleIndex PhaseMeasurementSamples = 8 * CoherentIqLive::BlockSamples;
constexpr int BandFilterHalf = 32;

bool usable(const DspIqSourceSnapshot &input, CoherentIqLive::Timeline timeline) {
    return input.source.isValid() && input.sampleClockValid && input.continuityId != 0 &&
           (timeline == CoherentIqLive::Timeline::Signal ||
            (input.sharedSampleTimeline && input.clockLocked && !input.clockId.isEmpty())) &&
           input.endComplexSample >= input.startComplexSample && !input.iq.empty() &&
           input.iq.size() % 2 == 0 && input.endComplexSample - input.startComplexSample == input.iq.size() / 2 &&
           std::isfinite(input.sampleRateHz) && input.sampleRateHz > 0 && std::isfinite(input.centerHz);
}

SampleIndex shiftIndex(SampleIndex index, int offset) {
    if (offset < 0) return index - std::min(index, SampleIndex(-offset));
    return index + std::min(std::numeric_limits<SampleIndex>::max() - index, SampleIndex(offset));
}

Complex sample(const DspIqSourceSnapshot &input, SampleIndex index) {
    const auto i = std::size_t(index - input.startComplexSample) * 2;
    return {input.iq[i], input.iq[i + 1]};
}

std::vector<float> slice(const DspIqSourceSnapshot &input, SampleIndex first, SampleIndex end) {
    const auto begin = input.iq.begin() + std::ptrdiff_t(2 * (first - input.startComplexSample));
    return {begin, begin + std::ptrdiff_t(2 * (end - first))};
}

struct IqPower {
    double total = 0.0;
    double noise = 0.0;

    bool hasMeasurableSignal() const { return total > 0 && noise < 0.8 * total; }
};

IqPower measureIqPower(FftTransformCore &fft, const std::vector<float> &iq) {
    constexpr int Length = CoherentIqLive::BlockSamples;
    // The 20th percentile of complex white-noise bin power is -log(0.8)
    // times the mean. Hann window energy is 3(N-1)/8.
    const double normalization = -std::log(0.8) * 3.0 * (Length - 1) / 8.0;
    std::array<double, Length> spectrum;
    IqPower power;
    const auto windows = iq.size() / (2 * Length);
    for (std::size_t window = 0; window < windows; ++window) {
        const float *samples = iq.data() + window * 2 * Length;
        fft.stageInputRange(samples, 0, Length);
        fft.execute();
        for (int n = 0; n < Length; ++n) {
            power.total += std::norm(Complex{samples[2 * n], samples[2 * n + 1]});
            spectrum[n] = std::norm(Complex{fft.output()[n][0], fft.output()[n][1]});
            if (!std::isfinite(spectrum[n])) return {};
        }
        const auto floor = spectrum.begin() + Length / 5;
        std::nth_element(spectrum.begin(), floor, spectrum.end());
        power.noise += *floor / normalization;
    }
    power.total /= windows * Length;
    power.noise /= windows;
    return power;
}

const std::array<std::array<double, 2 * BandFilterHalf>, 2> &bandInterpolator() {
    static const auto coefficients = [] {
        std::array<std::array<double, 2 * BandFilterHalf>, 2> result{};
        // Leave a transition band on each side of the receiver's Nyquist edge.
        constexpr double cutoff = 0.44;
        for (std::size_t phase = 0; phase < result.size(); ++phase) {
            auto &taps = result[phase];
            double sum = 0;
            for (int i = 0; i < int(taps.size()); ++i) {
                const double distance = i - (BandFilterHalf - 1) - double(phase) / 2;
                const double angle = TwoPi * distance / (2 * BandFilterHalf);
                const double window = 0.42 + 0.5 * std::cos(angle) + 0.08 * std::cos(2 * angle);
                const double sinc = distance == 0 ? 2 * cutoff : std::sin(TwoPi * cutoff * distance) / (0.5 * TwoPi * distance);
                sum += taps[std::size_t(i)] = window * sinc;
            }
            for (auto &tap : taps) tap /= sum;
        }
        return result;
    }();
    return coefficients;
}
}

DspSourceIdentity CoherentIqLive::outputIdentity() {
    return DspSourceIdentity::normalized(QStringLiteral("multi-sdr-coherent"), QStringLiteral("rx0"));
}

bool CoherentIqLive::calibrate(const std::vector<DspIqSourceSnapshot> &inputs,
                               int maximumDelay, QString &error) {
    aligned.clear();
    weights.clear();
    phaseBaselineSample.reset();
    measuredDriftHz.reset();
    error.clear();
    if (inputs.size() < 2 || inputs.size() > 8) {
        error = QStringLiteral("Live coherent IQ requires 2–8 sources");
        return false;
    }
    const auto &reference = inputs.front();
    rate = reference.sampleRateHz;
    center = reference.centerHz;
    clock = reference.clockId;
    if (rate > MaximumRate) {
        error = QStringLiteral("Live coherent IQ is limited to 192 kS/s in this build");
        return false;
    }
    auto first = reference.startComplexSample;
    auto end = reference.endComplexSample;
    QSet<QString> sources;
    for (const auto &input : inputs) {
        if (!usable(input, timeline) || input.clockId != clock || input.sampleRateHz != rate ||
            input.centerHz != center || sources.contains(input.source.key()) || input.source == outputIdentity()) {
            error = QStringLiteral("Live sources need verified common sample indices, clock, rate and center");
            return false;
        }
        sources.insert(input.source.key());
        first = std::max(first, input.startComplexSample);
        end = std::min(end, input.endComplexSample);
    }
    if (end <= first || end - first < 2048) {
        error = QStringLiteral("Waiting for 2048 common calibration samples");
        return false;
    }
    first = end - std::min<SampleIndex>(4096, end - first);
    const auto referenceIq = slice(reference, first, end);
    maximumDelay = std::clamp(maximumDelay, 0, MaximumDelay);
    std::vector<Input> calibrated{{reference.source, reference.continuityId}};
    FftTransformCore noiseFft;
    std::vector<IqPower> powers;
    if (options.noiseWeighted) {
        if (noiseFft.allocateBuffers(BlockSamples, FftTransformCore::Storage::OutOfPlace)) {
            const std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
            noiseFft.createPlanLocked();
        }
        if (!noiseFft.ready()) {
            error = QStringLiteral("Could not initialize the coherent IQ noise estimate");
            return false;
        }
        noiseFft.ensureWindow(FFT_WINDOW_HANN);
        powers.push_back(measureIqPower(noiseFft, referenceIq));
    }
    for (auto it = inputs.begin() + 1; it != inputs.end(); ++it) {
        const auto targetIq = slice(*it, first, end);
        const auto value = CoherentIqAligner::calibrate(reference.source, referenceIq, it->source,
            targetIq, rate, center, maximumDelay);
        if (!value.valid || value.coherence < 0.55) {
            error = QStringLiteral("Insufficient shared signal for %1").arg(it->source.key());
            return false;
        }
        const double delay = value.integerDelaySamples + value.fractionalDelaySamples;
        const int whole = int(std::floor(delay));
        calibrated.push_back({it->source, it->continuityId, whole, delay - whole,
            std::polar(value.gainCorrection, value.phaseCorrectionRadians)});
        if (options.noiseWeighted) powers.push_back(measureIqPower(noiseFft, targetIq));
    }
    if (options.noiseWeighted) {
        // Freeze the weights for this calibration. A band filled by a common
        // signal cannot provide an independent noise estimate at its floor.
        weights.resize(inputs.size(), 1.0 / double(inputs.size()));
        if (std::all_of(powers.begin(), powers.end(), [](const IqPower &power) { return power.hasMeasurableSignal(); })) {
            const double referenceSignal = powers.front().total - powers.front().noise;
            for (std::size_t i = 0; i < powers.size(); ++i) {
                const auto &power = powers[i];
                const double gainSquared = referenceSignal / (power.total - power.noise);
                calibrated[i].correction *= std::sqrt(gainSquared) / std::abs(calibrated[i].correction);
                weights[i] = 1.0 / std::max(1.0e-15, power.noise * gainSquared);
            }
            const double total = std::accumulate(weights.begin(), weights.end(), 0.0);
            for (auto &weight : weights) weight /= total;
        }
    }
    aligned = std::move(calibrated);
    next = end;
    return true;
}

bool CoherentIqLive::takeBlock(const std::vector<DspIqSourceSnapshot> &inputs,
                              DspIqSourceSnapshot &output, QString &error,
                              std::vector<std::vector<float>> *alignedChannels) {
    output = {};
    error.clear();
    if (alignedChannels) alignedChannels->clear();
    const auto reject = [&](const QString &reason) {
        if (alignedChannels) alignedChannels->clear();
        aligned.clear();
        weights.clear();
        phaseBaselineSample.reset();
        measuredDriftHz.reset();
        error = reason;
        return false;
    };
    if (aligned.empty() || inputs.size() != aligned.size())
        return reject(QStringLiteral("The coherent source group changed"));
    SampleIndex end = std::numeric_limits<SampleIndex>::max();
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto &input = inputs[i];
        const auto &plan = aligned[i];
        if (!usable(input, timeline) || input.source != plan.source || input.continuityId != plan.continuityId ||
            input.clockId != clock || input.sampleRateHz != rate || input.centerHz != center)
            return reject(QStringLiteral("IQ gap, clock or format change in %1").arg(plan.source.key()));
        const auto oldest = shiftIndex(input.startComplexSample, -plan.whole);
        if (next < oldest) return reject(QStringLiteral("Lost unread IQ in %1").arg(plan.source.key()));
        end = std::min(end, shiftIndex(input.endComplexSample, -plan.whole - (plan.fractional > 0 ? 1 : 0)));
    }
    if (end <= next || end - next < BlockSamples) return true;
    if (alignedChannels)
        alignedChannels->resize(inputs.size(), std::vector<float>(BlockSamples * 2));

    const auto phaseSamples = phaseBaselineSample ? next - *phaseBaselineSample : 0;
    const bool measurePhase = phaseSamples >= PhaseMeasurementSamples;
    double largestDriftHz = 0.0;
    std::vector<Complex> sum(BlockSamples);
    const bool weighted = !weights.empty();
    const double referenceWeight = weighted ? weights.front() : 1.0;
    double referencePower = 0.0;
    for (int n = 0; n < BlockSamples; ++n) {
        const auto reference = sample(inputs.front(), next + n);
        sum[n] = reference * referenceWeight;
        referencePower += std::norm(reference);
        if (alignedChannels) {
            (*alignedChannels)[0][2 * n] = float(reference.real());
            (*alignedChannels)[0][2 * n + 1] = float(reference.imag());
        }
    }
    for (std::size_t i = 1; i < inputs.size(); ++i) {
        auto &plan = aligned[i];
        auto index = shiftIndex(next, plan.whole);
        Complex cross{};
        double power = 0.0;
        const double weight = weighted ? weights[i] : 1.0;
        auto rotation = std::polar(1.0, plan.trackingPhase);
        const auto rotationStep = std::polar(1.0, plan.trackingAdvance / BlockSamples);
        for (int n = 0; n < BlockSamples; ++n, ++index) {
            auto corrected = sample(inputs[i], index);
            if (plan.fractional > 0)
                corrected += (sample(inputs[i], index + 1) - corrected) * plan.fractional;
            corrected *= plan.correction;
            cross += sample(inputs.front(), next + n) * std::conj(corrected);
            power += std::norm(corrected);
            if (options.trackPhase) {
                corrected *= rotation;
                rotation *= rotationStep;
            }
            sum[n] += corrected * weight;
            if (alignedChannels) {
                (*alignedChannels)[i][2 * n] = float(corrected.real());
                (*alignedChannels)[i][2 * n + 1] = float(corrected.imag());
            }
        }
        const double norm = std::sqrt(referencePower * power);
        const double phase = std::arg(cross);
        const double residual = std::remainder(phase - plan.trackingPhase - 0.5 * plan.trackingAdvance, TwoPi);
        if (!(norm > 0) || !std::isfinite(norm) || !(std::abs(cross) >= 0.35 * norm) ||
            std::abs(residual) > TwoPi / 6)
            return reject(QStringLiteral("Coherence or phase lock lost in %1").arg(plan.source.key()));
        if (options.trackPhase) {
            if (plan.previousPhase) {
                const double advance = std::remainder(phase - *plan.previousPhase, TwoPi);
                plan.estimatedAdvance += 0.125 * (advance - plan.estimatedAdvance);
            }
            plan.previousPhase = phase;
            // Adjust the slope for the next block. Keep the rotation
            // continuous at the boundary and let it remove the phase error.
            const double boundaryError = residual + 0.5 * (plan.estimatedAdvance - plan.trackingAdvance);
            plan.trackingPhase = std::remainder(plan.trackingPhase + plan.trackingAdvance, TwoPi);
            plan.trackingAdvance = std::clamp(plan.estimatedAdvance + 0.25 * boundaryError, -0.5, 0.5);
            largestDriftHz = std::max(largestDriftHz, std::abs(plan.estimatedAdvance) * rate / (TwoPi * BlockSamples));
        } else if (measurePhase) {
            const double phaseChange = std::remainder(phase - plan.phaseBaseline, TwoPi);
            largestDriftHz = std::max(largestDriftHz, std::abs(phaseChange) * rate / (TwoPi * phaseSamples));
        }
        if (!phaseBaselineSample || measurePhase) plan.phaseBaseline = phase;
    }
    if (measurePhase) measuredDriftHz = largestDriftHz;
    if (!phaseBaselineSample || measurePhase) phaseBaselineSample = next;
    output.source = outputIdentity();
    output.startComplexSample = next;
    next += BlockSamples;
    output.endComplexSample = next;
    output.sampleRateHz = rate;
    output.centerHz = center;
    output.clockId = clock;
    output.sampleClockValid = true;
    output.sharedSampleTimeline = output.clockLocked = timeline == Timeline::Hardware;
    output.iq.resize(BlockSamples * 2);
    for (int n = 0; n < BlockSamples; ++n) {
        const auto mean = weighted ? sum[n] : sum[n] / double(inputs.size());
        output.iq[2 * n] = float(mean.real());
        output.iq[2 * n + 1] = float(mean.imag());
        if (!std::isfinite(output.iq[2 * n]) || !std::isfinite(output.iq[2 * n + 1])) {
            output = {};
            return reject(QStringLiteral("Invalid coherent IQ output"));
        }
    }
    return true;
}

bool CoherentIqBandJoin::configure(const std::vector<DspIqSourceSnapshot> &inputs, QString &error) {
    configured = false;
    if (inputs.size() != 2 || !usable(inputs[0], CoherentIqLive::Timeline::Hardware) ||
        !usable(inputs[1], CoherentIqLive::Timeline::Hardware) ||
        inputs[0].source == inputs[1].source || inputs[0].source == CoherentIqLive::outputIdentity() ||
        inputs[1].source == CoherentIqLive::outputIdentity() || inputs[0].clockId != inputs[1].clockId ||
        inputs[0].sampleRateHz != inputs[1].sampleRateHz || inputs[0].sampleRateHz > CoherentIqLive::MaximumRate ||
        std::abs(std::abs(inputs[0].centerHz - inputs[1].centerHz) - inputs[0].sampleRateHz) > 1e-6) {
        error = QStringLiteral("IQ band stitching needs two synchronized RX at the same rate, with centers spaced by that rate");
        return false;
    }
    lowIndex = inputs[0].centerHz < inputs[1].centerHz ? 0 : 1;
    const auto &a = inputs[lowIndex];
    const auto &b = inputs[1 - lowIndex];
    const auto oldest = std::max(a.startComplexSample, b.startComplexSample);
    const auto end = std::min(a.endComplexSample, b.endComplexSample);
    if (end <= oldest || end - oldest < InputBlockSamples + 2 * BandFilterHalf ||
        end > std::numeric_limits<SampleIndex>::max() / 2) {
        error = QStringLiteral("Waiting for common IQ from both bands");
        return false;
    }
    low = {a.source, a.continuityId, a.centerHz};
    high = {b.source, b.continuityId, b.centerHz};
    next = oldest + BandFilterHalf;
    rate = a.sampleRateHz;
    clock = a.clockId;
    error.clear();
    configured = true;
    return true;
}

bool CoherentIqBandJoin::takeBlock(const std::vector<DspIqSourceSnapshot> &inputs,
                                  DspIqSourceSnapshot &output, QString &error) {
    output = {};
    error.clear();
    const auto fail = [&]() {
        configured = false;
        error = QStringLiteral("IQ gap, clock or format change while stitching bands");
        return false;
    };
    if (!configured || inputs.size() != 2) return fail();
    const auto &a = inputs[lowIndex];
    const auto &b = inputs[1 - lowIndex];
    const auto matches = [&](const DspIqSourceSnapshot &input, const Band &band) {
        return usable(input, CoherentIqLive::Timeline::Hardware) && input.source == band.source &&
               input.continuityId == band.continuity && input.clockId == clock &&
               input.sampleRateHz == rate && input.centerHz == band.center;
    };
    const auto end = std::min(a.endComplexSample, b.endComplexSample);
    if (!matches(a, low) || !matches(b, high) || end > std::numeric_limits<SampleIndex>::max() / 2 ||
        next - (BandFilterHalf - 1) < std::max(a.startComplexSample, b.startComplexSample))
        return fail();
    if (end <= next || end - next < InputBlockSamples + BandFilterHalf) return true;

    constexpr std::array<Complex, 4> rotation{Complex(1, 0), Complex(0, 1), Complex(-1, 0), Complex(0, -1)};
    const auto &interpolator = bandInterpolator();
    output.iq.resize(4 * InputBlockSamples);
    for (int n = 0; n < 2 * InputBlockSamples; ++n) {
        const SampleIndex inputSample = next + n / 2;
        const auto firstTap = inputSample - (BandFilterHalf - 1);
        const auto &taps = interpolator[n % 2];
        Complex left{}, right{};
        for (std::size_t i = 0; i < taps.size(); ++i) {
            left += taps[i] * sample(a, firstTap + i);
            right += taps[i] * sample(b, firstTap + i);
        }
        const auto translate = rotation[(2 * next + n) % rotation.size()];
        const auto joined = 0.5 * (std::conj(translate) * left + translate * right);
        if (!std::isfinite(joined.real()) || !std::isfinite(joined.imag())) {
            output = {};
            return fail();
        }
        output.iq[2 * n] = float(joined.real());
        output.iq[2 * n + 1] = float(joined.imag());
    }
    output.source = CoherentIqLive::outputIdentity();
    output.startComplexSample = 2 * next;
    next += InputBlockSamples;
    output.endComplexSample = 2 * next;
    output.centerHz = low.center + 0.5 * rate;
    output.sampleRateHz = 2 * rate;
    output.clockId = clock;
    output.sampleClockValid = output.clockLocked = output.sharedSampleTimeline = true;
    return true;
}
