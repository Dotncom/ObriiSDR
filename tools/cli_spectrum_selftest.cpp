#include "clispectrummeter.h"
#include "clispectrumaccumulator.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <thread>
#include <vector>

namespace {
thread_local bool rejectAllocations = false;
thread_local std::size_t allocationAttempts = 0;

struct RejectAllocations {
    RejectAllocations() { rejectAllocations = true; }
    ~RejectAllocations() { rejectAllocations = false; }
};
}

// Deterministic failure injection for C++ buffers, without huge allocations.
void *operator new(std::size_t size) {
    ++allocationAttempts;
    if (rejectAllocations) throw std::bad_alloc();
    if (void *memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
    try { return ::operator new[](size); }
    catch (const std::bad_alloc &) { return nullptr; }
}
void operator delete[](void *memory, const std::nothrow_t &) noexcept { std::free(memory); }

namespace {
constexpr double sampleRate = 8e6;
constexpr double centerHz = 100e6;
constexpr double pi = 3.14159265358979323846;

void addTone(std::vector<float> &iq, int fftSize, int bin, double amplitude) {
    for (std::size_t i = 0; i < iq.size() / 2; ++i) {
        const double phase = 2.0 * pi * bin * static_cast<double>(i) / fftSize;
        iq[2 * i] += static_cast<float>(amplitude * std::cos(phase));
        iq[2 * i + 1] += static_cast<float>(amplitude * std::sin(phase));
    }
}

bool checkPeak(const QJsonObject &peak, int size, int bin, double amplitude) {
    const double expectedOffset = bin * sampleRate / size;
    return std::abs(peak.value(QStringLiteral("frequencyHz")).toDouble() - centerHz - expectedOffset) < 1e-3 &&
           std::abs(peak.value(QStringLiteral("offsetHz")).toDouble() - expectedOffset) < 1e-6 &&
           std::abs(peak.value(QStringLiteral("levelDbfs")).toDouble() - 20.0 * std::log10(amplitude)) < 0.1;
}

bool checkTone(CliSpectrumMeter &meter, int fftSize, int bin) {
    // Leading samples verify the existing "last complete window" contract.
    constexpr int prefix = 19;
    std::vector<float> iq(static_cast<std::size_t>(fftSize + prefix) * 2);
    addTone(iq, fftSize, bin, 0.5);
    std::fill_n(iq.begin(), prefix * 2, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> trace;
    const QJsonObject result = meter.analyze(iq.data(), fftSize + prefix, sampleRate, centerHz, &trace);
    const QJsonArray peaks = result.value(QStringLiteral("peaks")).toArray();
    if (peaks.isEmpty() || trace.size() != static_cast<std::size_t>(fftSize)) return false;
    const auto highest = std::max_element(trace.begin(), trace.end());
    if (highest - trace.begin() != bin + fftSize / 2 || std::abs(*highest + 6.0206f) > 0.1f) return false;
    return result.value(QStringLiteral("fftSize")).toInt() == fftSize &&
           std::abs(result.value(QStringLiteral("binHz")).toDouble() - sampleRate / fftSize) < 1e-6 &&
           checkPeak(peaks.first().toObject(), fftSize, bin, 0.5);
}

bool checkSpectrum(int fftSize) {
    CliSpectrumMeter meter(fftSize);
    if (!meter.valid()) return false;
    for (int bin : {0, 7, -13, (fftSize - 1) / 2, -fftSize / 2})
        if (!checkTone(meter, fftSize, bin)) return false;

    std::vector<float> silence(static_cast<std::size_t>(fftSize) * 2, 0.0f);
    std::vector<float> trace;
    const QJsonObject result = meter.analyze(silence.data(), fftSize, sampleRate, centerHz, &trace);
    if (!result.value(QStringLiteral("peaks")).toArray().isEmpty() ||
        result.value(QStringLiteral("noiseFloorDbfs")).toDouble() != -160.0 ||
        !std::all_of(trace.begin(), trace.end(), [](float level) { return level == -160.0f; })) return false;

    for (int i = 0; i < fftSize; ++i) {
        silence[static_cast<std::size_t>(2 * i)] = std::numeric_limits<float>::quiet_NaN();
        silence[static_cast<std::size_t>(2 * i + 1)] = std::numeric_limits<float>::infinity();
    }
    if (!meter.analyze(silence.data(), fftSize, sampleRate, centerHz).value(QStringLiteral("peaks")).toArray().isEmpty())
        return false;
    for (int count : {-1, 0, fftSize - 1}) {
        trace.assign(1, 42.0f);
        if (!meter.analyze(silence.data(), count, sampleRate, centerHz, &trace).isEmpty() || !trace.empty()) return false;
    }
    for (double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
        trace.assign(1, 42.0f);
        if (!meter.analyze(silence.data(), fftSize, rate, centerHz, &trace).isEmpty() || !trace.empty()) return false;
    }
    trace.assign(1, 42.0f);
    if (!meter.analyze(nullptr, fftSize, sampleRate, centerHz, &trace).isEmpty() || !trace.empty()) return false;
    trace.assign(1, 42.0f);
    return meter.analyze(silence.data(), fftSize, sampleRate,
                         std::numeric_limits<double>::infinity(), &trace).isEmpty() && trace.empty();
}

bool checkNarrowPeaks() {
    constexpr int fftSize = 10007;
    CliSpectrumMeter meter(fftSize);
    if (!meter.valid()) return false;
    std::vector<float> iq(fftSize * 2, 0.0f);
    const std::array<int, 3> bins{{173, -389, 2101}};
    const std::array<double, 3> amplitudes{{0.5, 0.2, 0.05}};
    for (std::size_t i = 0; i < bins.size(); ++i) addTone(iq, fftSize, bins[i], amplitudes[i]);
    std::vector<float> trace;
    const QJsonObject result = meter.analyze(iq.data(), fftSize, sampleRate, centerHz, &trace);
    const QJsonArray peaks = result.value(QStringLiteral("peaks")).toArray();
    if (peaks.size() != 3 || trace.size() != fftSize) return false;
    for (std::size_t i = 0; i < bins.size(); ++i) {
        if (!checkPeak(peaks.at(static_cast<int>(i)).toObject(), fftSize, bins[i], amplitudes[i]) ||
            std::abs(trace[static_cast<std::size_t>(bins[i] + fftSize / 2)] -
                     20.0 * std::log10(amplitudes[i])) > 0.1) return false;
    }
    return result == meter.analyze(iq.data(), fftSize, sampleRate, centerHz);
}

bool checkDirectDft(int fftSize) {
    CliSpectrumMeter meter(fftSize);
    if (!meter.valid()) return false;
    std::vector<float> iq(static_cast<std::size_t>(fftSize) * 2);
    std::uint32_t random = 0x12345678u;
    for (float &value : iq) {
        random = random * 1664525u + 1013904223u;
        value = static_cast<float>((static_cast<double>(random) / 4294967296.0 - 0.5) * 0.02);
    }
    addTone(iq, fftSize, 37, 0.4);
    addTone(iq, fftSize, -81, 0.2);
    addTone(iq, fftSize, 3, 0.05);
    iq[23] = std::numeric_limits<float>::quiet_NaN();
    iq[46] = std::numeric_limits<float>::infinity();
    std::vector<float> weighted(iq.size());
    double sum = 0.0;
    for (int i = 0; i < fftSize; ++i) {
        const float gain = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * i / (fftSize - 1)));
        sum += gain;
        for (int component = 0; component < 2; ++component) {
            const auto index = static_cast<std::size_t>(2 * i + component);
            weighted[index] = std::isfinite(iq[index]) ? iq[index] * gain : 0.0f;
        }
    }
    // Independent small O(N^2) reference checks every bin, not just tones.
    std::vector<float> expected(static_cast<std::size_t>(fftSize));
    for (int k = 0; k < fftSize; ++k) {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < fftSize; ++i) {
            const double phase = -2.0 * pi * k * i / fftSize;
            const double c = std::cos(phase), s = std::sin(phase);
            re += weighted[static_cast<std::size_t>(2 * i)] * c - weighted[static_cast<std::size_t>(2 * i + 1)] * s;
            im += weighted[static_cast<std::size_t>(2 * i)] * s + weighted[static_cast<std::size_t>(2 * i + 1)] * c;
        }
        expected[static_cast<std::size_t>(k)] = static_cast<float>(
            (std::max)(-160.0, 10.0 * std::log10(re * re + im * im) - 20.0 * std::log10(sum)));
    }
    std::vector<float> trace;
    const QJsonObject result = meter.analyze(iq.data(), fftSize, sampleRate, centerHz, &trace);
    if (trace.size() != expected.size()) return false;
    std::vector<float> noise;
    for (int i = 0; i < fftSize; ++i) {
        if (std::abs(trace[static_cast<std::size_t>(i)] -
                     expected[static_cast<std::size_t>((i + (fftSize + 1) / 2) % fftSize)]) > 0.03f) return false;
        const int offset = i < (fftSize + 1) / 2 ? i : i - fftSize;
        if (std::abs(offset) > 2) noise.push_back(expected[static_cast<std::size_t>(i)]);
    }
    std::sort(noise.begin(), noise.end());
    return std::abs(result.value(QStringLiteral("noiseFloorDbfs")).toDouble() - noise[noise.size() / 2]) < 0.03;
}

bool checkAccumulation(int fftSize) {
    CliSpectrumAccumulator accumulator(fftSize);
    CliSpectrumMeter meter(fftSize);
    if (!accumulator.valid() || !meter.valid() || accumulator.data() || accumulator.available()) return false;
    const int total = fftSize * 3 + 17;
    std::vector<float> iq(static_cast<std::size_t>(total) * 2, 0.0f);
    addTone(iq, fftSize, -37, 0.5);
    // Uneven blocks straddle windows and can contain multiple complete FFTs.
    const std::array<int, 5> blocks{{17, 113, fftSize + 29, 7, fftSize * 2}};
    int cursor = 0, frames = 0, blockIndex = 0;
    const float *storage = nullptr;
    while (cursor < total) {
        const int blockEnd = cursor + (std::min)(blocks[static_cast<std::size_t>(blockIndex++ % blocks.size())], total - cursor);
        while (cursor < blockEnd) {
            const int consumed = accumulator.append(iq.data() + static_cast<std::size_t>(cursor) * 2, blockEnd - cursor);
            if (consumed <= 0) return false;
            cursor += consumed;
            if (!storage) storage = accumulator.data();
            if (storage != accumulator.data()) return false;
            if (accumulator.ready()) {
                if (!std::equal(storage, storage + fftSize * 2,
                                iq.data() + static_cast<std::size_t>(frames * fftSize) * 2)) return false;
                const auto before = allocationAttempts;
                if (accumulator.append(iq.data(), total) != 0 || allocationAttempts != before) return false;
                const QJsonArray peaks = meter.analyze(storage, accumulator.available(), sampleRate, centerHz)
                    .value(QStringLiteral("peaks")).toArray();
                if (peaks.isEmpty() || !checkPeak(peaks.first().toObject(), fftSize, -37, 0.5)) return false;
                ++frames;
                accumulator.reset();
            }
        }
    }
    if (frames != 3 || accumulator.available() != 17 || accumulator.ready() ||
        !std::equal(storage, storage + 34, iq.data() + fftSize * 6)) return false;
    const auto before = allocationAttempts;
    accumulator.reset();
    if (!accumulator.reset(fftSize) || accumulator.append(nullptr, 10) ||
        accumulator.append(iq.data(), 0) || accumulator.append(iq.data(), -1) || allocationAttempts != before ||
        accumulator.data() != storage || accumulator.available()) return false;
    // Discontinuities must discard the partial old window without reallocating.
    if (accumulator.append(iq.data(), 11) != 11) return false;
    accumulator.reset();
    if (accumulator.append(iq.data(), fftSize) != fftSize || !accumulator.ready() ||
        !std::equal(storage, storage + fftSize * 2, iq.data())) return false;
    if (accumulator.reset(0) || accumulator.data() || accumulator.available() || accumulator.valid() ||
        accumulator.append(iq.data(), fftSize)) return false;
    return accumulator.reset(fftSize + 1) && !accumulator.data() && !accumulator.available();
}

bool checkAllocationFailures() {
    {
        RejectAllocations reject;
        CliSpectrumMeter meter(512);
        if (meter.valid()) return false;
    }
    float sample[2]{};
    CliSpectrumAccumulator accumulator(512);
    {
        RejectAllocations reject;
        if (accumulator.append(sample, 1) != -1 || accumulator.data() || accumulator.available()) return false;
    }
    if (accumulator.append(sample, 1) != 1) return false;
    const auto before = allocationAttempts;
    {
        RejectAllocations reject;
        CliSpectrumAccumulator idle;
        CliSpectrumAccumulator upperBound(CliSpectrumLimits::maximumFftSize);
        if (!upperBound.valid() || upperBound.data() || upperBound.append(nullptr, 1) ||
            idle.valid() || idle.append(sample, 1)) return false;
        accumulator.reset();
        if (accumulator.append(nullptr, 1) || accumulator.append(sample, 0) ||
            accumulator.append(sample, 1) != 1) return false;
    }
    if (allocationAttempts != before) return false;
    CliSpectrumMeter meter(512);
    std::vector<float> iq(1024, 0.0f), trace;
    if (!meter.valid()) return false;
    {
        RejectAllocations reject;
        if (!meter.analyze(iq.data(), 512, sampleRate, centerHz, &trace).isEmpty() || !trace.empty()) return false;
    }
    return !meter.analyze(iq.data(), 512, sampleRate, centerHz, &trace).isEmpty() && trace.size() == 512;
}

bool checkConcurrentPlanners() {
    std::atomic<bool> okay{true};
    std::array<std::thread, 4> workers;
    for (auto &worker : workers) worker = std::thread([&okay] {
        for (int i = 0; i < 6; ++i) {
            CliSpectrumMeter meter(i % 2 ? 1009 : 512);
            if (!meter.valid() || !checkTone(meter, i % 2 ? 1009 : 512, 17)) okay.store(false);
        }
    });
    for (auto &worker : workers) worker.join();
    return okay.load();
}
}

int main() {
    for (int size : {-1, 0, 511, 50000001, (std::numeric_limits<int>::max)()}) {
        CliSpectrumMeter meter(size);
        CliSpectrumAccumulator accumulator(size);
        if (meter.valid() || accumulator.valid() || CliSpectrumLimits::supportsSize(size)) {
            std::cerr << "CLI spectrum invalid-size test failed\n";
            return 1;
        }
    }
    // No upper-bound FFT allocation or execution: largest executed test is 65537.
    for (int size : {512, 1000, 1009, 4096, 65537}) {
        if (!checkSpectrum(size)) {
            std::cerr << "CLI spectrum tone/ordering/validation test failed at size " << size << '\n';
            return 1;
        }
    }
    if (!checkNarrowPeaks() || !checkDirectDft(512) || !checkDirectDft(513) ||
        !checkAccumulation(512) || !checkAccumulation(1009) ||
        !checkAllocationFailures() || !checkConcurrentPlanners()) {
        std::cerr << "CLI spectrum peak/direct-DFT/accumulation/allocation/planner test failed\n";
        return 1;
    }
    std::cout << "CLI spectrum self-test passed (512, non-power-of-two/odd, 65537, narrow peaks, direct DFT, accumulation, allocation failure, concurrent planners)\n";
    return 0;
}
