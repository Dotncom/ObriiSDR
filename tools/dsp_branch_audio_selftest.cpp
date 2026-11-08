#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "audioprocessor.h"
#include "audiofilterchain.h"
#include "dspbranchaudiooutput.h"
#include "dspbranchengine.h"
#include "dspgraphmodel.h"
#include "iqbuffer.h"
#include "pfbchannelizer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

bool runDspBranchAudioQueueTests();
bool runDspBranchRemoteTests();

namespace {
constexpr double TwoPi = 6.28318530717958647692;
constexpr double PcmRate = 48000.0;
constexpr double SourceCenter = 100000000.0;
constexpr double SourceRate = 768000.0;
constexpr double BranchOffset = 96000.0;
constexpr double BranchBand = 12500.0;

void requireAudio(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

QJsonObject edge(const QString &source, const QString &target) {
    return {{"source", source}, {"target", target}};
}

QJsonObject block(const QString &id, const QString &type, const QJsonObject &settings = {}) {
    return {{"id", id}, {"type", type}, {"settings", settings}};
}

QString filterJson(const QString &id, AudioFilterKind kind) {
    auto stage = defaultAudioFilterStage(kind);
    stage.id = id;
    stage.enabled = true;
    stage.gainDb = -6.0;
    stage.frequencyHz = 2800.0;
    return encodeAudioFilterChain({stage});
}

QJsonObject audioGraph(int count = 1, bool enabled = true, bool pfb = false) {
    QJsonArray blocks{block("source", "iq_source")}, edges;
    if (pfb) {
        blocks.append(block("bank", "pfb_channelizer", {{"channels", 8}, {"tapsPerPhase", 16}}));
        edges.append(edge("source", "bank"));
    }
    for (int n = 0; n < count; ++n) {
        const auto id = QStringLiteral("b%1").arg(n);
        QJsonObject ddc{{"frequencyHz", SourceCenter + BranchOffset}, {"bandwidthHz", BranchBand}, {"outputRateHz", 96000.0}};
        QJsonObject output{{"device", 7}, {"volume", 0.35}};
        if (enabled) { ddc.insert("enabled", true); output.insert("enabled", true); }
        blocks.append(block(id, "ddc_channelizer", ddc));
        blocks.append(block(id + "-demod", "branch_demodulator",
            {{"modulation", MOD_NFM}, {"bandwidthHz", BranchBand}, {"lowPassHz", 3000.0}, {"squelchDb", -160.0}}));
        // Reverse lexical IDs ensure the compiler follows wires, not block sorting.
        blocks.append(block(id + "-a", "branch_audio_filter", {{"filters", filterJson(id + "-low", AudioFilterKind::LowPass)}}));
        blocks.append(block(id + "-z", "branch_audio_filter", {{"filters", filterJson(id + "-gain", AudioFilterKind::Gain)}}));
        blocks.append(block(id + "-out", "branch_audio_output", output));
        edges.append(edge(pfb ? "bank" : "source", id));
        edges.append(edge(id, id + "-demod"));
        edges.append(edge(id + "-demod", id + "-z"));
        edges.append(edge(id + "-z", id + "-a"));
        edges.append(edge(id + "-a", id + "-out"));
    }
    return {{"blocks", blocks}, {"connections", edges}};
}

void setting(QJsonObject &graph, const QString &id, const char *key, const QJsonValue &value) {
    auto blocks = graph.value("blocks").toArray();
    for (int i = 0; i < blocks.size(); ++i) {
        auto b = blocks[i].toObject();
        if (b.value("id").toString() != id) continue;
        auto settings = b.value("settings").toObject();
        settings.insert(QLatin1String(key), value); b.insert("settings", settings); blocks[i] = b;
        graph.insert("blocks", blocks); return;
    }
    throw std::runtime_error("Audio fixture block missing");
}

void disconnect(QJsonObject &graph, const QString &source, const QString &target) {
    QJsonArray remaining;
    for (const auto &e : graph.value("connections").toArray())
        if (e.toObject().value("source") != source || e.toObject().value("target") != target) remaining.append(e);
    graph.insert("connections", remaining);
}

void rejectedGraph(const QJsonObject &graph) {
    const auto plan = compileDspGraph(graph);
    requireAudio(!plan.errors.isEmpty() && plan.branches.isEmpty() && plan.audioRoutes.isEmpty() &&
        plan.spectrumRoutes.isEmpty() && plan.researchRoutes.isEmpty(), "Invalid audio graph retained executable routes");
}

void testCompiler() {
    requireAudio(dspLinkKind("ddc_channelizer", "branch_demodulator") == DspLinkKind::Iq &&
        dspLinkKind("branch_demodulator", "branch_audio_filter") == DspLinkKind::Audio &&
        dspLinkKind("branch_audio_filter", "branch_audio_output") == DspLinkKind::Audio &&
        dspLinkKind("branch_audio_output", "ddc_channelizer") == DspLinkKind::Invalid, "Branch audio port types wrong");
    auto graph = audioGraph();
    const auto plan = compileDspGraph(graph);
    requireAudio(plan.errors.isEmpty() && plan.branches.size() == 1 && plan.audioRoutes.size() == 4, "Complete branch audio graph did not compile");
    const auto &b = plan.branches.front();
    requireAudio(b.enabled && b.audioEnabled && b.demodulatorId == "b0-demod" && b.audioOutputId == "b0-out" &&
        b.modulation == MOD_NFM && b.audioBandwidthHz == BranchBand && b.audioLowPassHz == 3000 &&
        b.squelchDb == -160 && b.audioDevice == 7 && std::abs(b.volume - 0.35) < 1e-12, "Audio settings not propagated into branch plan");
    bool ok = false;
    const auto filters = decodeAudioFilterChain(b.audioFilters, &ok);
    requireAudio(ok && filters.size() == 2 && filters[0].id == "b0-gain" && filters[1].id == "b0-low", "Audio filters compiled out of wire order");
    for (const auto &id : {"b0-demod", "b0-z", "b0-a", "b0-out"})
        requireAudio(plan.audioRoutes.value(id) == "b0", "Audio route bound to wrong branch");
    const auto defaults = compileDspGraph(audioGraph(1, false));
    requireAudio(defaults.errors.isEmpty() && defaults.branches.size() == 1 &&
        !defaults.branches[0].enabled && !defaults.branches[0].audioEnabled, "Missing enabled settings start branch audio by default");
    auto incomplete = graph;
    disconnect(incomplete, "b0-a", "b0-out");
    const auto idle = compileDspGraph(incomplete);
    requireAudio(idle.errors.isEmpty() && idle.branches.size() == 1 && !idle.branches[0].audioEnabled &&
        idle.branches[0].audioOutputId.isEmpty() && idle.audioRoutes.isEmpty(), "Incomplete graph acquired implicit audio sink");
    auto cycle = graph;
    disconnect(cycle, "b0-demod", "b0-z");
    auto edges = cycle.value("connections").toArray(); edges.append(edge("b0-a", "b0-z")); cycle["connections"] = edges;
    requireAudio(compileDspGraph(cycle).errors.join(' ').contains("cycle", Qt::CaseInsensitive), "Typed filter cycle not detected");
    rejectedGraph(cycle);
    auto fanIn = graph; edges = fanIn["connections"].toArray(); edges.append(edge("b0-demod", "b0-a")); fanIn["connections"] = edges;
    rejectedGraph(fanIn);
    auto twoOutputs = graph;
    auto blocks = twoOutputs["blocks"].toArray(); blocks.append(block("other-out", "branch_audio_output")); twoOutputs["blocks"] = blocks;
    edges = twoOutputs["connections"].toArray(); edges.append(edge("b0-a", "other-out")); twoOutputs["connections"] = edges;
    rejectedGraph(twoOutputs);
    auto malformed = graph; setting(malformed, "b0-a", "filters", "not-json"); rejectedGraph(malformed);
    QVector<AudioFilterStage> many;
    for (int n = 0; n < 31; ++n) {
        auto stage = defaultAudioFilterStage(AudioFilterKind::Gain);
        stage.id = QStringLiteral("gain-%1").arg(n); many.append(stage);
    }
    auto bounded = graph; setting(bounded, "b0-z", "filters", encodeAudioFilterChain(many));
    const auto thirtyTwo = compileDspGraph(bounded);
    requireAudio(thirtyTwo.errors.isEmpty() && thirtyTwo.branches.size() == 1 &&
        decodeAudioFilterChain(thirtyTwo.branches.front().audioFilters).size() == 32, "Exact 32-filter branch limit rejected or truncated");
    auto extra = defaultAudioFilterStage(AudioFilterKind::Gain); extra.id = "gain-31"; many.append(extra);
    setting(bounded, "b0-z", "filters", encodeAudioFilterChain(many));
    requireAudio(compileDspGraph(bounded).errors.join(' ').contains("32"), "Aggregate 33-filter graph lacks explicit limit error");
    rejectedGraph(bounded);
    const auto eight = compileDspGraph(audioGraph(8));
    requireAudio(DspMaxBranches == 8 && eight.errors.isEmpty() && eight.branches.size() == 8 && eight.audioRoutes.size() == 32, "Eight audio branches not accepted");
    rejectedGraph(audioGraph(9));
    auto changed = graph; setting(changed, "b0-out", "enabled", false);
    requireAudio(compileDspGraph(changed).executionKey != plan.executionKey, "Audio enable change did not invalidate execution key");
    changed = graph; setting(changed, "b0-demod", "modulation", MOD_AM);
    requireAudio(compileDspGraph(changed).executionKey != plan.executionKey, "Demodulation change did not invalidate execution key");
    changed = graph; setting(changed, "b0-out", "volume", 0.65);
    const auto volumeOnly = compileDspGraph(changed);
    requireAudio(volumeOnly.errors.isEmpty() && volumeOnly.executionKey == plan.executionKey &&
        std::abs(volumeOnly.branches.front().volume - 0.65) < 1e-12, "Volume change resets execution or fails to update sink setting");
    changed = graph; blocks = changed["blocks"].toArray(); auto moved = blocks[0].toObject(); moved["x"] = 999; blocks[0] = moved; changed["blocks"] = blocks;
    requireAudio(compileDspGraph(changed).executionKey == plan.executionKey, "Layout-only edit restarts audio");
    const auto bank = compileDspGraph(audioGraph(1, true, true));
    requireAudio(bank.errors.isEmpty() && bank.branches.size() == 1 && bank.branches[0].pfb &&
        bank.pfbChannels == 8 && bank.pfbTaps == 16 && bank.branches[0].audioEnabled, "PFB audio chain failed to compile");
}

void testExplicitMainOrder() {
    QJsonObject graph{{"blocks", QJsonArray{block("out", "audio_output"), block("a", "filter_lowpass"),
        block("demod", "demodulator"), block("z", "filter_gain"), block("unused", "filter_notch")}},
        {"connections", QJsonArray{edge("z", "a"), edge("a", "out"), edge("demod", "z")}}};
    QString error = "stale error";
    requireAudio(compileDspAudioFilterOrder(graph, &error) == QStringList({"z", "a"}) && error.isEmpty(), "Explicit main filter order does not follow complete chain");
    const auto descriptive = compileDspGraph(graph);
    requireAudio(descriptive.errors.isEmpty() && descriptive.branches.isEmpty() && descriptive.audioRoutes.isEmpty() &&
        dspLinkKind("demodulator", "filter_gain") == DspLinkKind::Documentation, "Legacy main audio wires became automatically executable");
    auto invalid = graph; disconnect(invalid, "a", "out");
    requireAudio(compileDspAudioFilterOrder(invalid, &error).isEmpty() && !error.isEmpty(), "Incomplete main chain accepted");
    auto edges = invalid["connections"].toArray(); edges.append(edge("a", "z")); invalid["connections"] = edges;
    requireAudio(compileDspAudioFilterOrder(invalid, &error).isEmpty() && !error.isEmpty(), "Main audio cycle accepted");
    invalid = graph; edges = invalid["connections"].toArray(); edges.append(edge("demod", "a")); invalid["connections"] = edges;
    requireAudio(compileDspAudioFilterOrder(invalid, &error).isEmpty() && !error.isEmpty(), "Ambiguous main filter order accepted");
    requireAudio(compileDspAudioFilterOrder(audioGraph(), &error).isEmpty() && !error.isEmpty(), "Independent audio graph rewrites main filter order");
}

std::vector<float> modulatedIq(int mode, double rate, std::size_t samples, std::uint64_t first = 0, double offset = 0) {
    std::vector<float> iq(samples * 2);
    for (std::size_t n = 0; n < samples; ++n) {
        const double t = double(first + n) / rate;
        const double amplitude = mode == MOD_AM ? 0.4 * (1.0 + 0.5 * std::sin(TwoPi * 1000.0 * t)) : 0.4;
        const double phase = TwoPi * offset * t + (mode == MOD_AM ? 0.0 : 2.0 * std::sin(TwoPi * 1000.0 * t));
        iq[2*n] = float(amplitude * std::cos(phase)); iq[2*n+1] = float(amplitude * std::sin(phase));
    }
    return iq;
}

void samePcm(const std::vector<short> &a, const std::vector<short> &b, const char *message) {
    requireAudio(!a.empty() && a.size() == b.size(), message);
    int worst = 0;
    for (std::size_t n = 0; n < a.size(); ++n) worst = (std::max)(worst, std::abs(int(a[n]) - int(b[n])));
    requireAudio(worst <= 1, message);
}

void checkTone(const std::vector<short> &pcm) {
    // Discard 50 ms of FIR/DC/AGC startup, then inspect actual recovered 48 kHz PCM.
    const std::size_t begin = 2400;
    requireAudio(pcm.size() > begin + 4800, "Audio stream too short for frequency check");
    double energy = 0, re = 0, im = 0, otherRe = 0, otherIm = 0;
    std::vector<std::size_t> crossings;
    for (std::size_t n = begin; n < pcm.size(); ++n) {
        const double sample = pcm[n]; energy += sample * sample;
        const double phase = TwoPi * 1000.0 * double(n - begin) / PcmRate;
        const double other = TwoPi * 1250.0 * double(n - begin) / PcmRate;
        re += sample * std::cos(phase); im += sample * std::sin(phase);
        otherRe += sample * std::cos(other); otherIm += sample * std::sin(other);
        if (pcm[n-1] <= 0 && pcm[n] > 0) crossings.push_back(n);
    }
    const double rms = std::sqrt(energy / double(pcm.size() - begin));
    requireAudio(rms > 100 && crossings.size() > 20, "AM/FM demodulator produced silence or DC");
    const double frequency = (crossings.size() - 1) * PcmRate / double(crossings.back() - crossings.front());
    requireAudio(std::abs(frequency - 1000.0) < 3.0, "Recovered PCM frequency/timebase incorrect");
    requireAudio(std::hypot(re, im) > 5.0 * std::hypot(otherRe, otherIm) &&
        2.0 * std::hypot(re, im) / double(pcm.size() - begin) > 0.6 * rms, "Recovered audio lacks dominant modulation tone");
}

void testIndependentPcm() {
    for (int mode : {MOD_AM, MOD_NFM}) for (double rate : {48000.0, 96000.0, 100000.0}) {
        RadioSettings s; s.inputMode = INPUT_RF; s.modulationType = mode; s.sampleRate = rate;
        s.centerFrequency = s.listeningFrequency = SourceCenter; s.bandwidth = BranchBand;
        s.audioLowPassHz = 3000; s.audioEnabled = false;
        s.audioFilterChainJson = filterJson("stateful-lowpass", AudioFilterKind::LowPass);
        const std::size_t samples = std::size_t(rate * 0.6) + 14;
        const auto iq = modulatedIq(mode, rate, samples);
        AudioProcessor whole, chunks;
        whole.setLocalPlaybackEnabled(false); chunks.setLocalPlaybackEnabled(false);
        std::vector<short> expected, actual, part;
        requireAudio(whole.processIndependentIq(iq, s, true, expected), "Offline AM/FM demodulation rejected valid IQ");
        const std::size_t sizes[] = {1, 7, 509, 33, 4096, 127, 8191};
        std::size_t iteration = 0;
        for (std::size_t first = 0; first < samples; ++iteration) {
            const auto count = (std::min)(sizes[iteration % 7], samples - first);
            const std::vector<float> blockIq(iq.begin() + std::ptrdiff_t(2*first), iq.begin() + std::ptrdiff_t(2*(first+count)));
            requireAudio(chunks.processIndependentIq(blockIq, s, first == 0, part), "Irregular audio chunk rejected");
            actual.insert(actual.end(), part.begin(), part.end()); first += count;
        }
        samePcm(expected, actual, "Audio sample count or PCM depends on IQ chunk boundaries");
        requireAudio(std::abs(double(actual.size()) - samples * PcmRate / rate) <= 32.0, "Continuous PCM clock loses more than one fixed FIR startup delay");
        checkTone(actual);
        requireAudio(chunks.processIndependentIq(iq, s, true, part), "Explicit audio discontinuity rejected");
        samePcm(expected, part, "Discontinuity did not reset demodulator and stateful audio filters");
        const auto continuation = modulatedIq(mode, rate, samples, samples);
        requireAudio(chunks.processIndependentIq(continuation, s, false, part), "Continuous second audio block rejected");
        requireAudio(std::abs(double(part.size()) - samples * PcmRate / rate) <= 2.0, "FIR startup delay repeated on continuous block");
    }
    AudioProcessor invalid;
    invalid.setLocalPlaybackEnabled(false);
    RadioSettings s; s.inputMode = INPUT_RF; s.modulationType = MOD_AM; s.sampleRate = 96000;
    const std::vector<float> shortIqBlock(128, 0.0f);
    std::vector<short> pcm{123};
    for (double rate : {47999.0, 2000001.0, std::numeric_limits<double>::quiet_NaN()}) {
        s.sampleRate = rate;
        requireAudio(!invalid.processIndependentIq(shortIqBlock, s, false, pcm) && pcm.empty(), "Invalid independent audio rate accepted or stale PCM retained");
    }
    s.sampleRate = 96000;
    requireAudio(!invalid.processIndependentIq({0.0f, 0.0f, 0.0f}, s, false, pcm), "Odd interleaved IQ accepted");
    requireAudio(!invalid.processIndependentIq(std::vector<float>(262146, 0), s, false, pcm), "Independent audio allocation bound ignored");
    s.inputMode = INPUT_HF_NOISE_CANCEL;
    requireAudio(!invalid.processIndependentIq(shortIqBlock, s, false, pcm), "Independent API accepted non-RF input");
}

struct Capture {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<DspBranchPcmFrame> frames;
    void append(const DspBranchPcmFrame &frame) {
        // Empty frames are reset fences, not PCM delivery.
        if (frame.pcm.isEmpty()) return;
        { std::lock_guard<std::mutex> lock(mutex); frames.push_back(frame); }
        ready.notify_one();
    }
    bool waitFor(std::size_t count, int milliseconds = 4000) {
        std::unique_lock<std::mutex> lock(mutex);
        return ready.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] { return frames.size() >= count; });
    }
    std::vector<DspBranchPcmFrame> snapshot() {
        std::lock_guard<std::mutex> lock(mutex); return frames;
    }
};

struct SyntheticQueue {
    double previousRate = IqBuffer::sampleRateEstimate();
    std::size_t blocks = 0, floats = 0;
    explicit SyntheticQueue(std::uint64_t epoch) {
        IqBuffer::clear(epoch);
        // Avoid producer-side live-latency trimming, which is unrelated to queue consumption.
        IqBuffer::setSampleRateEstimate(0.0);
    }
    ~SyntheticQueue() { IqBuffer::clear(); IqBuffer::setSampleRateEstimate(previousRate); }
    void publish(const RadioSettings &source, std::uint64_t first) {
        const auto iq = modulatedIq(MOD_NFM, source.sampleRate, 65536, first, BranchOffset);
        IqBuffer::BlockMetadata metadata; metadata.valid = true; metadata.centerFrequencyHz = source.centerFrequency;
        requireAudio(IqBuffer::publish(iq.data(), iq.size(), true, true, 0, &metadata), "Cannot publish synthetic branch audio IQ");
        ++blocks; floats += iq.size();
    }
    void untouched() const {
        requireAudio(IqBuffer::queuedBlocks() == blocks && IqBuffer::queuedFloatCount() == floats,
            "Independent audio consumed the main source IQ queue");
    }
};

RadioSettings engineSource() {
    RadioSettings s; s.inputMode = INPUT_RF; s.sampleRate = SourceRate;
    s.centerFrequency = SourceCenter; s.listeningFrequency = SourceCenter + BranchOffset;
    s.audioEnabled = false; return s;
}

DspBranchFrame waitBranch(DspBranchEngine &engine, const QString &id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        for (auto &frame : engine.takeFrames()) if (frame.id == id) return std::move(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Branch audio worker did not report status");
}

void testEngine(bool pfb) {
    SyntheticQueue queue(pfb ? 18102 : 18101);
    Capture capture;
    // Engine is destroyed before capture/queue even when an assertion throws.
    DspBranchEngine engine;
    engine.setAudioConsumer([&capture](const DspBranchPcmFrame &frame) { capture.append(frame); });
    auto graph = audioGraph(1, true, pfb);
    const auto plan = compileDspGraph(graph);
    const auto source = engineSource();
    engine.configure(plan, source, true);
    for (std::size_t n = 0; n < 4; ++n) {
        queue.publish(source, n * 65536);
        requireAudio(capture.waitFor(n + 1), "Active branch audio produced no PCM without GUI/device");
        queue.untouched();
    }
    const auto frames = capture.snapshot();
    requireAudio(frames.size() == 4 && frames.front().reset, "Branch audio reset/count metadata wrong");
    std::vector<short> pcm;
    for (std::size_t n = 0; n < frames.size(); ++n) {
        const auto &frame = frames[n];
        requireAudio(frame.sinkId == "b0-out" && frame.generation == frames.front().generation && frame.device == 7 &&
            std::abs(frame.volume - 0.35f) < 1e-6f && !frame.pcm.isEmpty() && frame.pcm.size() % int(sizeof(short)) == 0,
            "Engine delivered malformed/wrong-route PCM metadata");
        requireAudio(n == 0 || !frame.reset, "Continuous branch audio spuriously reset");
        const auto count = std::size_t(frame.pcm.size()) / sizeof(short), begin = pcm.size();
        pcm.resize(begin + count); std::memcpy(pcm.data() + begin, frame.pcm.constData(), count * sizeof(short));
        if (n > 0) requireAudio(std::abs(double(count) - 65536.0 * PcmRate / SourceRate) <= 2.0, "Branch engine lost continuous PCM samples");
    }
    requireAudio(std::abs(double(pcm.size()) - 4.0 * 65536.0 * PcmRate / SourceRate) <= 64.0, "Engine PCM duration has repeated startup loss");
    checkTone(pcm);
    setting(graph, "b0-out", "volume", 0.65);
    const auto hotPlan = compileDspGraph(graph);
    engine.configure(hotPlan, source, true);
    // Allow one already-started worker iteration to retain the old sink volume.
    for (std::size_t n = 4; n < 6; ++n) {
        queue.publish(source, n * 65536);
        requireAudio(capture.waitFor(n + 1), "Volume hot update stopped PCM delivery");
    }
    const auto hot = capture.snapshot();
    requireAudio(hot.size() == 6 && std::abs(hot.back().volume - 0.65f) < 1e-6f, "Worker did not hot-update sink volume");
    for (std::size_t n = 4; n < hot.size(); ++n)
        requireAudio(hot[n].generation == frames.front().generation && !hot[n].reset &&
            std::abs(double(hot[n].pcm.size() / int(sizeof(short))) - 65536.0 * PcmRate / SourceRate) <= 2.0,
            "Volume-only update reset DSP history or interrupted the PCM clock");
    queue.untouched();
    auto simplified = source; simplified.simplifiedAudioChannelizer = true;
    engine.configure(hotPlan, simplified, true);
    queue.publish(source, 6 * 65536);
    requireAudio(!capture.waitFor(hot.size() + 1, 200) && engine.takeFrames().empty(), "Simplified mode still executes branch audio");
    queue.untouched();
    engine.configure(hotPlan, source, false);
    queue.publish(source, 7 * 65536);
    requireAudio(!capture.waitFor(hot.size() + 1, 200) && engine.takeFrames().empty(), "Inactive engine still executes branch audio");
    queue.untouched();
}

void testDisabledAndPfbBoundary() {
    const auto source = engineSource();
    {
        SyntheticQueue queue(18103); Capture capture; DspBranchEngine engine;
        auto graph = audioGraph(1, false); setting(graph, "b0", "enabled", true);
        engine.setAudioConsumer([&capture](const DspBranchPcmFrame &frame) { capture.append(frame); });
        engine.configure(compileDspGraph(graph), source, true); queue.publish(source, 0);
        const auto status = waitBranch(engine, "b0");
        requireAudio(status.error.isEmpty() && !capture.waitFor(1, 100), "Default-disabled output produced PCM");
        queue.untouched();
    }
    PfbChannelizer bank;
    requireAudio(bank.configure({SourceRate, 8, 16}), "PFB boundary fixture configuration failed");
    const int bin = bank.nearestCenterIndex(BranchOffset);
    const double boundary = bank.binCenterHz(bin) + bank.usablePassbandHz() / 2.0 - BranchBand / 2.0;
    requireAudio(bank.coversBand(bin, boundary, BranchBand) && !bank.coversBand(bin, boundary + 1.0, BranchBand),
        "PFB passband boundary containment wrong");
    requireAudio(!bank.coversBand(bank.nearestCenterIndex(SourceRate / 2.0 - 1), SourceRate / 2.0 - 1, BranchBand),
        "PFB accepted a band across input Nyquist");
    SyntheticQueue queue(18104); Capture capture; DspBranchEngine engine;
    auto graph = audioGraph(2, true, true);
    setting(graph, "b1", "frequencyHz", SourceCenter + boundary + 1.0);
    const auto plan = compileDspGraph(graph);
    requireAudio(plan.errors.isEmpty() && plan.branches.size() == 2, "PFB boundary audio graph did not compile");
    engine.setAudioConsumer([&capture](const DspBranchPcmFrame &frame) { capture.append(frame); });
    engine.configure(plan, source, true); queue.publish(source, 0);
    const auto rejected = waitBranch(engine, "b1");
    requireAudio(rejected.error.contains("PFB") && rejected.error.contains("boundary"), "Engine silently routed a PFB boundary-crossing band");
    requireAudio(capture.waitFor(1), "Rejected PFB band suppressed valid sibling audio branch");
    const auto delivered = capture.snapshot();
    requireAudio(delivered.size() == 1 && delivered.front().sinkId == "b0-out" && !delivered.front().pcm.isEmpty(),
        "Invalid PFB branch produced PCM or valid sibling lost its route");
    queue.untouched();
}

void testPfbNonRfRejected() {
    SyntheticQueue queue(18106); Capture capture; DspBranchEngine engine;
    auto source = engineSource(); source.inputMode = INPUT_HF1;
    engine.setAudioConsumer([&capture](const DspBranchPcmFrame &frame) { capture.append(frame); });
    engine.configure(compileDspGraph(audioGraph(1, true, true)), source, true);
    queue.publish(source, 0);
    const auto status = waitBranch(engine, "b0");
    requireAudio(!status.error.isEmpty() && status.error.contains("PFB") && !capture.waitFor(1, 100),
        "PFB processed a non-RF source or emitted audio for it");
    queue.untouched();
}

void testEngineFilterIsolation() {
    SyntheticQueue queue(18105); Capture capture; DspBranchEngine engine;
    auto graph = audioGraph(2);
    auto unity = defaultAudioFilterStage(AudioFilterKind::Gain);
    unity.id = "b1-gain"; unity.enabled = true; unity.gainDb = 0;
    setting(graph, "b1-z", "filters", encodeAudioFilterChain({unity}));
    const auto plan = compileDspGraph(graph);
    requireAudio(plan.errors.isEmpty() && plan.branches.size() == 2, "Parallel filter fixture did not compile");
    engine.setAudioConsumer([&capture](const DspBranchPcmFrame &frame) { capture.append(frame); });
    const auto source = engineSource();
    engine.configure(plan, source, true); queue.publish(source, 0);
    requireAudio(capture.waitFor(2), "Two independent audio sinks did not receive PCM");
    const auto frames = capture.snapshot();
    requireAudio(frames.size() == 2, "Parallel audio sinks duplicated delivery");
    double attenuated = 0, unattenuated = 0;
    int attenuatedCount = 0, unattenuatedCount = 0;
    for (const auto &frame : frames) {
        requireAudio(frame.reset && frame.generation == frames.front().generation &&
            frame.pcm.size() > 2 * 2400 && frame.pcm.size() % 2 == 0, "Parallel PCM clocks/reset metadata wrong");
        double energy = 0;
        for (int offset = 2 * 2400; offset < frame.pcm.size(); offset += int(sizeof(short))) {
            short sample = 0; std::memcpy(&sample, frame.pcm.constData() + offset, sizeof(sample));
            energy += double(sample) * sample;
        }
        if (frame.sinkId == "b0-out") { attenuated = energy; attenuatedCount = frame.pcm.size(); }
        else if (frame.sinkId == "b1-out") { unattenuated = energy; unattenuatedCount = frame.pcm.size(); }
        else requireAudio(false, "Parallel audio delivered to an unrelated sink");
    }
    requireAudio(attenuatedCount > 0 && attenuatedCount == unattenuatedCount && unattenuated > 0,
        "Independent audio sinks have mismatched durations or missing signal");
    requireAudio(std::abs(std::sqrt(attenuated / unattenuated) - std::pow(10.0, -6.0 / 20.0)) < 0.02,
        "Compiled branch filter gain ignored or leaked into sibling audio");
    queue.untouched();
}
}

// QCoreApplication is sufficient. Only the remote tests open synthetic loopback peers.
bool runDspBranchAudioTests() {
    requireAudio(runDspBranchAudioQueueTests(), "DSP audio queue admission tests failed");
    requireAudio(runDspBranchRemoteTests(), "DSP remote branch isolation tests failed");
    testCompiler();
    testExplicitMainOrder();
    testIndependentPcm();
    testEngine(false);
    testEngine(true);
    testDisabledAndPfbBoundary();
    testPfbNonRfRejected();
    testEngineFilterIsolation();
    std::cout << "DSP branch audio: graph/limits, explicit main order, AM/FM PCM continuity, engine isolation and PFB boundaries passed\n";
    return true;
}
