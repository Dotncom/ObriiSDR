#pragma once

#include <QString>
#include <memory>

struct RadioSettings;
struct DspStreamEndpoint;
struct SpectrumComputeOptions;
struct SpectrumFftFrame;

// Owns its socket and FFT state on one background thread. request() renews demand;
// it does not enqueue a frame or govern continuous IQ delivery.
class RemoteSpectrumWorker final {
public:
    RemoteSpectrumWorker();
    ~RemoteSpectrumWorker();
    RemoteSpectrumWorker(const RemoteSpectrumWorker &) = delete;
    RemoteSpectrumWorker &operator=(const RemoteSpectrumWorker &) = delete;

    void request(const RadioSettings &, const DspStreamEndpoint &,
                 const SpectrumComputeOptions &, int updateMs,
                 int overlapPercent, int localBackend);
    bool takeLatest(SpectrumFftFrame &);
    void cancel();
    QString status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
