#pragma once

#include "analyzerframechannel.h"
#include "dspsourceidentity.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

class MultiSdrLiveIq final {
public:
    struct Preview {
        AnalyzerSpectrumFrame frame;
        quint64 combinedSamples = 0;
        int sourceCount = 0;
        QString error;
        QString waiting;
        std::optional<double> maximumPhaseDriftHz;
    };

    ~MultiSdrLiveIq();
    QString start(std::vector<DspSourceIdentity> sources, int maximumDelay, bool virtualPair, bool alignBySignal = false);
    void reacquireSignal() { signalRestart = true; }
    void stop();
    Preview preview() const;

private:
    void run(std::vector<DspSourceIdentity> sources, int maximumDelay, bool virtualPair, bool alignBySignal);
    mutable std::mutex previewMutex;
    Preview latest;
    std::atomic<bool> stopping{false};
    std::atomic<bool> signalRestart{false};
    std::thread worker;
};
