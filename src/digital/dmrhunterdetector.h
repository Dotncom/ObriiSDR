#ifndef DMRHUNTERDETECTOR_H
#define DMRHUNTERDETECTOR_H

#include <QString>

#include <limits>
#include <vector>

struct DmrHunterSettings {
    bool enabled = false;
    double minWidthKhz = 6.0;
    double maxWidthKhz = 18.0;
    double thresholdDb = 10.0;
};

struct DmrHunterCandidate {
    bool valid = false;
    double centerHz = std::numeric_limits<double>::quiet_NaN();
    double widthHz = 0.0;
    float peakDb = -160.0f;
    float excessDb = 0.0f;
    float score = 0.0f;
};

struct DmrHunterResult {
    bool enabled = false;
    int candidates = 0;
    float noiseFloorDb = -160.0f;
    float thresholdDb = -150.0f;
    DmrHunterCandidate best;
    std::vector<DmrHunterCandidate> candidateList;
    QString statusText;
};

class DmrHunterDetector {
public:
    static constexpr double MinWidthKhz = 2.0;
    static constexpr double MaxWidthKhz = 50.0;
    static constexpr double MinThresholdDb = 3.0;
    static constexpr double MaxThresholdDb = 45.0;

    static DmrHunterSettings normalizedSettings(DmrHunterSettings settings);
    static DmrHunterResult analyze(const std::vector<float> &frequencies,
                                   const std::vector<float> &magnitudes,
                                   DmrHunterSettings settings);
};

#endif // DMRHUNTERDETECTOR_H
