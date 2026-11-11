#ifndef OBRII_CLI_ASYNC_WFM_AUDIO_H
#define OBRII_CLI_ASYNC_WFM_AUDIO_H

#include "cliaudiooutput.h"

#include <QJsonObject>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>

class CliAsyncWfmAudio {
public:
    struct Format {
        double rate, center, listen, bandwidth;
        QString filters;
        quint64 streamEpoch;

        bool operator==(const Format &other) const {
            return rate == other.rate && center == other.center && listen == other.listen &&
                   bandwidth == other.bandwidth && filters == other.filters &&
                   streamEpoch == other.streamEpoch;
        }
    };

    explicit CliAsyncWfmAudio(std::size_t blockSamples) {
        for (auto &block : queue) block.iq.resize(blockSamples * 2);
        worker = std::thread([this, blockSamples] { run(blockSamples); });
    }
    ~CliAsyncWfmAudio() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        worker.join();
    }

    void setFormat(const Format &next) {
        std::lock_guard<std::mutex> lock(mutex);
        if (active && format == next) return;
        reset();
        format = next;
        active = true;
    }
    bool suspend() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!active) return false;
        reset();
        active = false;
        return true;
    }
    void markGap() {
        std::lock_guard<std::mutex> lock(mutex);
        resetNext = true;
    }
    void submit(const float *iq, std::size_t samples) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (used == queue.size()) {
                ++droppedIqBlocks;
                droppedIqSamples += samples;
                resetNext = true;
                return;
            }
            auto &block = queue[(head + used) % queue.size()];
            std::copy_n(iq, samples * 2, block.iq.data());
            block.samples = samples;
            block.resetBefore = resetNext;
            resetNext = false;
            peakQueued = (std::max)(peakQueued, ++used);
        }
        ready.notify_one();
    }
    QByteArray takePcm(QString *error) {
        std::lock_guard<std::mutex> lock(mutex);
        *error = failure;
        QByteArray result;
        result.swap(pcm);
        return result;
    }
    QJsonObject status() {
        std::lock_guard<std::mutex> lock(mutex);
        return {{"active", active}, {"processedBlocks", double(processedBlocks)},
                {"queuedBlocks", int(used)}, {"peakQueuedBlocks", int(peakQueued)},
                {"droppedIqBlocks", double(droppedIqBlocks)}, {"droppedIqSamples", double(droppedIqSamples)},
                {"droppedPcmBytes", double(droppedPcmBytes)}};
    }

private:
    struct Block {
        std::vector<float> iq;
        std::size_t samples = 0;
        bool resetBefore = false;
    };

    // Called with the mutex held. The worker also checks the generation before publishing PCM.
    void reset() {
        ++generation;
        head = used = 0;
        resetNext = false;
        pcm.clear();
        failure.clear();
    }

    void run(std::size_t blockSamples) {
        Block block;
        block.iq.resize(blockSamples * 2);
        QByteArray output;
        CliAudioOutput demodulator;
        quint64 configuredGeneration = 0;
        for (;;) {
            Format current{};
            quint64 blockGeneration;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [this] { return stopping || used != 0; });
                if (stopping) return;
                std::swap(block, queue[head]);
                head = (head + 1) % queue.size();
                --used;
                current = format;
                blockGeneration = generation;
            }

            output.clear();
            QString error;
            bool ok = true;
            if (block.resetBefore || configuredGeneration != blockGeneration) {
                ok = demodulator.open({}, false, INPUT_RF, MOD_WFM, current.bandwidth,
                                     current.rate, current.center, current.listen, false, &error,
                                     [&output](const QByteArray &frame) { output.append(frame); });
                if (ok) {
                    demodulator.setFilterChain(current.filters);
                    configuredGeneration = blockGeneration;
                }
            }
            if (ok) ok = demodulator.process(block.iq.data(), block.samples, &error);

            std::lock_guard<std::mutex> lock(mutex);
            if (blockGeneration != generation) continue;
            if (!ok) {
                failure = error.isEmpty() ? QStringLiteral("WFM audio processing failed") : error;
                continue;
            }
            ++processedBlocks;
            // Bound unsent 48 kHz mono int16 PCM to 250 ms.
            constexpr int maximumPcmBytes = 24000;
            if (pcm.size() + output.size() <= maximumPcmBytes) pcm.append(output);
            else droppedPcmBytes += output.size();
        }
    }

    std::array<Block, 32> queue;
    std::mutex mutex;
    std::condition_variable ready;
    std::thread worker;
    Format format{};
    QByteArray pcm;
    QString failure;
    std::size_t head = 0, used = 0, peakQueued = 0;
    quint64 generation = 0, processedBlocks = 0, droppedIqBlocks = 0, droppedIqSamples = 0, droppedPcmBytes = 0;
    bool active = false, resetNext = false, stopping = false;
};

#endif
