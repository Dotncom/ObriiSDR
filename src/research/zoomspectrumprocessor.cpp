#include "zoomspectrumprocessor.h"
#include "fft.h"

#include <fftw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
constexpr double TwoPi = 6.28318530717958647692;
constexpr int MinimumFftLength = 512;
constexpr int MaximumFftLength = 2097152;
constexpr double MinimumChannelRateHz = 2000.0;
constexpr double MaximumChannelRateHz = 2000000.0;
constexpr std::size_t MaximumQueuedFrames = 32;

int requestedFftLength(double sampleRate, double binWidth) {
    if (!(sampleRate > 0.0) || !(binWidth > 0.0)) {
        return MinimumFftLength;
    }
    const long long requested = std::llround(sampleRate / binWidth);
    return static_cast<int>(std::clamp<long long>(requested,
                                                  MinimumFftLength,
                                                  MaximumFftLength));
}
}

ZoomSpectrumProcessor::ZoomSpectrumProcessor()
    : workerThread(&ZoomSpectrumProcessor::run, this) {
}

ZoomSpectrumProcessor::~ZoomSpectrumProcessor() {
    {
        std::lock_guard<std::mutex> lock(sampleMutex);
        stopping = true;
    }
    sampleCondition.notify_all();
    if (workerThread.joinable()) {
        workerThread.join();
    }
}

void ZoomSpectrumProcessor::configure(double lowHz,
                                      double highHz,
                                      double requestedBinWidthHz,
                                      int updateIntervalMs,
                                      bool enabled) {
    if (highHz < lowHz) {
        std::swap(lowHz, highHz);
    }
    const bool valid = std::isfinite(lowHz) && std::isfinite(highHz) &&
                       highHz > lowHz && std::isfinite(requestedBinWidthHz) &&
                       requestedBinWidthHz > 0.0;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        config.lowHz = lowHz;
        config.highHz = highHz;
        config.requestedBinWidthHz = std::clamp(requestedBinWidthHz, 0.01, 100000.0);
        config.updateIntervalMs = std::clamp(updateIntervalMs, 1, 5000);
        config.enabled = enabled && valid;
        ++config.generation;
        enabledFlag.store(config.enabled, std::memory_order_release);
    }
    sampleCondition.notify_all();
}

void ZoomSpectrumProcessor::setEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(configMutex);
        config.enabled = enabled && config.highHz > config.lowHz;
        ++config.generation;
        enabledFlag.store(config.enabled, std::memory_order_release);
    }
    sampleCondition.notify_all();
}

ZoomSpectrumProcessor::Configuration ZoomSpectrumProcessor::configuration() const {
    std::lock_guard<std::mutex> lock(configMutex);
    return config;
}

void ZoomSpectrumProcessor::resetDspState(std::uint64_t generation,
                                          double inputSampleRateHz,
                                          double inputCenterHz,
                                          int decimation,
                                          double channelSampleRateHz,
                                          int fftLength) {
    activeGeneration = generation;
    activeInputSampleRateHz = inputSampleRateHz;
    activeInputCenterHz = inputCenterHz;
    activeDecimation = decimation;
    activeChannelSampleRateHz = channelSampleRateHz;
    activeFftLength = fftLength;
    oscillator = {1.0f, 0.0f};
    lowPassState1 = {0.0f, 0.0f};
    lowPassState2 = {0.0f, 0.0f};
    decimationSum = {0.0, 0.0};
    decimationCount = 0;
    oscillatorSamples = 0;

    std::lock_guard<std::mutex> lock(sampleMutex);
    channelSamples.clear();
    totalChannelSamples = 0;
    lastFrameEndSample = 0;
    sampleCapacity = static_cast<std::size_t>(std::clamp<long long>(
        std::max<long long>(static_cast<long long>(fftLength) * 2LL,
                            static_cast<long long>(std::ceil(channelSampleRateHz * 3.0))),
        MinimumFftLength * 2LL,
        static_cast<long long>(MaximumFftLength) * 2LL));
}

void ZoomSpectrumProcessor::consumeIq(const float *interleavedIq,
                                      std::uint32_t complexSamples,
                                      double inputSampleRateHz,
                                      double inputCenterHz) {
    if (!enabledFlag.load(std::memory_order_acquire) || !interleavedIq ||
        complexSamples == 0 || !(inputSampleRateHz > 0.0)) {
        return;
    }
    const Configuration current = configuration();
    if (!current.enabled) {
        return;
    }
    const double selectedCenterHz = (current.lowHz + current.highHz) * 0.5;
    const double selectedSpanHz = current.highHz - current.lowHz;
    const double inputHalfSpanHz = inputSampleRateHz * 0.5;
    if (selectedCenterHz < inputCenterHz - inputHalfSpanHz ||
        selectedCenterHz > inputCenterHz + inputHalfSpanHz) {
        return;
    }

    const double targetChannelRateHz = std::clamp(selectedSpanHz * 2.5,
                                                   MinimumChannelRateHz,
                                                   std::min(MaximumChannelRateHz,
                                                            inputSampleRateHz));
    const int decimation = std::max(1, static_cast<int>(std::floor(
                                           inputSampleRateHz / targetChannelRateHz)));
    const double channelSampleRateHz = inputSampleRateHz / decimation;
    const int fftLength = requestedFftLength(channelSampleRateHz,
                                             current.requestedBinWidthHz);
    if (activeGeneration != current.generation ||
        activeInputSampleRateHz != inputSampleRateHz ||
        activeInputCenterHz != inputCenterHz ||
        activeDecimation != decimation || activeFftLength != fftLength) {
        resetDspState(current.generation,
                      inputSampleRateHz,
                      inputCenterHz,
                      decimation,
                      channelSampleRateHz,
                      fftLength);
    }

    const double offsetHz = selectedCenterHz - inputCenterHz;
    const double phaseStep = -TwoPi * offsetHz / inputSampleRateHz;
    const std::complex<float> oscillatorStep(static_cast<float>(std::cos(phaseStep)),
                                              static_cast<float>(std::sin(phaseStep)));
    const double cutoffHz = std::min(selectedSpanHz * 0.65,
                                     channelSampleRateHz * 0.42);
    const float alpha = static_cast<float>(std::clamp(TwoPi * cutoffHz / inputSampleRateHz,
                                                       0.000001,
                                                       1.0));
    std::vector<std::complex<float>> produced;
    produced.reserve(static_cast<std::size_t>(complexSamples / decimation + 2));
    for (std::uint32_t index = 0; index < complexSamples; ++index) {
        const std::complex<float> input(interleavedIq[index * 2],
                                        interleavedIq[index * 2 + 1]);
        const std::complex<float> mixed = input * oscillator;
        oscillator *= oscillatorStep;
        if ((++oscillatorSamples & 4095ULL) == 0ULL) {
            const float magnitude = std::abs(oscillator);
            if (magnitude > 0.0f) {
                oscillator /= magnitude;
            }
        }

        lowPassState1 += alpha * (mixed - lowPassState1);
        lowPassState2 += alpha * (lowPassState1 - lowPassState2);
        decimationSum += std::complex<double>(lowPassState2.real(), lowPassState2.imag());
        if (++decimationCount >= decimation) {
            produced.emplace_back(static_cast<float>(decimationSum.real() / decimationCount),
                                  static_cast<float>(decimationSum.imag() / decimationCount));
            decimationSum = {0.0, 0.0};
            decimationCount = 0;
        }
    }

    if (produced.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sampleMutex);
        for (const std::complex<float> sample : produced) {
            channelSamples.push_back(sample);
        }
        totalChannelSamples += produced.size();
        while (channelSamples.size() > sampleCapacity) {
            channelSamples.pop_front();
        }
    }
    sampleCondition.notify_one();
}

bool ZoomSpectrumProcessor::takeFrames(std::vector<ZoomSpectrumFrame> &frames) {
    std::lock_guard<std::mutex> lock(frameMutex);
    if (readyFrames.empty()) {
        return false;
    }
    frames.reserve(frames.size() + readyFrames.size());
    while (!readyFrames.empty()) {
        frames.push_back(std::move(readyFrames.front()));
        readyFrames.pop_front();
    }
    return true;
}

void ZoomSpectrumProcessor::run() {
    fftwf_complex *fftInput = nullptr;
    fftwf_complex *fftOutput = nullptr;
    fftwf_plan plan = nullptr;
    int plannedLength = 0;
    std::vector<std::complex<float>> snapshot;

    while (true) {
        int fftLength = 0;
        double channelRate = 0.0;
        int decimation = 1;
        Configuration activeConfig;
        {
            std::unique_lock<std::mutex> lock(sampleMutex);
            sampleCondition.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                if (stopping) {
                    return true;
                }
                const Configuration current = configuration();
                if (!current.enabled || activeGeneration != current.generation ||
                    activeFftLength <= 0 || channelSamples.size() < static_cast<std::size_t>(activeFftLength)) {
                    return false;
                }
                const std::uint64_t hop = static_cast<std::uint64_t>(std::max(
                    1.0, activeChannelSampleRateHz * current.updateIntervalMs / 1000.0));
                return lastFrameEndSample == 0 || totalChannelSamples >= lastFrameEndSample + hop;
            });
            if (stopping) {
                break;
            }
            activeConfig = configuration();
            if (!activeConfig.enabled || activeGeneration != activeConfig.generation ||
                activeFftLength <= 0 || channelSamples.size() < static_cast<std::size_t>(activeFftLength)) {
                continue;
            }
            const std::uint64_t hop = static_cast<std::uint64_t>(std::max(
                1.0, activeChannelSampleRateHz * activeConfig.updateIntervalMs / 1000.0));
            if (lastFrameEndSample != 0 && totalChannelSamples < lastFrameEndSample + hop) {
                continue;
            }
            fftLength = activeFftLength;
            channelRate = activeChannelSampleRateHz;
            decimation = activeDecimation;
            snapshot.resize(static_cast<std::size_t>(fftLength));
            const std::size_t start = channelSamples.size() - static_cast<std::size_t>(fftLength);
            for (int index = 0; index < fftLength; ++index) {
                snapshot[static_cast<std::size_t>(index)] =
                    channelSamples[start + static_cast<std::size_t>(index)];
            }
            lastFrameEndSample = totalChannelSamples;
        }

        if (plannedLength != fftLength) {
            if (plan) {
                std::lock_guard<std::mutex> plannerLock(fftwPlannerGlobalMutex());
                fftwf_destroy_plan(plan);
            }
            if (fftInput) fftwf_free(fftInput);
            if (fftOutput) fftwf_free(fftOutput);
            plan = nullptr;
            fftInput = nullptr;
            fftOutput = nullptr;
            fftInput = static_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) * fftLength));
            fftOutput = static_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) * fftLength));
            if (!fftInput || !fftOutput) {
                if (fftInput) fftwf_free(fftInput);
                if (fftOutput) fftwf_free(fftOutput);
                fftInput = nullptr;
                fftOutput = nullptr;
                plannedLength = 0;
                continue;
            }
            {
                std::lock_guard<std::mutex> plannerLock(fftwPlannerGlobalMutex());
                plan = fftwf_plan_dft_1d(fftLength,
                                         fftInput,
                                         fftOutput,
                                         FFTW_FORWARD,
                                         FFTW_ESTIMATE);
            }
            if (!plan) {
                plannedLength = 0;
                continue;
            }
            plannedLength = fftLength;
        }

        double windowSum = 0.0;
        for (int index = 0; index < fftLength; ++index) {
            const float window = fftLength > 1
                                     ? static_cast<float>(0.5 - 0.5 * std::cos(
                                           TwoPi * index / static_cast<double>(fftLength - 1)))
                                     : 1.0f;
            windowSum += window;
            fftInput[index][0] = snapshot[static_cast<std::size_t>(index)].real() * window;
            fftInput[index][1] = snapshot[static_cast<std::size_t>(index)].imag() * window;
        }
        fftwf_execute(plan);

        const Configuration completedConfig = configuration();
        if (!completedConfig.enabled || completedConfig.generation != activeConfig.generation ||
            !(channelRate > 0.0)) {
            continue;
        }
        const double selectedCenterHz = (completedConfig.lowHz + completedConfig.highHz) * 0.5;
        const double binWidthHz = channelRate / fftLength;
        ZoomSpectrumFrame frame;
        frame.selectedLowHz = completedConfig.lowHz;
        frame.selectedHighHz = completedConfig.highHz;
        frame.channelSampleRateHz = channelRate;
        frame.binWidthHz = binWidthHz;
        frame.fftLength = fftLength;
        frame.decimation = decimation;
        frame.sequence = ++nextFrameSequence;
        const double normalization = std::max(1.0, windowSum);
        for (int ordered = 0; ordered < fftLength; ++ordered) {
            const double frequency = selectedCenterHz +
                                     (ordered - fftLength / 2) * binWidthHz;
            if (frequency < completedConfig.lowHz || frequency > completedConfig.highHz) {
                continue;
            }
            const int raw = (ordered + fftLength / 2) % fftLength;
            const double magnitude = std::hypot(fftOutput[raw][0], fftOutput[raw][1]) /
                                     normalization;
            frame.frequencies.push_back(static_cast<float>(frequency));
            frame.levels.push_back(static_cast<float>(20.0 * std::log10(
                std::max(magnitude, 1.0e-12))));
        }
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            readyFrames.push_back(std::move(frame));
            while (readyFrames.size() > MaximumQueuedFrames) {
                readyFrames.pop_front();
            }
        }
    }

    if (plan) {
        std::lock_guard<std::mutex> plannerLock(fftwPlannerGlobalMutex());
        fftwf_destroy_plan(plan);
    }
    if (fftInput) fftwf_free(fftInput);
    if (fftOutput) fftwf_free(fftOutput);
}
