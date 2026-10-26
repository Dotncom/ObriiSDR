#ifndef SPECTRUMDISPLAYREDUCER_H
#define SPECTRUMDISPLAYREDUCER_H

#include <vector>

struct SpectrumDisplayFrame {
    std::vector<float> frequencies;
    std::vector<float> levels;
    std::vector<float> overlayLevels;
};

enum class SpectrumDisplayReduction {
    Peak = 0,
    AverageDb = 1,
    AveragePower = 2,
    Sample = 3,
    Minimum = 4
};

void prepareSpectrumDisplayFrame(const std::vector<float> &sourceFrequencies,
                                 const std::vector<float> &sourceLevels,
                                 const std::vector<float> *sourceOverlayLevels,
                                 int sourceCount,
                                 double minFrequency,
                                 double maxFrequency,
                                 int targetBins,
                                 float fallbackLevel,
                                 SpectrumDisplayReduction reduction,
                                 SpectrumDisplayFrame &output,
                                 bool sourceLevelsFrequencyOrdered = false,
                                 bool preserveFrequencyGaps = false);

#endif // SPECTRUMDISPLAYREDUCER_H
