#include "pfbchannelizer.h"

#include "fftplannermutex.h"
#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <new>

namespace {
constexpr double TwoPi = 6.28318530717958647692;
bool validConfig(const PfbChannelizer::Config &config) {
    return std::isfinite(config.sampleRateHz) && config.sampleRateHz >= 1.0 &&
           config.channels >= 8 && config.channels <= 64 &&
           (config.channels & (config.channels - 1)) == 0 &&
           config.tapsPerPhase >= 8 && config.tapsPerPhase <= 64 &&
           config.tapsPerPhase % 2 == 0;
}
}

struct PfbChannelizer::Impl {
    Config config;
    std::vector<float> coefficients;
    std::vector<std::complex<float>> history;
    std::size_t writeIndex = 0;
    int pending = 0;
    std::uint64_t samplesSeen = 0;
    fftwf_complex *fftIn = nullptr;
    fftwf_complex *fftOut = nullptr;
    fftwf_plan plan = nullptr;

    ~Impl() {
        if (plan) {
            std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
            fftwf_destroy_plan(plan);
        }
        fftwf_free(fftIn);
        fftwf_free(fftOut);
    }

    void transform() {
        const std::size_t length = history.size();
        const std::size_t channels = static_cast<std::size_t>(config.channels);
        const std::size_t newest = (writeIndex + length - 1) % length;
        for (std::size_t phase = 0; phase < channels; ++phase) {
            std::complex<float> sum(0.0f, 0.0f);
            std::size_t index = (newest + length - phase) % length;
            for (std::size_t tap = phase; tap < length; tap += channels) {
                sum += history[index] * coefficients[tap];
                index = index >= channels ? index - channels : index + length - channels;
            }
            // At t = m*M + M-1, sample t-l has residue M-1-phase. Feeding
            // that residue to FFTW_FORWARD performs the global negative mixer.
            fftIn[channels - 1 - phase][0] = sum.real();
            fftIn[channels - 1 - phase][1] = sum.imag();
        }
        fftwf_execute(plan);
    }
};

PfbChannelizer::PfbChannelizer() = default;
PfbChannelizer::~PfbChannelizer() = default;
PfbChannelizer::PfbChannelizer(PfbChannelizer &&) noexcept = default;
PfbChannelizer &PfbChannelizer::operator=(PfbChannelizer &&) noexcept = default;

bool PfbChannelizer::configure(const Config &config) {
    if (!validConfig(config)) return false;
    if (impl && impl->config.sampleRateHz == config.sampleRateHz &&
        impl->config.channels == config.channels &&
        impl->config.tapsPerPhase == config.tapsPerPhase) return true;
    try {
        auto next = std::make_unique<Impl>();
        next->config = config;
        const int length = config.channels * config.tapsPerPhase;
        next->history.resize(static_cast<std::size_t>(length));
        next->coefficients.resize(static_cast<std::size_t>(length));
        const double cutoff = 0.5 / config.channels;
        double sum = 0.0;
        for (int tap = 0; tap < length; ++tap) {
            const double distance = tap - (length - 1) * 0.5;
            const double argument = TwoPi * cutoff * distance;
            const double sinc = std::abs(argument) < 1e-12 ? 1.0 : std::sin(argument) / argument;
            const double position = static_cast<double>(tap) / (length - 1);
            const double window = 0.42 - 0.5 * std::cos(TwoPi * position) +
                                  0.08 * std::cos(2.0 * TwoPi * position);
            const double coefficient = 2.0 * cutoff * sinc * window;
            next->coefficients[static_cast<std::size_t>(tap)] = static_cast<float>(coefficient);
            sum += coefficient;
        }
        for (float &coefficient : next->coefficients) coefficient *= static_cast<float>(1.0 / sum);
        next->fftIn = fftwf_alloc_complex(config.channels);
        next->fftOut = fftwf_alloc_complex(config.channels);
        if (!next->fftIn || !next->fftOut) return false;
        {
            std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
            next->plan = fftwf_plan_dft_1d(config.channels, next->fftIn, next->fftOut,
                                           FFTW_FORWARD, FFTW_ESTIMATE);
        }
        if (!next->plan) return false;
        impl.swap(next);
        return true;
    } catch (const std::bad_alloc &) {
        return false;
    }
}

void PfbChannelizer::reset() {
    if (!impl) return;
    std::fill(impl->history.begin(), impl->history.end(), std::complex<float>(0.0f, 0.0f));
    impl->writeIndex = 0;
    impl->pending = 0;
    impl->samplesSeen = 0;
}

bool PfbChannelizer::isConfigured() const { return bool(impl); }
double PfbChannelizer::outputRateHz() const {
    return impl ? impl->config.sampleRateHz / impl->config.channels : 0.0;
}
double PfbChannelizer::binCenterHz(int binIndex) const {
    if (!impl || binIndex < 0 || binIndex >= impl->config.channels)
        return std::numeric_limits<double>::quiet_NaN();
    const int signedBin = binIndex < impl->config.channels / 2 ? binIndex : binIndex - impl->config.channels;
    return signedBin * outputRateHz();
}
int PfbChannelizer::nearestCenterIndex(double offsetHz) const {
    if (!impl || !std::isfinite(offsetHz) || offsetHz < -impl->config.sampleRateHz * 0.5 ||
        offsetHz >= impl->config.sampleRateHz * 0.5) return -1;
    const int signedBin = static_cast<int>(std::round(offsetHz / outputRateHz()));
    return (signedBin + impl->config.channels) % impl->config.channels;
}
double PfbChannelizer::usablePassbandHz() const {
    return impl ? outputRateHz() * (1.0 - 6.0 / impl->config.tapsPerPhase) : 0.0;
}
bool PfbChannelizer::coversBand(int binIndex, double centerOffsetHz, double bandwidthHz) const {
    const double center = binCenterHz(binIndex);
    if (!std::isfinite(center) || !std::isfinite(centerOffsetHz) ||
        !std::isfinite(bandwidthHz) || bandwidthHz <= 0.0) return false;
    const double halfWidth = bandwidthHz * 0.5;
    const double inputHalfWidth = impl->config.sampleRateHz * 0.5;
    return centerOffsetHz - halfWidth >= -inputHalfWidth &&
           centerOffsetHz + halfWidth <= inputHalfWidth &&
           std::abs(centerOffsetHz - center) + halfWidth <= usablePassbandHz() * 0.5;
}
double PfbChannelizer::delayInputSamples() const {
    return impl ? (impl->config.channels * impl->config.tapsPerPhase - 1) * 0.5 : 0.0;
}

PfbChannelizer::Result PfbChannelizer::processFloatIq(
    const float *samples, std::size_t floatCount, const std::vector<int> &selectedBins,
    std::vector<ChannelOutput> &outputs, const std::atomic_bool *cancel) {
    Result result;
    for (auto &output : outputs) output.iq.clear();
    const auto fail = [&](Status status) {
        outputs.clear();
        result.status = status;
        return result;
    };
    if (!impl) return fail(Status::NotConfigured);
    if ((floatCount != 0 && !samples) || (floatCount & 1U)) return fail(Status::InvalidInput);
    const std::size_t count = floatCount / 2;
    if (count > MaxInputSamples || count > (std::numeric_limits<std::uint64_t>::max)() - impl->samplesSeen)
        return fail(Status::ResourceLimit);
    if (selectedBins.size() > MaxSelectedBins) return fail(Status::InvalidSelection);
    std::uint64_t selectedMask = 0;
    for (int bin : selectedBins) {
        if (bin < 0 || bin >= impl->config.channels) return fail(Status::InvalidSelection);
        const std::uint64_t bit = std::uint64_t(1) << bin;
        if (selectedMask & bit) return fail(Status::InvalidSelection);
        selectedMask |= bit;
    }
    if (cancel && cancel->load(std::memory_order_relaxed)) return fail(Status::Cancelled);
    const std::size_t frames = (count + static_cast<std::size_t>(impl->pending)) / impl->config.channels;
    try {
        outputs.resize(selectedBins.size());
        for (std::size_t i = 0; i < selectedBins.size(); ++i) {
            outputs[i].binIndex = selectedBins[i];
            outputs[i].centerOffsetHz = binCenterHz(selectedBins[i]);
            outputs[i].sampleRateHz = outputRateHz();
            outputs[i].iq.reserve(frames * 2);
        }
    } catch (const std::bad_alloc &) {
        return fail(Status::AllocationFailure);
    }
    result.status = Status::Ok;
    for (std::size_t n = 0; n < count; ++n) {
        if ((n & 255U) == 0 && cancel && cancel->load(std::memory_order_relaxed)) {
            result.status = Status::Cancelled;
            break;
        }
        const float re = samples[2 * n], im = samples[2 * n + 1];
        impl->history[impl->writeIndex] = {std::isfinite(re) ? re : 0.0f,
                                         std::isfinite(im) ? im : 0.0f};
        impl->writeIndex = (impl->writeIndex + 1) % impl->history.size();
        ++impl->samplesSeen;
        ++result.inputSamplesConsumed;
        if (++impl->pending != impl->config.channels) continue;
        impl->pending = 0;
        if (selectedBins.empty()) continue;
        impl->transform();
        if (result.outputSamplesPerChannel == 0) result.firstOutputInputSample = impl->samplesSeen - 1;
        ++result.outputSamplesPerChannel;
        for (auto &output : outputs) {
            output.iq.push_back(impl->fftOut[output.binIndex][0]);
            output.iq.push_back(impl->fftOut[output.binIndex][1]);
        }
    }
    return result;
}
