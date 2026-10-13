#ifndef PFBCHANNELIZER_H
#define PFBCHANNELIZER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Opt-in analysis bank for complex IQ. One owner serializes this instance.
class PfbChannelizer {
public:
    static constexpr int MaxSelectedBins = 8;
    static constexpr std::size_t MaxInputSamples = 1U << 20;
    struct Config {
        double sampleRateHz = 0.0;
        int channels = 16;           // Power of two, 8..64.
        int tapsPerPhase = 16;       // Even, 8..64; higher means narrower guards.
    };
    struct ChannelOutput {
        int binIndex = -1;
        double centerOffsetHz = 0.0; // Relative to the input's RF center.
        double sampleRateHz = 0.0;
        std::vector<float> iq;       // Interleaved CF32, no AGC or clipping.
    };
    enum class Status {
        Ok, NotConfigured, InvalidInput, InvalidSelection,
        ResourceLimit, AllocationFailure, Cancelled
    };
    struct Result {
        Status status = Status::NotConfigured;
        std::size_t inputSamplesConsumed = 0;
        std::size_t outputSamplesPerChannel = 0;
        // Relative to the most recent reset; meaningful only when output exists.
        std::uint64_t firstOutputInputSample = 0;
    };

    PfbChannelizer();
    ~PfbChannelizer();
    PfbChannelizer(PfbChannelizer &&) noexcept;
    PfbChannelizer &operator=(PfbChannelizer &&) noexcept;
    PfbChannelizer(const PfbChannelizer &) = delete;
    PfbChannelizer &operator=(const PfbChannelizer &) = delete;

    // Transactional on failure. Identical configuration preserves stream state.
    bool configure(const Config &config);
    void reset(); // Start a new zero-padded epoch after a gap/retune.
    bool isConfigured() const;
    double outputRateHz() const;
    double binCenterHz(int binIndex) const; // NaN for an invalid bin.
    int nearestCenterIndex(double offsetHz) const; // -1 outside [-Fs/2, Fs/2).
    double usablePassbandHz() const; // Full width, not half width.
    bool coversBand(int binIndex, double centerOffsetHz, double bandwidthHz) const;
    double delayInputSamples() const;

    // Unique natural-order bins, at most eight, in caller-specified output order.
    // Empty selection advances history without materializing output. Input must
    // not alias outputs. Odd float counts are rejected without consuming input.
    // Cancellation returns a resumable prefix; retry the unconsumed suffix.
    Result processFloatIq(const float *samples, std::size_t floatCount,
                          const std::vector<int> &selectedBins,
                          std::vector<ChannelOutput> &outputs,
                          const std::atomic_bool *cancel = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // PFBCHANNELIZER_H
