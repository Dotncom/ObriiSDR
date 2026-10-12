#include "spectrumdisplayreducer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
struct BinAccumulator {
    float peak = -std::numeric_limits<float>::infinity();
    float minimum = std::numeric_limits<float>::infinity();
    double sumDb = 0.0;
    double sumPower = 0.0;
    float sample = std::numeric_limits<float>::quiet_NaN();
    double sampleDistance = std::numeric_limits<double>::infinity();
    int count = 0;

    void add(float value, double distance, SpectrumDisplayReduction reduction) {
        if (!std::isfinite(value)) return;
        switch (reduction) {
        case SpectrumDisplayReduction::AverageDb:
            sumDb += value;
            break;
        case SpectrumDisplayReduction::AveragePower:
            sumPower += std::pow(10.0, static_cast<double>(value) / 10.0);
            break;
        case SpectrumDisplayReduction::Minimum:
            minimum = (std::min)(minimum, value);
            break;
        case SpectrumDisplayReduction::Sample:
            if (distance < sampleDistance) {
                sampleDistance = distance;
                sample = value;
            }
            break;
        default:
            peak = (std::max)(peak, value);
            break;
        }
        ++count;
    }

    float value(SpectrumDisplayReduction reduction) const {
        if (count <= 0) return -std::numeric_limits<float>::infinity();
        switch (reduction) {
        case SpectrumDisplayReduction::AverageDb:
            return static_cast<float>(sumDb / count);
        case SpectrumDisplayReduction::AveragePower:
            return static_cast<float>(10.0 * std::log10((std::max)(sumPower / count, 1.0e-30)));
        case SpectrumDisplayReduction::Sample:
            return sample;
        case SpectrumDisplayReduction::Minimum:
            return minimum;
        case SpectrumDisplayReduction::Peak:
        default:
            return peak;
        }
    }
};

void fillMissingBins(std::vector<float> &values, float fallbackLevel) {
    int previousFilled = -1;
    float previousValue = fallbackLevel;
    for (int bin = 0; bin < static_cast<int>(values.size()); ++bin) {
        const float value = values[static_cast<std::size_t>(bin)];
        if (!std::isfinite(value)) {
            continue;
        }
        if (previousFilled < 0) {
            std::fill(values.begin(), values.begin() + bin, value);
        } else if (bin - previousFilled > 1) {
            const int gap = bin - previousFilled;
            for (int fill = previousFilled + 1; fill < bin; ++fill) {
                const float ratio = static_cast<float>(fill - previousFilled) /
                                    static_cast<float>(gap);
                values[static_cast<std::size_t>(fill)] =
                    previousValue + (value - previousValue) * ratio;
            }
        }
        previousFilled = bin;
        previousValue = value;
    }
    if (previousFilled < 0) {
        std::fill(values.begin(), values.end(), fallbackLevel);
    } else {
        std::fill(values.begin() + previousFilled + 1, values.end(), previousValue);
    }
}

bool hasRegularFrequencyGrid(const std::vector<float> &frequencies, int count) {
    if (count < 4 || static_cast<int>(frequencies.size()) < count) {
        return false;
    }
    const double first = frequencies.front();
    const double last = frequencies[static_cast<std::size_t>(count - 1)];
    const double step = (last - first) / static_cast<double>(count - 1);
    if (!std::isfinite(first) || !std::isfinite(last) || !std::isfinite(step) || step <= 0.0) {
        return false;
    }
    const double tolerance = std::max(std::abs(step) * 0.35, 32.0);
    for (const int index : {count / 4, count / 2, (count * 3) / 4}) {
        const double expected = first + step * static_cast<double>(index);
        if (std::abs(static_cast<double>(frequencies[static_cast<std::size_t>(index)]) - expected) >
            tolerance) {
            return false;
        }
    }
    return true;
}
}

void prepareSpectrumDisplayFrame(const std::vector<float> &sourceFrequencies,
                                 const std::vector<float> &sourceLevels,
                                 const std::vector<float> *sourceOverlayLevels,
                                 int sourceCount,
                                 double minFrequency,
                                 double maxFrequency,
                                 int targetBins,
                                 float fallbackLevel,
                                 SpectrumDisplayReduction reduction,
                                 SpectrumDisplayFrame &output) {
    const int availableCount = std::min({sourceCount,
                                         static_cast<int>(sourceFrequencies.size()),
                                         static_cast<int>(sourceLevels.size())});
    if (availableCount <= 0 || !std::isfinite(minFrequency) ||
        !std::isfinite(maxFrequency) || maxFrequency <= minFrequency) {
        output.frequencies.clear();
        output.levels.clear();
        output.overlayLevels.clear();
        return;
    }

    const int binCount = (std::min)(availableCount, (std::max)(targetBins, 2));
    output.frequencies.resize(static_cast<std::size_t>(binCount));
    output.levels.assign(static_cast<std::size_t>(binCount),
                         -std::numeric_limits<float>::infinity());
    const bool includeOverlay = sourceOverlayLevels &&
                                static_cast<int>(sourceOverlayLevels->size()) >= availableCount;
    if (includeOverlay) {
        output.overlayLevels.assign(static_cast<std::size_t>(binCount),
                                    -std::numeric_limits<float>::infinity());
    } else {
        output.overlayLevels.clear();
    }

    const double span = maxFrequency - minFrequency;
    if (hasRegularFrequencyGrid(sourceFrequencies, availableCount)) {
        const double sourceFirstFrequency = sourceFrequencies.front();
        const double sourceLastFrequency = sourceFrequencies[static_cast<std::size_t>(availableCount - 1)];
        const double sourceStep = (sourceLastFrequency - sourceFirstFrequency) /
                                  static_cast<double>(availableCount - 1);
        for (int bin = 0; bin < binCount; ++bin) {
            const double lowerFrequency =
                minFrequency + span * static_cast<double>(bin) / static_cast<double>(binCount);
            const double upperFrequency =
                minFrequency + span * static_cast<double>(bin + 1) / static_cast<double>(binCount);
            const int firstIndex = std::clamp(
                static_cast<int>(std::ceil((lowerFrequency - sourceFirstFrequency) / sourceStep)),
                0,
                availableCount);
            const int lastIndex = std::clamp(
                static_cast<int>(std::ceil((upperFrequency - sourceFirstFrequency) / sourceStep)),
                firstIndex,
                availableCount);
            BinAccumulator levelAccumulator;
            BinAccumulator overlayAccumulator;
            const double centerFrequency = (lowerFrequency + upperFrequency) * 0.5;
            for (int index = firstIndex; index < lastIndex; ++index) {
                const int shiftedIndex = (index + availableCount / 2) % availableCount;
                const float level = sourceLevels[static_cast<std::size_t>(shiftedIndex)];
                const double distance = reduction == SpectrumDisplayReduction::Sample
                    ? std::abs(static_cast<double>(sourceFrequencies[static_cast<std::size_t>(index)]) - centerFrequency)
                    : 0.0;
                levelAccumulator.add(level, distance, reduction);
                if (includeOverlay) {
                    const float overlayLevel =
                        (*sourceOverlayLevels)[static_cast<std::size_t>(shiftedIndex)];
                    overlayAccumulator.add(overlayLevel, distance, reduction);
                }
            }
            output.levels[static_cast<std::size_t>(bin)] = levelAccumulator.value(reduction);
            if (includeOverlay) {
                output.overlayLevels[static_cast<std::size_t>(bin)] = overlayAccumulator.value(reduction);
            }
        }
    } else {
        std::vector<BinAccumulator> levelAccumulators(static_cast<std::size_t>(binCount));
        std::vector<BinAccumulator> overlayAccumulators(includeOverlay
                                                            ? static_cast<std::size_t>(binCount)
                                                            : 0U);
        for (int index = 0; index < availableCount; ++index) {
            const float frequency = sourceFrequencies[static_cast<std::size_t>(index)];
            if (!std::isfinite(frequency)) {
                continue;
            }
            const double position = (static_cast<double>(frequency) - minFrequency) / span;
            if (position < 0.0 || position > 1.0) {
                continue;
            }
            const int bin = std::clamp(static_cast<int>(position * binCount), 0, binCount - 1);
            const double centerFrequency = minFrequency +
                                           (static_cast<double>(bin) + 0.5) * span / binCount;
            const double distance = reduction == SpectrumDisplayReduction::Sample
                ? std::abs(static_cast<double>(frequency) - centerFrequency) : 0.0;
            const int shiftedIndex = (index + availableCount / 2) % availableCount;
            const float level = sourceLevels[static_cast<std::size_t>(shiftedIndex)];
            levelAccumulators[static_cast<std::size_t>(bin)].add(level, distance, reduction);
            if (includeOverlay) {
                const float overlayLevel =
                    (*sourceOverlayLevels)[static_cast<std::size_t>(shiftedIndex)];
                overlayAccumulators[static_cast<std::size_t>(bin)].add(overlayLevel, distance, reduction);
            }
        }
        for (int bin = 0; bin < binCount; ++bin) {
            output.levels[static_cast<std::size_t>(bin)] =
                levelAccumulators[static_cast<std::size_t>(bin)].value(reduction);
            if (includeOverlay) {
                output.overlayLevels[static_cast<std::size_t>(bin)] =
                    overlayAccumulators[static_cast<std::size_t>(bin)].value(reduction);
            }
        }
    }

    fillMissingBins(output.levels, fallbackLevel);
    if (includeOverlay) {
        fillMissingBins(output.overlayLevels, fallbackLevel);
    }
    for (int bin = 0; bin < binCount; ++bin) {
        output.frequencies[static_cast<std::size_t>(bin)] = static_cast<float>(
            minFrequency + (static_cast<double>(bin) + 0.5) * span /
                               static_cast<double>(binCount));
    }
}
