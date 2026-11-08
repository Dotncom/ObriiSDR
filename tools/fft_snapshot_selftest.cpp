#include "fft.h"
#include "diagnosticlogging.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

// Standalone main: link fft.cpp, iqbuffer.cpp, gpufftbackend.cpp, fftplannermutex.cpp,
// fft_test_globals.cpp and moc_fft.cpp, with Qt Core/Concurrent and float FFTW.

namespace {
constexpr double TwoPi = 6.28318530717958647692;
int parityFrames = 0;
int referenceFrames = 0;

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void compare(const std::vector<float> &actual, const std::vector<float> &expected) {
    require(actual.size() == expected.size(), "output size mismatch");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        require(std::isfinite(actual[i]) && actual[i] == expected[i],
                "direct and IqBuffer outputs differ");
    }
}

std::vector<float> samples(int count, int frame = 0) {
    std::vector<float> iq(static_cast<std::size_t>(count) * 2);
    for (int i = 0; i < count; ++i) {
        const double t = i + frame * 3.0;
        iq[2 * i] = static_cast<float>(0.3 * std::cos(0.47 * t) + 0.1 * std::sin(1.31 * t));
        iq[2 * i + 1] = static_cast<float>(0.2 * std::cos(0.47 * t) - 0.17 * std::sin(0.73 * t));
    }
    if (count > 8) {
        iq[4] = std::numeric_limits<float>::quiet_NaN();
        iq[9] = std::numeric_limits<float>::infinity();
    }
    return iq;
}

void testParity() {
    FFTResult direct, local;
    direct.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    local.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    RadioSettings settings;
    settings.sampleRate = 48000.0;
    settings.centerFrequency = 1250.0;
    settings.hfNoiseCancelRefGainDb = 3.5;
    settings.hfNoiseCancelRefTiltDb = -4.0;
    settings.hfNoiseCancelRefDelayNs = 1500.0;
    for (int length : {1, 2, 7, 30, 127}) {
        settings.fftLength = length;
        for (int window = FFT_WINDOW_RECTANGULAR; window <= FFT_WINDOW_FLAT_TOP; ++window) {
            settings.fftWindowType = window;
            for (int mode = INPUT_RF; mode <= INPUT_HF_NOISE_CANCEL; ++mode) {
                settings.inputMode = mode;
                for (int count : {1, (std::max)(1, length / 2), length, length + 7}) {
                    for (int frame = 0; frame < 4; ++frame) {
                        settings.simplifiedAudioChannelizer = frame % 2 != 0;
                        settings.hfNoiseCancelFreeze = frame == 2;
                        settings.hfNoiseCancelDepth = frame * 0.65;
                        if (frame == 3) {
                            direct.resetHfNoiseCancelState();
                            local.resetHfNoiseCancelState();
                        }
                        const auto iq = samples(count, frame);
                        const auto original = iq;
                        IqBuffer::BlockMetadata metadata;
                        metadata.valid = frame != 1;
                        metadata.centerFrequencyHz = 2750.0 + frame;
                        metadata.tuning = frame == 2;
                        metadata.scanIndex = 12;
                        IqBuffer::clear();
                        require(IqBuffer::publish(iq.data(), iq.size(), false, true, 0, &metadata),
                                "publish failed");
                        std::vector<float> lf, lm, lr{42.0f}, df, dm, dr{42.0f};
                        IqBuffer::BlockMetadata captured;
                        require(local.storeFFTResults(settings, lf, lm, frame == 1 ? nullptr : &lr,
                                                      &captured), "local computation failed");
                        require(captured.valid == metadata.valid &&
                                    captured.centerFrequencyHz == metadata.centerFrequencyHz &&
                                    captured.tuning == metadata.tuning && captured.scanIndex == 12 &&
                                    captured.totalFloatCount == iq.size(), "metadata was not returned");
                        IqBuffer::clear();
                        require(direct.computeSnapshot(settings, iq, df, dm,
                                                       frame == 1 ? nullptr : &dr, &captured),
                                "direct computation depended on global IQ");
                        compare(df, lf);
                        compare(dm, lm);
                        compare(dr, lr);
                        require(std::memcmp(iq.data(), original.data(), iq.size() * sizeof(float)) == 0,
                                "caller IQ was modified");
                        ++parityFrames;
                    }
                }
            }
        }
    }
}

float referenceWindow(int index, int length, int type) {
    const double p = TwoPi * index / (std::max)(1, length - 1);
    switch (type) {
    case FFT_WINDOW_HANN: return static_cast<float>(0.5 - 0.5 * std::cos(p));
    case FFT_WINDOW_HAMMING: return static_cast<float>(0.54 - 0.46 * std::cos(p));
    case FFT_WINDOW_BLACKMAN_HARRIS:
        return static_cast<float>(0.35875 - 0.48829 * std::cos(p) +
                                  0.14128 * std::cos(2 * p) - 0.01168 * std::cos(3 * p));
    case FFT_WINDOW_FLAT_TOP:
        return static_cast<float>(0.21557895 - 0.41663158 * std::cos(p) +
                                  0.277263158 * std::cos(2 * p) - 0.083578947 * std::cos(3 * p) +
                                  0.006947368 * std::cos(4 * p));
    default: return 1.0f;
    }
}

std::vector<std::complex<double>> referenceDft(const RadioSettings &settings,
                                              const std::vector<float> &iq, int mode) {
    const int n = settings.fftLength;
    const int count = (std::min)(n, static_cast<int>(iq.size() / 2));
    const int start = static_cast<int>(iq.size() / 2) - count;
    std::vector<std::complex<double>> result(n);
    double amplitudeSum = 0.0;
    for (int i = 0; i < count; ++i) {
        const float w = referenceWindow(i, n, settings.fftWindowType);
        amplitudeSum += w;
        const float real = iq[2 * (start + i)];
        const float imag = iq[2 * (start + i) + 1];
        const std::complex<double> value(
            mode != INPUT_HF2 && std::isfinite(real) ? real * w : 0.0f,
            mode != INPUT_HF1 && std::isfinite(imag) ? imag * w : 0.0f);
        for (int k = 0; k < n; ++k) {
            const double phase = -TwoPi * i * k / n;
            result[k] += value * std::complex<double>(std::cos(phase), std::sin(phase));
        }
    }
    for (auto &value : result) {
        value /= (std::max)(1.0, amplitudeSum);
    }
    return result;
}

void compareAmplitude(float db, double amplitude) {
    const double actual = std::pow(10.0, db / 20.0);
    require(std::isfinite(db) && db >= -160.0f &&
                std::abs(actual - amplitude) < 0.00001 * (1.0 + amplitude),
            "independent DFT/normalization mismatch");
}

void testReferenceDft() {
    FFTResult fft;
    fft.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    RadioSettings settings;
    settings.sampleRate = 48000.0;
    settings.centerFrequency = 1500.0;
    settings.hfNoiseCancelDepth = 0.0;
    for (int n : {1, 7, 30, 127}) {
        settings.fftLength = n;
        for (int window = FFT_WINDOW_RECTANGULAR; window <= FFT_WINDOW_FLAT_TOP; ++window) {
            settings.fftWindowType = window;
            for (int count : {(std::max)(1, n / 2), n, n + 5}) {
                auto iq = samples(count);
                iq.push_back(123.0f); // A trailing incomplete pair must not shift the source.
                const auto main = referenceDft(settings, iq, INPUT_HF1);
                const auto ref = referenceDft(settings, iq, INPUT_HF2);
                const auto rf = referenceDft(settings, iq, INPUT_RF);
                for (int mode = INPUT_RF; mode <= INPUT_HF_NOISE_CANCEL; ++mode) {
                    settings.inputMode = mode;
                    std::vector<float> f, m, r;
                    require(fft.computeSnapshot(settings, iq, f, m, &r), "DFT snapshot failed");
                    require(f.size() == static_cast<std::size_t>(n) && m.size() == f.size(),
                            "FFT length changed");
                    for (int k = 0; k < n; ++k) {
                        require(f[k] == static_cast<float>((k - n / 2) * (settings.sampleRate / n) +
                                                           settings.centerFrequency), "axis changed");
                        if (mode == INPUT_HF_COMBINED) {
                            const double expected = k < n / 2 ? std::abs(main[n / 2 - k])
                                                              : std::abs(ref[k - n / 2]);
                            compareAmplitude(m[(k + n / 2) % n], expected);
                        } else {
                            compareAmplitude(m[k], std::abs(mode == INPUT_RF ? rf[k] :
                                                            mode == INPUT_HF2 ? ref[k] : main[k]));
                        }
                        if (mode == INPUT_HF_NOISE_CANCEL) {
                            require(r.size() == m.size(), "missing HF reference");
                            compareAmplitude(r[k], std::abs(ref[k]));
                        } else {
                            require(r.empty(), "stale HF reference");
                        }
                    }
                    ++referenceFrames;
                }
            }
        }
    }
}

void testMetadataAndFailures() {
    FFTResult fft;
    fft.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    RadioSettings settings;
    settings.fftLength = 31;
    settings.sampleRate = 48000.0;
    settings.centerFrequency = 10000.0;
    const auto iq = samples(31);
    std::vector<float> f, m, r, baseline;
    require(fft.computeSnapshot(settings, iq, baseline, m), "default metadata failed");
    for (bool valid : {false, true}) {
        for (double center : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(), 25000.0}) {
            IqBuffer::BlockMetadata metadata;
            metadata.valid = valid;
            metadata.centerFrequencyHz = center;
            require(fft.computeSnapshot(settings, iq, f, m, &r, &metadata), "metadata test failed");
            if (valid && std::isfinite(center) && center > 0.0) {
                require(f[15] == 25000.0f, "valid metadata did not correct center");
            } else {
                compare(f, baseline);
            }
        }
    }
    require(!fft.computeSnapshot(settings, {}, f, m), "empty IQ accepted");
    require(!fft.computeSnapshot(settings, {1.0f}, f, m), "incomplete IQ accepted");
    for (int length : {0, -1}) {
        settings.fftLength = length;
        require(!fft.computeSnapshot(settings, iq, f, m) && f.empty() && m.empty(),
                "invalid FFT length accepted");
    }
    settings.fftLength = 31;
    for (double rate : {0.0, -1.0}) {
        settings.sampleRate = rate;
        require(!fft.computeSnapshot(settings, iq, f, m) && f.empty() && m.empty(),
                "invalid sample rate accepted");
    }
    settings.sampleRate = 48000.0;
    IqBuffer::clear();
    require(!fft.storeFFTResults(settings, f, m), "empty IqBuffer accepted");
    require(fft.computeSnapshot(settings, iq, f, m), "failed to recover after invalid input");
}

void testHistoricalSnapshot() {
    RadioSettings settings;
    settings.fftLength = 30;
    settings.fftWindowType = FFT_WINDOW_HAMMING;
    const auto first = samples(41);
    const auto second = samples(17, 8);
    IqBuffer::clear();
    require(IqBuffer::publish(first.data(), first.size(), false), "first publish failed");
    const auto end = IqBuffer::stats().totalFloatCount;
    require(IqBuffer::publish(second.data(), second.size(), false), "second publish failed");
    FFTResult local, direct;
    local.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    direct.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    std::vector<float> lf, lm, df, dm;
    require(local.storeFFTResults(settings, lf, lm, nullptr, nullptr, end), "historical FFT failed");
    require(direct.computeSnapshot(settings, first, df, dm), "historical direct FFT failed");
    compare(df, lf);
    compare(dm, lm);
}

int profileReports = 0;
QString lastProfile;

void profileMessage(QtMsgType, const QMessageLogContext &, const QString &message) {
    if (message.startsWith(QStringLiteral("[FFT profile]"))) {
        ++profileReports;
        lastProfile = message;
    }
}

void testProfilingAndParallelBins() {
    RadioSettings settings;
    settings.fftLength = 1048576;
    settings.fftWindowType = FFT_WINDOW_HANN;
    const auto iq = samples(257);
    IqBuffer::clear();
    require(IqBuffer::publish(iq.data(), iq.size(), false), "profile publish failed");
    FFTResult local(true), direct(true);
    local.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    direct.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    std::vector<float> lf, lm, df, dm;
    const auto previousHandler = qInstallMessageHandler(profileMessage);
    setFobosVerboseLoggingEnabled(true);
    bool succeeded = true;
    bool coldPlanSkipped = false;
    for (int frame = 0; frame < 31; ++frame) {
        settings.simplifiedAudioChannelizer = frame % 2 != 0;
        succeeded = local.storeFFTResults(settings, lf, lm) && succeeded;
        succeeded = direct.computeSnapshot(settings, iq, df, dm) && succeeded;
        if (frame == 29) {
            coldPlanSkipped = profileReports == 0;
        }
    }
    setFobosVerboseLoggingEnabled(false);
    qInstallMessageHandler(previousHandler);
    require(succeeded, "large FFT failed");
    require(coldPlanSkipped && profileReports == 2, "profiling frame cadence changed");
    require(lastProfile.contains(QStringLiteral("snapshotMs 0 ")),
            "direct FFT reported a global snapshot acquisition");
    compare(df, lf);
    compare(dm, lm);
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("fft-snapshot-selftest"));
    QStandardPaths::setTestModeEnabled(true);
    setFobosVerboseLoggingEnabled(false);
    try {
        testParity();
        testReferenceDft();
        testMetadataAndFailures();
        testHistoricalSnapshot();
        testProfilingAndParallelBins();
        std::cout << "FFT snapshot tests passed: " << parityFrames << " exact parity frames, "
                  << referenceFrames << " independent DFT frames; metadata, historical snapshots, "
                     "invalid input, parallel bins and profiling verified\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
