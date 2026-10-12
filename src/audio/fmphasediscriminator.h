#pragma once
#include <cmath>

// Positive amplitude normalization cancels in arg(current * conj(previous)).
// The ordinary path uses float atan2; double products handle underflow/overflow.
inline float fmPhaseDifference(float i, float q, float previousI, float previousQ) {
    const float crossFloat = q*previousI - i*previousQ;
    const float dotFloat = i*previousI + q*previousQ;
    const float productScale = std::abs(crossFloat) + std::abs(dotFloat);
    if (productScale >= 1.0e-30f && std::isfinite(productScale))
        return std::atan2(crossFloat, dotFloat);
    const double cross = double(q)*previousI - double(i)*previousQ;
    const double dot = double(i)*previousI + double(q)*previousQ;
    return static_cast<float>(std::atan2(cross, dot));
}
