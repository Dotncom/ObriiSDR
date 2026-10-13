#ifndef SPECTRUMFFTWORKER_H
#define SPECTRUMFFTWORKER_H

#include "iqbuffer.h"
#include "radiosettings.h"
#include "dspstreammanager.h"
#include "spectrumcomputeoptions.h"

#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct SpectrumFftHistoryRow {
    std::vector<float> magnitudes;
    std::vector<float> referenceMagnitudes;
    IqBuffer::BlockMetadata metadata;
};

struct SpectrumFftFrame {
    quint64 requestId = 0;
    quint64 generation = 0;
    bool valid = false;
    bool badAlloc = false;
    QString error;
    RadioSettings settings;
    std::vector<float> frequencies;
    std::vector<float> magnitudes;
    std::vector<float> referenceMagnitudes;
    std::vector<SpectrumFftHistoryRow> historyRows;
    IqBuffer::BlockMetadata metadata;
};
class RemoteSpectrumWorker;

class SpectrumFftWorker {
public:
    SpectrumFftWorker();
    ~SpectrumFftWorker();

    SpectrumFftWorker(const SpectrumFftWorker &) = delete;
    SpectrumFftWorker &operator=(const SpectrumFftWorker &) = delete;

    void request(const RadioSettings &settings,
                 int fftBackendPreference,
                 int updateIntervalMs = 0,
                 int overlapPercent = 0,
                 bool batchWaterfallRows = false);
    bool takeLatest(SpectrumFftFrame &frame);
    bool resultReady() const noexcept { return resultAvailable.load(std::memory_order_acquire); }
    bool workOutstanding() const noexcept { return requestOutstanding.load(std::memory_order_acquire); }
    void resetHfNoiseCancelState();
    void configureRemote(const DspStreamEndpoint &endpoint, const SpectrumComputeOptions &options);
    QString remoteStatus() const;
    bool remoteEnabled() const { return remoteOptions.mode != SpectrumComputeMode::Local; }

private:
    void run();

    std::unique_ptr<RemoteSpectrumWorker> remoteWorker;
    SpectrumComputeOptions remoteOptions;
    DspStreamEndpoint remoteEndpoint;
    std::atomic<bool> releaseLocalPlan{false};

    std::thread workerThread;
    mutable std::mutex mutex;
    std::condition_variable wakeCondition;
    bool stopping = false;
    bool hasRequest = false;
    bool hasResult = false;
    std::atomic<bool> requestOutstanding{false};
    std::atomic<bool> resultAvailable{false};
    bool resetRequested = false;
    quint64 nextRequestId = 0;
    quint64 generation = 0;
    quint64 pendingGeneration = 0;
    RadioSettings pendingSettings;
    int pendingFftBackendPreference = 0;
    int pendingUpdateIntervalMs = 0;
    int pendingOverlapPercent = 0;
    bool pendingBatchWaterfallRows = false;
    SpectrumFftFrame latestResult;

    std::uint64_t lastProducedEndFloatCount = 0;
    std::uint64_t lastProducedEpoch = 0;
    int lastProducedFftLength = 0;
    double lastProducedSampleRate = 0.0;
    int lastProducedUpdateIntervalMs = 0;
    int lastProducedOverlapPercent = 0;
};

#endif // SPECTRUMFFTWORKER_H
