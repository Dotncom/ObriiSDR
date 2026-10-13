#pragma once

#include "dspgraphmodel.h"
#include "radiosettings.h"
#include <memory>
#include <vector>
struct DspStreamEndpoint;

struct DspBranchFrame {
    QString id;
    QString error;
    QString recordingStatus;
    double centerHz = 0.0;
    double sampleRateHz = 0.0;
    double binWidthHz = 0.0;
    double rbwHz = 0.0;
    int decimation = 1;
    int frontDecimation = 1;
    std::uint64_t sequence = 0;
    std::uint64_t generation = 0;
    std::uint64_t gaps = 0;
    double processingMs = 0.0;
    std::shared_ptr<const std::vector<float>> iq;
    std::vector<float> frequencies;
    std::vector<float> levels;
};

class DspBranchEngine final {
public:
    DspBranchEngine();
    ~DspBranchEngine();
    DspBranchEngine(const DspBranchEngine &) = delete;
    DspBranchEngine &operator=(const DspBranchEngine &) = delete;
    void configure(const DspGraphPlan &plan, const RadioSettings &source, bool active);
    void setExternalEndpoint(const DspStreamEndpoint &endpoint);
    void setRecording(const QString &branchId, const QString &directory);
    std::vector<DspBranchFrame> takeFrames();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
