#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#endif

// Archival PCM16: unity gain, I/Q interleaved, little endian. Unlike the
// legacy compact network stream this must never apply an implicit RF gain.
inline void encodeFullIq16(const float *input, char *output, std::size_t count) {
    std::size_t i = 0;
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    const __m128 sign = _mm_set1_ps(-0.0f);
    const __m128 infinity = _mm_set1_ps(INFINITY);
    const __m128 scale = _mm_set1_ps(32768.0f);
    const __m128i one = _mm_set1_epi32(1);
    for (; i + 8 <= count; i += 8) {
        __m128i parts[2];
        for (int k = 0; k < 2; ++k) {
            __m128 value = _mm_loadu_ps(input + i + 4 * k);
            value = _mm_and_ps(value, _mm_cmplt_ps(_mm_andnot_ps(sign, value), infinity));
            value = _mm_mul_ps(_mm_max_ps(_mm_set1_ps(-1.0f),
                                        _mm_min_ps(_mm_set1_ps(1.0f), value)), scale);
            const __m128i whole = _mm_cvttps_epi32(value);
            const __m128 fraction = _mm_sub_ps(value, _mm_cvtepi32_ps(whole));
            const __m128i up = _mm_and_si128(one, _mm_castps_si128(_mm_cmpge_ps(fraction, _mm_set1_ps(0.5f))));
            const __m128i down = _mm_and_si128(one, _mm_castps_si128(_mm_cmple_ps(fraction, _mm_set1_ps(-0.5f))));
            parts[k] = _mm_sub_epi32(_mm_add_epi32(whole, up), down);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i *>(output + i * 2),
                         _mm_packs_epi32(parts[0], parts[1]));
    }
#endif
    for (; i < count; ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, input + i, sizeof(bits));
        const float sample = (bits & 0x7f800000U) == 0x7f800000U ? 0.0f : input[i];
        const float scaled = (std::clamp)(sample, -1.0f, 1.0f) * 32768.0f;
        const int whole = static_cast<int>(scaled);
        const float fraction = scaled - whole;
        const int rounded = whole + (fraction >= 0.5f) - (fraction <= -0.5f);
        const auto value = static_cast<std::uint16_t>((std::clamp)(rounded, -32768, 32767));
        output[i * 2] = static_cast<char>(value & 255);
        output[i * 2 + 1] = static_cast<char>(value >> 8);
    }
}
