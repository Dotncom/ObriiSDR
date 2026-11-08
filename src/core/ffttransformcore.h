#ifndef FFTTRANSFORMCORE_H
#define FFTTRANSFORMCORE_H

#include "radiosettings.h"

#include <fftw3.h>
#include <vector>

// Single-consumer transform storage and math. Scheduling, GPU selection and
// stream/window admission belong to the caller; no radio globals are consulted.
class FftTransformCore {
public:
    enum class Storage { InPlace, OutOfPlace };
    enum class Input { Complex, IReal, QImaginary, QReal };
    static constexpr float FloorDb = -160.0f;

    FftTransformCore() = default;
    ~FftTransformCore();
    FftTransformCore(const FftTransformCore &) = delete;
    FftTransformCore &operator=(const FftTransformCore &) = delete;

    bool allocateBuffers(int length, Storage storage) noexcept;
    // Caller holds fftwPlannerGlobalMutex(), including any FFTW thread/wisdom
    // configuration around this call. Destruction locks that same mutex itself.
    bool createPlanLocked(unsigned flags = FFTW_ESTIMATE) noexcept;
    void releasePlan() noexcept;
    bool ready() const noexcept { return plan_ != nullptr; }
    int length() const noexcept { return length_; }
    fftwf_complex *input() const noexcept { return input_; }
    fftwf_complex *output() const noexcept { return output_; }
    void execute() const noexcept;

    void ensureWindow(int windowType);
    // A synchronous range dispatcher preserves each caller's existing parallel
    // policy without type erasure or per-bin callbacks.
    template <typename Dispatcher>
    void ensureWindow(int windowType, Dispatcher dispatch) {
        if (!beginWindow(windowType)) return;
        dispatch(length_, [this](int begin, int end) { fillWindowRange(begin, end); });
        finishWindow();
    }
    double windowAmplitudeSum(int sampleCount) const noexcept;

    // source points at the first selected IQ pair; [begin,end) lies inside the
    // copied prefix. Each pair is sanitized and written straight to FFT storage.
    void stageInputRange(const float *source, int begin, int end,
                         Input input = Input::Complex) const noexcept;
    void zeroInputTail(int sampleCount) const noexcept;

    static float magnitudeDb(float re, float im, float normalizationDb) noexcept;
    // Float normalization preserves GUI arithmetic, double preserves CLI levels.
    // Natural-order levels may compact forwards into input() for in-place plans;
    // that aliasing form must be called once sequentially over [0,length()).
    void writeMagnitudes(float *levels, int begin, int end, float normalizationDb,
                         float *frequencies = nullptr, double sampleRate = 0.0,
                         double centerFrequency = 0.0) const noexcept;
    void writeMagnitudes(float *levels, int begin, int end,
                         double normalizationDb) const noexcept;
    // Source and destination must not overlap. Odd lengths use ceil(N/2).
    void writeShiftedMagnitudes(const float *natural, float *shifted) const noexcept;

private:
    bool beginWindow(int windowType);
    void fillWindowRange(int begin, int end) noexcept;
    void finishWindow() noexcept;
    fftwf_complex *input_ = nullptr;
    fftwf_complex *output_ = nullptr;
    fftwf_plan plan_ = nullptr;
    int length_ = 0;
    Storage storage_ = Storage::OutOfPlace;
    int windowLength_ = 0;
    int windowType_ = FFT_WINDOW_RECTANGULAR;
    double windowSum_ = 0.0;
    std::vector<float> window_;
};

#endif
