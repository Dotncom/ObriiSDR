#ifndef OBRII_UHD_IQ_TX_STREAM_H
#define OBRII_UHD_IQ_TX_STREAM_H

#include <QString>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace UhdIqTx {
using Sample = std::complex<float>;
inline constexpr int DeviceRate = 1920000;
inline constexpr int MaximumPacketSamples = 4096;

struct Block {
    std::vector<Sample> iq;
    bool endBurst = false;
};

class Queue {
public:
    void reset(std::size_t limit) {
        std::lock_guard<std::mutex> lock(mutex_);
        blocks_.clear();
        count_ = 0;
        limit_ = limit;
        finishing_ = false;
    }

    bool push(Block block) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (finishing_ || blocks_.size() >= 4096 || block.iq.size() > limit_ - count_)
            return false;
        count_ += block.iq.size();
        blocks_.push_back(std::move(block));
        wake_.notify_one();
        return true;
    }

    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        finishing_ = true;
        wake_.notify_one();
    }

    std::size_t queuedSamples() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    // PTT can remain idle between bursts. Once keyed, lack of IQ ends the burst.
    bool take(Block &block, std::size_t minimumSamples, const std::atomic<bool> &cancel,
              bool keyed, QString *error) {
        using namespace std::chrono;
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = steady_clock::now() + milliseconds(400);
        while (!cancel.load(std::memory_order_relaxed)) {
            if (!blocks_.empty() && (count_ >= minimumSamples || finishing_ || endBurstQueued())) {
                block = std::move(blocks_.front());
                blocks_.pop_front();
                count_ -= block.iq.size();
                return true;
            }
            if (finishing_) return false;
            if ((keyed || !blocks_.empty()) && steady_clock::now() >= deadline) {
                if (blocks_.empty()) {
                    *error = QStringLiteral("UHD TX input underrun");
                    return false;
                }
                minimumSamples = 0;
                continue;
            }
            wake_.wait_for(lock, milliseconds(10));
        }
        return false;
    }

private:
    bool endBurstQueued() const {
        return std::any_of(blocks_.begin(), blocks_.end(), [](const Block &block) { return block.endBurst; });
    }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Block> blocks_;
    std::size_t count_ = 0;
    std::size_t limit_ = 0;
    bool finishing_ = true;
};

// Band-limited interpolation with a fixed integer time base. History and phase
// stay continuous across USB/network blocks, and reset at each burst boundary.
class Interpolator {
public:
    explicit Interpolator(int rate) : rate_(rate) {
        if (rate_ == DeviceRate) return;
        constexpr double pi = 3.14159265358979323846;
        for (std::size_t phase = 0; phase < taps_.size(); ++phase) {
            const double fraction = double(phase) / taps_.size();
            double sum = 0.0;
            for (std::size_t tap = 0; tap < TapCount; ++tap) {
                const double x = double(tap) - (TapCount / 2 - 1) + fraction;
                const double angle = 0.94 * pi * x;
                const double window = 0.5 + 0.5 * std::cos(pi * x / (TapCount / 2));
                const double weight = std::abs(x) >= TapCount / 2 ? 0.0 :
                    window * (std::abs(angle) < 1e-12 ? 1.0 : std::sin(angle) / angle);
                taps_[phase][tap] = float(weight);
                sum += weight;
            }
            for (auto &weight : taps_[phase]) weight /= float(sum);
        }
    }

    template<class Send>
    bool process(const Sample *iq, std::size_t count, Send send) {
        if (rate_ == DeviceRate) return !count || send(iq, count);
        std::size_t written = 0;
        for (std::size_t i = 0; i < count; ++i) {
            head_ = (head_ + TapCount - 1) % TapCount;
            history_[head_] = history_[head_ + TapCount] = iq[i];
            while (phase_ < DeviceRate) {
                const auto &kernel = taps_[std::size_t(phase_) * taps_.size() / DeviceRate];
                float iValue = 0.0f;
                float qValue = 0.0f;
                for (std::size_t tap = 0; tap < TapCount; ++tap) {
                    iValue += history_[head_ + tap].real() * kernel[tap];
                    qValue += history_[head_ + tap].imag() * kernel[tap];
                }
                output_[written++] = {sanitize(iValue), sanitize(qValue)};
                phase_ += rate_;
                if (written == output_.size()) {
                    if (!send(output_.data(), written)) return false;
                    written = 0;
                }
            }
            phase_ -= DeviceRate;
        }
        return !written || send(output_.data(), written);
    }

    template<class Send>
    bool finish(Send send) {
        const std::array<Sample, TapCount> zero{};
        const bool okay = rate_ == DeviceRate || process(zero.data(), zero.size(), send);
        history_.fill({});
        phase_ = 0;
        head_ = 0;
        return okay;
    }

    static float sanitize(float value) {
        return std::isfinite(value) ? (std::clamp)(value, -1.0f, 1.0f) : 0.0f;
    }

private:
    static constexpr std::size_t TapCount = 24;
    int rate_;
    int phase_ = 0;
    std::size_t head_ = 0;
    std::array<Sample, TapCount * 2> history_{};
    std::array<std::array<float, TapCount>, 128> taps_{};
    std::array<Sample, 4096> output_{};
};

template<class Send, class End>
std::uint64_t run(Queue &queue, int sourceRate, int prebufferMs, const std::atomic<bool> &cancel,
                  Send send, End end, QString *error) {
    Interpolator interpolator(sourceRate);
    bool keyed = false;
    std::uint64_t sent = 0;
    auto write = [&](const Sample *samples, std::size_t count) {
        while (count && !cancel.load(std::memory_order_relaxed)) {
            const auto size = (std::min)(count, std::size_t(4096));
            const bool first = !keyed;
            keyed = true;
            if (!send(samples, size, first, error)) return false;
            sent += size;
            count -= size;
            samples += size;
        }
        return !count;
    };
    auto endBurst = [&] {
        if (!keyed) return;
        if (error->isEmpty() && !cancel.load(std::memory_order_relaxed)) interpolator.finish(write);
        QString endError;
        if (!end(&endError) && error->isEmpty()) *error = endError;
        keyed = false;
    };
    Block block;
    while (queue.take(block, keyed ? 0 : std::size_t(sourceRate) * prebufferMs / 1000,
                      cancel, keyed, error)) {
        if (block.endBurst) endBurst();
        else if (!interpolator.process(block.iq.data(), block.iq.size(), write)) break;
        if (!error->isEmpty()) break;
    }
    endBurst();
    return sent;
}
}

#endif
