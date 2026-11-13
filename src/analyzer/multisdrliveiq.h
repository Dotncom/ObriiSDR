#pragma once

#include "analyzerframechannel.h"
#include "coherentiqlive.h"
#include "dspsourceidentity.h"

#include <atomic>
#include <memory>
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
        bool replayFinished = false;
        QString replayInterruption;
        std::vector<double> combiningWeights;
    };

    struct RecordingStatus {
        bool active = false;
        bool finishing = false;
        quint64 samples = 0;
        QString manifest;
        QString error;
    };

    ~MultiSdrLiveIq();
    QString start(std::vector<DspSourceIdentity> sources, int maximumDelay, bool virtualPair,
                  bool alignBySignal = false, bool stitchBands = false, CoherentIqLiveOptions options = {});
    QString startReplay(const QString &manifest);
    QString startRecording(const QString &directory);
    void stopRecording(const QString &reason = {});
    RecordingStatus recordingStatus() const;
    void reacquireSignal() { signalRestart = true; }
    void stop();
    Preview preview() const;

private:
    class Recorder;
    void run(std::vector<DspSourceIdentity> sources, int maximumDelay, bool virtualPair,
             bool alignBySignal, bool stitchBands, CoherentIqLiveOptions options);
    void runReplay(QString manifest);
    std::shared_ptr<Recorder> activeRecorder() const;
    mutable std::mutex previewMutex;
    Preview latest;
    std::shared_ptr<Recorder> recorder;
    bool recordable = false;
    std::atomic<bool> stopping{false};
    std::atomic<bool> signalRestart{false};
    std::thread worker;
};
