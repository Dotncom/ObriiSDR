#ifndef IQCHANNELIZER_H
#define IQCHANNELIZER_H

#include "radiosettings.h"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

class IqChannelizer {
public:
    struct Options {
        double outputRateHz = 0.0;
        double cutoffHz = 0.0;
        int decimationFactor = 0;
    };
    struct Result {
        bool valid = false;
        double inputRate = 0.0;
        double outputRate = 0.0;
        double centerFrequency = 0.0;
        double listeningFrequency = 0.0;
        double frequencyShift = 0.0;
        double cutoff = 0.0;
        double targetRate = 0.0;
        int decimationFactor = 1;
        int frontDecimationFactor = 1;
        int coarseDecimationFactor = 1;
        int inputSamples = 0;
        int outputSamples = 0;
        float agcLevel = 0.0f;
    };

    void reset(double initialNcoPhaseRadians = 0.0);
    Result processFloatIq(const float *samples,
                          std::size_t floatCount,
                          const RadioSettings &settings,
                          std::vector<float> &output,
                          bool applyAgc = true,
                          const Options *options = nullptr);

private:
    friend struct IqChannelizerTestAccess;
    static constexpr int FrontDecimatorOrder = 3;
    static constexpr int FrontDecimatorMaxFactor = 64;
    static constexpr int ResamplerPhases = 64;
    static constexpr int ResamplerTaps = 32;
    static constexpr int ResamplerHistory = 64;

    void configureFrontDecimator(int factor);
    bool pushFrontDecimatorSample(const std::complex<float> &sample,
                                  std::complex<float> &output);
    void configureResampler(double inputRate, double targetRate, double cutoff);
    void pushResamplerSample(const std::complex<float> &sample,
                             std::vector<float> &output,
                             bool applyAgc);
    std::complex<float> historySample(std::int64_t sampleIndex) const;

    double ncoPhase = 0.0;
    int configuredFrontDecimationFactor = 0;
    int frontDecimationWriteIndex = 0;
    int frontDecimationCount = 0;
    float frontDecimationScale = 1.0f;
    std::array<std::array<std::complex<float>, FrontDecimatorMaxFactor>,
               FrontDecimatorOrder> frontDecimationDelay = {};
    std::array<std::complex<float>, FrontDecimatorOrder> frontDecimationSums = {};
    std::complex<float> decimationSum = {0.0f, 0.0f};
    std::array<std::complex<float>, 3> preLowPassStates = {};
    int decimationCount = 0;
    float agcLevel = 0.01f;
    double configuredCoarseRate = 0.0;
    double configuredTargetRate = 0.0;
    double configuredCutoff = 0.0;
    std::array<std::array<float, ResamplerTaps>, ResamplerPhases> resamplerTaps = {};
    std::array<std::complex<float>, ResamplerHistory> resamplerHistory = {};
    std::uint64_t resamplerSamplesSeen = 0;
    int resamplerWriteIndex = 0;
    double nextOutputPosition = 0.0;
};

#endif // IQCHANNELIZER_H
