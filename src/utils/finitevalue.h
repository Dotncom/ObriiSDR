#ifndef OBRII_FINITEVALUE_H
#define OBRII_FINITEVALUE_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

template <typename Real>
inline bool finiteValue(Real value) noexcept {
    if constexpr (std::is_same_v<Real, float> && sizeof(float) == sizeof(std::uint32_t) &&
                  std::numeric_limits<float>::is_iec559) {
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7f800000U) != 0x7f800000U;
    }
    return std::isfinite(value);
}

#endif
