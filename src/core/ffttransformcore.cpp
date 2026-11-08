#include "ffttransformcore.h"
#include "fftplannermutex.h"
#include "../utils/finitevalue.h"

#include <limits>
#include <mutex>
#include <type_traits>

namespace {
template <typename Real>
float levelDb(Real re, Real im, Real normalizationDb) noexcept {
    // Preserve the established GUI float and CLI double overflow behavior.
    if constexpr (std::is_same_v<Real, float>) {
        if (!finiteValue(re)) re = 0.0f;
        if (!finiteValue(im)) im = 0.0f;
    }
    const Real power = re * re + im * im;
    return finiteValue(power) && power > Real(0) ?
        static_cast<float>((std::max)(Real(FftTransformCore::FloorDb),
            Real(10) * std::log10(power) - normalizationDb)) : FftTransformCore::FloorDb;
}

template <typename Real>
void writeLevels(const fftwf_complex *spectrum, float *levels, int begin, int end,
                 Real normalizationDb, int length, float *frequencies = nullptr,
                 double sampleRate = 0.0, double centerFrequency = 0.0) noexcept {
    for (int i = begin; i < end; ++i) {
        levels[i] = levelDb(Real(spectrum[i][0]), Real(spectrum[i][1]), normalizationDb);
        if (frequencies) frequencies[i] =
            (i - length / 2) * (sampleRate / length) + centerFrequency;
    }
}

template <FftTransformCore::Input Mode>
void stageInput(const float *source, fftwf_complex *destination, const float *window,
                int begin, int end) noexcept {
    for (int i = begin; i < end; ++i) {
        const auto offset = static_cast<std::size_t>(i) * 2;
        const float coefficient = window ? window[i] : 1.0f;
        if constexpr (Mode == FftTransformCore::Input::QImaginary) destination[i][0] = 0.0f;
        else {
            const float real = source[offset + (Mode == FftTransformCore::Input::QReal ? 1 : 0)];
            destination[i][0] = finiteValue(real) ? real * coefficient : 0.0f;
        }
        if constexpr (Mode == FftTransformCore::Input::Complex ||
                      Mode == FftTransformCore::Input::QImaginary) {
            const float imag = source[offset + 1];
            destination[i][1] = finiteValue(imag) ? imag * coefficient : 0.0f;
        } else destination[i][1] = 0.0f;
    }
}
}

FftTransformCore::~FftTransformCore() {
    releasePlan();
}

bool FftTransformCore::allocateBuffers(int length, Storage storage) noexcept {
    if (length > 0 && length_ == length && storage_ == storage && input_ && output_) return true;
    releasePlan();
    if (length <= 0 || static_cast<std::size_t>(length) >
            (std::numeric_limits<std::size_t>::max)() / sizeof(fftwf_complex)) return false;
    input_ = static_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) * length));
    storage_ = storage;
    output_ = storage == Storage::InPlace ? input_ :
        static_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) * length));
    if (!input_ || !output_) {
        releasePlan();
        return false;
    }
    length_ = length;
    return true;
}

bool FftTransformCore::createPlanLocked(unsigned flags) noexcept {
    if (!plan_ && input_ && output_ && length_ > 0)
        plan_ = fftwf_plan_dft_1d(length_, input_, output_, FFTW_FORWARD, flags);
    return ready();
}

void FftTransformCore::releasePlan() noexcept {
    if (plan_) {
        std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
        fftwf_destroy_plan(plan_);
        plan_ = nullptr;
    }
    if (output_ != input_) fftwf_free(output_);
    fftwf_free(input_);
    input_ = output_ = nullptr;
    length_ = 0;
}

void FftTransformCore::execute() const noexcept {
    fftwf_execute(plan_);
}

void FftTransformCore::ensureWindow(int type) {
    ensureWindow(type, [](int count, auto range) { range(0, count); });
}

bool FftTransformCore::beginWindow(int type) {
    const int normalized = normalizedFftWindowType(type);
    if (windowLength_ == length_ && windowType_ == normalized) return false;
    if (normalized == FFT_WINDOW_RECTANGULAR) window_.clear();
    else window_.resize(static_cast<std::size_t>(length_), 1.0f);
    // Publish a nonempty window only after every dispatched range completes.
    windowLength_ = 0;
    windowType_ = normalized;
    windowSum_ = window_.empty() ? (std::max)(0, length_) : 0.0;
    if (window_.empty()) {
        windowLength_ = length_;
        return false;
    }
    return true;
}

void FftTransformCore::fillWindowRange(int begin, int end) noexcept {
    if (window_.empty()) return;
    constexpr double TwoPi = 6.28318530717958647692;
    const double denominator = static_cast<double>((std::max)(1, length_ - 1));
    for (int index = begin; index < end; ++index) {
        const double phase = TwoPi * static_cast<double>(index) / denominator;
        double coefficient = 1.0;
        switch (windowType_) {
        case FFT_WINDOW_HANN:
            coefficient = 0.5 - 0.5 * std::cos(phase);
            break;
        case FFT_WINDOW_HAMMING:
            coefficient = 0.54 - 0.46 * std::cos(phase);
            break;
        case FFT_WINDOW_BLACKMAN_HARRIS:
            coefficient = 0.35875 - 0.48829 * std::cos(phase) +
                          0.14128 * std::cos(2.0 * phase) - 0.01168 * std::cos(3.0 * phase);
            break;
        case FFT_WINDOW_FLAT_TOP:
            coefficient = 0.21557895 - 0.41663158 * std::cos(phase) +
                          0.277263158 * std::cos(2.0 * phase) -
                          0.083578947 * std::cos(3.0 * phase) +
                          0.006947368 * std::cos(4.0 * phase);
            break;
        default:
            break;
        }
        window_[static_cast<std::size_t>(index)] = static_cast<float>(coefficient);
    }
}

void FftTransformCore::finishWindow() noexcept {
    if (window_.empty()) windowSum_ = (std::max)(0, length_);
    else for (float coefficient : window_) windowSum_ += coefficient;
    windowLength_ = length_;
}

double FftTransformCore::windowAmplitudeSum(int sampleCount) const noexcept {
    const int count = (std::clamp)(sampleCount, 0, windowLength_);
    if (window_.empty()) return static_cast<double>(count);
    if (count == windowLength_) return windowSum_;
    double sum = 0.0;
    for (int index = 0; index < count; ++index) sum += window_[static_cast<std::size_t>(index)];
    return sum;
}

void FftTransformCore::stageInputRange(const float *source, int begin, int end,
                                      Input input) const noexcept {
    const float *window = window_.empty() ? nullptr : window_.data();
    switch (input) {
    case Input::IReal: stageInput<Input::IReal>(source, input_, window, begin, end); break;
    case Input::QImaginary: stageInput<Input::QImaginary>(source, input_, window, begin, end); break;
    case Input::QReal: stageInput<Input::QReal>(source, input_, window, begin, end); break;
    case Input::Complex: stageInput<Input::Complex>(source, input_, window, begin, end); break;
    }
}

void FftTransformCore::zeroInputTail(int sampleCount) const noexcept {
    for (int i = sampleCount; i < length_; ++i) input_[i][0] = input_[i][1] = 0.0f;
}

float FftTransformCore::magnitudeDb(float re, float im, float normalizationDb) noexcept {
    return levelDb(re, im, normalizationDb);
}

void FftTransformCore::writeMagnitudes(float *levels, int begin, int end, float normalizationDb,
                                      float *frequencies, double sampleRate,
                                      double centerFrequency) const noexcept {
    writeLevels(output_, levels, begin, end, normalizationDb, length_, frequencies,
                sampleRate, centerFrequency);
}

void FftTransformCore::writeMagnitudes(float *levels, int begin, int end,
                                      double normalizationDb) const noexcept {
    writeLevels(output_, levels, begin, end, normalizationDb, length_);
}

void FftTransformCore::writeShiftedMagnitudes(const float *natural, float *shifted) const noexcept {
    const int firstNegative = (length_ + 1) / 2;
    for (int i = 0; i < length_; ++i) shifted[i] = natural[(i + firstNegative) % length_];
}
