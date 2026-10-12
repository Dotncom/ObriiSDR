#ifndef SPECTRUMSCIENCEANALYZER_H
#define SPECTRUMSCIENCEANALYZER_H

#include "spectrumoverlaytypes.h"

#include <QElapsedTimer>
#include <QJsonArray>

#include <algorithm>
#include <array>
#include <deque>
#include <utility>
#include <vector>

enum SpectrumDetectorMode {
    SPECTRUM_DETECTOR_SAMPLE = 0,
    SPECTRUM_DETECTOR_POSITIVE_PEAK = 1,
    SPECTRUM_DETECTOR_NEGATIVE_PEAK = 2,
    SPECTRUM_DETECTOR_RMS = 3,
    SPECTRUM_DETECTOR_AVERAGE = 4,
    SPECTRUM_DETECTOR_MEDIAN = 5,
    SPECTRUM_DETECTOR_QUASI_PEAK = 6
};

inline int normalizedSpectrumDetectorMode(int mode) {
    return (std::clamp)(mode,
                        static_cast<int>(SPECTRUM_DETECTOR_SAMPLE),
                        static_cast<int>(SPECTRUM_DETECTOR_QUASI_PEAK));
}

struct SpectrumScienceMetrics {
    bool valid = false;
    double rangeStartHz = 0.0;
    double rangeEndHz = 0.0;
    double peakFrequencyHz = 0.0;
    double centroidFrequencyHz = 0.0;
    double occupiedBandwidthHz = 0.0;
    double occupiedBandwidth90Hz = 0.0;
    double occupiedBandwidth95Hz = 0.0;
    double width3DbHz = 0.0;
    double width6DbHz = 0.0;
    double width20DbHz = 0.0;
    float peakDb = -160.0f;
    float noiseFloorDb = -160.0f;
    float snrDb = 0.0f;
    float channelPowerDb = -160.0f;
    float adjacentPowerLowerDb = -160.0f;
    float adjacentPowerUpperDb = -160.0f;
    float acprLowerDb = 0.0f;
    float acprUpperDb = 0.0f;
    int binCount = 0;
};

class SpectrumScienceAnalyzer {
public:
    static constexpr int MaximumMarkers = 64;
    int markerCount() const { return static_cast<int>(markers.size()); }
    int addMarker();
    bool removeMarker(int index);
    bool hasMarkers() const;
    QJsonArray markerConfiguration() const;
    void restoreMarkerConfiguration(const QJsonArray &configuration);
    void setTraceEnabled(bool maxHold, bool minHold, bool average);
    void setAverageTimeSeconds(double seconds);
    void setAverageFrameCount(int frames);
    void setDetector(int mode, int frames);
    void setVbwHz(double hz);
    void setPercentileTraces(bool p50, bool p90, bool p99);
    void resetTraces();
    void clear();

    void update(const std::vector<float> &frequencies,
                const std::vector<float> &levels);

    void setMarker(int index, double frequencyHz);
    void clearMarker(int index);
    void clearMarkers();
    SpectrumScienceMarker marker(int index) const;

    double strongestPeakFrequency() const;
    double adjacentPeakFrequency(int markerIndex, int direction) const;

    const std::vector<float> &maxHoldTrace() const { return maxHoldDb; }
    const std::vector<float> &minHoldTrace() const { return minHoldDb; }
    const std::vector<float> &averageTrace() const { return averageDb; }
    const std::vector<float> &percentile50Trace() const { return percentile50Db; }
    const std::vector<float> &percentile90Trace() const { return percentile90Db; }
    const std::vector<float> &percentile99Trace() const { return percentile99Db; }
    const std::vector<float> &frequencies() const { return frameFrequencies; }
    const std::vector<float> &levels() const { return frameLevels; }
    const SpectrumScienceMetrics &metrics() const { return currentMetrics; }

private:
    bool geometryMatches(const std::vector<float> &frequencies) const;
    void resetForGeometry(const std::vector<float> &frequencies,
                          const std::vector<float> &levels);
    void updateMarkerLevels();
    void updateMetrics();
    void updateDetector(const std::vector<float> &rawLevels, double elapsedSeconds);
    void updatePercentileTraces();
    int nearestBin(double frequencyHz) const;
    std::pair<int, int> activeRange() const;
    std::vector<int> localPeakBins() const;

    bool maxHoldEnabled = false;
    bool minHoldEnabled = false;
    bool averageEnabled = false;
    double averageTimeSeconds = 2.0;
    int averageFrameCount = 0;
    int detectorMode = SPECTRUM_DETECTOR_SAMPLE;
    int detectorFrames = 8;
    double vbwHz = 0.0;
    bool percentile50Enabled = false;
    bool percentile90Enabled = false;
    bool percentile99Enabled = false;
    std::vector<float> frameFrequencies;
    bool frequenciesAscending = false;
    std::vector<float> frameLevels;
    std::vector<float> maxHoldDb;
    std::vector<float> minHoldDb;
    std::vector<float> averagePower;
    std::vector<float> averageDb;
    std::vector<float> vbwPower;
    std::vector<float> quasiPeakAmplitude;
    std::vector<float> percentile50Db;
    std::vector<float> percentile90Db;
    std::vector<float> percentile99Db;
    std::deque<std::vector<float>> detectorHistory;
    std::deque<std::vector<float>> averageHistoryPower;
    std::vector<SpectrumScienceMarker> markers{
        {false, 0.0, -160.0f, QStringLiteral("A")},
        {false, 0.0, -160.0f, QStringLiteral("B")}
    };
    SpectrumScienceMetrics currentMetrics;
    QElapsedTimer updateClock;
};

#endif // SPECTRUMSCIENCEANALYZER_H
