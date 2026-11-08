#include "pfbchannelizer.h"
#include "iqchannelizer.h"
#include "channelizerutils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

struct IqChannelizerTestAccess {
    static void configure(IqChannelizer &c, double input, double target, double cutoff, int taps) {
        c.configureResampler(input, target, cutoff, taps);
    }
    static float coefficient(const IqChannelizer &c, int phase, int tap) {
        return c.resamplerTaps[static_cast<std::size_t>(phase)][static_cast<std::size_t>(tap)];
    }
    static void push(IqChannelizer &c, std::complex<float> sample, std::vector<float> &out) {
        c.pushResamplerSample(sample, out, false);
    }
};

namespace {
constexpr double Pi = 3.14159265358979323846;
using Complex = std::complex<double>;
using Outputs = std::vector<PfbChannelizer::ChannelOutput>;
using Status = PfbChannelizer::Status;

void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
Complex sample(const std::vector<float> &iq, std::size_t n) {
    return {iq[2 * n], iq[2 * n + 1]};
}
std::vector<float> tone(std::size_t count, double cycles, double amplitude = 0.2) {
    std::vector<float> iq(count * 2);
    for (std::size_t n = 0; n < count; ++n) {
        const auto value = std::polar(amplitude, 2 * Pi * cycles * n);
        iq[2 * n] = static_cast<float>(value.real());
        iq[2 * n + 1] = static_cast<float>(value.imag());
    }
    return iq;
}
double rms(const std::vector<float> &iq, std::size_t skip) {
    require(iq.size() / 2 > skip, "Too few samples for RMS");
    double power = 0;
    for (std::size_t n = skip; n < iq.size() / 2; ++n) power += std::norm(sample(iq, n));
    return std::sqrt(power / (iq.size() / 2 - skip));
}
void same(const std::vector<float> &a, const std::vector<float> &b, double tolerance = 0) {
    require(a.size() == b.size(), "Output length changed");
    for (std::size_t i = 0; i < a.size(); ++i)
        require(std::isfinite(a[i]) && std::abs(double(a[i]) - b[i]) <= tolerance,
                "Output waveform changed");
}
void append(Outputs &destination, const Outputs &source) {
    if (destination.empty()) destination.resize(source.size());
    require(destination.size() == source.size(), "Selection cardinality changed");
    for (std::size_t i = 0; i < source.size(); ++i)
        destination[i].iq.insert(destination[i].iq.end(), source[i].iq.begin(), source[i].iq.end());
}

std::vector<double> prototype(int channels, int taps) {
    const int length = channels * taps;
    std::vector<double> h(static_cast<std::size_t>(length));
    double total = 0;
    for (int n = 0; n < length; ++n) {
        const double distance = n - (length - 1) / 2.0;
        const double w = 0.42 - 0.5 * std::cos(2 * Pi * n / (length - 1)) +
                         0.08 * std::cos(4 * Pi * n / (length - 1));
        h[n] = std::sin(Pi * distance / channels) / (Pi * distance) * w;
        total += h[n];
    }
    for (auto &value : h) value /= total;
    return h;
}

void testConvolutionAndDelay() {
    std::mt19937 random(31415);
    std::uniform_real_distribution<float> distribution(-0.3f, 0.3f);
    for (int channels : {8, 16, 32, 64}) {
        for (int taps : {8, 16, 32, 64}) {
            const int length = channels * taps;
            const auto h = prototype(channels, taps);
            PfbChannelizer bank;
            require(bank.configure({2048000, channels, taps}), "Configure failed");
            require(bank.delayInputSamples() == (length - 1) * 0.5, "Delay metadata wrong");
            double moment = 0;
            for (int i = 0; i < length; ++i) moment += i * h[i];
            require(std::abs(moment - bank.delayInputSamples()) < 1e-8, "Prototype delay wrong");
            std::vector<float> input(static_cast<std::size_t>(length + channels * 13) * 2);
            for (auto &v : input) v = distribution(random);
            const std::vector<int> bins{0, 1, channels / 2, channels - 1};
            Outputs actual;
            const auto result = bank.processFloatIq(input.data(), input.size(), bins, actual);
            require(result.status == Status::Ok && result.firstOutputInputSample == std::uint64_t(channels - 1),
                    "First output timestamp wrong");
            for (std::size_t b = 0; b < bins.size(); ++b) {
                for (std::size_t m = 0; m < actual[b].iq.size() / 2; ++m) {
                    const int t = static_cast<int>((m + 1) * channels - 1);
                    Complex expected(0, 0);
                    for (int l = 0; l < length && l <= t; ++l)
                        expected += h[l] * sample(input, t - l) *
                                    std::polar(1.0, -2 * Pi * bins[b] * (t - l) / channels);
                    require(std::abs(sample(actual[b].iq, m) - expected) < 1e-6,
                            "PFB disagrees with direct mixed convolution");
                }
            }
            // Impulse at a non-frame-aligned time checks both modulation phase
            // and the causal FIR tail, including startup zero padding.
            bank.reset();
            std::fill(input.begin(), input.end(), 0.0f);
            const int impulse = channels + 3;
            input[2 * impulse] = 1.0f;
            bank.processFloatIq(input.data(), input.size(), bins, actual);
            for (std::size_t b = 0; b < bins.size(); ++b) {
                for (std::size_t m = 0; m < actual[b].iq.size() / 2; ++m) {
                    const int lag = static_cast<int>((m + 1) * channels - 1) - impulse;
                    const auto expected = lag < 0 || lag >= length ? Complex(0, 0) :
                        h[lag] * std::polar(1.0, -2 * Pi * bins[b] * impulse / channels);
                    require(std::abs(sample(actual[b].iq, m) - expected) < 1e-7, "Impulse timing/phase wrong");
                }
            }
        }
    }
    std::cout << "PASS direct convolution, impulse, delay: M=8/16/32/64, T=8/16/32/64\n";
}

void testTones() {
    double worstPassError = 0, worstLeakage = 0;
    for (int channels : {8, 16, 32, 64}) {
        for (int taps : {8, 16, 32, 64}) {
            PfbChannelizer bank;
            require(bank.configure({2048000, channels, taps}), "Configure failed");
            const double halfPass = bank.usablePassbandHz() / (2 * bank.outputRateHz());
            for (int bin : {0, 1, channels / 2 - 1, channels / 2, channels - 1}) {
                const int signedBin = bin < channels / 2 ? bin : bin - channels;
                for (double offset : {0.0, -halfPass, halfPass, 0.073}) {
                    bank.reset();
                    const auto input = tone(static_cast<std::size_t>(channels * (taps + 256)),
                                            (signedBin + offset) / channels, 1.25);
                    Outputs out;
                    const int neighbor = (bin + (offset < 0 ? channels - 1 : 1)) % channels;
                    const auto result = bank.processFloatIq(input.data(), input.size(), {bin, neighbor}, out);
                    require(result.status == Status::Ok && out.size() == 2, "Tone output missing");
                    require(out[0].binIndex == bin && out[0].centerOffsetHz == signedBin * bank.outputRateHz() &&
                            out[0].sampleRateHz == bank.outputRateHz(), "Channel metadata wrong");
                    const double gain = rms(out[0].iq, taps) / 1.25;
                    worstPassError = (std::max)(worstPassError, std::abs(gain - 1));
                    require(std::abs(gain - 1) < 0.002, "Usable passband gain changed or clipped");
                    const double leak = rms(out[1].iq, taps) / 1.25;
                    worstLeakage = (std::max)(worstLeakage, leak);
                    require(leak < 0.001, "Adjacent/off-bin rejection below 60 dB");
                    const auto step = sample(out[0].iq, taps + 1) / sample(out[0].iq, taps);
                    require(std::abs(step - std::polar(1.0, 2 * Pi * offset)) < 2e-5,
                            "Residual frequency sign or phase wrong");
                    const double t = (taps + 1) * channels - 1.0;
                    const auto expected = std::polar(1.25 * gain,
                        2 * Pi * offset * (t - bank.delayInputSamples()) / channels);
                    require(std::abs(sample(out[0].iq, taps) - expected) < 2e-5, "Tone group delay wrong");
                }
            }
        }
    }
    std::cout << "PASS tones, signs, no clipping: pass error=" << worstPassError
              << " adjacent rejection=" << -20 * std::log10(worstLeakage) << " dB\n";
}

void testChunkingGapsAndSelection() {
    const auto input = tone(123457, 0.07123);
    const PfbChannelizer::Config config{2000000, 32, 32};
    const std::vector<int> bins{0, 31, 2, 16, 1, 3, 4, 5};
    PfbChannelizer whole, split;
    require(whole.configure(config) && split.configure(config), "Configure failed");
    Outputs reference, actual, part;
    whole.processFloatIq(input.data(), input.size(), bins, reference);
    const std::array<std::size_t, 9> sizes{1, 17, 32, 33, 201, 4095, 9, 8127, 8192};
    std::size_t position = 0, iteration = 0, emitted = 0;
    while (position < input.size() / 2) {
        const auto count = (std::min)(sizes[iteration++ % sizes.size()], input.size() / 2 - position);
        require(split.configure(config), "Repeated config failed");
        const auto result = split.processFloatIq(input.data() + 2 * position, count * 2, bins, part);
        require(result.status == Status::Ok && result.inputSamplesConsumed == count, "Chunk rejected");
        if (result.outputSamplesPerChannel)
            require(result.firstOutputInputSample == (emitted + 1) * 32 - 1, "Chunk timestamp discontinuity");
        emitted += result.outputSamplesPerChannel;
        append(actual, part);
        position += count;
    }
    for (std::size_t b = 0; b < bins.size(); ++b) same(actual[b].iq, reference[b].iq);
    split.reset();
    whole.reset();
    const std::size_t prefix = 333;
    split.processFloatIq(input.data(), prefix * 2, {}, part);
    require(part.empty(), "Empty selection materialized output");
    whole.processFloatIq(input.data(), prefix * 2, bins, part);
    split.processFloatIq(input.data() + prefix * 2, input.size() - prefix * 2, bins, actual);
    whole.processFloatIq(input.data() + prefix * 2, input.size() - prefix * 2, bins, reference);
    for (std::size_t b = 0; b < bins.size(); ++b) same(actual[b].iq, reference[b].iq);
    // A dropped block must start a fresh epoch, not splice old FIR history.
    split.reset();
    whole.reset();
    const std::size_t afterGap = 7919;
    split.processFloatIq(input.data() + 2 * afterGap, input.size() - 2 * afterGap, bins, actual);
    whole.processFloatIq(input.data() + 2 * afterGap, input.size() - 2 * afterGap, bins, reference);
    for (std::size_t b = 0; b < bins.size(); ++b) same(actual[b].iq, reference[b].iq);
    require(split.configure({1000000, 8, 8}), "Changed config failed");
    require(whole.configure({1000000, 8, 8}), "Changed config failed");
    split.processFloatIq(input.data(), input.size(), {7, 0}, actual);
    whole.processFloatIq(input.data(), input.size(), {7, 0}, reference);
    same(actual[0].iq, reference[0].iq);
    same(actual[1].iq, reference[1].iq);
    std::cout << "PASS bit-exact chunking, empty selection, selection order, gaps, reconfigure\n";
}

void testGuards() {
    PfbChannelizer bank;
    Outputs out;
    float input[2]{1, 0};
    require(bank.processFloatIq(input, 2, {0}, out).status == Status::NotConfigured, "Unconfigured accepted");
    require(!bank.configure({0, 16, 16}) && !bank.configure({1000, 7, 16}) &&
            !bank.configure({1000, 128, 16}) && !bank.configure({1000, 12, 16}) &&
            !bank.configure({1000, 16, 7}) && !bank.configure({1000, 16, 65}) &&
            !bank.configure({std::numeric_limits<double>::infinity(), 16, 16}), "Invalid config accepted");
    require(bank.configure({1600000, 16, 16}), "Configure failed");
    require(bank.nearestCenterIndex(149999) == 1 && bank.nearestCenterIndex(-149999) == 15 &&
            bank.nearestCenterIndex(150000) == 2 && bank.nearestCenterIndex(-150000) == 14 &&
            bank.nearestCenterIndex(0) == 0 && bank.nearestCenterIndex(-800000) == 8 &&
            bank.nearestCenterIndex(800000) == -1, "Nearest-center convention wrong");
    require(bank.usablePassbandHz() == 62500 && bank.coversBand(1, 100000, 60000) &&
            !bank.coversBand(1, 130000, 10000) && !bank.coversBand(8, 790000, 1000) &&
            !bank.coversBand(8, -800000, 1000) && !bank.coversBand(16, 0, 1000) &&
            !bank.coversBand(0, 0, -1000), "Band guard incorrect");
    require(bank.processFloatIq(input, 1, {0}, out).status == Status::InvalidInput, "Odd CF32 accepted");
    require(bank.processFloatIq(nullptr, 2, {0}, out).status == Status::InvalidInput, "Null CF32 accepted");
    require(bank.processFloatIq(input, 2 * (PfbChannelizer::MaxInputSamples + 1), {0}, out).status ==
            Status::ResourceLimit, "Unbounded block accepted");
    for (const auto &bins : std::vector<std::vector<int>>{{0, 0}, {-1}, {16}, {0,1,2,3,4,5,6,7,8}})
        require(bank.processFloatIq(input, 2, bins, out).status == Status::InvalidSelection,
                "Invalid selection accepted");
    const auto valid = tone(1000, 0.125);
    bank.processFloatIq(valid.data(), 38, {0}, out);
    require(!bank.configure({100, 9, 16}) && bank.outputRateHz() == 100000, "Bad config destroyed state");
    PfbChannelizer fresh;
    require(fresh.configure({1600000, 16, 16}), "Configure failed");
    Outputs reference;
    fresh.processFloatIq(valid.data(), 38, {0}, reference);
    bank.processFloatIq(valid.data() + 38, valid.size() - 38, {0}, out);
    fresh.processFloatIq(valid.data() + 38, valid.size() - 38, {0}, reference);
    same(out[0].iq, reference[0].iq);
    bank.reset();
    auto corrupt = valid;
    corrupt[0] = std::numeric_limits<float>::quiet_NaN();
    corrupt[3] = std::numeric_limits<float>::infinity();
    bank.processFloatIq(corrupt.data(), corrupt.size(), {0}, out);
    require(std::all_of(out[0].iq.begin(), out[0].iq.end(), [](float v) { return std::isfinite(v); }),
            "Nonfinite input poisoned history");
    PfbChannelizer moved(std::move(bank));
    require(moved.isConfigured() && !bank.isConfigured(), "Plan move ownership broken");
    require(moved.processFloatIq(nullptr, 0, {0}, out).status == Status::Ok && out[0].iq.empty(),
            "Empty block rejected");
    std::cout << "PASS input, resource, coverage, transactional failure, and ownership guards\n";
}

void testCancellationAndPlannerConcurrency() {
    const auto input = tone(PfbChannelizer::MaxInputSamples, 0.123);
    PfbChannelizer bank, whole;
    require(bank.configure({50000000, 64, 64}) && whole.configure({50000000, 64, 64}), "Configure failed");
    Outputs out, accumulated, reference;
    std::atomic_bool cancel{true};
    auto result = bank.processFloatIq(input.data(), input.size(), {1, 2}, out, &cancel);
    require(result.status == Status::Cancelled && result.inputSamplesConsumed == 0 && out.empty(),
            "Pre-cancel consumed input");
    cancel.store(false);
    std::thread worker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        cancel.store(true);
    });
    result = bank.processFloatIq(input.data(), input.size(), {1, 2}, out, &cancel);
    worker.join();
    require(result.status == Status::Cancelled && result.inputSamplesConsumed < input.size() / 2,
            "Long process did not observe cancellation");
    // Zero consumed is permitted if the worker is scheduled before process.
    if (!out.empty()) append(accumulated, out);
    const auto consumed = result.inputSamplesConsumed;
    cancel.store(false);
    result = bank.processFloatIq(input.data() + 2 * consumed, input.size() - 2 * consumed, {1, 2}, out, &cancel);
    require(result.status == Status::Ok, "Cancelled suffix could not resume");
    append(accumulated, out);
    whole.processFloatIq(input.data(), input.size(), {1, 2}, reference);
    same(accumulated[0].iq, reference[0].iq);
    same(accumulated[1].iq, reference[1].iq);
    std::atomic_bool failed{false};
    std::vector<std::thread> planners;
    for (int i = 0; i < 4; ++i) planners.emplace_back([&] {
        for (int j = 0; j < 20; ++j) {
            PfbChannelizer local;
            Outputs localOut;
            if (!local.configure({2000000, 8 << (j % 4), 16}) ||
                local.processFloatIq(input.data(), 2048, {0}, localOut).status != Status::Ok)
                failed.store(true);
        }
    });
    for (auto &planner : planners) planner.join();
    require(!failed.load(), "Concurrent plan lifecycle failed");
    std::cout << "PASS cancellation/resume parity and concurrent FFTW plan lifecycle\n";
}

void testLegacyFir() {
    // Frozen pre-options 32-tap/64-phase Blackman design and sample schedule.
    constexpr double inputRate = 250000, targetRate = 192000, cutoff = 24000;
    std::array<std::array<float, 32>, 64> legacy{};
    const double normalized = cutoff / inputRate;
    IqChannelizer channel;
    IqChannelizerTestAccess::configure(channel, inputRate, targetRate, cutoff, 32);
    for (int phase = 0; phase < 64; ++phase) {
        double sum = 0;
        for (int tap = 0; tap < 32; ++tap) {
            const double distance = double(15 - tap) + double(phase) / 64;
            const double arg = Pi * (2.0 * normalized * distance);
            const double sinc = std::abs(2.0 * normalized * distance) < 1e-12 ? 1.0 : std::sin(arg) / arg;
            const double position = double(tap) / 31;
            const double blackman = 0.42 - 0.5 * std::cos(2 * Pi * position) + 0.08 * std::cos(4 * Pi * position);
            const double value = 2.0 * normalized * sinc * blackman;
            legacy[phase][tap] = static_cast<float>(value);
            sum += value;
        }
        const float scale = static_cast<float>(1 / sum);
        for (int tap = 0; tap < 32; ++tap) {
            legacy[phase][tap] *= scale;
            require(legacy[phase][tap] == IqChannelizerTestAccess::coefficient(channel, phase, tap),
                    "Legacy FIR coefficient changed");
        }
    }
    const auto input = tone(8192, 0.0193);
    std::vector<float> actual, expected;
    for (std::size_t n = 0; n < input.size() / 2; ++n)
        IqChannelizerTestAccess::push(channel, {input[2 * n], input[2 * n + 1]}, actual);
    for (double next = 16; next <= input.size() / 2 - 1 + 1e-9; next += inputRate / targetRate) {
        const double position = next - 16;
        const int center = static_cast<int>(std::floor(position));
        const int phase = (std::clamp)(static_cast<int>(std::lround((position - center) * 63)), 0, 63);
        std::complex<float> value(0, 0);
        for (int tap = 0; tap < 32; ++tap) {
            const int index = center - 15 + tap;
            if (index >= 0 && index < static_cast<int>(input.size() / 2))
                value += std::complex<float>(input[2 * index], input[2 * index + 1]) * legacy[phase][tap];
        }
        expected.push_back((std::clamp)(value.real(), -1.0f, 1.0f));
        expected.push_back((std::clamp)(value.imag(), -1.0f, 1.0f));
    }
    same(actual, expected);
    for (bool agc : {false, true}) {
        RadioSettings settings;
        settings.sampleRate = 2048000;
        settings.listeningFrequency += 130000;
        settings.modulationType = MOD_NFM;
        settings.bandwidth = 16000;
        IqChannelizer normal, explicitLegacy;
        IqChannelizer::Options options;
        options.outputRateHz = channelizerTargetRate(settings);
        options.cutoffHz = channelizerCutoff(settings, options.outputRateHz);
        normal.processFloatIq(input.data(), input.size(), settings, expected, agc);
        const auto result = explicitLegacy.processFloatIq(input.data(), input.size(), settings, actual, agc, &options);
        require(result.valid && result.firTapCount == 32 && result.firDelayCoarseSamples == 16,
                "Legacy defaults changed");
        same(actual, expected);
    }
    std::cout << "PASS bit-exact frozen legacy 32-tap/64-phase coefficients, samples, and default options\n";
}

void testFirOptions() {
    RadioSettings settings;
    settings.sampleRate = 192000;
    IqChannelizer::Options options;
    options.outputRateHz = settings.sampleRate;
    options.cutoffHz = 0.08 * settings.sampleRate;
    const auto input = tone(16384, 0.0193);
    std::vector<float> reference, actual, part;
    for (int taps : {16, 32, 64, 128, 256}) {
        IqChannelizer whole, split;
        options.firTapCount = taps;
        const auto result = whole.processFloatIq(input.data(), input.size(), settings, reference, false, &options);
        require(result.valid && result.firTapCount == taps && result.firDelayCoarseSamples == taps / 2.0 &&
                result.outputSamples == int(input.size() / 2) - taps / 2, "FIR tap/delay metadata wrong");
        actual.clear();
        for (std::size_t n = 0; n < input.size();) {
            const auto length = (std::min)(std::size_t(714), input.size() - n);
            require(split.processFloatIq(input.data() + n, length, settings, part, false, &options).valid,
                    "FIR chunk invalid");
            actual.insert(actual.end(), part.begin(), part.end());
            n += length;
        }
        same(actual, reference);
    }
    double legacyStop = 0, highStop = 0;
    const auto stopTone = tone(16384, 0.11);
    for (auto quality : {IqChannelizer::FirQuality::Legacy, IqChannelizer::FirQuality::Balanced,
                         IqChannelizer::FirQuality::High}) {
        IqChannelizer channel;
        options.firTapCount = 0;
        options.firQuality = quality;
        const auto result = channel.processFloatIq(stopTone.data(), stopTone.size(), settings, actual, false, &options);
        const int expectedTaps = quality == IqChannelizer::FirQuality::Legacy ? 32 :
                                 quality == IqChannelizer::FirQuality::Balanced ? 64 : 128;
        require(result.valid && result.firTapCount == expectedTaps, "FIR quality preset wrong");
        if (expectedTaps == 32) legacyStop = rms(actual, 256);
        if (expectedTaps == 128) highStop = rms(actual, 256);
    }
    require(highStop < legacyStop * 0.01, "High-quality FIR did not improve off-bin rejection");
    options.firQuality = IqChannelizer::FirQuality::Legacy;
    options.transitionHz = 18000;
    IqChannelizer channel;
    auto result = channel.processFloatIq(input.data(), input.size(), settings, actual, false, &options);
    require(result.valid && result.firTapCount == 64, "Transition did not choose 64 taps");
    auto invalid = options;
    invalid.firTapCount = 32;
    require(!channel.processFloatIq(input.data(), input.size(), settings, actual, false, &invalid).valid,
            "Insufficient explicit taps accepted");
    for (int taps : {-1, 15, 17, 258}) {
        invalid = options;
        invalid.firTapCount = taps;
        require(!channel.processFloatIq(input.data(), input.size(), settings, actual, false, &invalid).valid,
                "Invalid FIR tap count accepted");
    }
    for (double transition : {-1.0, 1.0, settings.sampleRate,
                              std::numeric_limits<double>::quiet_NaN()}) {
        invalid = options;
        invalid.transitionHz = transition;
        require(!channel.processFloatIq(input.data(), input.size(), settings, actual, false, &invalid).valid,
                "Invalid FIR transition accepted");
    }
    // Rejected option edits must not consume input or reset valid history.
    IqChannelizer control;
    control.processFloatIq(input.data(), input.size(), settings, reference, false, &options);
    control.processFloatIq(input.data(), input.size(), settings, reference, false, &options);
    channel.processFloatIq(input.data(), input.size(), settings, actual, false, &options);
    same(actual, reference);
    for (double rate : {2048000.0, 50000000.0}) {
        settings.sampleRate = rate;
        settings.listeningFrequency = settings.centerFrequency + 0.093 * rate;
        options.outputRateHz = 192000;
        options.cutoffHz = 24000;
        options.transitionHz = 0;
        options.firTapCount = 128;
        const auto shifted = tone(150017, 0.093 + 1000.0 / rate, 1e-4);
        IqChannelizer whole, split;
        require(whole.processFloatIq(shifted.data(), shifted.size(), settings, reference, false, &options).valid,
                "Decimated high-quality FIR invalid");
        actual.clear();
        for (std::size_t n = 0; n < shifted.size();) {
            const auto length = (std::min)(std::size_t(7142), shifted.size() - n);
            require(split.processFloatIq(shifted.data() + n, length, settings, part, false, &options).valid,
                    "Decimated high-quality FIR chunk invalid");
            actual.insert(actual.end(), part.begin(), part.end());
            n += length;
        }
        same(actual, reference, 5e-7);
        split.reset();
        IqChannelizer afterGap;
        split.processFloatIq(shifted.data() + 38, shifted.size() - 38, settings, actual, false, &options);
        afterGap.processFloatIq(shifted.data() + 38, shifted.size() - 38, settings, reference, false, &options);
        same(actual, reference);
    }
    std::cout << "PASS FIR presets, 16..256 taps, chunk parity, transition guards; stop improvement="
              << 20 * std::log10(legacyStop / highStop) << " dB\n";
}

void benchmark() {
    constexpr int channels = 32, taps = 16, rounds = 3;
    constexpr std::size_t count = 262144;
    const auto input = tone(count, 2.03125 / channels, 0.02);
    std::cout << "Benchmark: M=32 T=16, 262144 IQ/block, 3 rounds, legacy residual FIR;\n"
                 "  includes shared bank + residual DDC; excludes planning/warmup; responses differ.\n";
    volatile double checksum = 0;
    for (double rate : {2048000.0, 50000000.0}) {
        for (int branches : {1, 2, 4, 8}) {
            PfbChannelizer bank;
            require(bank.configure({rate, channels, taps}), "Benchmark configure failed");
            std::vector<int> bins;
            std::vector<IqChannelizer> direct(branches), residual(branches);
            std::vector<RadioSettings> directSettings(branches), residualSettings(branches);
            IqChannelizer::Options options;
            options.outputRateHz = rate / channels / 4;
            options.cutoffHz = options.outputRateHz * 0.15;
            for (int b = 0; b < branches; ++b) {
                const int bin = b + 1;
                bins.push_back(bin);
                auto &s = directSettings[b];
                s.sampleRate = rate;
                s.listeningFrequency = s.centerFrequency + (bin + 0.03125) * bank.outputRateHz();
                residualSettings[b] = s;
                residualSettings[b].sampleRate = bank.outputRateHz();
                residualSettings[b].centerFrequency += bank.binCenterHz(bin);
                require(bank.coversBand(bin, s.listeningFrequency - s.centerFrequency, options.cutoffHz * 2),
                        "Benchmark branch not covered");
            }
            Outputs out;
            std::vector<std::vector<float>> processed(branches);
            const auto run = [&](bool useBank) {
                if (useBank) require(bank.processFloatIq(input.data(), input.size(), bins, out).status == Status::Ok,
                                     "Benchmark PFB failed");
                for (int b = 0; b < branches; ++b) {
                    const auto result = useBank ?
                        residual[b].processFloatIq(out[b].iq.data(), out[b].iq.size(), residualSettings[b], processed[b], false, &options) :
                        direct[b].processFloatIq(input.data(), input.size(), directSettings[b], processed[b], false, &options);
                    require(result.valid, "Benchmark DDC failed");
                    if (!processed[b].empty()) checksum = checksum + processed[b].back();
                }
            };
            run(false);
            run(true);
            const auto measure = [&](bool useBank) {
                const auto start = std::chrono::steady_clock::now();
                for (int r = 0; r < rounds; ++r) run(useBank);
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / rounds;
            };
            const double directMs = measure(false), pfbMs = measure(true);
            std::cout << "Fs=" << rate << " branches=" << branches << " individual_ms=" << directMs
                      << " pfb_plus_residual_ms=" << pfbMs << " ratio_direct_over_pfb=" << directMs / pfbMs << '\n';
        }
    }
    std::cout << "checksum=" << checksum << '\n';
}
}

int main(int argc, char **argv) {
    try {
        std::cout << std::setprecision(7);
        if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) {
            benchmark();
        } else {
            require(argc == 1, "Usage: ObriiPfbChannelizerSelfTest [--benchmark]");
            testConvolutionAndDelay();
            testTones();
            testChunkingGapsAndSelection();
            testGuards();
            testCancellationAndPlannerConcurrency();
            testLegacyFir();
            testFirOptions();
            std::cout << "PFB/FIR selftest passed\n";
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "PFB/FIR selftest FAILED: " << error.what() << '\n';
        return 1;
    }
}
