#pragma once

#include "dspgraphmodel.h"
#include "radiosettings.h"
#include <functional>
#include <memory>
#include <vector>
struct DspStreamEndpoint;
struct DspBranchPcmFrame;

struct DspBranchExecutionStats {
    bool remoteWorkerStarted = false;
    std::size_t pendingRemoteBlocks = 0;
    std::size_t pendingRemoteBytes = 0;
    std::uint64_t droppedRemoteBlocks = 0;
};

struct DspBranchFrame {
    QString id;
    QString error;
    QString recordingStatus;
    QString audioStatus;
    double centerHz = 0.0;
    double sampleRateHz = 0.0;
    double binWidthHz = 0.0;
    double rbwHz = 0.0;
    int decimation = 1;
    int frontDecimation = 1;
    int pfbChannels = 0;
    int firTaps = 32;
    double bankProcessingMs = 0.0;
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
    using AudioConsumer = std::function<void(const DspBranchPcmFrame &)>;
    // Called on configuration/worker threads; keep bounded and do not re-enter this engine.
    void setAudioConsumer(AudioConsumer consumer);
    std::vector<DspBranchFrame> takeFrames();
    DspBranchExecutionStats executionStats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
