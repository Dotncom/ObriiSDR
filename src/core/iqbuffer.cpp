#include "iqbuffer.h"
#include "diagnosticlogging.h"

#include <deque>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include <QDebug>
#ifdef OBRII_IQBUFFER_SELFTEST
#include <functional>
#endif

namespace {

constexpr std::size_t MAX_QUEUED_BLOCKS = 256;
constexpr std::size_t MAX_QUEUED_FLOATS = 16 * 1024 * 1024;
constexpr std::size_t MIN_LIVE_QUEUED_FLOATS = 128 * 1024;
constexpr double MAX_LIVE_QUEUED_SECONDS = 1.25;
constexpr std::size_t MAX_SNAPSHOT_FLOATS = 16 * 1024 * 1024;
constexpr std::size_t SNAPSHOT_COPY_CHUNK_FLOATS = 256 * 1024;
constexpr std::size_t MAX_RECYCLED_BLOCKS = 8;
constexpr std::size_t MAX_PENDING_SNAPSHOT_BLOCKS = 8;
constexpr std::size_t MAX_PENDING_SNAPSHOT_FLOATS = 4 * 1024 * 1024;

struct PendingSnapshotBlock {
    std::vector<float> samples;
    IqBuffer::BlockMetadata metadata;
    std::uint64_t epoch = 0;
};

std::mutex g_iqMutex;
std::mutex g_audioQueueMutex;
std::mutex g_snapshotPublishMutex;
std::deque<PendingSnapshotBlock> g_pendingSnapshotBlocks;
std::size_t g_pendingSnapshotFloatCount = 0;
std::vector<float> g_iqSnapshot(MAX_SNAPSHOT_FLOATS);
std::size_t g_iqSnapshotStart = 0;
std::size_t g_iqSnapshotSize = 0;
std::deque<std::vector<float>> g_iqBlocks;
std::deque<std::uint64_t> g_iqBlockSequences;
std::deque<std::vector<float>> g_recycledIqBlocks;
std::size_t g_iqQueuedFloatCount = 0;
std::atomic<std::uint64_t> g_droppedQueuedBlocks{0};
std::atomic<std::uint64_t> g_droppedQueuedFloats{0};
std::atomic<std::uint64_t> g_audioBlockSequence{0};
std::atomic<std::uint64_t> g_skippedSnapshotBlocks{0};
std::atomic<std::uint64_t> g_deferredSnapshotBlocks{0};
std::atomic_bool g_snapshotLossPending{false};
std::atomic_bool g_discardPendingSnapshots{false};
std::atomic<std::uint64_t> g_iqSequence{0};
std::atomic<std::uint64_t> g_iqEpoch{1};
std::uint64_t g_totalSnapshotFloatCount = 0;
std::uint64_t g_snapshotResetGeneration = 1;
std::atomic<double> g_sampleRateEstimate{0.0};
IqBuffer::BlockMetadata g_latestMetadata;
std::atomic<std::uint64_t> g_traceEpoch{0};
std::atomic<int> g_tracePublishRemaining{0};
std::atomic<int> g_traceRejectRemaining{0};
std::atomic<int> g_traceSnapshotRemaining{0};
std::atomic<int> g_tracePopRemaining{0};

bool traceMatchesCurrentEpoch() {
    return fobosVerboseLoggingEnabled() &&
           g_traceEpoch.load(std::memory_order_relaxed) != 0 &&
           g_iqEpoch.load(std::memory_order_relaxed) ==
               g_traceEpoch.load(std::memory_order_relaxed);
}

bool traceMatchesPublishEpoch(std::uint64_t expectedEpoch) {
    return fobosVerboseLoggingEnabled() &&
           g_traceEpoch.load(std::memory_order_relaxed) != 0 &&
           (g_iqEpoch.load(std::memory_order_relaxed) ==
                g_traceEpoch.load(std::memory_order_relaxed) ||
            expectedEpoch == g_traceEpoch.load(std::memory_order_relaxed));
}

void logTraceState(const char *event) {
    if (!fobosVerboseLoggingEnabled()) {
        return;
    }
    qDebug() << "[IqBufferTrace]" << event
             << "epoch" << static_cast<qulonglong>(g_iqEpoch)
             << "traceEpoch" << static_cast<qulonglong>(g_traceEpoch)
             << "sequence" << static_cast<qulonglong>(g_iqSequence)
             << "snapshotStart" << g_iqSnapshotStart
             << "snapshotSize" << g_iqSnapshotSize
             << "queuedBlocks" << g_iqBlocks.size()
             << "queuedFloats" << g_iqQueuedFloatCount
             << "sampleRateEstimate" << g_sampleRateEstimate.load(std::memory_order_relaxed);
}

void recycleBlock(std::vector<float> &&block) {
    block.clear();
    if (block.capacity() == 0 || g_recycledIqBlocks.size() >= MAX_RECYCLED_BLOCKS) {
        return;
    }
    g_recycledIqBlocks.push_back(std::move(block));
}

void appendToSnapshot(const float *samples, std::size_t floatCount) {
    if (floatCount >= MAX_SNAPSHOT_FLOATS) {
        std::copy(samples + (floatCount - MAX_SNAPSHOT_FLOATS), samples + floatCount, g_iqSnapshot.begin());
        g_iqSnapshotStart = 0;
        g_iqSnapshotSize = MAX_SNAPSHOT_FLOATS;
        return;
    }

    if (g_iqSnapshotSize + floatCount > MAX_SNAPSHOT_FLOATS) {
        const std::size_t dropCount = g_iqSnapshotSize + floatCount - MAX_SNAPSHOT_FLOATS;
        g_iqSnapshotStart = (g_iqSnapshotStart + dropCount) % MAX_SNAPSHOT_FLOATS;
        g_iqSnapshotSize -= dropCount;
    }

    std::size_t writePos = (g_iqSnapshotStart + g_iqSnapshotSize) % MAX_SNAPSHOT_FLOATS;
    const std::size_t firstCopy = (std::min)(floatCount, MAX_SNAPSHOT_FLOATS - writePos);
    std::copy(samples, samples + firstCopy, g_iqSnapshot.begin() + static_cast<std::ptrdiff_t>(writePos));
    if (firstCopy < floatCount) {
        std::copy(samples + firstCopy, samples + floatCount, g_iqSnapshot.begin());
    }
    g_iqSnapshotSize += floatCount;
}

void appendSnapshotBlock(const float *samples, std::size_t floatCount,
                         const IqBuffer::BlockMetadata *metadata) {
    appendToSnapshot(samples, floatCount);
    g_totalSnapshotFloatCount += static_cast<std::uint64_t>(floatCount);
    const auto sequence = g_iqSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    g_latestMetadata = metadata ? *metadata : IqBuffer::BlockMetadata();
    g_latestMetadata.sequence = sequence;
    g_latestMetadata.epoch = g_iqEpoch.load(std::memory_order_relaxed);
    g_latestMetadata.totalFloatCount = g_totalSnapshotFloatCount;
    g_latestMetadata.floatCount = floatCount;
}

bool copySnapshotTail(std::vector<float> &out,
                      std::size_t maxFloatCount,
                      std::uint64_t requestedEndFloatCount,
                      std::uint64_t *sequence,
                      IqBuffer::BlockMetadata *metadata,
                      const char *source) {
    std::size_t allocatedCount = 0;
    std::uint64_t requestedCaptureEnd = 0;
    {
        std::lock_guard<std::mutex> lock(g_iqMutex);
        const std::uint64_t retainedStart =
            g_totalSnapshotFloatCount - static_cast<std::uint64_t>(g_iqSnapshotSize);
        requestedCaptureEnd = requestedEndFloatCount == 0
                                  ? g_totalSnapshotFloatCount
                                  : (std::min)(requestedEndFloatCount, g_totalSnapshotFloatCount);
        const std::uint64_t retainedBeforeEnd =
            requestedCaptureEnd > retainedStart ? requestedCaptureEnd - retainedStart : 0;
        allocatedCount = static_cast<std::size_t>((std::min)(
            retainedBeforeEnd,
            static_cast<std::uint64_t>(maxFloatCount)));
        allocatedCount -= allocatedCount % 2U;
        if (allocatedCount == 0) {
            out.clear();
            if (sequence) {
                *sequence = g_iqSequence;
            }
            if (metadata) {
                *metadata = g_latestMetadata;
            }
            const bool shouldTrace = traceMatchesCurrentEpoch() &&
                                     g_traceSnapshotRemaining > 0;
            if (shouldTrace) {
                --g_traceSnapshotRemaining;
                qDebug() << "[IqBufferTrace]" << source
                         << "empty"
                         << "epoch" << static_cast<qulonglong>(g_iqEpoch)
                         << "sequence" << static_cast<qulonglong>(g_iqSequence)
                         << "maxFloatCount" << maxFloatCount
                         << "snapshotStart" << g_iqSnapshotStart
                         << "snapshotSize" << g_iqSnapshotSize;
            }
            return false;
        }
    }

    // Allocate outside the shared IQ mutex. Large FFT snapshots can be tens of
    // megabytes; holding the mutex here starves the live audio consumer.
    out.resize(allocatedCount);

    std::size_t copyCount = 0;
    std::uint64_t captureStart = 0;
    std::uint64_t captureEnd = 0;
    std::uint64_t captureResetGeneration = 0;
    std::size_t copied = 0;
    {
        std::lock_guard<std::mutex> lock(g_iqMutex);
        copyCount = (std::min)({g_iqSnapshotSize, maxFloatCount, allocatedCount});
        copyCount -= copyCount % 2U;
        if (copyCount == 0) {
            out.clear();
            return false;
        }
        if (copyCount < out.size()) {
            out.resize(copyCount);
        }
        // A live tail snapshot follows the newest ring end at the moment the
        // copy actually begins. A historical snapshot must retain its exact
        // requested endpoint so batched FFT rows preserve their time spacing.
        captureEnd = requestedEndFloatCount == 0
                         ? g_totalSnapshotFloatCount
                         : requestedCaptureEnd;
        captureStart = captureEnd - copyCount;
        captureResetGeneration = g_snapshotResetGeneration;
        const std::uint64_t retainedStart =
            g_totalSnapshotFloatCount - static_cast<std::uint64_t>(g_iqSnapshotSize);
        if (captureStart < retainedStart || captureEnd > g_totalSnapshotFloatCount) {
            out.clear();
            return false;
        }
        const std::size_t snapshotOffset = static_cast<std::size_t>(captureStart - retainedStart);
        const std::size_t readStart =
            (g_iqSnapshotStart + snapshotOffset) % MAX_SNAPSHOT_FLOATS;
        const std::size_t firstChunkCount =
            (std::min)(SNAPSHOT_COPY_CHUNK_FLOATS, copyCount);
        const std::size_t firstCopy =
            (std::min)(firstChunkCount, MAX_SNAPSHOT_FLOATS - readStart);
        std::copy(g_iqSnapshot.begin() + static_cast<std::ptrdiff_t>(readStart),
                  g_iqSnapshot.begin() + static_cast<std::ptrdiff_t>(readStart + firstCopy),
                  out.begin());
        if (firstCopy < firstChunkCount) {
            std::copy(g_iqSnapshot.begin(),
                      g_iqSnapshot.begin() + static_cast<std::ptrdiff_t>(firstChunkCount - firstCopy),
                      out.begin() + static_cast<std::ptrdiff_t>(firstCopy));
        }
        copied = firstChunkCount;
        if (sequence) {
            *sequence = g_iqSequence;
        }
        if (metadata) {
            *metadata = g_latestMetadata;
        }
        const bool shouldTrace = traceMatchesCurrentEpoch() &&
                                 g_traceSnapshotRemaining > 0;
        if (shouldTrace) {
            --g_traceSnapshotRemaining;
            qDebug() << "[IqBufferTrace]" << source
                     << "chunked"
                     << "epoch" << static_cast<qulonglong>(g_iqEpoch)
                     << "sequence" << static_cast<qulonglong>(g_iqSequence)
                     << "maxFloatCount" << maxFloatCount
                     << "snapshotStart" << g_iqSnapshotStart
                     << "snapshotSize" << g_iqSnapshotSize
                     << "copyCount" << copyCount
                     << "chunkFloats" << SNAPSHOT_COPY_CHUNK_FLOATS;
        }
    }

    while (copied < copyCount) {
        const std::size_t chunkCount =
            (std::min)(SNAPSHOT_COPY_CHUNK_FLOATS, copyCount - copied);
        std::lock_guard<std::mutex> lock(g_iqMutex);
        if (g_snapshotResetGeneration != captureResetGeneration ||
            g_totalSnapshotFloatCount < captureEnd) {
            out.clear();
            return false;
        }

        const std::uint64_t retainedStart =
            g_totalSnapshotFloatCount - static_cast<std::uint64_t>(g_iqSnapshotSize);
        const std::uint64_t chunkAbsoluteStart = captureStart + copied;
        const std::uint64_t chunkAbsoluteEnd = chunkAbsoluteStart + chunkCount;
        if (chunkAbsoluteStart < retainedStart ||
            chunkAbsoluteEnd > g_totalSnapshotFloatCount) {
            out.clear();
            return false;
        }

        const std::size_t snapshotOffset = static_cast<std::size_t>(
            chunkAbsoluteStart - retainedStart);
        const std::size_t readStart =
            (g_iqSnapshotStart + snapshotOffset) % MAX_SNAPSHOT_FLOATS;
        const std::size_t firstCopy =
            (std::min)(chunkCount, MAX_SNAPSHOT_FLOATS - readStart);
        std::copy(g_iqSnapshot.begin() + static_cast<std::ptrdiff_t>(readStart),
                  g_iqSnapshot.begin() + static_cast<std::ptrdiff_t>(readStart + firstCopy),
                  out.begin() + static_cast<std::ptrdiff_t>(copied));
        if (firstCopy < chunkCount) {
            std::copy(g_iqSnapshot.begin(),
                      g_iqSnapshot.begin() + static_cast<std::ptrdiff_t>(chunkCount - firstCopy),
                      out.begin() + static_cast<std::ptrdiff_t>(copied + firstCopy));
        }
        copied += chunkCount;
    }
    return true;
}

std::size_t maxQueuedFloatsForLiveAudio() {
    const double sampleRateEstimate = g_sampleRateEstimate.load(std::memory_order_relaxed);
    if (sampleRateEstimate <= 0.0 || !std::isfinite(sampleRateEstimate)) {
        return MAX_QUEUED_FLOATS;
    }

    const double targetFloats = sampleRateEstimate * 2.0 * MAX_LIVE_QUEUED_SECONDS;
    if (targetFloats <= 0.0) {
        return MAX_QUEUED_FLOATS;
    }

    return (std::clamp)(static_cast<std::size_t>(targetFloats),
                        MIN_LIVE_QUEUED_FLOATS,
                        MAX_QUEUED_FLOATS);
}

} // namespace

#ifdef OBRII_IQBUFFER_SELFTEST
void iqBufferWithSnapshotLockedForTest(const std::function<void()> &action) {
    std::lock_guard<std::mutex> lock(g_iqMutex);
    action();
}
void iqBufferWithPublisherLockedForTest(const std::function<void()> &action) {
    std::lock_guard<std::mutex> lock(g_snapshotPublishMutex);
    action();
}
#endif

namespace IqBuffer {

bool publish(const float *samples,
             std::size_t floatCount,
             bool queueBlock,
             bool updateSnapshot,
             std::uint64_t expectedEpoch,
             const BlockMetadata *metadata) {
    if (!samples || floatCount == 0) {
        return false;
    }
    if (expectedEpoch != 0 &&
        expectedEpoch != g_iqEpoch.load(std::memory_order_acquire)) {
        return false;
    }

    std::vector<float> block;
    if (queueBlock) {
        {
            std::lock_guard<std::mutex> queueLock(g_audioQueueMutex);
            if (!g_recycledIqBlocks.empty()) {
                block = std::move(g_recycledIqBlocks.front());
                g_recycledIqBlocks.pop_front();
            }
        }
        block.assign(samples, samples + floatCount);
    }

    const std::uint64_t blockSequence =
        queueBlock
            ? g_audioBlockSequence.fetch_add(1, std::memory_order_relaxed) + 1
            : g_audioBlockSequence.load(std::memory_order_relaxed);
    if (queueBlock) {
        std::lock_guard<std::mutex> queueLock(g_audioQueueMutex);
        if (expectedEpoch != 0 &&
            expectedEpoch != g_iqEpoch.load(std::memory_order_acquire)) {
            recycleBlock(std::move(block));
            return false;
        }
        g_iqQueuedFloatCount += block.size();
        g_iqBlocks.push_back(std::move(block));
        g_iqBlockSequences.push_back(blockSequence);
        const std::size_t maxQueuedFloats = maxQueuedFloatsForLiveAudio();
        while (g_iqBlocks.size() > MAX_QUEUED_BLOCKS ||
               g_iqQueuedFloatCount > maxQueuedFloats) {
            const std::size_t droppedFloats = g_iqBlocks.front().size();
            g_iqQueuedFloatCount -= droppedFloats;
            g_droppedQueuedBlocks.fetch_add(1, std::memory_order_relaxed);
            g_droppedQueuedFloats.fetch_add(droppedFloats, std::memory_order_relaxed);
            recycleBlock(std::move(g_iqBlocks.front()));
            g_iqBlocks.pop_front();
            g_iqBlockSequences.pop_front();
        }
    } else {
        std::lock_guard<std::mutex> queueLock(g_audioQueueMutex);
        while (!g_iqBlocks.empty()) {
            recycleBlock(std::move(g_iqBlocks.front()));
            g_iqBlocks.pop_front();
        }
        g_iqBlockSequences.clear();
        g_iqQueuedFloatCount = 0;
    }

    // Never wait for an FFT reader in the USB callback. Short snapshot-lock
    // collisions are buffered and flushed in source order at the next publish.
    if (updateSnapshot) {
        std::unique_lock<std::mutex> publishLock(g_snapshotPublishMutex, std::try_to_lock);
        if (!publishLock.owns_lock()) {
            if (expectedEpoch != 0 && expectedEpoch != g_iqEpoch.load(std::memory_order_acquire)) {
                return false;
            }
            g_skippedSnapshotBlocks.fetch_add(1, std::memory_order_relaxed);
            g_snapshotLossPending.store(true, std::memory_order_relaxed);
            g_discardPendingSnapshots.store(true, std::memory_order_release);
            return true;
        }
        if (expectedEpoch != 0 && expectedEpoch != g_iqEpoch.load(std::memory_order_acquire)) {
            return false;
        }
        if (g_discardPendingSnapshots.exchange(false, std::memory_order_acquire)) {
            // A rejected publisher leaves a gap after the pending tail. That
            // older tail cannot be concatenated with the next accepted block.
            g_skippedSnapshotBlocks.fetch_add(g_pendingSnapshotBlocks.size(),
                                              std::memory_order_relaxed);
            g_pendingSnapshotBlocks.clear();
            g_pendingSnapshotFloatCount = 0;
        }
        std::unique_lock<std::mutex> snapshotLock(g_iqMutex, std::try_to_lock);
        if (snapshotLock.owns_lock()) {
            if (expectedEpoch != 0 &&
                expectedEpoch != g_iqEpoch.load(std::memory_order_acquire)) {
                return false;
            }
            if (g_snapshotLossPending.exchange(false, std::memory_order_relaxed)) {
                // An actual overflow must not splice samples across the gap
                // into a seemingly continuous FFT window.
                g_iqSnapshotStart = 0;
                g_iqSnapshotSize = 0;
                ++g_snapshotResetGeneration;
            }
            while (!g_pendingSnapshotBlocks.empty()) {
                const auto &pending = g_pendingSnapshotBlocks.front();
                if (pending.epoch == g_iqEpoch.load(std::memory_order_relaxed)) {
                    appendSnapshotBlock(pending.samples.data(), pending.samples.size(), &pending.metadata);
                }
                g_pendingSnapshotFloatCount -= pending.samples.size();
                g_pendingSnapshotBlocks.pop_front();
            }
            appendSnapshotBlock(samples, floatCount, metadata);
        } else {
            if (floatCount > MAX_PENDING_SNAPSHOT_FLOATS) {
                g_skippedSnapshotBlocks.fetch_add(g_pendingSnapshotBlocks.size() + 1,
                                                  std::memory_order_relaxed);
                g_pendingSnapshotBlocks.clear();
                g_pendingSnapshotFloatCount = 0;
                g_snapshotLossPending.store(true, std::memory_order_relaxed);
                return true;
            }
            while (g_pendingSnapshotBlocks.size() >= MAX_PENDING_SNAPSHOT_BLOCKS ||
                   g_pendingSnapshotFloatCount + floatCount > MAX_PENDING_SNAPSHOT_FLOATS) {
                g_pendingSnapshotFloatCount -= g_pendingSnapshotBlocks.front().samples.size();
                g_pendingSnapshotBlocks.pop_front();
                g_skippedSnapshotBlocks.fetch_add(1, std::memory_order_relaxed);
                g_snapshotLossPending.store(true, std::memory_order_relaxed);
            }
            PendingSnapshotBlock pending;
            pending.samples.assign(samples, samples + floatCount);
            pending.metadata = metadata ? *metadata : BlockMetadata();
            pending.epoch = g_iqEpoch.load(std::memory_order_relaxed);
            g_pendingSnapshotBlocks.push_back(std::move(pending));
            g_pendingSnapshotFloatCount += floatCount;
            g_deferredSnapshotBlocks.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (traceMatchesPublishEpoch(expectedEpoch) && g_tracePublishRemaining > 0) {
        --g_tracePublishRemaining;
        qDebug() << "[IqBufferTrace] publish accepted"
                 << "epoch" << static_cast<qulonglong>(g_iqEpoch)
                 << "expectedEpoch" << static_cast<qulonglong>(expectedEpoch)
                 << "sequence" << static_cast<qulonglong>(g_iqSequence)
                 << "floatCount" << floatCount
                 << "queueBlock" << queueBlock
                 << "updateSnapshot" << updateSnapshot
                 << "snapshotSkipped" << static_cast<qulonglong>(
                        g_skippedSnapshotBlocks.load(std::memory_order_relaxed));
    }
    return true;
}

bool snapshot(std::vector<float> &out, std::uint64_t *sequence, BlockMetadata *metadata) {
    return copySnapshotTail(out, MAX_SNAPSHOT_FLOATS, 0, sequence, metadata, "snapshot");
}

bool snapshotRecent(std::vector<float> &out,
                    std::size_t maxFloatCount,
                    std::uint64_t *sequence,
                    BlockMetadata *metadata) {
    return copySnapshotTail(out, maxFloatCount, 0, sequence, metadata, "snapshotRecent");
}

bool snapshotRecentEndingAt(std::vector<float> &out,
                            std::size_t maxFloatCount,
                            std::uint64_t endFloatCount,
                            std::uint64_t *sequence,
                            BlockMetadata *metadata) {
    if (endFloatCount == 0) {
        return snapshotRecent(out, maxFloatCount, sequence, metadata);
    }
    const bool copied = copySnapshotTail(out,
                                         maxFloatCount,
                                         endFloatCount,
                                         sequence,
                                         metadata,
                                         "snapshotRecentEndingAt");
    if (copied && metadata) {
        metadata->totalFloatCount = endFloatCount;
    }
    return copied;
}

bool popBlock(std::vector<float> &out, std::uint64_t *sequence) {
    std::lock_guard<std::mutex> lock(g_audioQueueMutex);
    const bool shouldTrace = traceMatchesCurrentEpoch() && g_tracePopRemaining > 0;
    if (g_iqBlocks.empty()) {
        out.clear();
        if (sequence) {
            *sequence = g_iqSequence;
        }
        if (shouldTrace) {
            --g_tracePopRemaining;
            qDebug() << "[IqBufferTrace] popBlock empty"
                     << "epoch" << static_cast<qulonglong>(g_iqEpoch)
                     << "sequence" << static_cast<qulonglong>(g_iqSequence)
                     << "queuedBlocks" << g_iqBlocks.size()
                     << "queuedFloats" << g_iqQueuedFloatCount;
        }
        return false;
    }

    const std::uint64_t blockSequence =
        g_iqBlockSequences.empty()
            ? g_iqSequence.load(std::memory_order_relaxed)
            : g_iqBlockSequences.front();
    const std::size_t blockFloats = g_iqBlocks.front().size();
    g_iqQueuedFloatCount -= blockFloats;
    out.swap(g_iqBlocks.front());
    recycleBlock(std::move(g_iqBlocks.front()));
    g_iqBlocks.pop_front();

    if (sequence) {
        *sequence = blockSequence;
    }
    if (!g_iqBlockSequences.empty()) {
        g_iqBlockSequences.pop_front();
    }
    if (shouldTrace) {
        --g_tracePopRemaining;
        qDebug() << "[IqBufferTrace] popBlock"
                 << "epoch" << static_cast<qulonglong>(g_iqEpoch)
                 << "blockSequence" << static_cast<qulonglong>(blockSequence)
                 << "blockFloats" << out.size()
                 << "remainingBlocks" << g_iqBlocks.size()
                 << "remainingFloats" << g_iqQueuedFloatCount;
    }
    return true;
}

void clear(std::uint64_t epoch) {
    std::scoped_lock lock(g_snapshotPublishMutex, g_iqMutex, g_audioQueueMutex);
    const std::uint64_t previousEpoch = g_iqEpoch;
    const std::uint64_t previousSequence = g_iqSequence;
    const std::size_t previousSnapshotStart = g_iqSnapshotStart;
    const std::size_t previousSnapshotSize = g_iqSnapshotSize;
    const std::size_t previousQueuedBlocks = g_iqBlocks.size();
    const std::size_t previousQueuedFloats = g_iqQueuedFloatCount;
    if (epoch != 0) {
        g_iqEpoch = epoch;
    }
    g_iqSnapshotStart = 0;
    g_iqSnapshotSize = 0;
    while (!g_iqBlocks.empty()) {
        recycleBlock(std::move(g_iqBlocks.front()));
        g_iqBlocks.pop_front();
    }
    g_iqBlockSequences.clear();
    g_iqQueuedFloatCount = 0;
    g_droppedQueuedBlocks.store(0, std::memory_order_relaxed);
    g_droppedQueuedFloats.store(0, std::memory_order_relaxed);
    g_audioBlockSequence.store(0, std::memory_order_relaxed);
    g_skippedSnapshotBlocks.store(0, std::memory_order_relaxed);
    g_deferredSnapshotBlocks.store(0, std::memory_order_relaxed);
    g_pendingSnapshotBlocks.clear();
    g_pendingSnapshotFloatCount = 0;
    g_snapshotLossPending.store(false, std::memory_order_relaxed);
    g_discardPendingSnapshots.store(false, std::memory_order_relaxed);
    g_latestMetadata = BlockMetadata();
    g_totalSnapshotFloatCount = 0;
    ++g_snapshotResetGeneration;
    ++g_iqSequence;
    if (epoch != 0 && traceMatchesCurrentEpoch()) {
        qDebug() << "[IqBufferTrace] clear"
                 << "requestedEpoch" << static_cast<qulonglong>(epoch)
                 << "previousEpoch" << static_cast<qulonglong>(previousEpoch)
                 << "currentEpoch" << static_cast<qulonglong>(g_iqEpoch)
                 << "sequenceBefore" << static_cast<qulonglong>(previousSequence)
                 << "sequenceAfter" << static_cast<qulonglong>(g_iqSequence)
                 << "previousSnapshotStart" << previousSnapshotStart
                 << "previousSnapshotSize" << previousSnapshotSize
                 << "previousQueuedBlocks" << previousQueuedBlocks
                 << "previousQueuedFloats" << previousQueuedFloats;
    }
}

std::size_t size() {
    std::lock_guard<std::mutex> lock(g_iqMutex);
    return g_iqSnapshotSize;
}

std::size_t queuedBlocks() {
    std::lock_guard<std::mutex> lock(g_audioQueueMutex);
    return g_iqBlocks.size();
}

std::size_t queuedFloatCount() {
    std::lock_guard<std::mutex> lock(g_audioQueueMutex);
    return g_iqQueuedFloatCount;
}

Stats stats() {
    std::lock_guard<std::mutex> snapshotLock(g_iqMutex);
    std::lock_guard<std::mutex> queueLock(g_audioQueueMutex);
    Stats result;
    result.epoch = g_iqEpoch;
    result.sequence = g_iqSequence;
    result.snapshotStart = g_iqSnapshotStart;
    result.snapshotSize = g_iqSnapshotSize;
    result.queuedBlocks = g_iqBlocks.size();
    result.queuedFloatCount = g_iqQueuedFloatCount;
    result.sampleRateEstimate = g_sampleRateEstimate.load(std::memory_order_relaxed);
    result.totalFloatCount = g_totalSnapshotFloatCount;
    result.droppedQueuedBlocks = g_droppedQueuedBlocks.load(std::memory_order_relaxed);
    result.droppedQueuedFloats = g_droppedQueuedFloats.load(std::memory_order_relaxed);
    result.skippedSnapshotBlocks =
        g_skippedSnapshotBlocks.load(std::memory_order_relaxed);
    return result;
}

DiscontinuityStats discontinuityStats() noexcept {
    DiscontinuityStats result;
    result.epoch = g_iqEpoch.load(std::memory_order_relaxed);
    result.droppedQueuedBlocks = g_droppedQueuedBlocks.load(std::memory_order_relaxed);
    result.skippedSnapshotBlocks = g_skippedSnapshotBlocks.load(std::memory_order_relaxed);
    result.deferredSnapshotBlocks = g_deferredSnapshotBlocks.load(std::memory_order_relaxed);
    return result;
}

void armRetuneTrace(std::uint64_t epoch,
                    int publishLogs,
                    int snapshotLogs,
                    int popLogs,
                    int rejectLogs) {
    std::lock_guard<std::mutex> snapshotLock(g_iqMutex);
    std::lock_guard<std::mutex> queueLock(g_audioQueueMutex);
    if (epoch == 0 || !fobosVerboseLoggingEnabled()) {
        g_traceEpoch = 0;
        g_tracePublishRemaining = 0;
        g_traceSnapshotRemaining = 0;
        g_tracePopRemaining = 0;
        g_traceRejectRemaining = 0;
        return;
    }

    g_traceEpoch = epoch;
    g_tracePublishRemaining = (std::max)(0, publishLogs);
    g_traceSnapshotRemaining = (std::max)(0, snapshotLogs);
    g_tracePopRemaining = (std::max)(0, popLogs);
    g_traceRejectRemaining = (std::max)(0, rejectLogs);
    logTraceState("armed");
}

void setSampleRateEstimate(double sampleRate) {
    g_sampleRateEstimate.store(sampleRate, std::memory_order_relaxed);
}

double sampleRateEstimate() {
    return g_sampleRateEstimate.load(std::memory_order_relaxed);
}

} // namespace IqBuffer
