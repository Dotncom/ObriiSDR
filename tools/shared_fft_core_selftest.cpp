#include "fft.h"
#include "fftplannermutex.h"
#include "clispectrummeter.h"
#include "diagnosticlogging.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QStandardPaths>

#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
constexpr double TwoPi = 6.28318530717958647692;
int comparedFrames = 0;
int referenceFrames = 0;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<float> samples(int count, int length, int frame = 0) {
    std::vector<float> iq(static_cast<std::size_t>(count) * 2);
    std::uint32_t random = 0x12345678u + frame;
    for (int i = 0; i < count; ++i) {
        for (int component = 0; component < 2; ++component) {
            random = random * 1664525u + 1013904223u;
            iq[2 * i + component] = static_cast<float>(
                (static_cast<double>(random) / 4294967296.0 - 0.5) * 0.02);
        }
        const double phase = TwoPi * i / length;
        iq[2 * i] += static_cast<float>(0.4 * std::cos(7 * phase) + 0.13 * std::cos(-13 * phase));
        iq[2 * i + 1] += static_cast<float>(0.4 * std::sin(7 * phase) + 0.13 * std::sin(-13 * phase));
    }
    if (count > 10) {
        iq[11] = std::numeric_limits<float>::quiet_NaN();
        iq[18] = std::numeric_limits<float>::infinity();
    }
    return iq;
}

float referenceWindow(int index, int length, int type) {
    const double phase = TwoPi * index / (std::max)(1, length - 1);
    switch (type) {
    case FFT_WINDOW_HANN: return static_cast<float>(0.5 - 0.5 * std::cos(phase));
    case FFT_WINDOW_HAMMING: return static_cast<float>(0.54 - 0.46 * std::cos(phase));
    case FFT_WINDOW_BLACKMAN_HARRIS:
        return static_cast<float>(0.35875 - 0.48829 * std::cos(phase) +
                                  0.14128 * std::cos(2 * phase) - 0.01168 * std::cos(3 * phase));
    case FFT_WINDOW_FLAT_TOP:
        return static_cast<float>(0.21557895 - 0.41663158 * std::cos(phase) +
                                  0.277263158 * std::cos(2 * phase) -
                                  0.083578947 * std::cos(3 * phase) +
                                  0.006947368 * std::cos(4 * phase));
    default: return 1.0f;
    }
}

std::vector<double> referenceDft(const std::vector<float> &iq, int length, int type) {
    const int count = (std::min)(length, static_cast<int>(iq.size() / 2));
    const auto start = iq.size() / 2 - count;
    std::vector<std::complex<double>> weighted(count);
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
        const float gain = referenceWindow(i, length, type);
        sum += gain;
        const float re = iq[2 * (start + i)], im = iq[2 * (start + i) + 1];
        weighted[i] = {std::isfinite(re) ? re * gain : 0.0f,
                       std::isfinite(im) ? im * gain : 0.0f};
    }
    std::vector<double> result(length);
    for (int k = 0; k < length; ++k) {
        std::complex<double> bin(0.0, 0.0);
        for (int i = 0; i < count; ++i) {
            const double phase = -TwoPi * k * i / length;
            bin += weighted[i] * std::complex<double>(std::cos(phase), std::sin(phase));
        }
        result[k] = std::abs(bin) / (std::max)(1.0, sum);
    }
    return result;
}

void compareReference(const std::vector<float> &levels, const std::vector<double> &amplitudes,
                      bool shifted) {
    const int length = static_cast<int>(levels.size());
    require(levels.size() == amplitudes.size(), "reference length mismatch");
    for (int i = 0; i < length; ++i) {
        const int natural = shifted ? (i + (length + 1) / 2) % length : i;
        const double expected = amplitudes[natural];
        const double actual = std::pow(10.0, levels[i] / 20.0);
        require(std::isfinite(levels[i]) && levels[i] >= -160.0f &&
                std::abs(actual - expected) < 1.0e-5 * (1.0 + expected),
                "shared FFT differs from independent DFT");
    }
}

void testMatchedWrappers() {
    FFTResult gui;
    gui.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    RadioSettings settings;
    settings.inputMode = INPUT_RF;
    settings.fftWindowType = FFT_WINDOW_HANN;
    settings.sampleRate = 48000.0;
    settings.centerFrequency = 145000000.0;
    for (int length : {512, 513, 1009, 1024}) {
        settings.fftLength = length;
        CliSpectrumMeter cli(length);
        require(cli.valid(), "CLI plan failed");
        for (int count : {length, length + 37}) {
            for (int frame = 0; frame < 3; ++frame) {
                const auto iq = samples(count, length, frame);
                const auto original = iq;
                std::vector<float> frequencies, natural, shifted;
                require(gui.computeSnapshot(settings, iq, frequencies, natural), "GUI computation failed");
                require(!cli.analyze(iq.data(), count, settings.sampleRate, settings.centerFrequency,
                                     &shifted).isEmpty(), "CLI computation failed");
                require(natural.size() == shifted.size() && natural.size() == static_cast<std::size_t>(length),
                        "wrapper output length mismatch");
                for (int i = 0; i < length; ++i) {
                    const int source = (i + (length + 1) / 2) % length;
                    require(std::abs(natural[source] - shifted[i]) < 0.03f,
                            "GUI natural / CLI shifted trace mismatch");
                    const float expected = static_cast<float>(settings.centerFrequency +
                        (i - length / 2) * settings.sampleRate / length);
                    require(frequencies[i] == expected, "GUI centered frequency axis changed");
                }
                if (frame == 0 && length <= 513) {
                    const auto reference = referenceDft(iq, length, FFT_WINDOW_HANN);
                    compareReference(natural, reference, false);
                    compareReference(shifted, reference, true);
                    ++referenceFrames;
                }
                require(std::memcmp(iq.data(), original.data(), iq.size() * sizeof(float)) == 0,
                        "shared FFT modified caller IQ");
                ++comparedFrames;
            }
        }
        for (int bin : {-(length / 2), -13, 0, 7, (length - 1) / 2}) {
            std::vector<float> iq(static_cast<std::size_t>(length) * 2);
            for (int i = 0; i < length; ++i) {
                const double phase = TwoPi * bin * i / length;
                iq[2 * i] = static_cast<float>(0.5 * std::cos(phase));
                iq[2 * i + 1] = static_cast<float>(0.5 * std::sin(phase));
            }
            std::vector<float> trace;
            const auto result = cli.analyze(iq.data(), length, settings.sampleRate,
                                            settings.centerFrequency, &trace);
            const auto peaks = result.value(QStringLiteral("peaks")).toArray();
            const auto peak = std::max_element(trace.begin(), trace.end());
            require(peak - trace.begin() == bin + length / 2, "odd/even shifted tone index mismatch");
            require(!peaks.isEmpty(), "tone peak missing");
            const auto first = peaks.first().toObject();
            const double offset = bin * (settings.sampleRate / length);
            require(std::abs(first.value(QStringLiteral("offsetHz")).toDouble() - offset) < 1.0e-8 &&
                    std::abs(first.value(QStringLiteral("frequencyHz")).toDouble() -
                             settings.centerFrequency - offset) < 1.0e-6,
                    "odd/even CLI peak frequency mapping mismatch");
        }
    }
}

void testSnapshotsAndWindows() {
    FFTResult gui;
    gui.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    RadioSettings settings;
    settings.inputMode = INPUT_RF;
    settings.sampleRate = 48000.0;
    for (int length : {1, 2, 7, 32, 127, 512, 513}) {
        settings.fftLength = length;
        for (int type = FFT_WINDOW_RECTANGULAR; type <= FFT_WINDOW_FLAT_TOP; ++type) {
            settings.fftWindowType = type;
            for (int count : {(std::max)(1, length / 3), length + 5}) {
                auto iq = samples(count, length);
                iq.push_back(42.0f);
                const auto reference = referenceDft(iq, length, type);
                std::vector<float> frequencies, levels;
                require(gui.computeSnapshot(settings, iq, frequencies, levels), "short snapshot failed");
                compareReference(levels, reference, false);
                ++referenceFrames;
                if (length >= 512 && count < length) {
                    CliSpectrumMeter cli(length);
                    std::vector<float> trace{42.0f};
                    require(cli.analyze(iq.data(), count, settings.sampleRate, 0.0, &trace).isEmpty() &&
                            trace.empty(), "CLI admitted a short window");
                }
            }
        }
    }
}

void testCoreStorage() {
    FftTransformCore core;
    require(!core.allocateBuffers(0, FftTransformCore::Storage::InPlace), "zero core size accepted");
    for (auto storage : {FftTransformCore::Storage::InPlace, FftTransformCore::Storage::OutOfPlace}) {
        for (int length : {7, 32, 513}) {
            require(core.allocateBuffers(length, storage), "core allocation failed");
            auto *input = core.input();
            auto *output = core.output();
            require((input == output) == (storage == FftTransformCore::Storage::InPlace),
                    "core storage policy ignored");
            {
                std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
                require(core.createPlanLocked(), "core plan failed");
            }
            core.ensureWindow(FFT_WINDOW_HANN);
            const auto iq = samples(length, length);
            for (int frame = 0; frame < 3; ++frame) {
                require(core.allocateBuffers(length, storage) && core.ready() &&
                        core.input() == input && core.output() == output,
                        "warm core plan/storage was replaced");
                core.ensureWindow(FFT_WINDOW_HANN);
                core.stageInputRange(iq.data(), 0, length);
                core.execute();
                std::vector<float> levels(length);
                const double normalization = 20.0 * std::log10((std::max)(1.0, core.windowAmplitudeSum(length)));
                if (storage == FftTransformCore::Storage::InPlace) {
                    float *compacted = reinterpret_cast<float *>(output);
                    core.writeMagnitudes(compacted, 0, length, normalization);
                    std::copy_n(compacted, length, levels.begin());
                } else core.writeMagnitudes(levels.data(), 0, length, normalization);
                compareReference(levels, referenceDft(iq, length, FFT_WINDOW_HANN), false);
                ++referenceFrames;
            }
        }
    }
    core.releasePlan();
    require(!core.ready() && core.length() == 0 && !core.input() && !core.output(),
            "released core retained plan or buffers");
}

void testMixedPlanners() {
    std::atomic<bool> okay{true};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&okay, worker] {
            try {
                for (int frame = 0; frame < 5; ++frame) {
                    const int length = (frame + worker) % 2 ? 512 : 513;
                    FFTResult gui;
                    gui.setBackendPreference(FFT_BACKEND_CPU_FFTW);
                    CliSpectrumMeter cli(length);
                    RadioSettings settings;
                    settings.fftLength = length;
                    settings.fftWindowType = FFT_WINDOW_HANN;
                    settings.inputMode = INPUT_RF;
                    const auto iq = samples(length, length, frame);
                    std::vector<float> frequencies, levels;
                    require(gui.computeSnapshot(settings, iq, frequencies, levels) && cli.valid() &&
                            !cli.analyze(iq.data(), length, settings.sampleRate, 0.0).isEmpty(),
                            "mixed concurrent plan failed");
                }
            } catch (...) { okay.store(false); }
        });
    }
    for (auto &thread : threads) thread.join();
    require(okay.load(), "mixed GUI/CLI planner serialization failed");
}

void testWriterPrecision() {
    FftTransformCore core;
    require(core.allocateBuffers(8, FftTransformCore::Storage::OutOfPlace), "writer storage failed");
    auto *spectrum = core.output();
    for (int i = 0; i < 8; ++i) spectrum[i][0] = spectrum[i][1] = 0.0f;
    spectrum[1][0] = 1.0f;
    spectrum[2][0] = std::numeric_limits<float>::quiet_NaN();
    spectrum[2][1] = 0.5f;
    spectrum[3][0] = std::numeric_limits<float>::infinity();
    spectrum[3][1] = 1.0f;
    spectrum[4][0] = (std::numeric_limits<float>::max)();
    spectrum[5][0] = 1.0e-30f;
    spectrum[6][0] = -1.0f;
    spectrum[7][1] = 0.25f;
    std::vector<float> gui(8), cli(8);
    core.writeMagnitudes(gui.data(), 0, 8, 20.0f * std::log10(2.0f));
    core.writeMagnitudes(cli.data(), 0, 8, 20.0 * std::log10(2.0));
    for (int i : {0, 5}) require(gui[i] == -160.0f && cli[i] == -160.0f, "writer floor changed");
    for (int i : {1, 6, 7}) {
        const double amplitude = i == 7 ? 0.125 : 0.5;
        const float expected = static_cast<float>(20.0 * std::log10(amplitude));
        require(std::abs(gui[i] - expected) < 0.0001f && std::abs(cli[i] - expected) < 0.0001f,
                "writer normalization changed");
    }
    require(std::abs(gui[2] + 12.0412f) < 0.0001f && std::abs(gui[3] + 6.0206f) < 0.0001f &&
            cli[2] == -160.0f && cli[3] == -160.0f,
            "writer nonfinite-component precision policies changed");
    require(gui[4] == -160.0f && cli[4] > 700.0f, "writer overflow precision policies changed");
}

void testInterruptedWindow() {
    FftTransformCore core;
    require(core.allocateBuffers(32, FftTransformCore::Storage::InPlace), "window test allocation failed");
    for (int type : {FFT_WINDOW_HANN, FFT_WINDOW_HAMMING, FFT_WINDOW_FLAT_TOP}) {
        bool interrupted = false;
        try {
            core.ensureWindow(type, [](int count, auto range) {
                range(0, count / 2);
                throw std::runtime_error("interrupted dispatch");
            });
        } catch (const std::runtime_error &) { interrupted = true; }
        require(interrupted && core.windowAmplitudeSum(32) == 0.0,
                "interrupted window was published");
        bool retried = false;
        core.ensureWindow(type, [&retried](int count, auto range) {
            retried = true;
            range(0, count);
        });
        double expected = 0.0;
        for (int i = 0; i < 32; ++i) expected += referenceWindow(i, 32, type);
        require(retried && core.windowAmplitudeSum(32) == expected, "interrupted window was cached");
        core.ensureWindow(type, [](int, auto) { throw std::runtime_error("warm window rebuilt"); });
    }
}

void benchmark() {
    for (int length : {65536, 1048576}) {
        RadioSettings settings;
        settings.inputMode = INPUT_RF;
        settings.fftWindowType = FFT_WINDOW_HANN;
        settings.fftLength = length;
        FFTResult gui(true);
        gui.setBackendPreference(FFT_BACKEND_CPU_FFTW);
        CliSpectrumMeter cli(length);
        const auto iq = samples(length, length);
        std::vector<float> frequencies, levels, trace;
        require(gui.computeSnapshot(settings, iq, frequencies, levels), "benchmark warmup failed");
        require(!cli.analyze(iq.data(), length, settings.sampleRate, 0.0, &trace).isEmpty(),
                "benchmark CLI warmup failed");
        constexpr int Frames = 8;
        QElapsedTimer timer;
        timer.start();
        for (int frame = 0; frame < Frames; ++frame)
            require(gui.computeSnapshot(settings, iq, frequencies, levels), "GUI benchmark failed");
        const double guiMs = timer.nsecsElapsed() / (1.0e6 * Frames);
        timer.restart();
        for (int frame = 0; frame < Frames; ++frame)
            require(!cli.analyze(iq.data(), length, settings.sampleRate, 0.0, &trace).isEmpty(),
                    "CLI benchmark failed");
        std::cout << "FFT " << length << ": GUI " << guiMs << " ms, CLI "
                  << timer.nsecsElapsed() / (1.0e6 * Frames) << " ms per warm frame\n";
    }
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("shared-fft-core-selftest"));
    QStandardPaths::setTestModeEnabled(true);
    setFobosVerboseLoggingEnabled(false);
    try {
        testMatchedWrappers();
        testSnapshotsAndWindows();
        testCoreStorage();
        testWriterPrecision();
        testInterruptedWindow();
        testMixedPlanners();
        if (app.arguments().contains(QStringLiteral("--benchmark"))) benchmark();
        std::cout << "Shared FFT core passed: " << comparedFrames << " GUI/CLI parity frames, "
                  << referenceFrames << " independent DFT frames; odd/even frequency mapping, "
                     "short-window contracts, all windows, interrupted dispatch, warm storage and mixed planners\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
