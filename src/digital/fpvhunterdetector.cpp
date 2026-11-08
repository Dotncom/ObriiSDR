#include "fpvhunterdetector.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int FPV_HUNTER_MIN_BINS = 3;
constexpr int FPV_HUNTER_MAX_REPORTED_CANDIDATES = 12;

} // namespace

FpvHunterSettings FpvHunterDetector::normalizedSettings(FpvHunterSettings settings) {
    settings.minWidthMhz = (std::clamp)(settings.minWidthMhz,
                                        MinWidthMhz,
                                        MaxWidthMhz);
    settings.maxWidthMhz = (std::clamp)(settings.maxWidthMhz,
                                        MinWidthMhz,
                                        MaxWidthMhz);
    settings.maxWidthMhz = (std::max)(settings.minWidthMhz, settings.maxWidthMhz);
    settings.thresholdDb = (std::clamp)(settings.thresholdDb,
                                        MinThresholdDb,
                                        MaxThresholdDb);
    return settings;
}

FpvHunterResult FpvHunterDetector::analyze(const std::vector<float> &frequencies,
                                           const std::vector<float> &magnitudes,
                                           FpvHunterSettings settings) {
    settings = normalizedSettings(settings);

    FpvHunterResult result;
    result.enabled = settings.enabled;

    if (!settings.enabled) {
        result.statusText = QStringLiteral("FPV Hunter: off");
        return result;
    }

    const int dataCount = std::min(static_cast<int>(frequencies.size()),
                                   static_cast<int>(magnitudes.size()));
    if (dataCount < 16) {
        result.statusText = QStringLiteral("FPV Hunter: waiting for spectrum");
        return result;
    }

    std::vector<double> frequencyDeltas;
    frequencyDeltas.reserve(static_cast<std::size_t>(dataCount - 1));
    for (int i = 1; i < dataCount; ++i) {
        const double previous = frequencies[static_cast<std::size_t>(i - 1)];
        const double current = frequencies[static_cast<std::size_t>(i)];
        const double delta = std::abs(current - previous);
        if (std::isfinite(delta) && delta > 0.0) {
            frequencyDeltas.push_back(delta);
        }
    }
    if (frequencyDeltas.empty()) {
        result.statusText = QStringLiteral("FPV Hunter: invalid spectrum range");
        return result;
    }
    std::sort(frequencyDeltas.begin(), frequencyDeltas.end());
    const double binHz = frequencyDeltas[frequencyDeltas.size() / 2];
    if (!std::isfinite(binHz) || binHz <= 0.0) {
        result.statusText = QStringLiteral("FPV Hunter: invalid spectrum range");
        return result;
    }

    std::vector<float> levels(static_cast<std::size_t>(dataCount), -160.0f);
    std::vector<float> finiteLevels;
    finiteLevels.reserve(static_cast<std::size_t>(dataCount));
    for (int i = 0; i < dataCount; ++i) {
        float level = magnitudes[static_cast<std::size_t>((i + dataCount / 2) % dataCount)];
        if (!std::isfinite(level)) {
            level = -160.0f;
        }
        levels[static_cast<std::size_t>(i)] = level;
        finiteLevels.push_back(level);
    }
    if (finiteLevels.empty()) {
        result.statusText = QStringLiteral("FPV Hunter: no finite spectrum levels");
        return result;
    }

    std::sort(finiteLevels.begin(), finiteLevels.end());
    const int floorIndex = (std::clamp)(static_cast<int>(finiteLevels.size() * 35 / 100),
                                        0,
                                        static_cast<int>(finiteLevels.size()) - 1);
    const float noiseFloorDb = finiteLevels[static_cast<std::size_t>(floorIndex)];
    const double minWidthHz = settings.minWidthMhz * 1000000.0;
    const double maxWidthHz = settings.maxWidthMhz * 1000000.0;
    const float thresholdDb = static_cast<float>(noiseFloorDb + settings.thresholdDb);
    const double gapBreakHz = (std::max)(binHz * 8.0, (std::max)(500000.0, minWidthHz * 0.25));
    result.noiseFloorDb = noiseFloorDb;
    result.thresholdDb = thresholdDb;

    auto binsAreContiguous = [&](int left, int right) {
        if (left < 0 || right < 0 || left >= dataCount || right >= dataCount) {
            return false;
        }
        const double leftFrequency = frequencies[static_cast<std::size_t>(left)];
        const double rightFrequency = frequencies[static_cast<std::size_t>(right)];
        const double delta = std::abs(rightFrequency - leftFrequency);
        return std::isfinite(delta) && delta <= gapBreakHz;
    };

    const int smoothRadius = (std::clamp)(static_cast<int>(std::lround(250000.0 / binHz)),
                                          1,
                                          8);
    std::vector<float> smoothed(static_cast<std::size_t>(dataCount), -160.0f);
    for (int i = 0; i < dataCount; ++i) {
        double sum = levels[static_cast<std::size_t>(i)];
        int count = 1;

        bool leftContiguous = true;
        bool rightContiguous = true;
        for (int step = 1; step <= smoothRadius; ++step) {
            const int left = i - step;
            if (left >= 0 && leftContiguous && binsAreContiguous(left, left + 1)) {
                sum += levels[static_cast<std::size_t>(left)];
                ++count;
            } else {
                leftContiguous = false;
            }

            const int right = i + step;
            if (right < dataCount && rightContiguous && binsAreContiguous(right - 1, right)) {
                sum += levels[static_cast<std::size_t>(right)];
                ++count;
            } else {
                rightContiguous = false;
            }
        }
        smoothed[static_cast<std::size_t>(i)] = static_cast<float>(sum / count);
    }

    int runStart = -1;
    auto finishRun = [&](int runEnd) {
        if (runStart < 0 || runEnd < runStart) {
            return;
        }

        int start = runStart;
        int end = runEnd;
        const float softThreshold = noiseFloorDb + (thresholdDb - noiseFloorDb) * 0.45f;
        while (start > 0 &&
               binsAreContiguous(start - 1, start) &&
               smoothed[static_cast<std::size_t>(start - 1)] >= softThreshold) {
            --start;
        }
        while (end + 1 < dataCount &&
               binsAreContiguous(end, end + 1) &&
               smoothed[static_cast<std::size_t>(end + 1)] >= softThreshold) {
            ++end;
        }

        const int bins = end - start + 1;
        if (bins < FPV_HUNTER_MIN_BINS) {
            return;
        }

        const double widthHz = std::abs(static_cast<double>(frequencies[static_cast<std::size_t>(end)]) -
                                        static_cast<double>(frequencies[static_cast<std::size_t>(start)]));
        if (widthHz < minWidthHz || widthHz > maxWidthHz) {
            return;
        }

        double weightedFrequency = 0.0;
        double weightSum = 0.0;
        double levelSum = 0.0;
        float peakDb = -160.0f;
        for (int i = start; i <= end; ++i) {
            const float level = levels[static_cast<std::size_t>(i)];
            const double weight = (std::max)(0.05, static_cast<double>(level - noiseFloorDb));
            weightedFrequency += static_cast<double>(frequencies[static_cast<std::size_t>(i)]) * weight;
            weightSum += weight;
            levelSum += level;
            peakDb = (std::max)(peakDb, level);
        }
        if (weightSum <= 0.0) {
            return;
        }

        const double centerHz = weightedFrequency / weightSum;
        const float averageDb = static_cast<float>(levelSum / static_cast<double>(bins));
        const float excessDb = averageDb - noiseFloorDb;
        const float peakExcessDb = peakDb - noiseFloorDb;
        const float edgeLevel = (std::max)(levels[static_cast<std::size_t>(start)],
                                           levels[static_cast<std::size_t>(end)]);
        const float edgeDropDb = peakDb - edgeLevel;
        const double widthMhz = widthHz / 1000000.0;

        QString type = QStringLiteral("wide unknown");
        if (widthMhz >= 4.0 && widthMhz <= 12.0 && edgeDropDb >= 2.5f) {
            type = QStringLiteral("analog-like FPV");
        } else if (widthMhz >= 8.0) {
            type = QStringLiteral("digital-like/wide video");
        }

        const float score = excessDb * 1.6f +
                            peakExcessDb * 0.9f +
                            static_cast<float>((std::min)(widthMhz, 12.0) * 0.35) +
                            (type.startsWith(QStringLiteral("analog")) ? 4.0f : 0.0f);

        FpvHunterCandidate candidate;
        candidate.valid = true;
        candidate.centerHz = centerHz;
        candidate.widthHz = widthHz;
        candidate.peakDb = peakDb;
        candidate.averageDb = averageDb;
        candidate.excessDb = excessDb;
        candidate.score = score;
        candidate.type = type;

        ++result.candidates;
        result.candidateList.push_back(candidate);
        if (score > result.best.score || !result.best.valid) {
            result.best = candidate;
        }
    };

    for (int i = 0; i < dataCount; ++i) {
        if (i > 0 && !binsAreContiguous(i - 1, i)) {
            finishRun(i - 1);
            runStart = -1;
        }
        if (smoothed[static_cast<std::size_t>(i)] >= thresholdDb) {
            if (runStart < 0) {
                runStart = i;
            }
        } else if (runStart >= 0) {
            finishRun(i - 1);
            runStart = -1;
        }
    }
    if (runStart >= 0) {
        finishRun(dataCount - 1);
    }
    std::sort(result.candidateList.begin(),
              result.candidateList.end(),
              [](const FpvHunterCandidate &a, const FpvHunterCandidate &b) {
                  return a.score > b.score;
              });
    if (static_cast<int>(result.candidateList.size()) > FPV_HUNTER_MAX_REPORTED_CANDIDATES) {
        result.candidateList.resize(FPV_HUNTER_MAX_REPORTED_CANDIDATES);
    }

    if (!result.best.valid) {
        result.statusText =
            QStringLiteral("FPV Hunter: no wide video carrier, floor %1 dB, threshold +%2 dB")
                .arg(noiseFloorDb, 0, 'f', 1)
                .arg(settings.thresholdDb, 0, 'f', 1);
        return result;
    }

    result.statusText =
        QStringLiteral("FPV Hunter: %1 candidate%2, best %3 at %4 MHz, width %5 MHz, peak %6 dB, avg +%7 dB over floor")
            .arg(result.candidates)
            .arg(result.candidates == 1 ? QString() : QStringLiteral("s"))
            .arg(result.best.type)
            .arg(result.best.centerHz / 1000000.0, 0, 'f', 3)
            .arg(result.best.widthHz / 1000000.0, 0, 'f', 2)
            .arg(result.best.peakDb, 0, 'f', 1)
            .arg(result.best.excessDb, 0, 'f', 1);
    return result;
}
