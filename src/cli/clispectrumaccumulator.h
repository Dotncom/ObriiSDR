#ifndef OBRII_CLI_SPECTRUM_ACCUMULATOR_H
#define OBRII_CLI_SPECTRUM_ACCUMULATOR_H

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace CliSpectrumLimits {
constexpr int minimumFftSize = 512;
constexpr int maximumFftSize = 50000000;

inline bool supportsSize(int size) noexcept {
    return size >= minimumFftSize && size <= maximumFftSize;
}
}

// One contiguous, non-overlapping FFT window. Construction and reset do not
// allocate or copy IQ; storage is allocated once, on the first nonempty append.
class CliSpectrumAccumulator {
public:
    explicit CliSpectrumAccumulator(int fftSize = 0) noexcept
        : size(CliSpectrumLimits::supportsSize(fftSize) ? fftSize : 0) {}
    CliSpectrumAccumulator(const CliSpectrumAccumulator &) = delete;
    CliSpectrumAccumulator &operator=(const CliSpectrumAccumulator &) = delete;

    bool valid() const noexcept { return size != 0; }
    int fftSize() const noexcept { return size; }
    int available() const noexcept { return used; } // Complex samples, not floats.
    bool ready() const noexcept { return valid() && used == size; }
    const float *data() const noexcept { return storage.get(); }

    std::unique_ptr<float[]> takeWindow() noexcept {
        if (!ready()) return {};
        used = 0;
        return std::move(storage);
    }
    void recycleWindow(std::unique_ptr<float[]> buffer) noexcept {
        // Only return a buffer obtained from this accumulator's takeWindow().
        if (!storage && !used) storage = std::move(buffer);
    }

    void reset() noexcept { used = 0; }

    // A size change releases storage; reset(0) disables the accumulator.
    bool reset(int fftSize) noexcept {
        reset();
        const int nextSize = CliSpectrumLimits::supportsSize(fftSize) ? fftSize : 0;
        if (nextSize != size) {
            storage.reset();
            size = nextSize;
        }
        return valid();
    }

    // Returns the consumed prefix in complex samples, or -1 on allocation
    // failure. A full window consumes nothing until reset; callers must retain
    // and resubmit the unconsumed tail after analyzing data().
    int append(const float *interleavedIq, int complexSamples) noexcept {
        if (!valid() || !interleavedIq || complexSamples <= 0 || ready()) return 0;
        if (!storage) {
            storage.reset(new (std::nothrow) float[static_cast<std::size_t>(size) * 2]);
            if (!storage) return -1;
        }
        const int consumed = (std::min)(complexSamples, size - used);
        std::copy_n(interleavedIq, static_cast<std::size_t>(consumed) * 2,
                    storage.get() + static_cast<std::size_t>(used) * 2);
        used += consumed;
        return consumed;
    }

private:
    int size = 0;
    int used = 0;
    std::unique_ptr<float[]> storage;
};

#endif
