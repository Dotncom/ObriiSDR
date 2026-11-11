#pragma once

#include <cmath>

namespace DisplayLevelStep {
constexpr int DefaultNetworkTenths = 1;
constexpr int MaximumTenths = 100;

inline float rounded(float levelDb, int stepTenths) noexcept {
    if (stepTenths <= 0 || !std::isfinite(levelDb)) return levelDb;
    const double stepIndex = std::round(static_cast<double>(levelDb) * 10.0 / stepTenths);
    return static_cast<float>(stepIndex * stepTenths / 10.0);
}
}
