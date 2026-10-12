#ifndef DMR_CIC_DECIMATOR_H
#define DMR_CIC_DECIMATOR_H

#include <array>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

// Five box filters: SIMD CIC for normal ratios, the original float path for
// large ratios where fixed-point gain would sacrifice weak-signal resolution.
class DmrCicDecimator {
public:
    static constexpr int MaxFixedPointFactor = 128;
    void reset() { factor_ = 0; }
    void configure(int factor) {
        factor = (std::clamp)(factor, 1, 65536);
        if (factor_ == factor) return;
        factor_ = factor;
        count_ = 0;
        integrators_ = {};
        delays_ = {};
        legacyIndex_ = 0;
        legacySums_ = {};
        legacyAverage_ = {};
        for (auto &buffer : legacyBuffers_)
            buffer.assign(factor > MaxFixedPointFactor ? factor : 0, {});
        const double gain = std::pow(static_cast<double>(factor), Order);
        scale_ = std::exp2((std::min)(28.0, std::floor(std::log2(std::exp2(59.0) / gain))));
        outputScale_ = 1.0 / (gain * scale_);
    }
    bool push(std::complex<float> sample, std::complex<float> &output) {
        if (factor_ <= 1) { output = sample; return true; }
        if (factor_ > MaxFixedPointFactor) return pushLegacy(sample, output);
        std::array<std::uint64_t, 2> value;
#if defined(_M_X64) || defined(__SSE2__)
        auto input = _mm_setr_ps(sample.real(), sample.imag(), 0.0f, 0.0f);
        input = _mm_and_ps(input, _mm_cmpord_ps(input, input));
        input = _mm_min_ps(_mm_max_ps(input, _mm_set1_ps(-4.0f)), _mm_set1_ps(4.0f));
        const auto fixed = _mm_cvttps_epi32(_mm_mul_ps(input, _mm_set1_ps(float(scale_))));
        auto accumulated = _mm_unpacklo_epi32(fixed, _mm_srai_epi32(fixed, 31));
        for (int stage = 0; stage < Order; ++stage) {
            accumulated = _mm_add_epi64(accumulated, _mm_loadu_si128(
                reinterpret_cast<const __m128i *>(integrators_[stage].data())));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(integrators_[stage].data()), accumulated);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i *>(value.data()), accumulated);
#elif defined(__aarch64__) || defined(_M_ARM64)
        const float raw[2] = {sample.real(), sample.imag()};
        auto input = vld1_f32(raw);
        input = vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(input), vceq_f32(input, input)));
        input = vmin_f32(vmax_f32(input, vdup_n_f32(-4.0f)), vdup_n_f32(4.0f));
        auto accumulated = vreinterpretq_u64_s64(vmovl_s32(vcvt_s32_f32(
            vmul_n_f32(input, float(scale_)))));
        for (int stage = 0; stage < Order; ++stage) {
            accumulated = vaddq_u64(accumulated, vld1q_u64(integrators_[stage].data()));
            vst1q_u64(integrators_[stage].data(), accumulated);
        }
        vst1q_u64(value.data(), accumulated);
#else
        value = {quantize(sample.real()), quantize(sample.imag())};
        for (int stage = 0; stage < Order; ++stage) {
            for (int axis = 0; axis < 2; ++axis) {
                integrators_[stage][axis] += value[axis];
                value[axis] = integrators_[stage][axis];
            }
        }
#endif
        if (++count_ < factor_) return false;
        count_ = 0;
        for (int stage = 0; stage < Order; ++stage) {
            for (int axis = 0; axis < 2; ++axis) {
                const auto previous = delays_[stage][axis];
                delays_[stage][axis] = value[axis];
                value[axis] -= previous;
            }
        }
        output = {static_cast<float>(signedValue(value[0]) * outputScale_),
                  static_cast<float>(signedValue(value[1]) * outputScale_)};
        return true;
    }
private:
    static constexpr int Order = 5;
    // Keep the large-ratio fallback out of the full-rate SIMD inlining budget.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__)
    __attribute__((noinline))
#endif
    bool pushLegacy(std::complex<float> sample, std::complex<float> &output) {
        const float inverse = 1.0f / float(factor_);
        for (int stage = 0; stage < Order - 1; ++stage) {
            const auto previous = legacyBuffers_[stage][legacyIndex_];
            legacyBuffers_[stage][legacyIndex_] = sample;
            legacySums_[stage] += sample - previous;
            sample = legacySums_[stage] * inverse;
        }
        if (++legacyIndex_ == factor_) legacyIndex_ = 0;
        legacyAverage_ += sample;
        if (++count_ < factor_) return false;
        output = legacyAverage_ * inverse;
        legacyAverage_ = {};
        count_ = 0;
        return true;
    }
    std::uint64_t quantize(float value) const {
        if (value != value) return 0;
        const auto fixed = static_cast<std::int32_t>(
            (std::clamp)(value, -4.0f, 4.0f) * static_cast<float>(scale_));
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(fixed));
    }
    static double signedValue(std::uint64_t value) {
        return value & (std::uint64_t(1) << 63)
            ? -static_cast<double>(~value) - 1.0 : static_cast<double>(value);
    }
    int factor_ = 0;
    int count_ = 0;
    double scale_ = 1.0;
    double outputScale_ = 1.0;
    std::array<std::array<std::uint64_t, 2>, Order> integrators_ = {};
    std::array<std::array<std::uint64_t, 2>, Order> delays_ = {};
    int legacyIndex_ = 0;
    std::array<std::vector<std::complex<float>>, Order - 1> legacyBuffers_;
    std::array<std::complex<float>, Order - 1> legacySums_ = {};
    std::complex<float> legacyAverage_ = {};
};

#endif
