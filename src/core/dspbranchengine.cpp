#include "dspbranchengine.h"
#include "iqbuffer.h"
#include "iqchannelizer.h"
#include "dspstreammanager.h"
#include "dspstreamrecorder.h"

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

struct BranchState {
    DspBranchConfig config;
    IqChannelizer channelizer;
    std::vector<float> channel;
    std::deque<float> history;
    DspStreamBlock stream;
    bool gap = true;
    std::uint64_t nextSample = 0;
    std::uint64_t transportGaps = 0;
    Clock::time_point lastIqFrame{};
    explicit BranchState(const DspBranchConfig &c) : config(c) {
        channelizer.reset();
    }
    void reset() { channelizer.reset(); history.clear(); gap = true; nextSample = 0; }
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
    DspStreamEndpoint endpoint;
    QHash<QString, QString> recordingRequests;
    std::map<QString, DspBranchFrame> ready;

    // Start only after all state (including endpoint and recording maps) exists.
    Impl() { worker = std::thread([this]() { run(); }); }
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
        DspStreamManager streams;
        std::map<QString, std::unique_ptr<DspStreamRecorder>> recorders;
        QHash<QString, QString> recordingDirectories;
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
                    const auto remote = endpoint;
                    const auto requestedRecorders = recordingRequests;
                    lock.unlock();
                    branches.clear();
                    streams.clear(); streams.configure(remote);
                    for (auto it = recorders.begin(); it != recorders.end();) {
                        if (!running || !requestedRecorders.contains(it->first) || recordingDirectories.value(it->first) != requestedRecorders.value(it->first)) {
                            recordingDirectories.remove(it->first); it = recorders.erase(it);
                        } else ++it;
                    }
                    try {
                        if (running) for (auto it = requestedRecorders.cbegin(); it != requestedRecorders.cend(); ++it) {
                            if (recorders.count(it.key())) continue;
                            auto recorder = std::make_unique<DspStreamRecorder>(); recorder->start(it.value());
                            recordingDirectories.insert(it.key(), it.value()); recorders[it.key()] = std::move(recorder);
                        }
                        if (running) for (const auto &c : configs) if (c.enabled) branches.push_back(std::make_unique<BranchState>(c));
                    } catch (const std::exception &) {
                        branches.clear(); recorders.clear(); recordingDirectories.clear();
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
                    frame.generation = revision; frame.gaps = gaps+b->transportGaps;
                    const auto recording = recorders.find(b->config.id);
                    DspStreamRecorder *recorder = recording == recorders.end() ? nullptr : recording->second.get();
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
                    bool failed = false;
                    for (std::size_t offset = 0; offset < b->channel.size(); offset += DspStreamProtocol::MaxSamples*2U) {
                        if (!current(revision)) { failed = true; break; }
                        auto &block = b->stream;
                        block.stream = b->config.id; block.generation = revision; block.epoch = epoch;
                        block.firstSample = b->nextSample; block.gap = b->gap;
                        block.sampleRate = frame.sampleRateHz; block.centerHz = frame.centerHz;
                        block.returnIq = b->config.iqConsumer || (recorder && recorder->active()); block.ffts = b->config.ffts;
                        const auto end = std::min(b->channel.size(), offset+DspStreamProtocol::MaxSamples*2U);
                        block.iq.assign(b->channel.begin()+std::ptrdiff_t(offset), b->channel.begin()+std::ptrdiff_t(end));
                        auto output = streams.process(block, b->config.external);
                        b->nextSample += block.iq.size()/2U;
                        if (!output.error.isEmpty()) {
                            if (!b->gap) ++b->transportGaps;
                            frame.error = output.error; b->gap = true; b->history.clear();
                            publish(frame, revision); failed = true; break;
                        }
                        b->gap = false;
                        if (recorder && recorder->active() && output.iq) recorder->append(block, *output.iq);
                        if (output.iq && b->config.iqConsumer) {
                            b->history.insert(b->history.end(), output.iq->begin(), output.iq->end());
                            while (b->history.size() > MaxChannelFloats) b->history.pop_front();
                        }
                        for (auto &spectrum : output.spectra) {
                            spectrum.decimation = frame.decimation; spectrum.frontDecimation = frame.frontDecimation;
                            spectrum.gaps = gaps+b->transportGaps; spectrum.processingMs = std::chrono::duration<double, std::milli>(Clock::now()-started).count();
                            publish(std::move(spectrum), revision);
                        }
                    }
                    if (failed) continue;
                    const bool publishIq = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-b->lastIqFrame).count() >= 50;
                    if (b->config.iqConsumer && publishIq) {
                        const auto count = std::min(b->history.size(), std::size_t(b->config.iqSamples)*2U);
                        frame.iq = std::make_shared<const std::vector<float>>(b->history.end()-static_cast<std::ptrdiff_t>(count), b->history.end());
                    }
                    frame.processingMs = std::chrono::duration<double, std::milli>(Clock::now()-started).count();
                    if (recorder) frame.recordingStatus = recorder->status();
                    if (publishIq) { publish(frame, revision); b->lastIqFrame = Clock::now(); }
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
    active = active && !source.simplifiedAudioChannelizer;
    std::lock_guard<std::mutex> lock(impl->mutex);
    const auto &old = impl->source;
    if (impl->key == plan.executionKey && impl->active == active && old.sampleRate == source.sampleRate &&
        old.centerFrequency == source.centerFrequency && old.listeningFrequency == source.listeningFrequency && old.inputMode == source.inputMode) return;
    impl->key = plan.executionKey; impl->source = source; impl->active = active;
    impl->configurations = plan.branches; ++impl->generation; impl->ready.clear();
    impl->wake.notify_one();
}

void DspBranchEngine::setExternalEndpoint(const DspStreamEndpoint &endpoint) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->endpoint == endpoint) return;
    impl->endpoint = endpoint; ++impl->generation; impl->ready.clear(); impl->wake.notify_one();
}

void DspBranchEngine::setRecording(const QString &branchId, const QString &directory) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (directory.isEmpty()) impl->recordingRequests.remove(branchId);
    else if (impl->recordingRequests.size() < 2 || impl->recordingRequests.contains(branchId)) impl->recordingRequests.insert(branchId, directory);
    ++impl->generation; impl->ready.clear(); impl->wake.notify_one();
}

std::vector<DspBranchFrame> DspBranchEngine::takeFrames() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    std::vector<DspBranchFrame> frames;
    for (auto &entry : impl->ready) frames.push_back(std::move(entry.second));
    impl->ready.clear();
    return frames;
}
