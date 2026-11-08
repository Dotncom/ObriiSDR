#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <cstdint>
#include <memory>

struct DspBranchPcmFrame {
    QString sinkId;
    std::uint64_t generation = 0;
    int device = -1;
    float volume = 0.5f;
    bool reset = false;
    QByteArray pcm; // 48 kHz mono signed 16-bit PCM.
};

// Bounded audio delivery independent of GUI updates and spectrum frame dropping.
class DspBranchAudioOutput final {
public:
    DspBranchAudioOutput();
    ~DspBranchAudioOutput();
    void setActiveSinks(const QSet<QString> &ids);
    void enqueue(const DspBranchPcmFrame &frame);
    std::uint64_t droppedFrames() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
