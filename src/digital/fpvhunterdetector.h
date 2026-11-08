#ifndef FPVHUNTERDETECTOR_H
#define FPVHUNTERDETECTOR_H

#include <QString>

#include <limits>
#include <vector>

struct FpvHunterSettings {
    bool enabled = false;
    double minWidthMhz = 4.0;
    double maxWidthMhz = 40.0;
    double thresholdDb = 8.0;
};

struct FpvHunterCandidate {
    bool valid = false;
    double centerHz = std::numeric_limits<double>::quiet_NaN();
    double widthHz = 0.0;
    float peakDb = -160.0f;
    float averageDb = -160.0f;
    float excessDb = 0.0f;
    float score = 0.0f;
    QString type;
};

struct FpvHunterResult {
    bool enabled = false;
    int candidates = 0;
    float noiseFloorDb = -160.0f;
    float thresholdDb = -152.0f;
    FpvHunterCandidate best;
    std::vector<FpvHunterCandidate> candidateList;
    QString statusText;
};

class FpvHunterDetector {
public:
    static constexpr double MinWidthMhz = 0.5;
    static constexpr double MaxWidthMhz = 80.0;
    static constexpr double MinThresholdDb = 2.0;
    static constexpr double MaxThresholdDb = 40.0;

    static FpvHunterSettings normalizedSettings(FpvHunterSettings settings);
    static FpvHunterResult analyze(const std::vector<float> &frequencies,
                                   const std::vector<float> &magnitudes,
                                   FpvHunterSettings settings);
};

#endif // FPVHUNTERDETECTOR_H
