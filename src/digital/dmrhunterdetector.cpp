#include "dmrhunterdetector.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int DMR_HUNTER_MIN_BINS = 2;
constexpr int DMR_HUNTER_MAX_REPORTED_CANDIDATES = 12;
constexpr double DMR_HUNTER_MAX_BIN_WIDTH_HZ = 6000.0;

} // namespace

DmrHunterSettings DmrHunterDetector::normalizedSettings(DmrHunterSettings settings) {
    settings.minWidthKhz = (std::clamp)(settings.minWidthKhz,
                                        MinWidthKhz,
                                        MaxWidthKhz);
    settings.maxWidthKhz = (std::clamp)(settings.maxWidthKhz,
                                        MinWidthKhz,
                                        MaxWidthKhz);
    settings.maxWidthKhz = (std::max)(settings.minWidthKhz, settings.maxWidthKhz);
    settings.thresholdDb = (std::clamp)(settings.thresholdDb,
                                        MinThresholdDb,
                                        MaxThresholdDb);
    return settings;
}

DmrHunterResult DmrHunterDetector::analyze(const std::vector<float> &frequencies,
                                           const std::vector<float> &magnitudes,
                                           DmrHunterSettings settings) {
    settings = normalizedSettings(settings);

    DmrHunterResult result;
    result.enabled = settings.enabled;

    if (!settings.enabled) {
        result.statusText = QStringLiteral("DMR Hunter: off");
        return result;
    }

    const int dataCount = std::min(static_cast<int>(frequencies.size()),
                                   static_cast<int>(magnitudes.size()));
    if (dataCount < 16) {
        result.statusText = QStringLiteral("DMR Hunter: waiting for spectrum");
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
        result.statusText = QStringLiteral("DMR Hunter: invalid spectrum range");
        return result;
    }
    std::sort(frequencyDeltas.begin(), frequencyDeltas.end());
    const double binHz = frequencyDeltas[frequencyDeltas.size() / 2];
    if (!std::isfinite(binHz) || binHz <= 0.0) {
        result.statusText = QStringLiteral("DMR Hunter: invalid spectrum range");
        return result;
    }

    if (binHz > DMR_HUNTER_MAX_BIN_WIDTH_HZ) {
        result.statusText =
            QStringLiteral("DMR Hunter: FFT resolution too coarse (%1 kHz/bin)")
                .arg(binHz / 1000.0, 0, 'f', 1);
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

    std::sort(finiteLevels.begin(), finiteLevels.end());
    const int floorIndex = (std::clamp)(static_cast<int>(finiteLevels.size() * 35 / 100),
                                        0,
                                        static_cast<int>(finiteLevels.size()) - 1);
    const float noiseFloorDb = finiteLevels[static_cast<std::size_t>(floorIndex)];
    const float thresholdDb = static_cast<float>(noiseFloorDb + settings.thresholdDb);
    const double minWidthHz = settings.minWidthKhz * 1000.0;
    const double maxWidthHz = settings.maxWidthKhz * 1000.0;
    const double gapBreakHz = (std::max)(binHz * 8.0, maxWidthHz * 4.0);
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

    const int smoothRadius = (std::clamp)(static_cast<int>(std::lround(2000.0 / binHz)),
                                          0,
                                          3);
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
        if (bins < DMR_HUNTER_MIN_BINS) {
            return;
        }

        const double widthHz = std::abs(static_cast<double>(frequencies[static_cast<std::size_t>(end)]) -
                                        static_cast<double>(frequencies[static_cast<std::size_t>(start)]));
        if (widthHz < minWidthHz || widthHz > maxWidthHz) {
            return;
        }

        double weightedFrequency = 0.0;
        double weightSum = 0.0;
        float peakDb = -160.0f;
        for (int i = start; i <= end; ++i) {
            const float level = levels[static_cast<std::size_t>(i)];
            const double weight = (std::max)(0.05, static_cast<double>(level - noiseFloorDb));
            weightedFrequency += static_cast<double>(frequencies[static_cast<std::size_t>(i)]) * weight;
            weightSum += weight;
            peakDb = (std::max)(peakDb, level);
        }
        if (weightSum <= 0.0) {
            return;
        }

        const double centerHz = weightedFrequency / weightSum;
        const float excessDb = peakDb - noiseFloorDb;
        const double targetWidthHz = 12500.0;
        const float widthPenalty =
            static_cast<float>(std::abs(widthHz - targetWidthHz) / targetWidthHz);
        const float score = excessDb * 2.0f - widthPenalty * 3.0f;

        DmrHunterCandidate candidate;
        candidate.valid = true;
        candidate.centerHz = centerHz;
        candidate.widthHz = widthHz;
        candidate.peakDb = peakDb;
        candidate.excessDb = excessDb;
        candidate.score = score;

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
              [](const DmrHunterCandidate &a, const DmrHunterCandidate &b) {
                  return a.score > b.score;
              });
    if (static_cast<int>(result.candidateList.size()) > DMR_HUNTER_MAX_REPORTED_CANDIDATES) {
        result.candidateList.resize(DMR_HUNTER_MAX_REPORTED_CANDIDATES);
    }

    if (!result.best.valid) {
        result.statusText =
            QStringLiteral("DMR Hunter: no narrow carrier, floor %1 dB, threshold +%2 dB")
                .arg(noiseFloorDb, 0, 'f', 1)
                .arg(settings.thresholdDb, 0, 'f', 1);
        return result;
    }

    result.statusText =
        QStringLiteral("DMR Hunter: %1 candidate%2, best at %3 MHz, width %4 kHz, peak %5 dB, +%6 dB")
            .arg(result.candidates)
            .arg(result.candidates == 1 ? QString() : QStringLiteral("s"))
            .arg(result.best.centerHz / 1000000.0, 0, 'f', 6)
            .arg(result.best.widthHz / 1000.0, 0, 'f', 1)
            .arg(result.best.peakDb, 0, 'f', 1)
            .arg(result.best.excessDb, 0, 'f', 1);
    return result;
}
