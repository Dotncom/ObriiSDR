#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "dspbranchengine.h"
#include "iqbuffer.h"
#include "iqchannelizer.h"
#include "dspstreammanager.h"
#include "dspstreamrecorder.h"
#include "audioprocessor.h"
#include "dspbranchaudiooutput.h"
#include "pfbchannelizer.h"

#include <algorithm>
#include <atomic>
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

struct BranchOutputState {
    std::deque<float> history;
    bool gap = true;
    std::uint64_t transportGaps = 0;
    Clock::time_point lastIqFrame{};
    std::unique_ptr<AudioProcessor> demodulator;
    std::vector<short> pcm;
    double audioRate = 0.0;
    double squelchHoldSeconds = 0.0;
    void reset() { history.clear(); gap = true; audioRate = 0; squelchHoldSeconds = 0; }
};
struct BranchState : BranchOutputState {
    DspBranchConfig config;
    IqChannelizer channelizer;
    std::vector<float> channel;
    DspStreamBlock stream;
    std::uint64_t nextSample = 0;
    explicit BranchState(const DspBranchConfig &c) : config(c) {
        channelizer.reset();
    }
    void reset() { channelizer.reset(); BranchOutputState::reset(); nextSample = 0; }
};
struct RemoteBranchJob {
    DspBranchConfig config;
    DspStreamBlock block;
    DspBranchFrame frame;
    std::uint64_t revision = 0;
    Clock::time_point started;
};
}

struct DspBranchEngine::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::thread remoteWorker;
    std::condition_variable remoteWake;
    std::atomic_bool remoteCancel{false};
    std::uint64_t remoteRevision = 0, droppedRemoteBlocks = 0;
    std::deque<RemoteBranchJob> remoteJobs;
    QHash<QString, std::uint64_t> remoteDrops;
    bool stopping = false, active = false;
    std::uint64_t generation = 0;
    QByteArray key;
    QVector<DspBranchConfig> configurations;
    int pfbChannels = 0, pfbTaps = 16;
    AudioConsumer audioConsumer;
    RadioSettings source;
    DspStreamEndpoint endpoint;
    QHash<QString, QString> recordingRequests;
    std::map<QString, DspBranchFrame> ready;

    // Start only after all state (including endpoint and recording maps) exists.
    Impl() { worker = std::thread([this]() { run(); }); }
    ~Impl() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; remoteCancel = true; }
        wake.notify_one();
        remoteWake.notify_one();
        worker.join();
        if (remoteWorker.joinable()) remoteWorker.join();
    }
    bool current(std::uint64_t value, std::uint64_t remote = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        return !stopping && active && generation == value && (!remote || remote == remoteRevision);
    }
    void publish(DspBranchFrame frame, std::uint64_t value, std::uint64_t remote = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stopping && active && generation == value && (!remote || remote == remoteRevision)) ready[frame.id] = std::move(frame);
    }
    void publishAudio(const DspBranchPcmFrame &frame, std::uint64_t value, std::uint64_t remote = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stopping && active && generation == value && (!remote || remote == remoteRevision) && audioConsumer) audioConsumer(frame);
    }
    void resetAudio(bool externalOnly = false) {
        // The caller owns mutex, so old-generation delivery cannot follow the reset.
        if (!audioConsumer) return;
        for (const auto &c : configurations) if (c.audioEnabled && (!externalOnly || c.external)) {
            DspBranchPcmFrame frame;
            frame.sinkId = c.audioOutputId; frame.generation = generation;
            frame.device = c.audioDevice; frame.volume = float(c.volume); frame.reset = true;
            audioConsumer(frame);
        }
    }
    void invalidateRemote() {
        // Called with mutex held; no producer or result may cross this barrier.
        ++remoteRevision; remoteCancel = true;
        remoteJobs.clear(); remoteDrops.clear();
        remoteWake.notify_one();
    }
    void enqueueRemote(RemoteBranchJob job) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || !active || job.block.generation != generation) return;
        if (!remoteWorker.joinable()) remoteWorker = std::thread([this]() { runRemote(); });
        job.revision = remoteRevision;
        const auto same = [&](const auto &pending) { return pending.block.stream == job.block.stream; };
        const auto perBranch = std::count_if(remoteJobs.begin(), remoteJobs.end(), same);
        if (perBranch >= 2 || remoteJobs.size() >= 2U*DspMaxBranches) {
            const auto oldest = perBranch >= 2 ? std::find_if(remoteJobs.begin(), remoteJobs.end(), same) : remoteJobs.begin();
            const QString droppedId = oldest->block.stream;
            remoteJobs.erase(oldest);
            ++droppedRemoteBlocks; ++remoteDrops[droppedId];
            const auto next = std::find_if(remoteJobs.begin(), remoteJobs.end(), [&](const auto &pending) { return pending.block.stream == droppedId; });
            if (next != remoteJobs.end()) next->block.gap = true;
            else if (droppedId == job.block.stream) job.block.gap = true;
        }
        remoteJobs.push_back(std::move(job));
        remoteWake.notify_one();
    }
    bool consumeOutput(DspStreamResult output, const DspStreamBlock &block,
                       const DspBranchConfig &config, BranchOutputState &state,
                       DspBranchFrame frame, DspStreamRecorder *recorder,
                       Clock::time_point started, std::uint64_t remote = 0) {
        if (!current(block.generation, remote)) return false;
        if (!output.error.isEmpty()) {
            if (!state.gap) ++state.transportGaps;
            frame.error = output.error; state.reset();
            frame.gaps += state.transportGaps;
            if (config.audioEnabled) {
                DspBranchPcmFrame fence;
                fence.sinkId = config.audioOutputId; fence.generation = block.generation;
                fence.device = config.audioDevice; fence.volume = float(config.volume); fence.reset = true;
                publishAudio(fence, block.generation, remote);
            }
            publish(std::move(frame), block.generation, remote);
            return false;
        }
        const bool outputGap = state.gap || block.gap || output.gap;
        state.gap = false;
        frame.sampleRateHz = output.sampleRate > 0 ? output.sampleRate : block.sampleRate;
        frame.gaps += state.transportGaps;
        if (outputGap) state.history.clear();
        if (recorder && recorder->active() && output.iq && !output.iq->empty()) {
            DspStreamBlock recorded;
            recorded.stream = block.stream; recorded.generation = block.generation; recorded.epoch = block.epoch;
            recorded.sampleRate = frame.sampleRateHz; recorded.centerHz = block.centerHz;
            for (std::size_t start = 0; start < output.iq->size(); start += DspStreamProtocol::MaxSamples*2U) {
                recorded.firstSample = output.firstSample + start/2;
                recorded.gap = outputGap && start == 0;
                const auto finish = std::min(output.iq->size(), start+DspStreamProtocol::MaxSamples*2U);
                recorded.iq.assign(output.iq->begin()+std::ptrdiff_t(start), output.iq->begin()+std::ptrdiff_t(finish));
                // Admit one bounded write; an admitted disk operation finishes asynchronously.
                if (!current(block.generation, remote)) return false;
                recorder->append(recorded, recorded.iq);
            }
        }
        if (config.audioEnabled && output.iq && !output.iq->empty()) {
            if (config.audioBandwidthHz > config.bandwidthHz || config.audioBandwidthHz >= frame.sampleRateHz*0.9) {
                frame.audioStatus = "Audio band exceeds DDC/output passband";
            } else {
                if (!state.demodulator) state.demodulator = std::make_unique<AudioProcessor>();
                RadioSettings audio;
                audio.inputMode = INPUT_RF;
                audio.centerFrequency = audio.listeningFrequency = frame.centerHz;
                audio.sampleRate = frame.sampleRateHz; audio.modulationType = config.modulation;
                audio.bandwidth = config.audioBandwidthHz; audio.audioLowPassHz = config.audioLowPassHz;
                audio.audioFilterChainJson = config.audioFilters;
                const bool reset = outputGap || state.audioRate != frame.sampleRateHz;
                if (state.demodulator->processIndependentIq(*output.iq, audio, reset, state.pcm)) {
                    state.audioRate = frame.sampleRateHz;
                    double power = 0;
                    for (float v : *output.iq) power += double(v)*v;
                    power /= std::max(std::size_t(1), output.iq->size()/2);
                    const bool open = 10.0*std::log10(std::max(1e-30, power)) >= config.squelchDb;
                    if (open) state.squelchHoldSeconds = 0.12;
                    else state.squelchHoldSeconds -= double(output.iq->size()/2)/frame.sampleRateHz;
                    if (!open && state.squelchHoldSeconds <= 0) std::fill(state.pcm.begin(), state.pcm.end(), 0);
                    DspBranchPcmFrame pcm;
                    pcm.sinkId = config.audioOutputId; pcm.generation = block.generation;
                    pcm.device = config.audioDevice; pcm.volume = float(config.volume); pcm.reset = reset;
                    pcm.pcm = QByteArray(reinterpret_cast<const char *>(state.pcm.data()), int(state.pcm.size()*sizeof(short)));
                    publishAudio(pcm, block.generation, remote);
                    frame.audioStatus = "PCM 48000 Hz mono";
                } else frame.audioStatus = "Audio requires 48 kS/s..2 MS/s processed IQ";
            }
        }
        if (output.iq && config.iqConsumer) {
            state.history.insert(state.history.end(), output.iq->begin(), output.iq->end());
            while (state.history.size() > MaxChannelFloats) state.history.pop_front();
        }
        frame.processingMs = std::chrono::duration<double, std::milli>(Clock::now()-started).count();
        for (auto &spectrum : output.spectra) {
            spectrum.decimation = frame.decimation; spectrum.frontDecimation = frame.frontDecimation;
            spectrum.firTaps = frame.firTaps; spectrum.pfbChannels = frame.pfbChannels;
            spectrum.bankProcessingMs = frame.bankProcessingMs;
            spectrum.gaps = frame.gaps; spectrum.processingMs = frame.processingMs;
            publish(std::move(spectrum), block.generation, remote);
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-state.lastIqFrame).count() >= 50) {
            if (config.iqConsumer) {
                const auto count = std::min(state.history.size(), std::size_t(config.iqSamples)*2U);
                frame.iq = std::make_shared<const std::vector<float>>(state.history.end()-static_cast<std::ptrdiff_t>(count), state.history.end());
            }
            if (recorder) frame.recordingStatus = recorder->status();
            publish(std::move(frame), block.generation, remote);
            state.lastIqFrame = Clock::now();
        }
        return true;
    }
    void runRemote() {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
        setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
        DspStreamManager streams;
        std::map<QString, BranchOutputState> states;
        std::map<QString, std::unique_ptr<DspStreamRecorder>> recorders;
        QHash<QString, QString> directories;
        std::uint64_t revision = 0;
        for (;;) {
            RemoteBranchJob job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                remoteWake.wait(lock, [&]() { return stopping || remoteRevision != revision || !remoteJobs.empty(); });
                if (stopping) return;
                if (remoteRevision != revision) {
                    revision = remoteRevision;
                    const auto remote = endpoint;
                    QHash<QString, QString> requested;
                    if (active) for (const auto &c : configurations)
                        if (c.enabled && c.external && recordingRequests.contains(c.id)) requested.insert(c.id, recordingRequests.value(c.id));
                    lock.unlock();
                    streams.clear(); streams.configure(remote); states.clear();
                    for (auto it = recorders.begin(); it != recorders.end();) {
                        if (!requested.contains(it->first) || requested.value(it->first) != directories.value(it->first)) it = recorders.erase(it);
                        else ++it;
                    }
                    directories = std::move(requested);
                    continue;
                }
                job = std::move(remoteJobs.front()); remoteJobs.pop_front();
                if (!active || job.block.generation != generation || job.revision != revision) continue;
                job.frame.gaps += remoteDrops.value(job.block.stream);
                for (const auto &c : configurations) if (c.id == job.config.id) { job.config.volume = c.volume; break; }
                remoteCancel = false;
            }
            try {
                auto &state = states[job.config.id];
                job.block.gap = job.block.gap || state.gap;
                auto output = streams.process(job.block, true, &remoteCancel);
                if (remoteCancel.load()) continue;
                DspStreamRecorder *recorder = nullptr;
                if (directories.contains(job.config.id)) {
                    if (!current(job.block.generation, job.revision)) continue;
                    auto &entry = recorders[job.config.id];
                    if (!entry) {
                        auto pending = std::make_unique<DspStreamRecorder>();
                        if (!current(job.block.generation, job.revision)) continue;
                        pending->start(directories.value(job.config.id));
                        entry = std::move(pending);
                    }
                    recorder = entry.get();
                }
                consumeOutput(std::move(output), job.block, job.config, state, std::move(job.frame), recorder, job.started, job.revision);
            } catch (const std::exception &) {
                states.erase(job.config.id);
                job.frame.error = "External DDC branch resource limit";
                publish(std::move(job.frame), job.block.generation, job.revision);
            }
        }
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
        PfbChannelizer pfb;
        int bankChannels = 0, bankTaps = 16;
        std::vector<PfbChannelizer::ChannelOutput> bankOutputs;
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
                    auto requestedRecorders = recordingRequests;
                    for (const auto &c : configs) if (c.external) requestedRecorders.remove(c.id);
                    bankChannels = pfbChannels; bankTaps = pfbTaps;
                    lock.unlock();
                    branches.clear();
                    pfb.reset();
                    streams.clear();
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
                    // Volume is a sink parameter, not a reason to reset every channel's DSP history.
                    for (auto &b : branches) for (const auto &config : configurations)
                        if (b->config.id == config.id) { b->config.volume = config.volume; break; }
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
                    pfb.reset();
                    if (lost) ++gaps;
                    if (newEpoch) {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (!stopping && active && generation == revision) { invalidateRemote(); resetAudio(true); }
                    }
                }
                if (newEpoch && metadata.floatCount > 0 && raw.size() > metadata.floatCount)
                    raw.erase(raw.begin(), raw.end()-static_cast<std::ptrdiff_t>(metadata.floatCount));
                epoch = stats.epoch; lastEnd = stats.totalFloatCount; lossCount = snapshotLoss;
                bankOutputs.clear();
                bool bankValid = false;
                double bankMs = 0.0;
                if (bankChannels > 0) {
                    const auto startedBank = Clock::now();
                    PfbChannelizer::Config bankConfig;
                    bankConfig.sampleRateHz = settings.sampleRate;
                    bankConfig.channels = bankChannels; bankConfig.tapsPerPhase = bankTaps;
                    if (settings.inputMode == INPUT_RF && pfb.configure(bankConfig)) {
                        std::vector<int> selected;
                        for (const auto &b : branches) if (b->config.pfb) {
                            const double center = b->config.frequencyHz > 0 ? b->config.frequencyHz : settings.listeningFrequency;
                            const int bin = pfb.nearestCenterIndex(center-settings.centerFrequency);
                            if (pfb.coversBand(bin, center-settings.centerFrequency, b->config.bandwidthHz) &&
                                std::find(selected.begin(), selected.end(), bin) == selected.end()) selected.push_back(bin);
                        }
                        bankValid = pfb.processFloatIq(raw.data(), raw.size(), selected, bankOutputs).status == PfbChannelizer::Status::Ok;
                    }
                    bankMs = std::chrono::duration<double, std::milli>(Clock::now()-startedBank).count();
                }
                for (auto &b : branches) {
                    if (!current(revision)) break;
                    const auto started = Clock::now();
                    DspBranchFrame frame;
                    frame.id = b->config.id; frame.sequence = lastEnd / 2U;
                    frame.generation = revision; frame.gaps = gaps;
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
                    const std::vector<float> *input = &raw;
                    if (b->config.pfb) {
                        const double offsetHz = frame.centerHz-settings.centerFrequency;
                        const int bin = pfb.nearestCenterIndex(offsetHz);
                        const auto channel = std::find_if(bankOutputs.begin(), bankOutputs.end(),
                            [bin](const auto &output) { return output.binIndex == bin; });
                        if (!bankValid || !pfb.coversBand(bin, offsetHz, b->config.bandwidthHz) || channel == bankOutputs.end()) {
                            frame.error = "Band crosses PFB guard/channel boundary; change bank size or use direct DDC";
                            b->reset(); publish(std::move(frame), revision); continue;
                        }
                        branchSettings.sampleRate = channel->sampleRateHz;
                        branchSettings.centerFrequency += channel->centerOffsetHz;
                        input = &channel->iq;
                        if (input->empty()) continue;
                    }
                    IqChannelizer::Options options;
                    options.outputRateHz = frame.sampleRateHz;
                    options.cutoffHz = halfBand;
                    options.decimationFactor = b->config.decimation;
                    options.firTapCount = b->config.firTaps;
                    options.transitionHz = b->config.transitionHz;
                    const auto result = b->channelizer.processFloatIq(input->data(), input->size(), branchSettings, b->channel, false, &options);
                    if (!result.valid) {
                        frame.error = "Invalid DDC decimation or output sample rate";
                        b->reset(); publish(std::move(frame), revision); continue;
                    }
                    frame.sampleRateHz = result.outputRate; frame.decimation = result.decimationFactor;
                    frame.frontDecimation = result.frontDecimationFactor;
                    frame.firTaps = result.firTapCount;
                    frame.pfbChannels = b->config.pfb ? bankChannels : 0;
                    frame.bankProcessingMs = b->config.pfb ? bankMs : 0;
                    const double channelRate = result.outputRate;
                    for (std::size_t offset = 0; offset < b->channel.size(); offset += DspStreamProtocol::MaxSamples*2U) {
                        if (!current(revision)) break;
                        auto &block = b->stream;
                        block.stream = b->config.id; block.generation = revision; block.epoch = epoch;
                        block.firstSample = b->nextSample; block.gap = b->gap;
                        block.sampleRate = channelRate; block.centerHz = frame.centerHz;
                        block.returnIq = b->config.iqConsumer || b->config.audioEnabled || (recorder && recorder->active()); block.ffts = b->config.ffts;
                        if (b->config.external) {
                            std::lock_guard<std::mutex> lock(mutex);
                            block.returnIq = block.returnIq || recordingRequests.contains(b->config.id);
                        }
                        const auto end = std::min(b->channel.size(), offset+DspStreamProtocol::MaxSamples*2U);
                        block.iq.assign(b->channel.begin()+std::ptrdiff_t(offset), b->channel.begin()+std::ptrdiff_t(end));
                        b->nextSample += block.iq.size()/2U;
                        if (b->config.external) {
                            RemoteBranchJob job;
                            job.config = b->config; job.block = std::move(block);
                            job.frame = frame; job.started = started;
                            enqueueRemote(std::move(job)); b->gap = false;
                        } else {
                            auto output = streams.process(block, false);
                            if (!consumeOutput(std::move(output), block, b->config, *b, frame, recorder, started)) break;
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
    active = active && !source.simplifiedAudioChannelizer;
    std::lock_guard<std::mutex> lock(impl->mutex);
    const auto &old = impl->source;
    if (impl->key == plan.executionKey && impl->active == active && old.sampleRate == source.sampleRate &&
        old.centerFrequency == source.centerFrequency && old.listeningFrequency == source.listeningFrequency && old.inputMode == source.inputMode) {
        impl->configurations = plan.branches;
        return;
    }
    impl->key = plan.executionKey; impl->source = source; impl->active = active;
    impl->configurations = plan.branches; ++impl->generation; impl->ready.clear();
    impl->pfbChannels = plan.pfbChannels; impl->pfbTaps = plan.pfbTaps;
    impl->invalidateRemote();
    impl->resetAudio();
    impl->wake.notify_one();
}

void DspBranchEngine::setExternalEndpoint(const DspStreamEndpoint &endpoint) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->endpoint == endpoint) return;
    impl->endpoint = endpoint;
    impl->invalidateRemote(); impl->resetAudio(true);
    for (const auto &c : impl->configurations) if (c.external) {
        impl->ready.erase(c.id);
        for (const auto &fft : c.ffts) impl->ready.erase(fft.id);
    }
}

void DspBranchEngine::setRecording(const QString &branchId, const QString &directory) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (directory.isEmpty()) impl->recordingRequests.remove(branchId);
    else if (impl->recordingRequests.size() < DspMaxBranches || impl->recordingRequests.contains(branchId)) impl->recordingRequests.insert(branchId, directory);
    ++impl->generation; impl->ready.clear(); impl->invalidateRemote(); impl->resetAudio(); impl->wake.notify_one();
}

void DspBranchEngine::setAudioConsumer(AudioConsumer consumer) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->audioConsumer = std::move(consumer);
}

std::vector<DspBranchFrame> DspBranchEngine::takeFrames() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    std::vector<DspBranchFrame> frames;
    for (auto &entry : impl->ready) frames.push_back(std::move(entry.second));
    impl->ready.clear();
    return frames;
}

DspBranchExecutionStats DspBranchEngine::executionStats() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    DspBranchExecutionStats stats;
    stats.remoteWorkerStarted = impl->remoteWorker.joinable();
    stats.pendingRemoteBlocks = impl->remoteJobs.size();
    for (const auto &job : impl->remoteJobs) stats.pendingRemoteBytes += job.block.iq.size()*sizeof(float);
    stats.droppedRemoteBlocks = impl->droppedRemoteBlocks;
    return stats;
}
