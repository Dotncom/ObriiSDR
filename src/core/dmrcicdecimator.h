#ifndef DMR_CIC_DECIMATOR_H
#define DMR_CIC_DECIMATOR_H

#include <array>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
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
#if defined(_M_X64) || defined(__SSE2__)
    bool mixRfBlock(const float *iq, std::size_t pairs, float rotI, float rotQ,
                    float stepI, float stepQ, std::vector<float> &out) {
        if (factor_ <= 1 || factor_ > MaxFixedPointFactor) return false;
        out.resize((pairs + count_) / factor_ * 2);
        float *output = out.data();
        int count = count_;
        const __m128 scale = _mm_set1_ps(static_cast<float>(scale_));
        const __m128 signI = _mm_setr_ps(-0.0f, 0.0f, 0.0f, 0.0f);
        const __m128 magnitudeMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        const __m128 maxFinite = _mm_set1_ps((std::numeric_limits<float>::max)());
        const __m128 stepReal = _mm_set1_ps(stepI);
        const __m128 stepImag = _mm_set1_ps(stepQ);
        const auto multiply = [signI](__m128 z, __m128 real, __m128 imag) {
            const __m128 swapped = _mm_shuffle_ps(z, z, _MM_SHUFFLE(3, 2, 0, 1));
            return _mm_add_ps(_mm_mul_ps(z, real), _mm_xor_ps(_mm_mul_ps(swapped, imag), signI));
        };
        __m128 rotation = _mm_setr_ps(rotI, rotQ, 0.0f, 0.0f);
        // Keep the five integrators local across the whole RF block.
        __m128i a = loadFixed(integrators_[0]);
        __m128i b = loadFixed(integrators_[1]);
        __m128i c = loadFixed(integrators_[2]);
        __m128i d = loadFixed(integrators_[3]);
        __m128i e = loadFixed(integrators_[4]);
        for (std::size_t n = 0; n < pairs; ++n, iq += 2) {
            __m128 input = _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(iq)));
            input = _mm_and_ps(input, _mm_cmple_ps(_mm_and_ps(input, magnitudeMask), maxFinite));
            const __m128 real = _mm_shuffle_ps(rotation, rotation, _MM_SHUFFLE(0, 0, 0, 0));
            const __m128 imag = _mm_shuffle_ps(rotation, rotation, _MM_SHUFFLE(1, 1, 1, 1));
            a = _mm_add_epi64(a, quantizeFixed(multiply(input, real, imag), scale));
            b = _mm_add_epi64(b, a);
            c = _mm_add_epi64(c, b);
            d = _mm_add_epi64(d, c);
            e = _mm_add_epi64(e, d);
            rotation = multiply(rotation, stepReal, stepImag);
            if ((n & 4095) == 4095) {
                const __m128 squared = _mm_mul_ps(rotation, rotation);
                const float norm = std::sqrt(_mm_cvtss_f32(squared) +
                    _mm_cvtss_f32(_mm_shuffle_ps(squared, squared, _MM_SHUFFLE(1, 1, 1, 1))));
                if (norm > 0.0f) rotation = _mm_div_ps(rotation, _mm_set1_ps(norm));
            }
            if (++count == factor_) {
                count = 0;
                std::array<std::uint64_t, 2> value;
                storeFixed(value, e);
                const auto sample = fixedOutput(value);
                *output++ = sample.real();
                *output++ = sample.imag();
            }
        }
        count_ = count;
        storeFixed(integrators_[0], a);
        storeFixed(integrators_[1], b);
        storeFixed(integrators_[2], c);
        storeFixed(integrators_[3], d);
        storeFixed(integrators_[4], e);
        return true;
    }
#endif
    bool push(std::complex<float> sample, std::complex<float> &output) {
        if (factor_ <= 1) { output = sample; return true; }
        if (factor_ > MaxFixedPointFactor) return pushLegacy(sample, output);
        std::array<std::uint64_t, 2> value;
#if defined(_M_X64) || defined(__SSE2__)
        const auto input = _mm_setr_ps(sample.real(), sample.imag(), 0.0f, 0.0f);
        auto accumulated = quantizeFixed(input, _mm_set1_ps(float(scale_)));
        for (int stage = 0; stage < Order; ++stage) {
            accumulated = _mm_add_epi64(accumulated, loadFixed(integrators_[stage]));
            storeFixed(integrators_[stage], accumulated);
        }
        storeFixed(value, accumulated);
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
        output = fixedOutput(value);
        return true;
    }
private:
    static constexpr int Order = 5;
#if defined(_M_X64) || defined(__SSE2__)
    static __m128i quantizeFixed(__m128 input, __m128 scale) {
        input = _mm_and_ps(input, _mm_cmpord_ps(input, input));
        input = _mm_min_ps(_mm_max_ps(input, _mm_set1_ps(-4.0f)), _mm_set1_ps(4.0f));
        const auto fixed = _mm_cvttps_epi32(_mm_mul_ps(input, scale));
        return _mm_unpacklo_epi32(fixed, _mm_srai_epi32(fixed, 31));
    }
    static __m128i loadFixed(const std::array<std::uint64_t, 2> &value) {
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(value.data()));
    }
    static void storeFixed(std::array<std::uint64_t, 2> &value, __m128i lanes) {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(value.data()), lanes);
    }
#endif
    std::complex<float> fixedOutput(std::array<std::uint64_t, 2> value) {
        for (int stage = 0; stage < Order; ++stage) {
            for (int axis = 0; axis < 2; ++axis) {
                const auto previous = delays_[stage][axis];
                delays_[stage][axis] = value[axis];
                value[axis] -= previous;
            }
        }
        return {static_cast<float>(signedValue(value[0]) * outputScale_),
                static_cast<float>(signedValue(value[1]) * outputScale_)};
    }
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
