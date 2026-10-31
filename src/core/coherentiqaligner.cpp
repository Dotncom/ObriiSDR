#include "coherentiqaligner.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace {
using Complex = std::complex<double>;

Complex sampleAt(const std::vector<float> &iq, std::size_t index) {
    return {iq[index * 2U], iq[index * 2U + 1U]};
}
struct Correlation {
    Complex cross{};
    double referencePower = 0.0;
    double targetPower = 0.0;
    std::size_t count = 0;

    double coherence() const {
        const double denominator = std::sqrt(referencePower * targetPower);
        return denominator > 0.0 ? std::abs(cross) / denominator : 0.0;
    }
};

Correlation correlate(const std::vector<float> &referenceIq,
                      const std::vector<float> &targetIq,
                      int delay,
                      std::size_t stride) {
    const std::size_t referenceSamples = referenceIq.size() / 2U;
    const std::size_t targetSamples = targetIq.size() / 2U;
    const std::size_t referenceStart = delay < 0 ? std::size_t(-delay) : 0U;
    const std::size_t targetStart = delay > 0 ? std::size_t(delay) : 0U;
    const std::size_t common = std::min(referenceSamples - std::min(referenceStart, referenceSamples),
                                        targetSamples - std::min(targetStart, targetSamples));
    Correlation result;
    for (std::size_t offset = 0; offset < common; offset += std::max<std::size_t>(1, stride)) {
        const Complex reference = sampleAt(referenceIq, referenceStart + offset);
        const Complex target = sampleAt(targetIq, targetStart + offset);
        result.cross += reference * std::conj(target);
        result.referencePower += std::norm(reference);
        result.targetPower += std::norm(target);
        ++result.count;
    }
    return result;
}

double parabolicOffset(double left, double center, double right) {
    const double denominator = left - 2.0 * center + right;
    if (!std::isfinite(denominator) || std::abs(denominator) < 1.0e-12) return 0.0;
    return std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5);
}
}

CoherentIqCalibration CoherentIqAligner::calibrate(
    const DspSourceIdentity &reference,
    const std::vector<float> &referenceIq,
    const DspSourceIdentity &target,
    const std::vector<float> &targetIq,
    double sampleRateHz,
    double centerHz,
    int maximumDelaySamples) {
    CoherentIqCalibration result;
    result.reference = reference;
    result.target = target;
    result.sampleRateHz = sampleRateHz;
    result.centerHz = centerHz;
    if (!reference.isValid() || !target.isValid() || reference == target ||
        referenceIq.size() < 512U || targetIq.size() < 512U ||
        referenceIq.size() % 2U || targetIq.size() % 2U ||
        !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0 ||
        !std::isfinite(centerHz)) {
        result.error = QStringLiteral("Invalid coherent-IQ calibration input");
        return result;
    }
    for (float value : referenceIq)
        if (!std::isfinite(value)) { result.error = QStringLiteral("Reference IQ contains non-finite samples"); return result; }
    for (float value : targetIq)
        if (!std::isfinite(value)) { result.error = QStringLiteral("Target IQ contains non-finite samples"); return result; }

    const int sampleBound = int(std::min(referenceIq.size(), targetIq.size()) / 2U) - 64;
    maximumDelaySamples = std::clamp(maximumDelaySamples, 0, std::max(0, sampleBound));
    const std::size_t commonSamples = std::min(referenceIq.size(), targetIq.size()) / 2U;
    const std::size_t coarseStride = std::max<std::size_t>(1, commonSamples / 4096U);
    int bestDelay = 0;
    double bestCoherence = -1.0;
    for (int delay = -maximumDelaySamples; delay <= maximumDelaySamples; ++delay) {
        const Correlation candidate = correlate(referenceIq, targetIq, delay, coarseStride);
        const double value = candidate.coherence();
        if (candidate.count >= 64U && value > bestCoherence) {
            bestCoherence = value;
            bestDelay = delay;
        }
    }
    const Correlation best = correlate(referenceIq, targetIq, bestDelay, 1);
    if (best.count < 128U || best.referencePower <= 1.0e-18 || best.targetPower <= 1.0e-18) {
        result.error = QStringLiteral("Insufficient common signal power for coherent calibration");
        return result;
    }
    const Correlation left = correlate(referenceIq, targetIq, bestDelay - 1, coarseStride);
    const Correlation right = correlate(referenceIq, targetIq, bestDelay + 1, coarseStride);
    result.integerDelaySamples = bestDelay;
    result.fractionalDelaySamples = parabolicOffset(left.coherence(), best.coherence(), right.coherence());
    result.phaseCorrectionRadians = std::arg(best.cross);
    result.gainCorrection = std::sqrt(best.referencePower / best.targetPower);
    result.coherence = best.coherence();
    result.comparedSamples = best.count;
    result.valid = std::isfinite(result.gainCorrection) && result.gainCorrection > 0.0 &&
                   std::isfinite(result.phaseCorrectionRadians) &&
                   std::isfinite(result.fractionalDelaySamples) && result.coherence >= 0.10;
    if (!result.valid)
        result.error = QStringLiteral("Correlation is too weak for a trustworthy delay/phase calibration");
    return result;
}

std::vector<float> CoherentIqAligner::alignTarget(
    const std::vector<float> &targetIq,
    const CoherentIqCalibration &calibration,
    std::size_t maximumComplexSamples) {
    std::vector<float> output;
    if (!calibration.valid || targetIq.size() < 4U || targetIq.size() % 2U) return output;
    const std::size_t samples = targetIq.size() / 2U;
    const double offset = calibration.integerDelaySamples + calibration.fractionalDelaySamples;
    const double first = std::max(0.0, -offset);
    const double lastExclusive = std::min<double>(samples - 1U, samples - offset - 1.0);
    if (!(lastExclusive > first)) return output;
    std::size_t count = static_cast<std::size_t>(std::floor(lastExclusive - first));
    if (maximumComplexSamples) count = std::min(count, maximumComplexSamples);
    output.resize(count * 2U);
    const Complex correction = std::polar(calibration.gainCorrection,
                                          calibration.phaseCorrectionRadians);
    for (std::size_t index = 0; index < count; ++index) {
        const double position = first + index + offset;
        const std::size_t lower = static_cast<std::size_t>(std::floor(position));
        const double fraction = position - lower;
        const Complex a = sampleAt(targetIq, lower);
        const Complex b = sampleAt(targetIq, std::min(lower + 1U, samples - 1U));
        const Complex corrected = (a + (b - a) * fraction) * correction;
        output[index * 2U] = static_cast<float>(corrected.real());
        output[index * 2U + 1U] = static_cast<float>(corrected.imag());
    }
    return output;
}
