#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define OBRII_IQ8_SSE2 1
#endif

namespace ObriiIq8Quantizer {

inline char quantizeOneScaled(float sample, float gain) {
    if (!std::isfinite(sample)) sample = 0.0f;
    const float scaled = (std::clamp)(sample * gain, -1.0f, 1.0f) * 127.0f;
    const int value = scaled >= 0.0f
        ? static_cast<int>(scaled + 0.5f)
        : static_cast<int>(scaled - 0.5f);
    return static_cast<char>((std::clamp)(value, -128, 127));
}

inline char quantizeOne(float sample) { return quantizeOneScaled(sample, 16.0f); }

inline void quantizeScaled(const float *samples, char *output, std::size_t count, float inputGain) {
    std::size_t index = 0;
#if defined(OBRII_IQ8_SSE2)
    const __m128 signBit = _mm_set1_ps(-0.0f);
    const __m128 infinity = _mm_set1_ps(INFINITY);
    const __m128 gain = _mm_set1_ps(inputGain);
    const __m128 lower = _mm_set1_ps(-1.0f);
    const __m128 upper = _mm_set1_ps(1.0f);
    const __m128 level = _mm_set1_ps(127.0f);
    const __m128 zero = _mm_setzero_ps();
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128 minusHalf = _mm_set1_ps(-0.5f);
    for (; index + 16 <= count; index += 16) {
        __m128i rounded[4];
        for (int group = 0; group < 4; ++group) {
            __m128 value = _mm_loadu_ps(samples + index + group * 4);
            const __m128 absValue = _mm_andnot_ps(signBit, value);
            value = _mm_and_ps(value, _mm_cmplt_ps(absValue, infinity));
            value = _mm_mul_ps(value, gain);
            value = _mm_max_ps(lower, _mm_min_ps(upper, value));
            value = _mm_mul_ps(value, level);
            const __m128 nonnegative = _mm_cmpge_ps(value, zero);
            const __m128 offset = _mm_or_ps(_mm_and_ps(nonnegative, half),
                                            _mm_andnot_ps(nonnegative, minusHalf));
            rounded[group] = _mm_cvttps_epi32(_mm_add_ps(value, offset));
        }
        const __m128i first = _mm_packs_epi32(rounded[0], rounded[1]);
        const __m128i second = _mm_packs_epi32(rounded[2], rounded[3]);
        const __m128i packed = _mm_packs_epi16(first, second);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(output + index), packed);
    }
#endif
    for (; index < count; ++index) output[index] = quantizeOneScaled(samples[index], inputGain);
}

inline void quantize(const float *samples, char *output, std::size_t count) {
    quantizeScaled(samples, output, count, 16.0f);
}

} // namespace ObriiIq8Quantizer
