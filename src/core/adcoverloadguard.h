#pragma once

#include <algorithm>
#include <cstdint>

// One-way overload protection, driven by pre-DC ADC rail counts, not RSSI.
class AdcOverloadGuard {
public:
    static constexpr double RailThresholdPercent = 0.01;
    struct Reading {
        bool valid = false;
        double railPercent = 0;
        int requestedVga = -1;
    };
    void reset() { havePrevious = false; badWindows = 0; }
    Reading update(std::uint32_t generation, std::uint32_t inspected,
                   std::uint32_t rails, int vga, bool enabled, bool eligible,
                   std::int64_t nowMs) {
        Reading out;
        if (!havePrevious || previousGeneration != generation) {
            havePrevious = true;
            previousGeneration = generation;
            previousInspected = inspected;
            previousRails = rails;
            badWindows = 0;
            settleUntil = nowMs + 750;
            return out;
        }
        const std::uint32_t count = inspected - previousInspected;
        const std::uint32_t clipped = rails - previousRails;
        previousInspected = inspected;
        previousRails = rails;
        if (count < 65536 || clipped > count) {
            badWindows = 0;
            return out;
        }
        out.valid = true;
        out.railPercent = 100.0 * clipped / count;
        if (!enabled || !eligible || nowMs < settleUntil || nowMs < cooldownUntil || vga <= 0) {
            badWindows = 0;
            return out;
        }
        if (out.railPercent < RailThresholdPercent) {
            badWindows = 0;
            return out;
        }
        if (++badWindows >= 2) {
            out.requestedVga = (std::max)(0, vga - (out.railPercent >= 1.0 ? 2 : 1));
            cooldownUntil = nowMs + 1000;
            badWindows = 0;
        }
        return out;
    }
private:
    bool havePrevious = false;
    std::uint32_t previousGeneration = 0, previousInspected = 0, previousRails = 0;
    int badWindows = 0;
    std::int64_t settleUntil = 0, cooldownUntil = 0;
};
