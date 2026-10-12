#include "dspbranchengine.h"
#include "iqbuffer.h"
#include "iqchannelizer.h"
#include "fftplannermutex.h"

#include <fftw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t MaxInputFloats = 2U * 1024U * 1024U;
constexpr std::size_t MaxChannelFloats = 2U * 65536U;

double coefficient(int index, int n, int window) {
    const double p = 6.28318530717958647692 * index / (n - 1);
    switch (window) {
    case FFT_WINDOW_HANN: return 0.5 - 0.5 * std::cos(p);
    case FFT_WINDOW_HAMMING: return 0.54 - 0.46 * std::cos(p);
    case FFT_WINDOW_BLACKMAN_HARRIS: return 0.35875 - 0.48829 * std::cos(p) + 0.14128 * std::cos(2*p) - 0.01168 * std::cos(3*p);
    case FFT_WINDOW_FLAT_TOP: return 0.21557895 - 0.41663158 * std::cos(p) + 0.277263158 * std::cos(2*p) - 0.083578947 * std::cos(3*p) + 0.006947368 * std::cos(4*p);
    default: return 1.0;
    }
}

struct BranchFft {
    DspBranchFftConfig config;
    fftwf_complex *input = nullptr, *output = nullptr;
    fftwf_plan plan = nullptr;
    std::vector<float> window;
    double windowSum = 0.0;
    Clock::time_point lastFrame{};
    explicit BranchFft(const DspBranchFftConfig &f) : config(f) {
        input = fftwf_alloc_complex(f.length);
        output = fftwf_alloc_complex(f.length);
        if (input && output) {
            std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
            plan = fftwf_plan_dft_1d(f.length, input, output, FFTW_FORWARD, FFTW_ESTIMATE);
        }
        window.resize(f.length);
        for (int i = 0; i < f.length; ++i) {
            window[i] = static_cast<float>(coefficient(i, f.length, f.window));
            windowSum += window[i];
        }
    }
    ~BranchFft() {
        if (plan) { std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex()); fftwf_destroy_plan(plan); }
        if (input) fftwf_free(input);
        if (output) fftwf_free(output);
    }
    bool compute(const std::deque<float> &history, DspBranchFrame &frame) {
        if (!plan || history.size() < std::size_t(config.length)*2U) return false;
        const auto now = Clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now-lastFrame).count() < config.refreshMs) return false;
        lastFrame = now;
        const std::size_t start = history.size() - std::size_t(config.length)*2U;
        for (int i = 0; i < config.length; ++i) {
            input[i][0] = history[start+2U*i] * window[i];
            input[i][1] = history[start+2U*i+1U] * window[i];
        }
        fftwf_execute(plan);
        frame.id = config.id;
        frame.binWidthHz = frame.sampleRateHz / config.length;
        frame.rbwHz = frame.binWidthHz * fftWindowEnbwBins(config.window);
        frame.frequencies.resize(config.length);
        frame.levels.resize(config.length);
        for (int i = 0; i < config.length; ++i) {
            const int j = (i + config.length/2) % config.length;
            const double amplitude = std::hypot(double(output[j][0]), double(output[j][1])) / windowSum;
            frame.levels[i] = static_cast<float>(20.0 * std::log10(std::max(amplitude, 1.0e-10)));
            frame.frequencies[i] = static_cast<float>(frame.centerHz + (i-config.length/2)*frame.binWidthHz);
        }
        return true;
    }
};

struct BranchState {
    DspBranchConfig config;
    IqChannelizer channelizer;
    std::vector<float> channel;
    std::deque<float> history;
    std::vector<std::unique_ptr<BranchFft>> ffts;
    Clock::time_point lastIqFrame{};
    explicit BranchState(const DspBranchConfig &c) : config(c) {
        channelizer.reset();
        for (const auto &f : c.ffts) ffts.push_back(std::make_unique<BranchFft>(f));
    }
    void reset() { channelizer.reset(); history.clear(); }
};
}

struct DspBranchEngine::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool stopping = false, active = false;
    std::uint64_t generation = 0;
    QByteArray key;
    QVector<DspBranchConfig> configurations;
    RadioSettings source;
    std::map<QString, DspBranchFrame> ready;

    Impl() : worker([this]() { run(); }) {}
    ~Impl() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        wake.notify_one();
        worker.join();
    }
    bool current(std::uint64_t value) {
        std::lock_guard<std::mutex> lock(mutex);
        return !stopping && active && generation == value;
    }
    void publish(DspBranchFrame frame, std::uint64_t value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stopping && active && generation == value) ready[frame.id] = std::move(frame);
    }
    void run() {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
        setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
        std::vector<std::unique_ptr<BranchState>> branches;
        std::vector<float> raw;
        std::uint64_t revision = 0, epoch = 0, lastEnd = 0, gaps = 0, lossCount = 0;
        for (;;) {
            RadioSettings settings;
            {
                std::unique_lock<std::mutex> lock(mutex);
                const auto changed = [this, revision]() { return stopping || generation != revision; };
                if (!active && generation == revision) wake.wait(lock, changed);
                else wake.wait_for(lock, std::chrono::milliseconds(2), changed);
                if (stopping) return;
                if (generation != revision) {
                    revision = generation;
                    const auto configs = configurations;
                    const bool running = active;
                    settings = source;
                    lock.unlock();
                    branches.clear();
                    try {
                        if (running) for (const auto &c : configs) if (c.enabled) branches.push_back(std::make_unique<BranchState>(c));
                    } catch (const std::exception &) {
                        branches.clear();
                        for (const auto &c : configs) { DspBranchFrame f; f.id = c.id; f.error = "Cannot allocate DDC branch"; publish(std::move(f), revision); }
                    }
                    epoch = 0; lastEnd = 0; gaps = 0; lossCount = 0;
                } else {
                    settings = source;
                    if (!active) continue;
                }
            }
            if (branches.empty()) continue;
            try {
                const auto stats = IqBuffer::stats();
                if (stats.totalFloatCount == lastEnd && stats.epoch == epoch) continue;
                const std::size_t available = std::min(stats.snapshotSize, MaxInputFloats);
                if (available < 2) continue;
                const bool newEpoch = stats.epoch != epoch;
                const auto snapshotLoss = IqBuffer::discontinuityStats().skippedSnapshotBlocks;
                const bool lost = !newEpoch && (stats.totalFloatCount < lastEnd ||
                    stats.totalFloatCount-lastEnd > available || snapshotLoss != lossCount);
                const std::size_t count = newEpoch || lost ? available :
                    static_cast<std::size_t>(stats.totalFloatCount-lastEnd);
                IqBuffer::BlockMetadata metadata;
                if (!IqBuffer::snapshotRecentEndingAt(raw, count, stats.totalFloatCount, nullptr, &metadata) ||
                    raw.size() != count || metadata.epoch != stats.epoch) continue;
                if (!current(revision)) continue;
                if (newEpoch || lost) {
                    for (auto &b : branches) b->reset();
                    if (lost) ++gaps;
                }
                if (newEpoch && metadata.floatCount > 0 && raw.size() > metadata.floatCount)
                    raw.erase(raw.begin(), raw.end()-static_cast<std::ptrdiff_t>(metadata.floatCount));
                epoch = stats.epoch; lastEnd = stats.totalFloatCount; lossCount = snapshotLoss;
                for (auto &b : branches) {
                    if (!current(revision)) break;
                    const auto started = Clock::now();
                    DspBranchFrame frame;
                    frame.id = b->config.id; frame.sequence = lastEnd / 2U;
                    frame.generation = revision; frame.gaps = gaps;
                    frame.centerHz = b->config.frequencyHz > 0.0 ? b->config.frequencyHz : settings.listeningFrequency;
                    frame.sampleRateHz = b->config.outputRateHz;
                    const double halfBand = b->config.bandwidthHz / 2.0;
                    if (!std::isfinite(settings.sampleRate) || settings.sampleRate <= 0.0 ||
                        settings.inputMode == INPUT_HF_NOISE_CANCEL ||
                        (metadata.valid && (metadata.tuning || std::abs(metadata.centerFrequencyHz-settings.centerFrequency) > 1.0)) ||
                        std::abs(frame.centerHz-settings.centerFrequency)+halfBand > settings.sampleRate*0.5 ||
                        halfBand >= frame.sampleRateHz*0.45) {
                        frame.error = "Invalid DDC band/rate or source transition";
                        b->reset(); publish(std::move(frame), revision); continue;
                    }
                    RadioSettings branchSettings = settings;
                    branchSettings.listeningFrequency = frame.centerHz;
                    IqChannelizer::Options options;
                    options.outputRateHz = frame.sampleRateHz;
                    options.cutoffHz = halfBand;
                    options.decimationFactor = b->config.decimation;
                    const auto result = b->channelizer.processFloatIq(raw.data(), raw.size(), branchSettings, b->channel, false, &options);
                    if (!result.valid) {
                        frame.error = "Invalid DDC decimation or output sample rate";
                        b->reset(); publish(std::move(frame), revision); continue;
                    }
                    frame.sampleRateHz = result.outputRate; frame.decimation = result.decimationFactor;
                    frame.frontDecimation = result.frontDecimationFactor;
                    const std::size_t first = b->channel.size() > MaxChannelFloats ? b->channel.size()-MaxChannelFloats : 0;
                    for (std::size_t i = first; i < b->channel.size(); ++i) b->history.push_back(b->channel[i]);
                    while (b->history.size() > MaxChannelFloats) b->history.pop_front();
                    const bool publishIq = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-b->lastIqFrame).count() >= 50;
                    if (b->config.iqConsumer && publishIq) {
                        const auto count = std::min(b->history.size(), std::size_t(b->config.iqSamples)*2U);
                        frame.iq = std::make_shared<const std::vector<float>>(b->history.end()-static_cast<std::ptrdiff_t>(count), b->history.end());
                    }
                    frame.processingMs = std::chrono::duration<double, std::milli>(Clock::now()-started).count();
                    if (publishIq) { publish(frame, revision); b->lastIqFrame = Clock::now(); }
                    for (auto &fft : b->ffts) {
                        auto spectrum = frame;
                        spectrum.iq.reset();
                        if (fft->compute(b->history, spectrum)) {
                            spectrum.processingMs = std::chrono::duration<double, std::milli>(Clock::now()-started).count();
                            publish(std::move(spectrum), revision);
                        } else if (!fft->plan) {
                            spectrum.id = fft->config.id; spectrum.error = "Cannot allocate branch FFT";
                            publish(std::move(spectrum), revision);
                        }
                    }
                }
            } catch (const std::exception &) {
                for (auto &b : branches) { b->reset(); DspBranchFrame f; f.id = b->config.id; f.error = "DDC branch resource limit"; publish(std::move(f), revision); }
            }
        }
    }
};

DspBranchEngine::DspBranchEngine() : impl(std::make_unique<Impl>()) {}
DspBranchEngine::~DspBranchEngine() = default;

void DspBranchEngine::configure(const DspGraphPlan &plan, const RadioSettings &source, bool active) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    const auto &old = impl->source;
    if (impl->key == plan.executionKey && impl->active == active && old.sampleRate == source.sampleRate &&
        old.centerFrequency == source.centerFrequency && old.listeningFrequency == source.listeningFrequency && old.inputMode == source.inputMode) return;
    impl->key = plan.executionKey; impl->source = source; impl->active = active;
    impl->configurations = plan.branches; ++impl->generation; impl->ready.clear();
    impl->wake.notify_one();
}

std::vector<DspBranchFrame> DspBranchEngine::takeFrames() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    std::vector<DspBranchFrame> frames;
    for (auto &entry : impl->ready) frames.push_back(std::move(entry.second));
    impl->ready.clear();
    return frames;
}
