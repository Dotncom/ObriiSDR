#pragma once

#include <QString>
#include <QtGlobal>
#include <memory>
#include <vector>

struct DspStreamBlock;

// All calls, including destruction and queries, belong to the branch worker.
// Synchronous file I/O must never run on the audio thread.
class DspStreamRecorder final {
public:
    DspStreamRecorder();
    ~DspStreamRecorder();
    DspStreamRecorder(const DspStreamRecorder &) = delete;
    DspStreamRecorder &operator=(const DspStreamRecorder &) = delete;

    // Requires an existing, explicitly selected absolute directory. maxBytes
    // caps session-wide IQ payload, rounded down to whole complex samples.
    // At most 128 segments; gaps, changed metadata and sample jumps split files.
    bool start(const QString &directory, quint64 maxBytes = 256 * 1024 * 1024);
    void append(const DspStreamBlock &block, const std::vector<float> &processedIq);
    void stop();
    bool active() const;
    QString status() const;
    // Current or last segment's CF32LE file; the matching sidecar is .json.
    QString path() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
