#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dspbranchengine.h"
#include "dspbranchaudiooutput.h"
#include "dspstreammanager.h"
#include "iqbuffer.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr double SourceRate = 192000.0;
constexpr double SourceCenter = 100000000.0;
constexpr double Offset = 24000.0;
constexpr double ChannelRate = 96000.0;
constexpr std::size_t SourceSamples = 12288;
const QString LocalSink = QStringLiteral("z-local-out");
const QString RemoteSink = QStringLiteral("a-remote-out");

void requireRemote(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int milliseconds = 600) {
    const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (Clock::now() < end);
    return predicate();
}

struct Capture {
    std::mutex mutex;
    std::vector<DspBranchPcmFrame> frames;
    bool overflow = false;
    Capture() { frames.reserve(256); }
    void append(const DspBranchPcmFrame &frame) {
        if (frame.pcm.isEmpty()) return; // Reset fences are not PCM delivery.
        std::lock_guard<std::mutex> lock(mutex);
        if (frames.size() < 256) frames.push_back(frame);
        else overflow = true;
    }
    std::vector<DspBranchPcmFrame> snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        requireRemote(!overflow, "Remote isolation PCM capture exceeded its bound");
        return frames;
    }
    std::size_t count(const QString &sink) {
        std::lock_guard<std::mutex> lock(mutex);
        requireRemote(!overflow, "Remote isolation PCM capture exceeded its bound");
        return std::size_t(std::count_if(frames.cbegin(), frames.cend(),
            [&](const DspBranchPcmFrame &frame) { return frame.sinkId == sink; }));
    }
};

struct SyntheticQueue {
    double previousRate = IqBuffer::sampleRateEstimate();
    std::size_t blocks = 0;
    std::vector<float> iq;
    explicit SyntheticQueue(std::uint64_t epoch) : iq(SourceSamples * 2) {
        IqBuffer::clear(epoch);
        IqBuffer::setSampleRateEstimate(0.0);
        // Exactly 64 modulation periods per block, so reusing it remains continuous.
        for (std::size_t i = 0; i < SourceSamples; ++i) {
            const double t = double(i) / SourceRate;
            const double phase = 6.28318530717958647692 * Offset * t +
                2.0 * std::sin(6.28318530717958647692 * 1000.0 * t);
            iq[2*i] = float(0.4 * std::cos(phase));
            iq[2*i+1] = float(0.4 * std::sin(phase));
        }
    }
    ~SyntheticQueue() { IqBuffer::clear(); IqBuffer::setSampleRateEstimate(previousRate); }
    void publish(const RadioSettings &source) {
        IqBuffer::BlockMetadata metadata;
        metadata.valid = true; metadata.centerFrequencyHz = source.centerFrequency;
        requireRemote(IqBuffer::publish(iq.data(), iq.size(), true, true, 0, &metadata),
            "Remote isolation fixture could not publish synthetic IQ");
        ++blocks;
    }
    void untouched() const {
        requireRemote(IqBuffer::queuedBlocks() == blocks && IqBuffer::queuedFloatCount() == blocks * iq.size(),
            "Remote/local branches consumed the main source IQ queue");
    }
};

RadioSettings sourceSettings(double center = SourceCenter) {
    RadioSettings source;
    source.inputMode = INPUT_RF; source.sampleRate = SourceRate;
    source.centerFrequency = center; source.listeningFrequency = center + Offset;
    source.audioEnabled = false;
    return source;
}

DspGraphPlan audioPlan(bool remote = true, double center = SourceCenter) {
    QJsonArray blocks{QJsonObject{{"id", "source"}, {"type", "iq_source"}}}, edges;
    for (const auto &id : {QStringLiteral("a-remote"), QStringLiteral("z-local")}) {
        if (!remote && id == "a-remote") continue;
        blocks.append(QJsonObject{{"id", id}, {"type", "ddc_channelizer"}, {"settings", QJsonObject{
            {"enabled", true}, {"externalProcessing", id == "a-remote"}, {"frequencyHz", center + Offset},
            {"bandwidthHz", 12500.0}, {"outputRateHz", ChannelRate}}}});
        blocks.append(QJsonObject{{"id", id + "-demod"}, {"type", "branch_demodulator"},
            {"settings", QJsonObject{{"modulation", MOD_NFM}, {"bandwidthHz", 12500.0},
                {"lowPassHz", 3000.0}, {"squelchDb", -160.0}}}});
        blocks.append(QJsonObject{{"id", id + "-out"}, {"type", "branch_audio_output"},
            {"settings", QJsonObject{{"enabled", true}}}});
        edges.append(QJsonObject{{"source", "source"}, {"target", id}});
        edges.append(QJsonObject{{"source", id}, {"target", id + "-demod"}});
        edges.append(QJsonObject{{"source", id + "-demod"}, {"target", id + "-out"}});
    }
    const auto plan = compileDspGraph({{"blocks", blocks}, {"connections", edges}});
    requireRemote(plan.errors.isEmpty() && plan.branches.size() == (remote ? 2 : 1),
        "Remote isolation graph did not compile");
    return plan;
}

// All sockets and protocol barriers belong to the test's QCoreApplication thread.
// Engine callbacks only take Capture's lock; they never re-enter the engine.
struct HeldPeer {
    struct Connection {
        QTcpSocket *socket = nullptr;
        QByteArray buffer, reply;
        bool hello = false;
        DspStreamProcessor processor;
    };
    struct Request {
        std::size_t connection = 0, samples = 0;
        std::uint64_t generation = 0, first = 0;
        bool gap = false;
    };
    QTcpServer server;
    DspStreamEndpoint endpoint;
    std::vector<std::unique_ptr<Connection>> connections;
    std::vector<Request> requests;
    QString error;
    bool holdHandshake = true, holdReply = true;

    HeldPeer() {
        requireRemote(server.listen(QHostAddress::LocalHost, 0), "Remote isolation peer could not bind loopback");
        endpoint.enabled = true; endpoint.address = QStringLiteral("127.0.0.1");
        endpoint.port = server.serverPort(); endpoint.token = QString(64, 'r');
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                auto connection = std::make_unique<Connection>();
                connection->socket = server.nextPendingConnection();
                connection->socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                connection->socket->setReadBufferSize(16 + DspStreamProtocol::MaxJsonBytes + DspStreamProtocol::MaxPayloadBytes);
                const auto index = connections.size();
                auto *socket = connection->socket;
                connections.push_back(std::move(connection));
                QObject::connect(socket, &QTcpSocket::readyRead, &server, [this, index] { receive(index); });
                receive(index);
            }
        });
    }
    ~HeldPeer() {
        for (auto &c : connections) {
            QObject::disconnect(c->socket, nullptr, &server, nullptr);
            c->socket->abort();
            delete c->socket;
        }
    }
    void send(Connection &c, const QByteArray &packet) {
        // Cancellation is allowed to close a peer before its delayed reply is released.
        if (c.socket->state() != QAbstractSocket::ConnectedState) return;
        if (c.socket->write(packet) != packet.size()) error = QStringLiteral("Loopback reply write failed");
        c.socket->flush();
    }
    void receive(std::size_t index) {
        auto &c = *connections[index];
        c.buffer.append(c.socket->readAll());
        while (error.isEmpty()) {
            QJsonObject header; QByteArray payload;
            const int parsed = DspStreamProtocol::extract(c.buffer, header, payload, error);
            if (parsed <= 0) return;
            if (header.value("type") == "hello") {
                if (!payload.isEmpty() || header.value("version").toInt() != 1 || header.value("token") != endpoint.token) {
                    error = QStringLiteral("Invalid synthetic peer handshake"); return;
                }
                c.hello = holdHandshake;
                if (!holdHandshake) send(c, DspStreamProtocol::encode({{"type", "hello"}, {"version", 1}}));
            } else {
                DspStreamBlock block;
                if (!DspStreamProtocol::decodeRequest(header, payload, block, error)) return;
                if (block.stream != "a-remote" || block.sampleRate != ChannelRate || block.iq.size()/2 > SourceSamples) {
                    error = QStringLiteral("Remote worker received source IQ or a local branch"); return;
                }
                requests.push_back({index, block.iq.size()/2, block.generation, block.firstSample, block.gap});
                const auto reply = DspStreamProtocol::response(block, c.processor.process(block));
                if (holdReply) {
                    if (!c.reply.isEmpty()) { error = QStringLiteral("More than one remote request in flight"); return; }
                    c.reply = reply;
                } else send(c, reply);
            }
        }
    }
    bool helloHeld() const {
        return std::any_of(connections.cbegin(), connections.cend(), [](const auto &c) { return c->hello; });
    }
    bool replyHeld() const {
        return std::any_of(connections.cbegin(), connections.cend(), [](const auto &c) { return !c->reply.isEmpty(); });
    }
    bool closed(std::size_t index) const {
        return connections[index]->socket->state() == QAbstractSocket::UnconnectedState;
    }
    void releaseHandshake() {
        holdHandshake = false;
        for (auto &c : connections) if (c->hello) {
            c->hello = false; send(*c, DspStreamProtocol::encode({{"type", "hello"}, {"version", 1}}));
        }
    }
    void releaseReplies() {
        holdReply = false;
        for (auto &c : connections) if (!c->reply.isEmpty()) {
            send(*c, c->reply); c->reply.clear();
        }
    }
    void healthy() const { requireRemote(error.isEmpty(), "Synthetic remote peer rejected the engine protocol"); }
};

void publishLocal(SyntheticQueue &queue, Capture &capture, const RadioSettings &source) {
    const auto count = capture.count(LocalSink);
    queue.publish(source);
    requireRemote(waitUntil([&] { return capture.count(LocalSink) > count; }),
        "Stalled external processing blocked local PCM delivery");
    queue.untouched();
}

void continuousLocal(const std::vector<DspBranchPcmFrame> &frames, std::size_t begin, std::uint64_t generation) {
    bool found = false;
    for (std::size_t i = begin; i < frames.size(); ++i) if (frames[i].sinkId == LocalSink) {
        found = true;
        requireRemote(frames[i].generation == generation && !frames[i].reset &&
            std::abs(frames[i].pcm.size()/2 - int(SourceSamples * 48000 / SourceRate)) <= 2,
            "Remote stall/endpoint change reset local PCM or interrupted its sample clock");
    }
    requireRemote(found, "No continuous local PCM was observed");
}

void boundedQueue(const DspBranchExecutionStats &stats) {
    requireRemote(stats.remoteWorkerStarted && stats.pendingRemoteBlocks <= 2 &&
        stats.pendingRemoteBlocks <= 16 && stats.pendingRemoteBytes <=
            stats.pendingRemoteBlocks * DspStreamProtocol::MaxSamples * 2 * sizeof(float),
        "Remote pending queue exceeded its per-branch block/byte bound");
}

void testLazyLocalWorker() {
    SyntheticQueue queue(18201); Capture capture; DspBranchEngine engine;
    engine.setAudioConsumer([&](const DspBranchPcmFrame &f) { capture.append(f); });
    engine.configure(audioPlan(false), sourceSettings(), true);
    for (int i = 0; i < 3; ++i) publishLocal(queue, capture, sourceSettings());
    const auto stats = engine.executionStats();
    requireRemote(!stats.remoteWorkerStarted && stats.pendingRemoteBlocks == 0 &&
        stats.pendingRemoteBytes == 0 && stats.droppedRemoteBlocks == 0,
        "Local-only graph started an external worker or queued remote work");
}

void testStalledRemoteAndOverflow() {
    HeldPeer peer; SyntheticQueue queue(18202); Capture capture; DspBranchEngine engine;
    const auto source = sourceSettings(); const auto plan = audioPlan();
    engine.setAudioConsumer([&](const DspBranchPcmFrame &f) { capture.append(f); });
    engine.setExternalEndpoint(peer.endpoint); engine.configure(plan, source, true);
    requireRemote(!engine.executionStats().remoteWorkerStarted, "External worker started before the first post-DDC job");
    publishLocal(queue, capture, source);
    requireRemote(waitUntil([&] { return peer.helloHeld(); }), "External handshake did not reach the hold barrier");
    const auto firstLocal = capture.snapshot().front();
    for (int i = 0; i < 5; ++i) {
        publishLocal(queue, capture, source); boundedQueue(engine.executionStats());
    }
    requireRemote(peer.requests.empty() && capture.count(RemoteSink) == 0,
        "Held handshake unexpectedly delivered remote PCM");
    peer.releaseHandshake();
    requireRemote(waitUntil([&] { return peer.replyHeld(); }), "External work did not reach the reply barrier");
    const auto before = engine.executionStats().droppedRemoteBlocks;
    for (int i = 0; i < 6; ++i) {
        publishLocal(queue, capture, source); boundedQueue(engine.executionStats());
    }
    const auto full = engine.executionStats();
    requireRemote(full.pendingRemoteBlocks == 2 && full.pendingRemoteBytes > 0 && full.droppedRemoteBlocks > before &&
        peer.requests.size() == 1 && capture.count(RemoteSink) == 0, "Held reply did not exercise bounded remote queue dropping");
    continuousLocal(capture.snapshot(), 1, firstLocal.generation);
    peer.releaseReplies();
    requireRemote(waitUntil([&] { return capture.count(RemoteSink) >= 2 && engine.executionStats().pendingRemoteBlocks == 0; }),
        "External branch did not recover from queue overflow");
    bool skipped = false;
    for (std::size_t i = 1; i < peer.requests.size(); ++i) {
        const auto &previous = peer.requests[i-1]; const auto &next = peer.requests[i];
        if (next.first > previous.first + previous.samples) {
            skipped = true; requireRemote(next.gap, "Dropped remote IQ resumed without an explicit gap");
        }
    }
    requireRemote(skipped, "Remote overflow test observed no skipped sample range");
    const auto delivered = capture.snapshot();
    int remoteFrames = 0;
    for (const auto &frame : delivered) if (frame.sinkId == RemoteSink) {
        if (++remoteFrames == 2) requireRemote(frame.reset, "Remote audio did not reset after dropped post-DDC IQ");
    }
    peer.holdReply = true;
    publishLocal(queue, capture, source);
    requireRemote(waitUntil([&] { return peer.replyHeld(); }), "Stop test could not hold a new remote reply");
    for (int i = 0; i < 3; ++i) publishLocal(queue, capture, source);
    requireRemote(engine.executionStats().pendingRemoteBlocks == 2, "Stop test did not populate the remote queue");
    engine.configure(plan, source, false);
    const auto stopped = engine.executionStats();
    requireRemote(stopped.pendingRemoteBlocks == 0 && stopped.pendingRemoteBytes == 0 &&
        stopped.droppedRemoteBlocks >= full.droppedRemoteBlocks, "Stopping retained remote jobs or reset cumulative drops");
    requireRemote(waitUntil([&] { return peer.closed(peer.requests.back().connection); }, 500),
        "Disabling the engine did not cancel its outstanding remote wait");
    peer.healthy(); queue.untouched();
}

void testEndpointContinuity() {
    HeldPeer oldPeer, newPeer;
    oldPeer.holdHandshake = newPeer.holdHandshake = false;
    SyntheticQueue queue(18203); Capture capture; DspBranchEngine engine;
    const auto source = sourceSettings();
    engine.setAudioConsumer([&](const DspBranchPcmFrame &f) { capture.append(f); });
    engine.setExternalEndpoint(oldPeer.endpoint); engine.configure(audioPlan(), source, true);
    publishLocal(queue, capture, source);
    requireRemote(waitUntil([&] { return oldPeer.replyHeld(); }), "Endpoint test did not hold the old reply");
    const auto oldConnection = oldPeer.requests.back().connection;
    const auto frames = capture.snapshot(); const auto begin = frames.size();
    engine.setExternalEndpoint(newPeer.endpoint);
    oldPeer.releaseReplies();
    for (int i = 0; i < 3; ++i) publishLocal(queue, capture, source);
    requireRemote(waitUntil([&] { return oldPeer.closed(oldConnection) && newPeer.replyHeld(); }),
        "Endpoint change did not cancel the old wait and connect the new peer");
    continuousLocal(capture.snapshot(), begin, frames.back().generation);
    requireRemote(capture.count(RemoteSink) == 0, "Old endpoint reply produced stale remote PCM");
    newPeer.releaseReplies();
    requireRemote(waitUntil([&] { return capture.count(RemoteSink) > 0; }), "New endpoint did not produce remote PCM");
    oldPeer.healthy(); newPeer.healthy(); queue.untouched();
}

void testGenerationCancellation(int mode) {
    HeldPeer peer; peer.holdHandshake = false;
    SyntheticQueue queue(18204 + std::uint64_t(mode)); Capture capture; DspBranchEngine engine;
    auto source = sourceSettings(); auto plan = audioPlan();
    engine.setAudioConsumer([&](const DspBranchPcmFrame &f) { capture.append(f); });
    engine.setExternalEndpoint(peer.endpoint); engine.configure(plan, source, true);
    publishLocal(queue, capture, source);
    requireRemote(waitUntil([&] { return peer.replyHeld(); }), "Cancellation test did not hold an old-generation reply");
    const auto old = peer.requests.back();
    for (int i = 0; i < 3; ++i) publishLocal(queue, capture, source);
    requireRemote(engine.executionStats().pendingRemoteBlocks == 2, "Cancellation test did not populate the remote queue");
    if (mode == 0) { source = sourceSettings(SourceCenter + 1000.0); plan = audioPlan(true, source.centerFrequency); }
    else if (mode == 1) plan = audioPlan(false);
    else source.simplifiedAudioChannelizer = true;
    engine.configure(plan, source, true);
    const auto begin = capture.snapshot().size();
    const auto cancelled = engine.executionStats();
    requireRemote(cancelled.pendingRemoteBlocks == 0 && cancelled.pendingRemoteBytes == 0,
        "Retune/removal/simplified mode retained queued remote IQ");
    peer.releaseReplies();
    requireRemote(waitUntil([&] { return peer.closed(old.connection); }, 500),
        "Generation change did not cancel an outstanding remote wait");
    if (mode == 2) {
        queue.publish(source);
        requireRemote(!waitUntil([&] { return capture.snapshot().size() != begin; }, 80),
            "Simplified mode delivered stale or new branch PCM");
    } else {
        publishLocal(queue, capture, source);
        if (mode == 0) requireRemote(waitUntil([&] { return capture.count(RemoteSink) > 0; }),
            "Retuned external branch did not recover");
        waitUntil([] { return false; }, 20);
    }
    const auto frames = capture.snapshot();
    for (std::size_t i = begin; i < frames.size(); ++i)
        requireRemote(frames[i].generation > old.generation && (mode == 0 || frames[i].sinkId != RemoteSink),
            "Retune/removal published stale-generation or removed-branch PCM");
    for (const auto &frame : engine.takeFrames())
        requireRemote(mode != 2 && frame.generation > old.generation && (mode == 0 || frame.id != "a-remote"),
            "Retune/removal/simplified mode published stale branch status");
    peer.healthy(); queue.untouched();
}

void testStopCancellation(bool handshake) {
    HeldPeer peer; peer.holdHandshake = handshake;
    SyntheticQueue queue(handshake ? 18208 : 18207); Capture capture;
    auto engine = std::make_unique<DspBranchEngine>();
    engine->setAudioConsumer([&](const DspBranchPcmFrame &f) { capture.append(f); });
    engine->setExternalEndpoint(peer.endpoint); engine->configure(audioPlan(), sourceSettings(), true);
    publishLocal(queue, capture, sourceSettings());
    requireRemote(waitUntil([&] { return handshake ? peer.helloHeld() : peer.replyHeld(); }),
        "Stop test did not reach its remote wait barrier");
    const auto started = Clock::now();
    engine.reset();
    requireRemote(Clock::now() - started < std::chrono::milliseconds(600),
        "Engine stop waited for the full remote handshake/reply timeout");
    requireRemote(waitUntil([&] { return peer.closed(0); }), "Stopped engine retained its remote socket");
    peer.healthy(); queue.untouched();
}
}

bool runDspBranchRemoteTests() {
    const auto started = Clock::now();
    testLazyLocalWorker();
    testStalledRemoteAndOverflow();
    testEndpointContinuity();
    for (int mode = 0; mode < 3; ++mode) testGenerationCancellation(mode);
    testStopCancellation(false);
    testStopCancellation(true);
    std::cout << "DSP remote branch: lazy worker, local PCM isolation, bounded queue/gaps, endpoint continuity and cancellation passed ("
              << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count() << " ms)\n";
    return true;
}
