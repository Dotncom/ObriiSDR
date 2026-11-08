#pragma once

#include "dspbranchaudiooutput.h"
#include "dspgraphmodel.h"
#include <map>
#include <mutex>
#include <utility>

// Private queue shared by the audio worker and its hardware-free admission tests.
class DspBranchAudioQueue final {
public:
    static constexpr int MaxPendingBytes = 48000 * 2 / 4;
    struct Pending {
        DspBranchPcmFrame frame;
        std::uint64_t admission = 0;
    };
    using Batch = std::map<QString, Pending>;

    void setActiveSinks(const QSet<QString> &ids) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.clear();
        for (const auto &id : ids) {
            if (active_.size() == DspMaxBranches) break;
            active_.insert(id);
        }
        for (auto it = pending_.begin(); it != pending_.end();)
            if (!active_.contains(it->first)) it = pending_.erase(it);
            else ++it;
        for (auto it = admitted_.begin(); it != admitted_.end();)
            if (!active_.contains(it->first)) it = admitted_.erase(it);
            else ++it;
    }

    void enqueue(const DspBranchPcmFrame &frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_.contains(frame.sinkId) || (!frame.reset && frame.pcm.isEmpty()) || frame.pcm.size() % 2) return;
        auto insertion = admitted_.try_emplace(frame.sinkId);
        auto &admitted = insertion.first->second;
        if (frame.generation < admitted.generation) return;
        bool fence = insertion.second || frame.reset || frame.generation != admitted.generation || frame.device != admitted.device;
        auto &pending = pending_[frame.sinkId];
        const bool keepPrevious = !fence && pending.admission == admitted.token;
        const bool previousReset = keepPrevious && pending.frame.reset;
        QByteArray previous;
        if (keepPrevious) previous.swap(pending.frame.pcm);
        if (frame.pcm.size() > MaxPendingBytes - previous.size()) {
            previous.clear(); fence = true; ++dropped_;
        }
        if (fence) admitted.token = ++nextAdmission_;
        admitted.generation = frame.generation;
        admitted.device = frame.device;
        pending.admission = admitted.token;
        pending.frame = frame;
        pending.frame.pcm = previous + frame.pcm.right(MaxPendingBytes);
        pending.frame.reset = fence || previousReset;
    }

    Batch drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        Batch frames;
        frames.swap(pending_);
        return frames;
    }

    // The consumer must not reenter this queue. Keep device submission inside
    // this fence so invalidation cannot overtake the final admission check.
    template <typename Consumer>
    bool deliver(const Pending &pending, Consumer &&consumer) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto admitted = admitted_.find(pending.frame.sinkId);
        if (!active_.contains(pending.frame.sinkId) || admitted == admitted_.end() ||
            admitted->second.generation != pending.frame.generation ||
            admitted->second.token != pending.admission) return false;
        std::forward<Consumer>(consumer)(pending.frame);
        return true;
    }

    template <typename Consumer>
    void withActiveSinks(Consumer &&consumer) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::forward<Consumer>(consumer)(active_);
    }

    std::uint64_t droppedFrames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    struct Admission {
        std::uint64_t generation = 0, token = 0;
        int device = -1;
    };
    mutable std::mutex mutex_;
    QSet<QString> active_;
    Batch pending_;
    std::map<QString, Admission> admitted_;
    std::uint64_t nextAdmission_ = 0, dropped_ = 0;
};
