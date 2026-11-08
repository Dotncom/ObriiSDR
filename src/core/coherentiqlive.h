#pragma once

#include "dspiqsourceregistry.h"

#include <complex>

class CoherentIqLive final {
public:
    enum class Timeline { Hardware, Signal };
    explicit CoherentIqLive(Timeline timeline = Timeline::Hardware) : timeline(timeline) {}
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
                   DspIqSourceSnapshot &output, QString &error);

private:
    const Timeline timeline;
    struct Input {
        DspSourceIdentity source;
        std::uint64_t continuityId = 0;
        int whole = 0;
        double fractional = 0.0;
        std::complex<double> correction{1.0, 0.0};
    };
    std::vector<Input> aligned;
    std::uint64_t next = 0;
    double rate = 0;
    double center = 0;
    QString clock;
};
