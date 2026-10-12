#include "spectrumdisplayreducer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void interpolate(std::vector<float> &levels, float fallback) {
    int previous = -1;
    for (int i = 0; i < static_cast<int>(levels.size()); ++i) {
        if (!std::isfinite(levels[i])) continue;
        if (previous < 0) std::fill(levels.begin(), levels.begin() + i, levels[i]);
        else for (int j = previous + 1; j < i; ++j)
            levels[j] = levels[previous] + (levels[i] - levels[previous]) *
                (static_cast<float>(j - previous) / static_cast<float>(i - previous));
        previous = i;
    }
    std::fill(levels.begin() + previous + 1, levels.end(), previous < 0 ? fallback : levels[previous]);
}

std::vector<float> reference(const std::vector<float> &frequencies,
                             const std::vector<float> &values, double low, double high,
                             int bins, SpectrumDisplayReduction mode, bool regular) {
    std::vector<float> result(bins, std::numeric_limits<float>::quiet_NaN());
    const int count = static_cast<int>(values.size());
    const double step = count > 1 ? (frequencies.back() - frequencies.front()) / (count - 1) : 1;
    for (int bin = 0; bin < bins; ++bin) {
        const double left = low + (high - low) * bin / bins;
        const double right = low + (high - low) * (bin + 1) / bins;
        const double center = (left + right) * 0.5;
        double sum = 0, power = 0, distance = std::numeric_limits<double>::infinity();
        float peak = -std::numeric_limits<float>::infinity();
        float minimum = std::numeric_limits<float>::infinity(), sample = 0;
        int hits = 0;
        for (int i = 0; i < count; ++i) {
            bool belongs = false;
            if (regular) {
                belongs = i >= std::clamp(static_cast<int>(std::ceil((left - frequencies.front()) / step)), 0, count) &&
                          i < std::clamp(static_cast<int>(std::ceil((right - frequencies.front()) / step)), 0, count);
            } else if (std::isfinite(frequencies[i]) && frequencies[i] >= low && frequencies[i] <= high) {
                belongs = std::clamp(static_cast<int>((frequencies[i] - low) / (high - low) * bins), 0, bins - 1) == bin;
            }
            if (!belongs) continue;
            const float value = values[(i + count / 2) % count];
            if (!std::isfinite(value)) continue;
            ++hits;
            sum += value;
            power += std::pow(10.0, static_cast<double>(value) / 10.0);
            peak = (std::max)(peak, value);
            minimum = (std::min)(minimum, value);
            const double delta = std::abs(frequencies[i] - center);
            if (delta < distance) { distance = delta; sample = value; }
        }
        if (hits == 0) continue;
        switch (mode) {
        case SpectrumDisplayReduction::AverageDb: result[bin] = static_cast<float>(sum / hits); break;
        case SpectrumDisplayReduction::AveragePower:
            result[bin] = static_cast<float>(10.0 * std::log10((std::max)(power / hits, 1.0e-30))); break;
        case SpectrumDisplayReduction::Minimum: result[bin] = minimum; break;
        case SpectrumDisplayReduction::Sample: result[bin] = sample; break;
        default: result[bin] = peak; break;
        }
    }
    interpolate(result, -140);
    return result;
}

void compare(const std::vector<float> &actual, const std::vector<float> &expected) {
    require(actual.size() == expected.size(), "output size mismatch");
    for (std::size_t i = 0; i < actual.size(); ++i)
        require(std::isfinite(actual[i]) && std::abs(actual[i] - expected[i]) < 0.0001f, "detector value mismatch");
}

void testReducers() {
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> noise(-170, 5);
    for (bool regular : {true, false}) {
        for (int count : {2, 3, 128, 513, 4096}) {
            std::vector<float> frequencies(count), levels(count), overlay(count);
            for (int i = 0; i < count; ++i) {
                frequencies[i] = regular ? i * 1000.0f : i * 1000.0f + i * i * 13.0f;
                levels[i] = noise(rng); overlay[i] = noise(rng);
                if (i % 17 == 0) levels[i] = std::numeric_limits<float>::quiet_NaN();
            }
            // Grids shorter than four points take the irregular fallback.
            const bool regularPath = regular && count >= 4;
            for (int target : {2, 31, 1024}) for (int zoom : {1, 4, 128}) {
                const double center = frequencies.back() * 0.5;
                const double span = frequencies.back() / zoom;
                const double low = center - span * 0.5, high = center + span * 0.5;
                for (int m = 0; m < 5; ++m) {
                    const auto mode = static_cast<SpectrumDisplayReduction>(m);
                    SpectrumDisplayFrame output;
                    prepareSpectrumDisplayFrame(frequencies, levels, &overlay, count,
                        low, high, target, -140, mode, output);
                    const int bins = (std::min)(count, (std::max)(target, 2));
                    compare(output.levels, reference(frequencies, levels, low, high, bins, mode, regularPath));
                    compare(output.overlayLevels, reference(frequencies, overlay, low, high, bins, mode, regularPath));
                    for (int i = 0; i < bins; ++i)
                        require(std::abs(output.frequencies[i] - static_cast<float>(low + (i + 0.5) * (high - low) / bins)) < 0.01f,
                                "frequency axis mismatch");
                }
            }
        }
    }
    SpectrumDisplayFrame output;
    prepareSpectrumDisplayFrame({0, 1000}, {1, 2}, nullptr, 2, 10, 10, 10, -140,
                                SpectrumDisplayReduction::Peak, output);
    require(output.levels.empty(), "invalid range retained a stale frame");
    prepareSpectrumDisplayFrame({0, 1000}, {NAN, INFINITY}, nullptr, 2, 0, 1001, 2, -140,
                                SpectrumDisplayReduction::Peak, output);
    compare(output.levels, {-140, -140});
    require(output.overlayLevels.empty(), "overlay retained without a source");
    prepareSpectrumDisplayFrame({500}, {-83}, nullptr, 1, 0, 1000, 64, -140,
                                SpectrumDisplayReduction::Peak, output);
    compare(output.levels, {-83});
}

void benchmark() {
    constexpr int count = 4194304;
    std::vector<float> frequencies(count), levels(count);
    for (int i = 0; i < count; ++i) {
        frequencies[i] = static_cast<float>(75000000.0 + 50000000.0 * i / count);
        levels[i] = -120 + static_cast<float>(i % 71);
    }
    SpectrumDisplayFrame output;
    for (int m = 0; m < 5; ++m) {
        const auto start = std::chrono::steady_clock::now();
        for (int repeat = 0; repeat < 5; ++repeat)
            prepareSpectrumDisplayFrame(frequencies, levels, nullptr, count, 75000000, 125000000,
                                        1600, -140, static_cast<SpectrumDisplayReduction>(m), output);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 5;
        std::cout << "4M -> 1600 bins, mode " << m << ": " << ms << " ms\n";
    }
}
}

int main() {
    try {
        testReducers();
        std::cout << "Spectrum reducer tests passed\n";
        benchmark();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
