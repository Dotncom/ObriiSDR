#include "coherentiqlive.h"

#include "coherentiqaligner.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using Complex = std::complex<double>;
using SampleIndex = std::uint64_t;
constexpr double TwoPi = 6.28318530717958647693;
constexpr SampleIndex PhaseMeasurementSamples = 8 * CoherentIqLive::BlockSamples;

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
}

DspSourceIdentity CoherentIqLive::outputIdentity() {
    return DspSourceIdentity::normalized(QStringLiteral("multi-sdr-coherent"), QStringLiteral("rx0"));
}

bool CoherentIqLive::calibrate(const std::vector<DspIqSourceSnapshot> &inputs,
                               int maximumDelay, QString &error) {
    aligned.clear();
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
    for (auto it = inputs.begin() + 1; it != inputs.end(); ++it) {
        const auto value = CoherentIqAligner::calibrate(reference.source, referenceIq, it->source,
            slice(*it, first, end), rate, center, maximumDelay);
        if (!value.valid || value.coherence < 0.55) {
            error = QStringLiteral("Insufficient shared signal for %1").arg(it->source.key());
            return false;
        }
        const double delay = value.integerDelaySamples + value.fractionalDelaySamples;
        const int whole = int(std::floor(delay));
        calibrated.push_back({it->source, it->continuityId, whole, delay - whole,
            std::polar(value.gainCorrection, value.phaseCorrectionRadians)});
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
    double referencePower = 0.0;
    for (int n = 0; n < BlockSamples; ++n) {
        sum[n] = sample(inputs.front(), next + n);
        referencePower += std::norm(sum[n]);
        if (alignedChannels) {
            (*alignedChannels)[0][2 * n] = float(sum[n].real());
            (*alignedChannels)[0][2 * n + 1] = float(sum[n].imag());
        }
    }
    for (std::size_t i = 1; i < inputs.size(); ++i) {
        auto &plan = aligned[i];
        auto index = shiftIndex(next, plan.whole);
        Complex cross{};
        double power = 0.0;
        for (int n = 0; n < BlockSamples; ++n, ++index) {
            auto corrected = sample(inputs[i], index);
            if (plan.fractional > 0)
                corrected += (sample(inputs[i], index + 1) - corrected) * plan.fractional;
            corrected *= plan.correction;
            cross += sample(inputs.front(), next + n) * std::conj(corrected);
            power += std::norm(corrected);
            sum[n] += corrected;
            if (alignedChannels) {
                (*alignedChannels)[i][2 * n] = float(corrected.real());
                (*alignedChannels)[i][2 * n + 1] = float(corrected.imag());
            }
        }
        const double norm = std::sqrt(referencePower * power);
        const double phase = std::arg(cross);
        if (!(norm > 0) || !std::isfinite(norm) || !(std::abs(cross) >= 0.35 * norm) ||
            std::abs(phase) > 1.0471975511965976)
            return reject(QStringLiteral("Coherence or phase lock lost in %1").arg(plan.source.key()));
        if (measurePhase) {
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
        const auto mean = sum[n] / double(inputs.size());
        output.iq[2 * n] = float(mean.real());
        output.iq[2 * n + 1] = float(mean.imag());
        if (!std::isfinite(output.iq[2 * n]) || !std::isfinite(output.iq[2 * n + 1])) {
            output = {};
            return reject(QStringLiteral("Invalid coherent IQ output"));
        }
    }
    return true;
}
