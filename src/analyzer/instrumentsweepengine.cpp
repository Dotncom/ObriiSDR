#include "instrumentsweepengine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace {
bool finitePositive(double value) {
    return std::isfinite(value) && value > 0.0;
}
}

InstrumentSweepPlan InstrumentSweepEngine::createPlan(const InstrumentSweepSettings &requested) {
    InstrumentSweepPlan plan;
    plan.settings = requested;
    if (!std::isfinite(requested.startHz) || !std::isfinite(requested.stopHz) ||
        requested.stopHz <= requested.startHz) {
        plan.error = QStringLiteral("Sweep stop frequency must be above start frequency");
        return plan;
    }
    if (!finitePositive(requested.captureBandwidthHz)) {
        plan.error = QStringLiteral("Sweep capture bandwidth must be positive");
        return plan;
    }
    if (requested.edgeDiscardFraction < 0.0 || requested.edgeDiscardFraction >= 0.45) {
        plan.error = QStringLiteral("Sweep edge discard must be between 0 and 45 percent");
        return plan;
    }
    if (requested.overlapFraction < 0.0 || requested.overlapFraction >= 0.95) {
        plan.error = QStringLiteral("Sweep overlap must be between 0 and 95 percent");
        return plan;
    }

    const double usableBandwidth = requested.captureBandwidthHz *
                                   (1.0 - 2.0 * requested.edgeDiscardFraction);
    const double stepHz = usableBandwidth * (1.0 - requested.overlapFraction);
    if (!finitePositive(usableBandwidth) || !finitePositive(stepHz)) {
        plan.error = QStringLiteral("Sweep settings leave no usable bandwidth");
        return plan;
    }

    const double spanHz = requested.stopHz - requested.startHz;
    const int estimatedSegments = spanHz <= usableBandwidth
                                      ? 1
                                      : 1 + static_cast<int>(std::ceil(
                                            (spanHz - usableBandwidth) / stepHz));
    if (estimatedSegments > std::max(1, requested.maximumSegments)) {
        plan.error = QStringLiteral("Sweep requires %1 segments; limit is %2")
                         .arg(estimatedSegments)
                         .arg(std::max(1, requested.maximumSegments));
        return plan;
    }

    const double usableHalf = usableBandwidth * 0.5;
    QVector<double> centers;
    centers.reserve(estimatedSegments);
    if (estimatedSegments == 1) {
        centers.append((requested.startHz + requested.stopHz) * 0.5);
    } else {
        for (int index = 0; index < estimatedSegments; ++index)
            centers.append(requested.startHz + usableHalf + index * stepHz);
        centers.last() = requested.stopHz - usableHalf;
    }

    for (double center : centers) {
        if (!plan.segments.isEmpty() &&
            std::abs(center - plan.segments.constLast().centerHz) < 1.0e-9)
            continue;
        InstrumentSweepSegment segment;
        segment.index = plan.segments.size();
        segment.centerHz = center;
        segment.usableMinimumHz = std::max(requested.startHz, center - usableHalf);
        segment.usableMaximumHz = std::min(requested.stopHz, center + usableHalf);
        plan.segments.append(segment);
    }
    return plan;
}

bool InstrumentSweepEngine::begin(const InstrumentSweepPlan &plan) {
    clear();
    if (!plan.isValid()) return false;
    currentPlan = plan;
    outputBinWidthHz = finitePositive(plan.settings.outputBinWidthHz)
                           ? plan.settings.outputBinWidthHz
                           : plan.settings.captureBandwidthHz / 65536.0;
    const double spanHz = plan.settings.stopHz - plan.settings.startHz;
    const double maximumBins = static_cast<double>(
        std::clamp(plan.settings.maximumOutputBins, 2, 67108864));
    const std::size_t binCount = static_cast<std::size_t>(
        std::clamp(std::ceil(spanHz / outputBinWidthHz) + 1.0, 2.0, maximumBins));
    outputBinWidthHz = spanHz / static_cast<double>(binCount - 1);
    try {
        weightedLinearPower.assign(binCount, 0.0);
        weightSums.assign(binCount, 0.0);
        counts.assign(binCount, 0);
    } catch (const std::bad_alloc &) {
        clear();
        return false;
    }
    return true;
}

bool InstrumentSweepEngine::addFrame(int segmentIndex,
                                     double frameMinimumHz,
                                     double frameMaximumHz,
                                     const std::vector<float> &levelsDb) {
    if (!currentPlan.isValid() || levelsDb.size() < 2 || segmentIndex < 0 ||
        segmentIndex >= currentPlan.segments.size() ||
        !std::isfinite(frameMinimumHz) || !std::isfinite(frameMaximumHz) ||
        frameMaximumHz <= frameMinimumHz)
        return false;

    const InstrumentSweepSegment &segment = currentPlan.segments[segmentIndex];
    const double frameStep = (frameMaximumHz - frameMinimumHz) /
                             static_cast<double>(levelsDb.size() - 1);
    const double usableSpan = segment.usableMaximumHz - segment.usableMinimumHz;
    if (!finitePositive(frameStep) || !finitePositive(usableSpan)) return false;

    const double usableMinimum = std::max(segment.usableMinimumHz, frameMinimumHz);
    const double usableMaximum = std::min(segment.usableMaximumHz, frameMaximumHz);
    const qint64 maximumTarget = static_cast<qint64>(weightedLinearPower.size() - 1);
    const qint64 firstTarget = std::clamp<qint64>(
        static_cast<qint64>(std::ceil(
            (usableMinimum - currentPlan.settings.startHz) / outputBinWidthHz)),
        0, maximumTarget);
    const qint64 lastTarget = std::clamp<qint64>(
        static_cast<qint64>(std::floor(
            (usableMaximum - currentPlan.settings.startHz) / outputBinWidthHz)),
        0, maximumTarget);

    std::vector<float> sourceLinearPower;
    try {
        sourceLinearPower.resize(levelsDb.size(), std::numeric_limits<float>::quiet_NaN());
    } catch (const std::bad_alloc &) {
        return false;
    }
    for (std::size_t source = 0; source < levelsDb.size(); ++source) {
        if (std::isfinite(levelsDb[source])) {
            sourceLinearPower[source] = static_cast<float>(
                std::pow(10.0, static_cast<double>(levelsDb[source]) / 10.0));
        }
    }

    // Sample every output bin rather than scattering source points to their
    // nearest destination. Different FFT/output-grid steps would otherwise
    // leave deterministic one-bin holes between valid sweep data.
    for (qint64 targetIndex = firstTarget; targetIndex <= lastTarget; ++targetIndex) {
        const double frequency = currentPlan.settings.startHz +
                                 static_cast<double>(targetIndex) * outputBinWidthHz;
        const double sourcePosition = std::clamp(
            (frequency - frameMinimumHz) / frameStep,
            0.0, static_cast<double>(levelsDb.size() - 1));
        const std::size_t lower = static_cast<std::size_t>(std::floor(sourcePosition));
        const std::size_t upper = std::min(lower + 1, levelsDb.size() - 1);
        const double fraction = sourcePosition - static_cast<double>(lower);
        const bool lowerValid = std::isfinite(sourceLinearPower[lower]);
        const bool upperValid = std::isfinite(sourceLinearPower[upper]);
        if (!lowerValid && !upperValid) continue;
        double linearPower = 0.0;
        if (lowerValid && upperValid) {
            const double lowerPower = sourceLinearPower[lower];
            const double upperPower = sourceLinearPower[upper];
            linearPower = lowerPower + (upperPower - lowerPower) * fraction;
        } else {
            linearPower = lowerValid ? sourceLinearPower[lower] : sourceLinearPower[upper];
        }
        const std::size_t target = static_cast<std::size_t>(targetIndex);
        const double normalized = std::clamp(
            (frequency - segment.usableMinimumHz) / usableSpan, 0.0, 1.0);
        const double edgeWeight = std::max(0.05, std::sin(3.14159265358979323846 * normalized));
        weightedLinearPower[target] += linearPower * edgeWeight;
        weightSums[target] += edgeWeight;
        if (counts[target] != std::numeric_limits<quint32>::max()) ++counts[target];
    }
    return true;
}

bool InstrumentSweepEngine::updateResultForSegment(int segmentIndex,
                                                   std::vector<float> &frequenciesHz,
                                                   std::vector<float> &levelsDb,
                                                   float initialLevelDb) const {
    if (!currentPlan.isValid() || weightedLinearPower.empty() ||
        segmentIndex < 0 || segmentIndex >= currentPlan.segments.size())
        return false;

    const std::size_t binCount = weightedLinearPower.size();
    if (frequenciesHz.size() != binCount || levelsDb.size() != binCount) {
        frequenciesHz.resize(binCount);
        levelsDb.assign(binCount, initialLevelDb);
        for (std::size_t index = 0; index < binCount; ++index) {
            frequenciesHz[index] = static_cast<float>(
                currentPlan.settings.startHz + static_cast<double>(index) * outputBinWidthHz);
        }
    }

    const InstrumentSweepSegment &segment = currentPlan.segments[segmentIndex];
    const qint64 lastBin = static_cast<qint64>(binCount - 1);
    const qint64 first = std::clamp<qint64>(
        static_cast<qint64>(std::floor(
            (segment.usableMinimumHz - currentPlan.settings.startHz) / outputBinWidthHz)),
        0, lastBin);
    const qint64 last = std::clamp<qint64>(
        static_cast<qint64>(std::ceil(
            (segment.usableMaximumHz - currentPlan.settings.startHz) / outputBinWidthHz)),
        0, lastBin);
    for (qint64 index = first; index <= last; ++index) {
        const std::size_t target = static_cast<std::size_t>(index);
        if (weightSums[target] <= 0.0) continue;
        const double power = weightedLinearPower[target] / weightSums[target];
        levelsDb[target] = static_cast<float>(
            10.0 * std::log10(std::max(power, 1.0e-30)));
    }
    return true;
}

InstrumentSweepResult InstrumentSweepEngine::finish() const {
    InstrumentSweepResult result;
    if (!currentPlan.isValid() || weightedLinearPower.empty()) return result;
    result.frequenciesHz.resize(weightedLinearPower.size());
    result.levelsDb.resize(weightedLinearPower.size(),
                           std::numeric_limits<float>::quiet_NaN());
    result.contributionCounts = counts;
    for (std::size_t index = 0; index < weightedLinearPower.size(); ++index) {
        result.frequenciesHz[index] = static_cast<float>(
            currentPlan.settings.startHz + static_cast<double>(index) * outputBinWidthHz);
        if (weightSums[index] > 0.0) {
            const double power = weightedLinearPower[index] / weightSums[index];
            result.levelsDb[index] = static_cast<float>(10.0 * std::log10(std::max(power, 1.0e-30)));
        } else {
            ++result.missingBins;
        }
    }
    return result;
}

void InstrumentSweepEngine::clear() {
    currentPlan = {};
    weightedLinearPower.clear();
    weightSums.clear();
    counts.clear();
    outputBinWidthHz = 0.0;
}
