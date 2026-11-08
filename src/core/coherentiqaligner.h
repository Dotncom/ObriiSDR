#pragma once

#include "dspsourceidentity.h"

#include <QString>

#include <cstddef>
#include <vector>

struct CoherentIqCalibration {
    DspSourceIdentity reference;
    DspSourceIdentity target;
    double sampleRateHz = 0.0;
    double centerHz = 0.0;
    int integerDelaySamples = 0;
    double fractionalDelaySamples = 0.0;
    double phaseCorrectionRadians = 0.0;
    double gainCorrection = 1.0;
    double coherence = 0.0;
    std::size_t comparedSamples = 0;
    bool valid = false;
    QString error;
};
class CoherentIqAligner final {
public:
    // Positive delay means target[n + delay] corresponds to reference[n].
    // This is a one-shot calibration routine, not part of the live hot path.
    static CoherentIqCalibration calibrate(
        const DspSourceIdentity &reference,
        const std::vector<float> &referenceIq,
        const DspSourceIdentity &target,
        const std::vector<float> &targetIq,
        double sampleRateHz,
        double centerHz,
        int maximumDelaySamples = 4096);

    // Applies delay, fractional-delay, phase and gain correction. The returned
    // vector contains interleaved complex floats and only the common valid span.
    static std::vector<float> alignTarget(const std::vector<float> &targetIq,
                                          const CoherentIqCalibration &calibration,
                                          std::size_t maximumComplexSamples = 0);
};
