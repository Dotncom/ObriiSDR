#ifndef OBRII_LIVE_CENTER_RETUNE_H
#define OBRII_LIVE_CENTER_RETUNE_H
#include <algorithm>
#include <cmath>
#include <cstdint>

struct LiveCenterRetuneDecision {
    bool send = false;
    double targetHz = 0.0;
    int waitMs = 0;
};

// Callers capture their native handle, so a later GUI selection cannot redirect
// a command already submitted to the persistent worker. Standard reports the
// achieved hardware frequency; Agile only acknowledges the requested value.
template<class AgileSetter, class StandardSetter>
int runLiveFobosRetune(bool agile, double logicalHz, double offsetHz, double &actualHz,
                      AgileSetter setAgile, StandardSetter setStandard) {
    double hardwareHz = logicalHz + offsetHz;
    const int result = agile ? setAgile(hardwareHz) : setStandard(hardwareHz, &hardwareHz);
    actualHz = result == 0 ? hardwareHz - offsetHz : logicalHz;
    return result;
}

// All clocks are supplied by the caller, allowing input bursts and delayed USB
// completions to be tested without a receiver or wall-clock sleeps.
inline LiveCenterRetuneDecision liveCenterRetuneDecision(
    bool continuous, double desiredHz, double confirmedHz, double sampleRateHz,
    double stepPercent, std::int64_t inputAgeMs, std::int64_t commandAgeMs,
    int safeIntervalMs, bool force = false, std::int64_t frameWaitMs = 0) {
    LiveCenterRetuneDecision result;
    if (!std::isfinite(desiredHz) || !std::isfinite(confirmedHz) ||
        !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0) return result;
    const double delta = desiredHz - confirmedHz;
    if (std::abs(delta) < 0.5 && !force) return result;
    constexpr int quietMs = 60;
    const bool finished = inputAgeMs >= quietMs;
    const double percent = std::isfinite(stepPercent)
                               ? (std::clamp)(stepPercent, 0.1, 100.0) : 5.0;
    const double stepHz = (std::max)(1.0, sampleRateHz * percent / 100.0);
    if (!finished && (!continuous || std::abs(delta) + 0.001 < stepHz)) {
        result.waitMs = static_cast<int>(quietMs - (std::max<std::int64_t>)(0, inputAgeMs));
        return result;
    }
    const int intervalMs = continuous ? 40 : (std::max)(0, safeIntervalMs);
    const auto commandWaitMs = intervalMs - (std::max<std::int64_t>)(0, commandAgeMs);
    // Give intermediate steps time to produce a fresh display frame. Once the
    // gesture ends, the exact requested frequency takes precedence.
    const auto movingFrameWaitMs = continuous && !finished && !force
        ? (std::min)(frameWaitMs, quietMs - (std::max<std::int64_t>)(0, inputAgeMs)) : 0;
    const auto waitMs = (std::max)(commandWaitMs, movingFrameWaitMs);
    if (waitMs > 0) {
        result.waitMs = static_cast<int>(waitMs);
        return result;
    }
    result.send = true;
    result.targetHz = finished ? desiredHz : confirmedHz + std::copysign(
        std::floor((std::abs(delta) + 0.001) / stepHz) * stepHz, delta);
    return result;
}
#endif
