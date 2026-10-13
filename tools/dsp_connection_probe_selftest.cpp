#include "dspconnectionprobe.h"
#include "dspstreammanager.h"
#include "dspstreamtransport_p.h"
#include "spectrumcomputesession.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace DspConnectionProbeProtocol;
constexpr int ShortDeadlineMs = 3000;
constexpr int ProbeDeadlineMs = 25000;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate()) return true;
        QThread::msleep(2);
    } while (timer.elapsed() < timeoutMs);
    return predicate();
}

DspStreamEndpoint syntheticEndpoint(quint16 port = 1) {
    DspStreamEndpoint endpoint;
    endpoint.address = QStringLiteral("127.0.0.1");
    endpoint.port = port;
    endpoint.token = QString(64, QLatin1Char('p'));
    return endpoint;
}

void noSecrets(const DspConnectionProbeResult &result, const QString &token) {
    if (token.isEmpty()) return;
    require(!result.detail.contains(token) && !result.errorCode.contains(token),
            "Probe diagnostic exposed a session token");
}

DspConnectionProbeResult finish(DspConnectionProbe &probe, const DspStreamEndpoint &endpoint,
                                int timeoutMs = ShortDeadlineMs) {
    require(waitUntil([&] { return !probe.running(); }, timeoutMs), "Probe exceeded its test deadline");
    const auto result = probe.result();
    noSecrets(result, endpoint.token);
    return result;
}

void failure(const DspConnectionProbeResult &result, const char *code) {
    require(result.stage == DspProbeStage::Failed && result.errorCode == QLatin1String(code),
            "Probe returned the wrong failure classification");
    require(result.uploadBytes == 0 && result.downloadBytes == 0,
            "Rejected preflight exchanged synthetic throughput data");
}

struct LoopbackServer {
    DspComputeServer server;
    DspStreamEndpoint endpoint = syntheticEndpoint();

    LoopbackServer() {
        require(server.start(endpoint.address, 0, endpoint.token, 64), "Probe server start failed");
        require(waitUntil([&] { return server.port() != 0 || !server.running(); }, ShortDeadlineMs) &&
                server.port() != 0, "Probe server bind failed");
        endpoint.port = server.port();
    }
    ~LoopbackServer() { server.requestStop(); server.stop(); }
};

void successTest() {
    LoopbackServer loopback;
    DspConnectionProbe probe;
    require(!probe.running() && probe.result().stage == DspProbeStage::Idle, "Probe was not initially idle");
    // Preflight must not require enabling the compute route first.
    loopback.endpoint.enabled = false;
    require(probe.start(loopback.endpoint), "Loopback probe start failed");
    const auto result = finish(probe, loopback.endpoint, ProbeDeadlineMs);
    require(result.stage == DspProbeStage::Complete && result.errorCode.isEmpty() && result.detail.isEmpty(),
            "Synthetic loopback probe did not complete");
    require(result.uploadBytes > 0 && result.uploadBytes <= MaxBytes && result.uploadBytes % ChunkBytes == 0 &&
            result.downloadBytes > 0 && result.downloadBytes <= MaxBytes && result.downloadBytes % ChunkBytes == 0,
            "Probe synthetic byte counts exceeded their bounds");
    require(result.pingCount == PingCount, "Probe did not measure every latency sample");
    for (double value : {result.connectMs, result.handshakeMs, result.rttMinMs, result.rttMedianMs,
                         result.rttP95Ms, result.jitterMs, result.uploadMBps, result.downloadMBps})
        require(std::isfinite(value) && value >= 0, "Probe returned invalid timing or throughput statistics");
    require(result.rttMinMs <= result.rttMedianMs && result.rttMedianMs <= result.rttP95Ms,
            "Probe latency percentiles are not ordered");
    require(result.uploadMBps > 0 && result.downloadMBps > 0, "Completed probe has no measured throughput");
    require(result.mainFftMaxLength == SpectrumComputeProtocol::maxLengthForBudget(64),
            "Probe lost the worker memory capability");
    const auto again = probe.result();
    require(again.stage == result.stage && again.uploadBytes == result.uploadBytes &&
            again.downloadBytes == result.downloadBytes && again.rttMedianMs == result.rttMedianMs,
            "Completed probe snapshot changed unexpectedly");
    std::cout << "Connection probe synthetic loopback and latency statistics: passed\n";
}

void closeTo(double actual, double expected) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= 1e-9 * (std::max)(1.0, std::abs(expected)),
            "Connection traffic estimate differs from its known value");
}

void estimateTests() {
    DspConnectionProbeResult measured;
    measured.stage = DspProbeStage::Complete;
    measured.uploadMBps = 100;
    measured.downloadMBps = 80;
    measured.rttMedianMs = 2;
    measured.mainFftMaxLength = 1048576;
    auto estimate = estimateDspConnection(measured, 1000000, 100000, 20, 0, 100);
    closeTo(estimate.continuousUploadMBps, 8);
    closeTo(estimate.windowUploadMBps, 40);
    closeTo(estimate.spectrumDownloadMBps, 20);
    closeTo(estimate.requestedFramesPerSecond, 50);
    closeTo(estimate.continuousFramesPerSecond, 10);
    closeTo(estimate.continuousSpectrumDownloadMBps, 4);
    closeTo(estimate.transportMsPerWindow, 800000.0 / 75000 + 400000.0 / 60000 + 6);
    closeTo(estimate.networkFramesPerSecond, 1000 / estimate.transportMsPerWindow);
    require(estimate.memoryFits && estimate.windowsSnapshotFits && !estimate.windowsFit && estimate.continuousFits,
            "Window and continuous feasibility did not use their own cadence");
    estimate = estimateDspConnection(measured, 1000000, 100000, 50, 0, 100);
    closeTo(estimate.requestedFramesPerSecond, 20);
    closeTo(estimate.continuousFramesPerSecond, 10);
    require(estimate.windowsFit && estimate.continuousFits, "Known feasible slower-window estimate rejected");

    estimate = estimateDspConnection(measured, 1000000, 100000, 20, 50, 100);
    closeTo(estimate.requestedFramesPerSecond, 50);
    closeTo(estimate.windowUploadMBps, 40);
    closeTo(estimate.spectrumDownloadMBps, 20);
    closeTo(estimate.continuousFramesPerSecond, 20);
    closeTo(estimate.continuousSpectrumDownloadMBps, 8);
    estimate = estimateDspConnection(measured, 1000000, 100000, 20, 90, 100);
    closeTo(estimate.requestedFramesPerSecond, 50);
    closeTo(estimate.continuousFramesPerSecond, 50);
    closeTo(estimate.continuousSpectrumDownloadMBps, 20);
    // Continuous IQ uploads only the new hop, not another complete window.
    require(!estimate.windowsFit && estimate.continuousFits &&
            estimate.continuousFramesPerSecond > estimate.networkFramesPerSecond,
            "Continuous feasibility reused the full-window transport limit");
    estimate = estimateDspConnection(measured, 1000000, 100000, 200, 50, 100);
    closeTo(estimate.requestedFramesPerSecond, 5);
    closeTo(estimate.continuousFramesPerSecond, 5);
    closeTo(estimate.windowUploadMBps, 4);
    closeTo(estimate.spectrumDownloadMBps, 2);
    closeTo(estimate.continuousSpectrumDownloadMBps, 2);

    estimate = estimateDspConnection(measured, 1000000, 100000, 20, 0, 5);
    closeTo(estimate.transportMsPerWindow, 800000.0 / 5000 + 400000.0 / 60000 + 6);
    require(estimate.memoryFits && !estimate.windowsFit && !estimate.continuousFits,
            "Estimate ignored the upload budget");
    estimate = estimateDspConnection(measured, 10000000, 100000, 100, 0, 100);
    require(estimate.windowsFit && !estimate.continuousFits, "Estimate lost continuous-upload bandwidth headroom");

    const int twoChunks = SpectrumComputeProtocol::ChunkSamples * 2;
    estimate = estimateDspConnection(measured, 1048576, twoChunks, 1, 0, 100);
    closeTo(estimate.transportMsPerWindow, twoChunks * 8.0 / 75000 + twoChunks * 4.0 / 60000 + 8);
    auto constrained = measured;
    constrained.downloadMBps = 10;
    estimate = estimateDspConnection(constrained, 1000000, 100000, 20, 0, 100);
    require(!estimate.windowsFit && estimate.continuousFits,
            "Continuous feasibility reused window-mode download traffic");
    constrained = measured;
    constrained.mainFftMaxLength = 99999;
    estimate = estimateDspConnection(constrained, 1000000, 100000, 20, 0, 100);
    require(!estimate.memoryFits && !estimate.windowsFit && !estimate.continuousFits,
            "Estimate ignored the remote memory cap");
    constrained = measured;
    constrained.mainFftMaxLength = 100000;
    require(estimateDspConnection(constrained, 1000000, 100000, 20, 0, 100).memoryFits,
            "Estimate rejected an FFT exactly at the memory cap");
    constrained.rttMedianMs = 100;
    estimate = estimateDspConnection(constrained, 1000000, 100000, 20, 0, 100);
    require(!estimate.windowsFit && !estimate.continuousFits, "Estimate ignored the latency-limited frame rate");

    for (const int overlap : {-10, 0, 50, 95, 110}) {
        estimate = estimateDspConnection(measured, 1000000, 100000, 1, overlap, 100);
        closeTo(estimate.requestedFramesPerSecond, 1000);
        closeTo(estimate.windowUploadMBps, 800);
        closeTo(estimate.spectrumDownloadMBps, 400);
    }
    closeTo(estimateDspConnection(measured, 1000000, 100000, 1, -10, 100).continuousFramesPerSecond, 10);
    closeTo(estimateDspConnection(measured, 1000000, 100000, 1, 110, 100).continuousFramesPerSecond, 200);

    constexpr int snapshotCap = 8388608;
    constrained = measured;
    constrained.mainFftMaxLength = snapshotCap + 1;
    estimate = estimateDspConnection(constrained, 1000000, snapshotCap, 2000, 0, 100);
    require(estimate.memoryFits && estimate.windowsSnapshotFits && estimate.windowsFit && estimate.continuousFits,
            "Window exactly at the local snapshot cap was rejected");
    estimate = estimateDspConnection(constrained, 1000000, snapshotCap + 1, 2000, 0, 100);
    require(estimate.memoryFits && !estimate.windowsSnapshotFits && !estimate.windowsFit && estimate.continuousFits,
            "Local snapshot cap was ignored or incorrectly applied to continuous mode");
    for (const auto &invalid : {
             estimateDspConnection(measured, std::numeric_limits<double>::quiet_NaN(), 100000, 20, 0, 100),
             estimateDspConnection(measured, 0, 100000, 20, 0, 100),
             estimateDspConnection(measured, 1000000, 511, 20, 0, 100),
             estimateDspConnection(measured, 1000000, 100000, 0, 0, 100),
             estimateDspConnection(measured, 1000000, 100000, 20, 0, 0)}) {
        require(invalid.requestedFramesPerSecond == 0 && invalid.continuousUploadMBps == 0 &&
                invalid.continuousFramesPerSecond == 0 && invalid.continuousSpectrumDownloadMBps == 0 &&
                !invalid.memoryFits && !invalid.windowsSnapshotFits && !invalid.windowsFit && !invalid.continuousFits,
                "Invalid inputs produced a usable connection estimate");
    }
    for (const bool failed : {false, true}) {
        constrained = measured;
        if (failed) constrained.stage = DspProbeStage::Failed;
        else constrained.downloadMBps = 0;
        estimate = estimateDspConnection(constrained, 1000000, 100000, 20, 0, 100);
        closeTo(estimate.continuousUploadMBps, 8);
        closeTo(estimate.requestedFramesPerSecond, 50);
        closeTo(estimate.continuousFramesPerSecond, 10);
        closeTo(estimate.continuousSpectrumDownloadMBps, 4);
        require(estimate.windowsSnapshotFits && !estimate.memoryFits && !estimate.windowsFit && !estimate.continuousFits &&
                estimate.networkFramesPerSecond == 0, "Unavailable measurement was treated as usable");
    }
    std::cout << "Connection probe traffic, headroom, latency and memory estimates: passed\n";
}

void invalidEndpointTests() {
    std::vector<DspStreamEndpoint> endpoints;
    for (const char *address : {"", "localhost", "not-an-address", "0.0.0.0", "::", "239.1.2.3"}) {
        auto endpoint = syntheticEndpoint();
        endpoint.address = QLatin1String(address);
        endpoints.push_back(endpoint);
    }
    auto endpoint = syntheticEndpoint(0);
    endpoints.push_back(endpoint);
    for (const auto &token : {QString(), QString(31, QLatin1Char('a')), QString(129, QLatin1Char('a')),
                             QString(32, QLatin1Char(' ')), QString(32, QChar(1)), QString(32, QChar(0x100))}) {
        endpoint = syntheticEndpoint();
        endpoint.token = token;
        endpoints.push_back(endpoint);
    }
    DspConnectionProbe probe;
    for (const auto &invalid : endpoints) {
        require(probe.start(invalid), "Invalid-input probe could not start its validation");
        const auto result = finish(probe, invalid);
        failure(result, "invalid_endpoint");
        require(result.pingCount == 0 && result.connectMs == 0, "Invalid endpoint attempted a connection");
    }
}

DspStreamBlock halfWindow(std::uint64_t first) {
    DspStreamBlock block;
    block.stream = QStringLiteral("probe-ddc-continuity");
    block.generation = 7;
    block.epoch = 3;
    block.firstSample = first;
    block.sampleRate = 48000;
    block.centerHz = 100000000;
    block.returnIq = true;
    block.ffts.append({QStringLiteral("probe-ddc-fft"), 1024, FFT_WINDOW_HANN, 20});
    block.iq.resize(1024);
    for (std::size_t i = 0; i < 512; ++i) {
        const double phase = 6.28318530717958647692 * 1500 * double(first + i) / block.sampleRate;
        block.iq[2 * i] = float(0.5 * std::cos(phase));
        block.iq[2 * i + 1] = float(0.5 * std::sin(phase));
    }
    return block;
}

void busyAndAuthenticationTests() {
    LoopbackServer loopback;
    DspStreamManager ddc;
    auto endpoint = loopback.endpoint;
    endpoint.enabled = true;
    ddc.configure(endpoint);
    auto block = halfWindow(0);
    auto before = ddc.process(block, true);
    require(before.error.isEmpty() && before.iq && *before.iq == block.iq && before.spectra.empty(),
            "DDC first half-window failed before the probe");
    DspConnectionProbe probe;
    auto wrongToken = endpoint;
    wrongToken.token = QString(64, QLatin1Char('x'));
    require(probe.start(wrongToken), "Authentication probe could not start");
    failure(finish(probe, wrongToken), "authentication_failed");
    require(probe.start(endpoint), "Busy probe could not start");
    failure(finish(probe, endpoint), "busy");
    block = halfWindow(512);
    const auto after = ddc.process(block, true);
    require(after.error.isEmpty() && after.iq && *after.iq == block.iq && after.spectra.size() == 1,
            "Rejected probe interrupted DDC or discarded its retained half-window");
    const auto &frame = after.spectra.front();
    require(frame.error.isEmpty() && frame.levels.size() == 1024 && frame.frequencies.size() == 1024,
            "DDC spectrum after busy rejection is malformed");
    const auto peak = std::max_element(frame.levels.begin(), frame.levels.end());
    require(std::abs(*peak + 6.0206) < 0.02 &&
            std::abs(frame.frequencies[std::size_t(peak - frame.levels.begin())] - 100001500.0) < 16,
            "Probe rejection corrupted retained DDC samples");
    ddc.clear();
    std::cout << "Connection probe authentication/busy rejection preserves DDC history and IQ: passed\n";
}

QByteArray oversizedHeader(bool jsonTooLarge) {
    QByteArray bytes("OBDSP001", 8);
    bytes.resize(16);
    qToLittleEndian<quint32>(jsonTooLarge ? quint32(DspStreamProtocol::MaxJsonBytes) + 1 : 2,
                            reinterpret_cast<uchar *>(bytes.data() + 8));
    qToLittleEndian<quint32>(jsonTooLarge ? 0 : quint32(DspStreamProtocol::MaxPayloadBytes) + 1,
                            reinterpret_cast<uchar *>(bytes.data() + 12));
    return bytes;
}

// All fake-peer QObjects stay on the calling Qt thread. Only the production
// probe owns a background socket; waitUntil pumps this bounded scripted peer.
class ScriptedPeer {
public:
    enum class Mode { Unsupported, WrongVersion, HelloPayload, UnknownError, OversizedJson, OversizedPayload,
                      BadPong, HoldHandshake, HoldLatency, HoldUpload, HoldDownload };
    explicit ScriptedPeer(Mode mode) : mode(mode) {
        require(server.listen(QHostAddress::LocalHost, 0), "Scripted probe peer could not bind");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            if (socket) { valid = false; server.close(); return; }
            socket.reset(server.nextPendingConnection());
            if (!socket) { valid = false; return; }
            socket->setReadBufferSize(DspStreamTransport::PacketLimit);
            QObject::connect(socket.get(), &QTcpSocket::readyRead, &server, [this] { read(); });
            read();
        });
    }
    DspStreamEndpoint endpoint() const { return syntheticEndpoint(server.serverPort()); }
    bool held() const { return atBarrier; }
    bool healthy() const { return valid && sawHello; }

private:
    QTcpServer server;
    std::unique_ptr<QTcpSocket> socket;
    Mode mode;
    QByteArray buffer;
    bool valid = true, sawHello = false, atBarrier = false;
    int pings = 0, uploadBytes = 0, uploadSequence = 0;

    void reject() { valid = false; socket->abort(); }
    void raw(const QByteArray &bytes) {
        if (bytes.isEmpty() || socket->bytesToWrite() + bytes.size() > DspStreamTransport::PacketLimit ||
            socket->write(bytes) != bytes.size()) reject();
    }
    void write(const QJsonObject &header, const QByteArray &payload = {}) {
        raw(DspStreamProtocol::encode(header, payload));
    }
    void read() {
        buffer.append(socket->readAll());
        if (buffer.size() > 2 * DspStreamTransport::PacketLimit) { reject(); return; }
        while (valid && !buffer.isEmpty()) {
            QJsonObject header;
            QByteArray payload;
            QString error;
            const int result = DspStreamProtocol::extract(buffer, header, payload, error);
            if (result == 0) return;
            if (result < 0) { reject(); return; }
            respond(header, payload);
        }
    }
    void respond(const QJsonObject &header, const QByteArray &payload) {
        const auto type = header.value("type").toString();
        if (!sawHello) {
            if (type != "hello" || header.value("version") != 1 || header.value("purpose") != "connection_probe" ||
                header.value("token") != endpoint().token || !payload.isEmpty()) { reject(); return; }
            sawHello = true;
            if (mode == Mode::HoldHandshake) { atBarrier = true; return; }
            if (mode == Mode::OversizedJson || mode == Mode::OversizedPayload) {
                raw(oversizedHeader(mode == Mode::OversizedJson)); return;
            }
            if (mode == Mode::UnknownError) {
                // A peer-supplied error must not echo arbitrary text or credentials.
                write({{"type", "error"}, {"code", "unknown"}, {"error", endpoint().token}}); return;
            }
            QJsonObject hello{{"type", "hello"}, {"version", mode == Mode::WrongVersion ? 2 : 1},
                              {"mainFftVersion", 1}, {"mainFftMaxLength", 1048576}};
            if (mode != Mode::Unsupported) hello["connectionProbeVersion"] = Version;
            write(hello, mode == Mode::HelloPayload ? QByteArray("unexpected") : QByteArray());
        } else if (type == "probe_ping") {
            if (!payload.isEmpty() || header.value("id") != pings || pings >= PingCount) { reject(); return; }
            if (mode == Mode::HoldLatency) { atBarrier = true; return; }
            write({{"type", "probe_pong"}, {"id", mode == Mode::BadPong ? pings + 1 : pings}});
            ++pings;
        } else if (type == "probe_upload_begin") {
            if (pings != PingCount || !payload.isEmpty()) { reject(); return; }
            if (mode == Mode::HoldUpload) { atBarrier = true; return; }
            write({{"type", "probe_upload_ready"}});
        } else if (type == "probe_upload_data") {
            if (header.value("id") != uploadSequence++ || payload != QByteArray(ChunkBytes, char(0x5a)) ||
                uploadBytes > MaxBytes - payload.size()) { reject(); return; }
            uploadBytes += int(payload.size());
        } else if (type == "probe_upload_end") {
            if (!payload.isEmpty() || uploadBytes <= 0 || header.value("bytes") != uploadBytes) { reject(); return; }
            write({{"type", "probe_upload_done"}, {"bytes", uploadBytes}});
        } else if (type == "probe_download" && mode == Mode::HoldDownload && payload.isEmpty()) {
            atBarrier = true;
        } else {
            reject();
        }
    }
};

void fakePeerTests() {
    using Mode = ScriptedPeer::Mode;
    struct Case { Mode mode; const char *code; };
    for (const auto &test : {Case{Mode::Unsupported, "unsupported"}, Case{Mode::WrongVersion, "protocol"},
                             Case{Mode::HelloPayload, "protocol"}, Case{Mode::UnknownError, "protocol"},
                             Case{Mode::OversizedJson, "handshake"}, Case{Mode::OversizedPayload, "handshake"},
                             Case{Mode::BadPong, "protocol"}}) {
        ScriptedPeer peer(test.mode);
        DspConnectionProbe probe;
        const auto endpoint = peer.endpoint();
        require(probe.start(endpoint), "Scripted probe could not start");
        failure(finish(probe, endpoint), test.code);
        require(peer.healthy(), "Scripted peer did not receive the expected authenticated probe hello");
    }
    std::cout << "Connection probe unsupported capability and malformed/oversized replies: passed\n";
}

void cancellationTests() {
    using Mode = ScriptedPeer::Mode;
    struct Case { Mode mode; DspProbeStage stage; };
    for (const auto &test : {Case{Mode::HoldHandshake, DspProbeStage::Handshake},
                             Case{Mode::HoldLatency, DspProbeStage::Latency},
                             Case{Mode::HoldUpload, DspProbeStage::Upload},
                             Case{Mode::HoldDownload, DspProbeStage::Download}}) {
        ScriptedPeer peer(test.mode);
        DspConnectionProbe probe;
        const auto endpoint = peer.endpoint();
        require(probe.start(endpoint), "Cancellation probe could not start");
        require(waitUntil([&] { return peer.held() || !probe.running(); }, ProbeDeadlineMs) &&
                peer.healthy() && peer.held() && probe.running() && probe.result().stage == test.stage,
                "Probe did not reach the requested cancellation barrier");
        require(!probe.start(endpoint), "An active probe accepted a second start");
        QElapsedTimer timer;
        timer.start();
        probe.cancel();
        require(timer.elapsed() < 1000, "Probe cancel blocked its caller");
        const auto result = finish(probe, endpoint);
        require(timer.elapsed() < ShortDeadlineMs && result.stage == DspProbeStage::Cancelled &&
                result.errorCode.isEmpty() && result.detail.isEmpty(), "Probe cancellation was not bounded and clean");
        auto invalid = endpoint;
        invalid.port = 0;
        require(probe.start(invalid), "Cancelled probe could not restart");
        failure(finish(probe, invalid), "invalid_endpoint");
    }
    ScriptedPeer peer(Mode::HoldHandshake);
    auto probe = std::make_unique<DspConnectionProbe>();
    require(probe->start(peer.endpoint()), "Destructor cancellation probe could not start");
    require(waitUntil([&] { return peer.held() || !probe->running(); }, ShortDeadlineMs) && peer.held(),
            "Destructor cancellation did not reach its barrier");
    QElapsedTimer timer;
    timer.start();
    probe.reset();
    require(timer.elapsed() < ShortDeadlineMs, "Probe destruction did not cancel its stalled socket promptly");
    std::cout << "Connection probe cancellation at handshake/latency/upload/download and restart: passed\n";
}

void malformedRequestTests() {
    LoopbackServer loopback;
    for (const bool missingToken : {false, true}) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, loopback.endpoint.port);
        require(socket.waitForConnected(ShortDeadlineMs), "Malformed-auth test could not connect");
        QJsonObject hello{{"type", "hello"}, {"version", 1}, {"purpose", "connection_probe"}};
        if (!missingToken) hello["token"] = 42;
        require(DspStreamTransport::send(socket, DspStreamProtocol::encode(hello)), "Malformed-auth write failed");
        QByteArray buffer, payload;
        QJsonObject reply;
        require(DspStreamTransport::receive(socket, buffer, reply, payload, ShortDeadlineMs) && payload.isEmpty() &&
                reply.value("type") == "error" && reply.value("code") == "authentication_failed",
                "Server accepted malformed probe authentication");
        require(!reply.value("error").toString().contains(loopback.endpoint.token), "Server rejection exposed its token");
    }
    for (const bool jsonTooLarge : {false, true}) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, loopback.endpoint.port);
        require(socket.waitForConnected(ShortDeadlineMs), "Oversized-request test could not connect");
        require(DspStreamTransport::send(socket, oversizedHeader(jsonTooLarge)), "Oversized-request write failed");
        QElapsedTimer timer;
        timer.start();
        while (socket.state() != QAbstractSocket::UnconnectedState && timer.elapsed() < ShortDeadlineMs) {
            socket.waitForReadyRead(25);
            require(socket.readAll().isEmpty(), "Server replied to an oversized unauthenticated packet");
        }
        require(socket.state() == QAbstractSocket::UnconnectedState, "Server did not promptly reject oversized framing");
    }
    std::cout << "Connection probe malformed authentication and oversized request rejection: passed\n";
}
}

bool dspConnectionProbeSelfTest() {
    try {
        require(QCoreApplication::instance() != nullptr, "Connection probe self-test needs a QCoreApplication");
        estimateTests();
        invalidEndpointTests();
        successTest();
        busyAndAuthenticationTests();
        fakePeerTests();
        cancellationTests();
        malformedRequestTests();
        std::cout << "DSP connection probe self-test passed\n";
        return true;
    } catch (const std::exception &error) {
        // All test failures use fixed text, never endpoint or peer-provided data.
        std::cerr << "DSP connection probe self-test failed: " << error.what() << '\n';
        return false;
    }
}
