#pragma once

#include <atomic>
#include <cstdint>

class IqFrameQueueBudget {
public:
    static constexpr std::int64_t MaxPendingBytes = 32LL * 1024LL * 1024LL;
    explicit IqFrameQueueBudget(std::int64_t limit = MaxPendingBytes)
        : maxPendingBytes(limit > 0 && limit <= MaxPendingBytes ? limit : MaxPendingBytes) {}

    bool reserve(std::int64_t bytes, std::uint64_t samples) {
        if (bytes <= 0 || bytes > maxPendingBytes) {
            noteDrop(samples);
            return false;
        }
        std::int64_t pending = pendingBytes.load(std::memory_order_relaxed);
        do {
            if (pending > maxPendingBytes - bytes) {
                noteDrop(samples);
                return false;
            }
        } while (!pendingBytes.compare_exchange_weak(pending, pending + bytes,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed));
        return true;
    }

    void release(std::int64_t bytes) {
        pendingBytes.fetch_sub(bytes, std::memory_order_acq_rel);
    }

    std::uint64_t takeDroppedFrames() {
        return droppedFrames.exchange(0, std::memory_order_acq_rel);
    }

    std::uint64_t takeDroppedSamples() {
        return droppedSamples.exchange(0, std::memory_order_acq_rel);
    }

    std::int64_t pending() const {
        return pendingBytes.load(std::memory_order_acquire);
    }

private:
    const std::int64_t maxPendingBytes;
    void noteDrop(std::uint64_t samples) {
        droppedSamples.fetch_add(samples, std::memory_order_relaxed);
        droppedFrames.fetch_add(1, std::memory_order_release);
    }

    std::atomic<std::int64_t> pendingBytes{0};
    std::atomic<std::uint64_t> droppedFrames{0};
    std::atomic<std::uint64_t> droppedSamples{0};
};
