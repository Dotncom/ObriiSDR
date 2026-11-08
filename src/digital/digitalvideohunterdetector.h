#ifndef DIGITALVIDEOHUNTERDETECTOR_H
#define DIGITALVIDEOHUNTERDETECTOR_H

#include <QString>

#include <limits>
#include <vector>

struct DigitalVideoHunterSettings {
    bool enabled = false;
    double minWidthMhz = 2.0;
    double maxWidthMhz = 80.0;
    double thresholdDb = 6.0;
};

struct DigitalVideoHunterCandidate {
    bool valid = false;
    double centerHz = std::numeric_limits<double>::quiet_NaN();
    double widthHz = 0.0;
    float peakDb = -160.0f;
    float averageDb = -160.0f;
    float excessDb = 0.0f;
    float flatnessDb = 0.0f;
    float occupancy = 0.0f;
    float score = 0.0f;
    QString type;
};

struct DigitalVideoHunterResult {
    bool enabled = false;
    int candidates = 0;
    float noiseFloorDb = -160.0f;
    float thresholdDb = -154.0f;
    DigitalVideoHunterCandidate best;
    std::vector<DigitalVideoHunterCandidate> candidateList;
    QString statusText;
};

class DigitalVideoHunterDetector {
public:
    static constexpr double MinWidthMhz = 0.5;
    static constexpr double MaxWidthMhz = 120.0;
    static constexpr double MinThresholdDb = 2.0;
    static constexpr double MaxThresholdDb = 35.0;

    static DigitalVideoHunterSettings normalizedSettings(DigitalVideoHunterSettings settings);
    static DigitalVideoHunterResult analyze(const std::vector<float> &frequencies,
                                            const std::vector<float> &magnitudes,
                                            DigitalVideoHunterSettings settings);
};

#endif // DIGITALVIDEOHUNTERDETECTOR_H
