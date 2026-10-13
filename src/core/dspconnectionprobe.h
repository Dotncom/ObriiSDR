#pragma once
#include <QString>
#include <atomic>
#include <memory>

struct DspStreamEndpoint;
class QTcpSocket;
class QByteArray;

enum class DspProbeStage { Idle, Connecting, Handshake, Latency, Upload, Download, Complete, Cancelled, Failed };
struct DspConnectionProbeResult {
    DspProbeStage stage = DspProbeStage::Idle;
    QString errorCode, detail;
    double connectMs = 0, handshakeMs = 0;
    double rttMinMs = 0, rttMedianMs = 0, rttP95Ms = 0, jitterMs = 0;
    double uploadMBps = 0, downloadMBps = 0;
    int pingCount = 0, uploadBytes = 0, downloadBytes = 0, mainFftMaxLength = 0;
};

// A user-started synthetic test, on its own thread/socket. Never reads IQ or enables DSP.
class DspConnectionProbe final {
public:
    DspConnectionProbe();
    ~DspConnectionProbe();
    bool start(const DspStreamEndpoint &endpoint);
    void cancel();
    bool running() const;
    DspConnectionProbeResult result() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct DspConnectionEstimate {
    double continuousUploadMBps = 0, windowUploadMBps = 0, spectrumDownloadMBps = 0;
    double requestedFramesPerSecond = 0, transportMsPerWindow = 0, networkFramesPerSecond = 0;
    double continuousFramesPerSecond = 0, continuousSpectrumDownloadMBps = 0;
    bool memoryFits = false, windowsSnapshotFits = false, windowsFit = false, continuousFits = false;
};
// 25% bandwidth headroom; network-only estimates, not a remote FFT benchmark.
DspConnectionEstimate estimateDspConnection(const DspConnectionProbeResult &result, double sampleRate,
                                           int fftLength, int updateMs, int overlapPercent, int uploadBudgetMBps);

namespace DspConnectionProbeProtocol {
constexpr int Version = 1;
constexpr int PingCount = 12;
constexpr int ChunkBytes = 256 * 1024;
constexpr int MaxBytes = 128 * 1024 * 1024;
constexpr int DurationMs = 1500;
// Called only after authentication and exclusive diagnostic-session admission.
void serve(QTcpSocket &socket, QByteArray &buffer, const std::atomic<bool> &stop);
}
