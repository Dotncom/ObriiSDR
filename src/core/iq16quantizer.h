#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define OBRII_IQ16_SSE2 1
#endif

namespace ObriiIq16Quantizer {

inline std::int16_t quantizeOne(float value, double scale, int &clipped) {
    double normalized = double(value) / scale;
    if (normalized > 32767.0) { normalized = 32767.0; ++clipped; }
    if (normalized < -32767.0) { normalized = -32767.0; ++clipped; }
    return std::int16_t(std::lrint(normalized));
}

inline void quantize(const float *samples, char *output, std::size_t count,
                     double scale, int &clipped) {
    std::size_t index = 0;
#if defined(OBRII_IQ16_SSE2)
    const __m128d denominator = _mm_set1_pd(scale);
    const __m128d lower = _mm_set1_pd(-32767.0);
    const __m128d upper = _mm_set1_pd(32767.0);
    for (; index + 4 <= count; index += 4) {
        const __m128 values = _mm_loadu_ps(samples + index);
        __m128d first = _mm_div_pd(_mm_cvtps_pd(values), denominator);
        __m128d second = _mm_div_pd(_mm_cvtps_pd(_mm_movehl_ps(values, values)), denominator);
        const int firstClip = _mm_movemask_pd(_mm_or_pd(_mm_cmpgt_pd(first, upper),
                                                       _mm_cmplt_pd(first, lower)));
        const int secondClip = _mm_movemask_pd(_mm_or_pd(_mm_cmpgt_pd(second, upper),
                                                        _mm_cmplt_pd(second, lower)));
        clipped += (firstClip & 1) + (firstClip >> 1) +
                   (secondClip & 1) + (secondClip >> 1);
        first = _mm_max_pd(lower, _mm_min_pd(upper, first));
        second = _mm_max_pd(lower, _mm_min_pd(upper, second));
        const __m128i integers = _mm_unpacklo_epi64(_mm_cvtpd_epi32(first),
                                                    _mm_cvtpd_epi32(second));
        const __m128i packed = _mm_packs_epi32(integers, _mm_setzero_si128());
        _mm_storel_epi64(reinterpret_cast<__m128i *>(output + index * 2), packed);
    }
#endif
    for (; index < count; ++index) {
        const std::uint16_t value = std::uint16_t(quantizeOne(samples[index], scale, clipped));
        output[index * 2] = char(value & 0xffU);
        output[index * 2 + 1] = char(value >> 8);
    }
}

} // namespace ObriiIq16Quantizer
