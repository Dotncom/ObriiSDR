#include "spectrumscienceanalyzer.h"
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>

QJsonArray SpectrumScienceAnalyzer::markerConfiguration() const {
    QJsonArray result;
    for (const auto &marker : markers)
        result.append(QJsonObject{{"label", marker.label}, {"enabled", marker.enabled}, {"frequencyHz", marker.frequencyHz}});
    return result;
}
void SpectrumScienceAnalyzer::restoreMarkerConfiguration(const QJsonArray &configuration) {
    markers.resize(2);
    clearMarkers();
    for (int i = 0; i < (std::min)(configuration.size(), MaximumMarkers); ++i) {
        if (i >= markerCount()) addMarker();
        const QJsonObject object = configuration[i].toObject();
        const double frequency = object.value("frequencyHz").toDouble();
        if (object.value("enabled").toBool() && std::isfinite(frequency) && frequency >= 0)
            setMarker(i, frequency);
    }
}

namespace {
constexpr float kFloorDb = -200.0f;

double dbToPower(float db) {
    return std::pow(10.0, static_cast<double>(db) / 10.0);
}

float powerToDb(double power) {
    return power > 1.0e-20
               ? static_cast<float>(10.0 * std::log10(power))
               : kFloorDb;
}
}

void SpectrumScienceAnalyzer::setTraceEnabled(bool maxHold, bool minHold, bool average) {
    if (maxHold && !maxHoldEnabled && !frameLevels.empty()) {
        maxHoldDb = frameLevels;
    }
    if (minHold && !minHoldEnabled && !frameLevels.empty()) {
        minHoldDb = frameLevels;
    }
    if (average && !averageEnabled && !frameLevels.empty()) {
        averageDb = frameLevels;
        averagePower.resize(frameLevels.size());
        for (std::size_t i = 0; i < frameLevels.size(); ++i) {
            averagePower[i] = static_cast<float>(dbToPower(frameLevels[i]));
        }
        updateClock.restart();
    }
    maxHoldEnabled = maxHold;
    minHoldEnabled = minHold;
    averageEnabled = average;
}

void SpectrumScienceAnalyzer::setAverageTimeSeconds(double seconds) {
    averageTimeSeconds = std::clamp(seconds, 0.05, 60.0);
}

void SpectrumScienceAnalyzer::setAverageFrameCount(int frames) {
    const int normalized = std::clamp(frames, 0, 10000);
    if (averageFrameCount == normalized) return;
    averageFrameCount = normalized;
    averageHistoryPower.clear();
}

void SpectrumScienceAnalyzer::setDetector(int mode, int frames) {
    const int normalizedMode = normalizedSpectrumDetectorMode(mode);
    const int normalizedFrames = std::clamp(frames, 1, 256);
    if (detectorMode == normalizedMode && detectorFrames == normalizedFrames) return;
    detectorMode = normalizedMode;
    detectorFrames = normalizedFrames;
    detectorHistory.clear();
    quasiPeakAmplitude.clear();
    vbwPower.clear();
}

void SpectrumScienceAnalyzer::setVbwHz(double hz) {
    const double normalized = std::isfinite(hz) ? std::clamp(hz, 0.0, 10000.0) : 0.0;
    if (std::abs(vbwHz - normalized) < 1.0e-12) return;
    vbwHz = normalized;
    vbwPower.clear();
}

void SpectrumScienceAnalyzer::setPercentileTraces(bool p50, bool p90, bool p99) {
    percentile50Enabled = p50;
    percentile90Enabled = p90;
    percentile99Enabled = p99;
    if (!p50) percentile50Db.clear();
    if (!p90) percentile90Db.clear();
    if (!p99) percentile99Db.clear();
}

void SpectrumScienceAnalyzer::resetTraces() {
    if (frameLevels.empty()) {
        maxHoldDb.clear();
        minHoldDb.clear();
        averagePower.clear();
        averageDb.clear();
        percentile50Db.clear();
        percentile90Db.clear();
        percentile99Db.clear();
        averageHistoryPower.clear();
        updateClock.invalidate();
        return;
    }
    maxHoldDb = frameLevels;
    minHoldDb = frameLevels;
    averagePower.resize(frameLevels.size());
    averageDb = frameLevels;
    for (std::size_t i = 0; i < frameLevels.size(); ++i) {
        averagePower[i] = static_cast<float>(dbToPower(frameLevels[i]));
    }
    averageHistoryPower.clear();
    updateClock.restart();
}

void SpectrumScienceAnalyzer::clear() {
    frameFrequencies.clear();
    frameLevels.clear();
    maxHoldDb.clear();
    minHoldDb.clear();
    averagePower.clear();
    averageDb.clear();
    vbwPower.clear();
    quasiPeakAmplitude.clear();
    percentile50Db.clear();
    percentile90Db.clear();
    percentile99Db.clear();
    detectorHistory.clear();
    averageHistoryPower.clear();
    currentMetrics = {};
    updateClock.invalidate();
    updateMarkerLevels();
}

bool SpectrumScienceAnalyzer::geometryMatches(const std::vector<float> &frequencies) const {
    if (frequencies.size() != frameFrequencies.size() || frequencies.empty()) {
        return false;
    }
    const double span = std::abs(static_cast<double>(frequencies.back()) - frequencies.front());
    const double tolerance = std::max(1.0, span / std::max<std::size_t>(1, frequencies.size()) * 0.25);
    return std::abs(static_cast<double>(frequencies.front()) - frameFrequencies.front()) <= tolerance &&
           std::abs(static_cast<double>(frequencies.back()) - frameFrequencies.back()) <= tolerance;
}

void SpectrumScienceAnalyzer::resetForGeometry(const std::vector<float> &frequencies,
                                               const std::vector<float> &levels) {
    frameFrequencies = frequencies;
    frequenciesAscending = std::is_sorted(frequencies.begin(), frequencies.end()) &&
        std::all_of(frequencies.begin(), frequencies.end(), [](float value) { return std::isfinite(value); });
    detectorHistory.clear();
    averageHistoryPower.clear();
    quasiPeakAmplitude.clear();
    vbwPower.clear();
    updateDetector(levels, 0.05);
    resetTraces();
    updatePercentileTraces();
}

void SpectrumScienceAnalyzer::updateDetector(const std::vector<float> &rawLevels,
                                             double elapsedSeconds) {
    if (rawLevels.empty()) {
        frameLevels.clear();
        return;
    }
    detectorHistory.push_back(rawLevels);
    while (static_cast<int>(detectorHistory.size()) > detectorFrames) {
        detectorHistory.pop_front();
    }
    frameLevels = rawLevels;
    if (detectorMode == SPECTRUM_DETECTOR_POSITIVE_PEAK ||
        detectorMode == SPECTRUM_DETECTOR_NEGATIVE_PEAK) {
        for (std::size_t bin = 0; bin < frameLevels.size(); ++bin) {
            float detected = detectorHistory.front()[bin];
            for (const auto &historyFrame : detectorHistory) {
                detected = detectorMode == SPECTRUM_DETECTOR_POSITIVE_PEAK
                               ? std::max(detected, historyFrame[bin])
                               : std::min(detected, historyFrame[bin]);
            }
            frameLevels[bin] = detected;
        }
    } else if (detectorMode == SPECTRUM_DETECTOR_RMS) {
        for (std::size_t bin = 0; bin < frameLevels.size(); ++bin) {
            double power = 0.0;
            for (const auto &historyFrame : detectorHistory) power += dbToPower(historyFrame[bin]);
            frameLevels[bin] = powerToDb(power / detectorHistory.size());
        }
    } else if (detectorMode == SPECTRUM_DETECTOR_AVERAGE) {
        for (std::size_t bin = 0; bin < frameLevels.size(); ++bin) {
            double amplitude = 0.0;
            for (const auto &historyFrame : detectorHistory) {
                amplitude += std::sqrt(dbToPower(historyFrame[bin]));
            }
            amplitude /= detectorHistory.size();
            frameLevels[bin] = powerToDb(amplitude * amplitude);
        }
    } else if (detectorMode == SPECTRUM_DETECTOR_MEDIAN) {
        std::vector<float> values;
        values.reserve(detectorHistory.size());
        for (std::size_t bin = 0; bin < frameLevels.size(); ++bin) {
            values.clear();
            for (const auto &historyFrame : detectorHistory) values.push_back(historyFrame[bin]);
            const std::size_t middle = values.size() / 2;
            std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
            frameLevels[bin] = values[middle];
        }
    } else if (detectorMode == SPECTRUM_DETECTOR_QUASI_PEAK) {
        if (quasiPeakAmplitude.size() != frameLevels.size()) {
            quasiPeakAmplitude.resize(frameLevels.size());
            for (std::size_t bin = 0; bin < frameLevels.size(); ++bin) {
                quasiPeakAmplitude[bin] = static_cast<float>(std::sqrt(dbToPower(rawLevels[bin])));
            }
        }
        const double dt = std::clamp(elapsedSeconds, 0.0001, 2.0);
        for (std::size_t bin = 0; bin < frameLevels.size(); ++bin) {
            const double amplitude = std::sqrt(dbToPower(rawLevels[bin]));
            const double tau = amplitude >= quasiPeakAmplitude[bin] ? 0.001 : 0.55;
            const double alpha = 1.0 - std::exp(-dt / tau);
            quasiPeakAmplitude[bin] = static_cast<float>(
                quasiPeakAmplitude[bin] + alpha * (amplitude - quasiPeakAmplitude[bin]));
            frameLevels[bin] = powerToDb(static_cast<double>(quasiPeakAmplitude[bin]) * quasiPeakAmplitude[bin]);
        }
    }

    if (vbwHz > 0.0) {
        if (vbwPower.size() != frameLevels.size()) {
            vbwPower.resize(frameLevels.size());
            for (std::size_t bin = 0; bin < frameLevels.size(); ++bin) {
                vbwPower[bin] = static_cast<float>(dbToPower(frameLevels[bin]));
            }
        }
        const double tau = 1.0 / (6.28318530717958647692 * vbwHz);
        const double alpha = 1.0 - std::exp(-std::clamp(elapsedSeconds, 0.0001, 2.0) / tau);
        for (std::size_t bin = 0; bin < frameLevels.size(); ++bin) {
            const double power = dbToPower(frameLevels[bin]);
            vbwPower[bin] = static_cast<float>(vbwPower[bin] + alpha * (power - vbwPower[bin]));
            frameLevels[bin] = powerToDb(vbwPower[bin]);
        }
    } else {
        vbwPower.clear();
    }
    updatePercentileTraces();
}

void SpectrumScienceAnalyzer::updatePercentileTraces() {
    auto calculate = [this](double fraction, std::vector<float> &output) {
        output.resize(frameLevels.size());
        std::vector<float> values;
        values.reserve(detectorHistory.size());
        for (std::size_t bin = 0; bin < frameLevels.size(); ++bin) {
            values.clear();
            for (const auto &historyFrame : detectorHistory) values.push_back(historyFrame[bin]);
            const std::size_t index = static_cast<std::size_t>(
                std::clamp(fraction, 0.0, 1.0) * static_cast<double>(values.size() - 1));
            std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
            output[bin] = values[index];
        }
    };
    if (percentile50Enabled) calculate(0.50, percentile50Db);
    if (percentile90Enabled) calculate(0.90, percentile90Db);
    if (percentile99Enabled) calculate(0.99, percentile99Db);
}

void SpectrumScienceAnalyzer::update(const std::vector<float> &frequencies,
                                     const std::vector<float> &levels) {
    const std::size_t count = std::min(frequencies.size(), levels.size());
    if (count < 2) {
        clear();
        return;
    }

    std::vector<float> trimmedFrequencies(frequencies.begin(), frequencies.begin() + count);
    std::vector<float> trimmedLevels(levels.begin(), levels.begin() + count);
    for (float &level : trimmedLevels) {
        if (!std::isfinite(level)) {
            level = kFloorDb;
        }
    }

    double elapsedSeconds = 0.05;
    if (updateClock.isValid()) {
        elapsedSeconds = std::clamp(updateClock.restart() / 1000.0, 0.001, 2.0);
    } else {
        updateClock.start();
    }
    if (!geometryMatches(trimmedFrequencies)) {
        resetForGeometry(trimmedFrequencies, trimmedLevels);
    } else {
        frameFrequencies.swap(trimmedFrequencies);
        updateDetector(trimmedLevels, elapsedSeconds);
    }
    const double alpha = 1.0 - std::exp(-elapsedSeconds / averageTimeSeconds);

    if (maxHoldDb.size() != count || minHoldDb.size() != count ||
        averagePower.size() != count || averageDb.size() != count) {
        resetTraces();
    }
    for (std::size_t i = 0; i < count; ++i) {
        const float level = frameLevels[i];
        if (maxHoldEnabled) {
            maxHoldDb[i] = std::max(maxHoldDb[i], level);
        }
        if (minHoldEnabled) {
            minHoldDb[i] = std::min(minHoldDb[i], level);
        }
        if (averageEnabled) {
            if (averageFrameCount <= 0) {
                const double power = dbToPower(level);
                averagePower[i] = static_cast<float>(averagePower[i] + alpha * (power - averagePower[i]));
                averageDb[i] = powerToDb(averagePower[i]);
            }
        }
    }
    if (averageEnabled && averageFrameCount > 0) {
        std::vector<float> powers(frameLevels.size());
        for (std::size_t i = 0; i < frameLevels.size(); ++i) {
            powers[i] = static_cast<float>(dbToPower(frameLevels[i]));
        }
        averageHistoryPower.push_back(std::move(powers));
        while (static_cast<int>(averageHistoryPower.size()) > averageFrameCount) {
            averageHistoryPower.pop_front();
        }
        averagePower.assign(frameLevels.size(), 0.0f);
        for (const auto &historyFrame : averageHistoryPower) {
            for (std::size_t i = 0; i < averagePower.size(); ++i) averagePower[i] += historyFrame[i];
        }
        for (std::size_t i = 0; i < averagePower.size(); ++i) {
            averagePower[i] /= static_cast<float>(averageHistoryPower.size());
            averageDb[i] = powerToDb(averagePower[i]);
        }
    } else if (!averageEnabled) {
        averageHistoryPower.clear();
    }
    updateMarkerLevels();
    updateMetrics();
}

void SpectrumScienceAnalyzer::setMarker(int index, double frequencyHz) {
    if (index < 0 || index >= static_cast<int>(markers.size()) || !std::isfinite(frequencyHz)) {
        return;
    }
    markers[static_cast<std::size_t>(index)].enabled = true;
    markers[static_cast<std::size_t>(index)].frequencyHz = frequencyHz;
    updateMarkerLevels();
    updateMetrics();
}

void SpectrumScienceAnalyzer::clearMarker(int index) {
    if (index < 0 || index >= static_cast<int>(markers.size())) {
        return;
    }
    markers[static_cast<std::size_t>(index)].enabled = false;
    markers[static_cast<std::size_t>(index)].levelDb = -160.0f;
    updateMetrics();
}

void SpectrumScienceAnalyzer::clearMarkers() {
    for (int index = 0; index < markerCount(); ++index) clearMarker(index);
}

int SpectrumScienceAnalyzer::addMarker() {
    if (markerCount() >= MaximumMarkers) return -1;
    const int index = markerCount();
    markers.push_back({false, 0.0, -160.0f, QStringLiteral("M%1").arg(index + 1)});
    return index;
}

bool SpectrumScienceAnalyzer::removeMarker(int index) {
    // A/B remain the compatible measurement-range and delta pair.
    if (index < 2 || index >= markerCount()) return false;
    markers.erase(markers.begin() + index);
    for (int i = 2; i < markerCount(); ++i) markers[i].label = QStringLiteral("M%1").arg(i + 1);
    return true;
}

bool SpectrumScienceAnalyzer::hasMarkers() const {
    return std::any_of(markers.begin(), markers.end(), [](const auto &m) { return m.enabled; });
}

SpectrumScienceMarker SpectrumScienceAnalyzer::marker(int index) const {
    return index >= 0 && index < static_cast<int>(markers.size())
               ? markers[static_cast<std::size_t>(index)]
               : SpectrumScienceMarker{};
}

int SpectrumScienceAnalyzer::nearestBin(double frequencyHz) const {
    if (frameFrequencies.empty() || !std::isfinite(frequencyHz)) {
        return -1;
    }
    if (frequenciesAscending) {
        const auto found = std::lower_bound(frameFrequencies.begin(), frameFrequencies.end(), frequencyHz);
        if (found == frameFrequencies.begin()) return 0;
        if (found == frameFrequencies.end())
            return static_cast<int>(std::lower_bound(frameFrequencies.begin(), frameFrequencies.end(), frameFrequencies.back()) - frameFrequencies.begin());
        const int right = static_cast<int>(found - frameFrequencies.begin());
        if (frequencyHz - frameFrequencies[static_cast<std::size_t>(right - 1)] <= *found - frequencyHz)
            return static_cast<int>(std::lower_bound(frameFrequencies.begin(), found, frameFrequencies[static_cast<std::size_t>(right - 1)]) - frameFrequencies.begin());
        return right;
    }
    int bestBin = 0;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (int i = 0; i < static_cast<int>(frameFrequencies.size()); ++i) {
        const double distance = std::abs(static_cast<double>(frameFrequencies[static_cast<std::size_t>(i)]) - frequencyHz);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestBin = i;
        }
    }
    return bestBin;
}

void SpectrumScienceAnalyzer::updateMarkerLevels() {
    for (SpectrumScienceMarker &entry : markers) {
        if (!entry.enabled) continue;
        const int bin = nearestBin(entry.frequencyHz);
        entry.levelDb = bin >= 0 && bin < static_cast<int>(frameLevels.size())
                            ? frameLevels[static_cast<std::size_t>(bin)]
                            : -160.0f;
    }
}

std::pair<int, int> SpectrumScienceAnalyzer::activeRange() const {
    if (frameLevels.empty()) return {-1, -1};
    if (markers[0].enabled && markers[1].enabled) {
        const int a = nearestBin(markers[0].frequencyHz);
        const int b = nearestBin(markers[1].frequencyHz);
        return {(std::min)(a, b), (std::max)(a, b)};
    }
    return {0, static_cast<int>(frameLevels.size()) - 1};
}

void SpectrumScienceAnalyzer::updateMetrics() {
    currentMetrics = {};
    const auto [first, last] = activeRange();
    if (first < 0 || last < first || last >= static_cast<int>(frameLevels.size())) return;

    std::vector<float> sorted;
    sorted.reserve(static_cast<std::size_t>(last - first + 1));
    double powerSum = 0.0;
    double weightedFrequency = 0.0;
    int peakBin = first;
    for (int i = first; i <= last; ++i) {
        const float level = frameLevels[static_cast<std::size_t>(i)];
        sorted.push_back(level);
        if (level > frameLevels[static_cast<std::size_t>(peakBin)]) peakBin = i;
        const double power = dbToPower(level);
        powerSum += power;
        weightedFrequency += power * frameFrequencies[static_cast<std::size_t>(i)];
    }
    if (sorted.empty() || powerSum <= 0.0) return;
    const std::size_t noiseIndex = static_cast<std::size_t>(std::floor((sorted.size() - 1) * 0.4));
    std::nth_element(sorted.begin(), sorted.begin() + noiseIndex, sorted.end());
    const float noise = sorted[noiseIndex];

    const double noisePower = dbToPower(noise);
    double occupiedPower = 0.0;
    for (int i = first; i <= last; ++i) {
        occupiedPower += std::max(0.0,
                                  dbToPower(frameLevels[static_cast<std::size_t>(i)]) - noisePower);
    }
    const bool havePowerAboveNoise = occupiedPower > powerSum * 1.0e-6;
    const double obwPower = havePowerAboveNoise ? occupiedPower : powerSum;
    auto occupiedWidth = [&](double fraction) {
        const double excluded = (1.0 - std::clamp(fraction, 0.0, 1.0)) * 0.5;
        const double lowerTarget = obwPower * excluded;
        const double upperTarget = obwPower * (1.0 - excluded);
        double cumulative = 0.0;
        int obwFirst = first;
        int obwLast = last;
        bool lowerFound = false;
        for (int i = first; i <= last; ++i) {
            const double rawPower = dbToPower(frameLevels[static_cast<std::size_t>(i)]);
            cumulative += havePowerAboveNoise ? std::max(0.0, rawPower - noisePower) : rawPower;
            if (!lowerFound && cumulative >= lowerTarget) {
                obwFirst = i;
                lowerFound = true;
            }
            if (cumulative >= upperTarget) {
                obwLast = i;
                break;
            }
        }
        return std::abs(static_cast<double>(frameFrequencies[static_cast<std::size_t>(obwLast)]) -
                        frameFrequencies[static_cast<std::size_t>(obwFirst)]);
    };
    auto widthBelowPeak = [&](double dropDb) {
        const float threshold = frameLevels[static_cast<std::size_t>(peakBin)] - static_cast<float>(dropDb);
        int left = peakBin;
        int right = peakBin;
        while (left > first && frameLevels[static_cast<std::size_t>(left - 1)] >= threshold) --left;
        while (right < last && frameLevels[static_cast<std::size_t>(right + 1)] >= threshold) ++right;
        return std::abs(static_cast<double>(frameFrequencies[static_cast<std::size_t>(right)]) -
                        frameFrequencies[static_cast<std::size_t>(left)]);
    };

    currentMetrics.valid = true;
    currentMetrics.rangeStartHz = frameFrequencies[static_cast<std::size_t>(first)];
    currentMetrics.rangeEndHz = frameFrequencies[static_cast<std::size_t>(last)];
    currentMetrics.peakFrequencyHz = frameFrequencies[static_cast<std::size_t>(peakBin)];
    currentMetrics.centroidFrequencyHz = weightedFrequency / powerSum;
    currentMetrics.occupiedBandwidth90Hz = occupiedWidth(0.90);
    currentMetrics.occupiedBandwidth95Hz = occupiedWidth(0.95);
    currentMetrics.occupiedBandwidthHz = occupiedWidth(0.99);
    currentMetrics.width3DbHz = widthBelowPeak(3.0);
    currentMetrics.width6DbHz = widthBelowPeak(6.0);
    currentMetrics.width20DbHz = widthBelowPeak(20.0);
    currentMetrics.peakDb = frameLevels[static_cast<std::size_t>(peakBin)];
    currentMetrics.noiseFloorDb = noise;
    currentMetrics.snrDb = currentMetrics.peakDb - noise;
    currentMetrics.channelPowerDb = powerToDb(powerSum);
    currentMetrics.binCount = last - first + 1;
    if (markers[0].enabled && markers[1].enabled && last > first) {
        const double startHz = frameFrequencies[static_cast<std::size_t>(first)];
        const double endHz = frameFrequencies[static_cast<std::size_t>(last)];
        const double channelWidthHz = std::abs(endHz - startHz);
        double lowerPower = 0.0;
        double upperPower = 0.0;
        int lowerBins = 0;
        int upperBins = 0;
        for (std::size_t i = 0; i < frameFrequencies.size(); ++i) {
            const double frequency = frameFrequencies[i];
            if (frequency >= startHz - channelWidthHz && frequency < startHz) {
                lowerPower += dbToPower(frameLevels[i]);
                ++lowerBins;
            } else if (frequency > endHz && frequency <= endHz + channelWidthHz) {
                upperPower += dbToPower(frameLevels[i]);
                ++upperBins;
            }
        }
        if (lowerBins > 0) {
            currentMetrics.adjacentPowerLowerDb = powerToDb(lowerPower);
            currentMetrics.acprLowerDb = currentMetrics.adjacentPowerLowerDb - currentMetrics.channelPowerDb;
        }
        if (upperBins > 0) {
            currentMetrics.adjacentPowerUpperDb = powerToDb(upperPower);
            currentMetrics.acprUpperDb = currentMetrics.adjacentPowerUpperDb - currentMetrics.channelPowerDb;
        }
    }
}

std::vector<int> SpectrumScienceAnalyzer::localPeakBins() const {
    std::vector<int> peaks;
    const auto [first, last] = activeRange();
    if (first < 0 || last - first < 2) return peaks;
    const float threshold = currentMetrics.valid ? currentMetrics.noiseFloorDb + 3.0f : kFloorDb;
    for (int i = first + 1; i < last; ++i) {
        const float level = frameLevels[static_cast<std::size_t>(i)];
        if (level >= threshold && level > frameLevels[static_cast<std::size_t>(i - 1)] &&
            level >= frameLevels[static_cast<std::size_t>(i + 1)]) {
            peaks.push_back(i);
        }
    }
    return peaks;
}

double SpectrumScienceAnalyzer::strongestPeakFrequency() const {
    return currentMetrics.valid ? currentMetrics.peakFrequencyHz
                                : std::numeric_limits<double>::quiet_NaN();
}

double SpectrumScienceAnalyzer::adjacentPeakFrequency(int markerIndex, int direction) const {
    const std::vector<int> peaks = localPeakBins();
    if (peaks.empty()) return strongestPeakFrequency();
    const SpectrumScienceMarker current = marker(markerIndex);
    const double origin = current.enabled ? current.frequencyHz : strongestPeakFrequency();
    double candidate = std::numeric_limits<double>::quiet_NaN();
    for (int bin : peaks) {
        const double frequency = frameFrequencies[static_cast<std::size_t>(bin)];
        if (direction < 0 && frequency < origin &&
            (!std::isfinite(candidate) || frequency > candidate)) candidate = frequency;
        if (direction >= 0 && frequency > origin &&
            (!std::isfinite(candidate) || frequency < candidate)) candidate = frequency;
    }
    if (std::isfinite(candidate)) return candidate;
    double wrapped = frameFrequencies[static_cast<std::size_t>(peaks.front())];
    for (int bin : peaks) {
        const double frequency = frameFrequencies[static_cast<std::size_t>(bin)];
        wrapped = direction < 0 ? std::max(wrapped, frequency) : std::min(wrapped, frequency);
    }
    return wrapped;
}
