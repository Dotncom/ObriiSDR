#pragma once

#include "dspiqsourceregistry.h"

#include <complex>
#include <optional>

struct CoherentIqLiveOptions {
    bool noiseWeighted = false;
    bool trackPhase = false;
};

class CoherentIqLive final {
public:
    enum class Timeline { Hardware, Signal };
    explicit CoherentIqLive(Timeline timeline = Timeline::Hardware, CoherentIqLiveOptions options = {})
        : timeline(timeline), options(options) {}
    static constexpr int BlockSamples = 1024;
    static constexpr int MaximumDelay = 512;
    static constexpr double MaximumRate = 192000.0;

    static DspSourceIdentity outputIdentity();
    // The first input is the reference. Signal mode takes indices already
    // mapped to the reference by correlation, without asserting hardware sync.
    bool calibrate(const std::vector<DspIqSourceSnapshot> &inputs, int maximumDelay, QString &error);
    // A successful call with empty IQ means a source has not reached this block yet.
    // A failed call invalidates the session and requires fresh calibration.
    bool takeBlock(const std::vector<DspIqSourceSnapshot> &inputs,
                   DspIqSourceSnapshot &output, QString &error,
                   std::vector<std::vector<float>> *alignedChannels = nullptr);
    // Largest measured relative phase rate against the reference, in Hz.
    std::optional<double> maximumPhaseDriftHz() const { return measuredDriftHz; }
    const std::vector<double> &combiningWeights() const { return weights; }

private:
    const Timeline timeline;
    const CoherentIqLiveOptions options;
    struct Input {
        DspSourceIdentity source;
        std::uint64_t continuityId = 0;
        int whole = 0;
        double fractional = 0.0;
        std::complex<double> correction{1.0, 0.0};
        double phaseBaseline = 0.0;
        std::optional<double> previousPhase;
        double estimatedAdvance = 0.0;
        double trackingPhase = 0.0;
        double trackingAdvance = 0.0;
    };
    std::vector<Input> aligned;
    std::vector<double> weights;
    std::uint64_t next = 0;
    double rate = 0;
    double center = 0;
    QString clock;
    std::optional<std::uint64_t> phaseBaselineSample;
    std::optional<double> measuredDriftHz;
};

// Two adjacent, equally sampled bands on a shared timeline. Each branch is
// interpolated continuously before translating it to its place in the output.
class CoherentIqBandJoin final {
public:
    static constexpr int InputBlockSamples = CoherentIqLive::BlockSamples / 2;

    bool configure(const std::vector<DspIqSourceSnapshot> &inputs, QString &error);
    bool takeBlock(const std::vector<DspIqSourceSnapshot> &inputs, DspIqSourceSnapshot &output, QString &error);

private:
    struct Band {
        DspSourceIdentity source;
        std::uint64_t continuity = 0;
        double center = 0.0;
    };
    Band low, high;
    std::size_t lowIndex = 0;
    std::uint64_t next = 0;
    double rate = 0.0;
    QString clock;
    bool configured = false;
};
