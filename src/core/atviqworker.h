#pragma once

#include "iqchannelizer.h"
#include "fulliqrecordingencoder.h"
#include <QByteArray>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// Keep the receiver callback independent of the wideband video DDC. Both
// input memory and output back-pressure are bounded; a gap resets its DSP
// state and is explicitly reported before any subsequent video samples.
class AtvIqWorker {
public:
    using Output = std::function<void(const QByteArray &, double, int)>;
    using Gap = std::function<void(std::uint64_t)>;
    static constexpr std::size_t MaxInputBytes = 32 * 1024 * 1024;

    AtvIqWorker(Output output, Gap gap) : output_(std::move(output)), gap_(std::move(gap)),
                                        thread_([this] { run(); }) {}
    ~AtvIqWorker() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; queue_.clear(); }
        wake_.notify_one();
        thread_.join();
    }
    void configure(const RadioSettings &settings, bool enabled, bool reset) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reset || enabled_ != enabled) {
            ++generation_;
            queue_.clear(); pendingBytes_ = 0;
            resetPending_ = true;
        }
        settings_ = settings;
        enabled_ = enabled;
    }
    void push(const float *samples, std::size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || stop_ || !samples || count < 2 || count % 2) return;
        const bool oversized = count > MaxInputBytes / sizeof(float);
        const std::size_t bytes = oversized ? MaxInputBytes + 1 : count * sizeof(float);
        if (oversized || pendingBytes_ > MaxInputBytes - bytes) {
            for (const auto &job : queue_) dropped_ += job.samples.size() / 2;
            queue_.clear(); pendingBytes_ = 0;
            ++generation_; resetPending_ = true;
            if (oversized) { dropped_ += count / 2; return; }
        }
        Job job;
        job.samples.swap(spareInput_);
        job.samples.assign(samples, samples + count);
        job.settings = settings_;
        job.generation = generation_;
        pendingBytes_ += bytes;
        queue_.push_back(std::move(job));
        wake_.notify_one();
    }
private:
    struct Job { std::vector<float> samples; RadioSettings settings; std::uint64_t generation; };
    void run() {
        IqChannelizer channel;
        std::vector<float> samples;
        float gain = 0;
        std::uint64_t activeGeneration = 0;
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                job = std::move(queue_.front()); queue_.pop_front();
                pendingBytes_ -= job.samples.size() * sizeof(float);
                if (job.generation != generation_ || !enabled_) continue;
                if (activeGeneration != job.generation || resetPending_) {
                    activeGeneration = job.generation;
                    channel.reset(); gain = 0; resetPending_ = false;
                    gap_(dropped_); dropped_ = 0;
                }
            }
            const auto result = channel.processFloatIq(job.samples.data(), job.samples.size(),
                                                       job.settings, samples, false);
            if (!result.valid || samples.empty()) {
                std::lock_guard<std::mutex> lock(mutex_);
                recycleInput(job);
                continue;
            }
            double power = 0;
            float peak = 0;
            for (float sample : samples) { power += double(sample) * sample; peak = (std::max)(peak, std::abs(sample)); }
            const float rms = float(std::sqrt(power / (samples.size() / 2)));
            const float desired = (std::clamp)((std::min)(0.28f / (std::max)(rms, 1e-6f),
                                                         0.82f / (std::max)(peak, 1e-6f)), 0.25f, 128.0f);
            gain = gain <= 0 ? desired : gain + (desired - gain) * (desired < gain ? 0.05f : 0.002f);
            // Limit this block's gain immediately on a sudden strong signal.
            const float blockGain = (std::min)(gain, 0.95f / (std::max)(peak, 1e-6f));
            for (float &sample : samples) sample *= blockGain;
            QByteArray encoded(int(samples.size() * 2), Qt::Uninitialized);
            encodeFullIq16(samples.data(), encoded.data(), samples.size());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (enabled_ && job.generation == generation_)
                    output_(encoded, result.outputRate, int(samples.size() / 2));
                else if (enabled_)
                    dropped_ += job.samples.size() / 2;
                recycleInput(job);
            }
        }
    }
    // Reuse normal receiver blocks instead of allocating/zeroing megabytes
    // on every callback. Keep at most one spare, capped independently.
    void recycleInput(Job &job) {
        if (job.samples.capacity() <= 1024 * 1024 &&
            job.samples.capacity() > spareInput_.capacity())
            spareInput_.swap(job.samples);
    }
    Output output_;
    Gap gap_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::vector<float> spareInput_;
    RadioSettings settings_;
    std::size_t pendingBytes_ = 0;
    std::uint64_t generation_ = 1, dropped_ = 0;
    bool enabled_ = false, stop_ = false, resetPending_ = true;
    std::thread thread_;
};
