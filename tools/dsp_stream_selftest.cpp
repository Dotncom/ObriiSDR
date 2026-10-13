#include "dspstreammanager.h"
#include "dspstreamtransport_p.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>

bool dspStreamRecorderSelfTest();
bool dspConnectionProbeSelfTest();

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
DspStreamBlock tone(std::size_t count = 4096) {
    DspStreamBlock b; b.stream = "channel"; b.generation = 1; b.epoch = 9;
    b.sampleRate = 96000; b.centerHz = 100100000; b.returnIq = true;
    b.ffts.append({"fft", 2048, FFT_WINDOW_HANN, 20}); b.iq.resize(count*2);
    for (std::size_t i = 0; i < count; ++i) {
        const double p = 6.28318530717958647692*1500*i/b.sampleRate;
        b.iq[i*2] = float(0.5*std::cos(p)); b.iq[i*2+1] = float(0.5*std::sin(p));
    }
    return b;
}
void checkTone(const DspStreamResult &r) {
    require(r.error.isEmpty() && r.spectra.size() == 1, "Tone result missing");
    const auto &f = r.spectra.front();
    const auto peak = std::max_element(f.levels.begin(), f.levels.end());
    require(std::abs(*peak+6.0206) < 0.01 && std::abs(f.frequencies[peak-f.levels.begin()]-100101500) < 10, "Tone amplitude/frequency wrong");
    require(f.rbwHz > 70 && f.rbwHz < 71, "ENBW/RBW incorrect");
}
void codecTests() {
    auto b = tone(); auto packet = DspStreamProtocol::encode(DspStreamProtocol::request(b), DspStreamProtocol::floats(b.iq));
    QByteArray partial, payload; QJsonObject json; QString error;
    for (int i = 0; i < packet.size()-1; ++i) {
        partial.append(packet[i]);
        require(DspStreamProtocol::extract(partial, json, payload, error) == 0, "Fragmentation parser consumed incomplete data");
    }
    partial.append(packet.back());
    require(DspStreamProtocol::extract(partial, json, payload, error) == 1 && partial.isEmpty(), "Complete packet rejected");
    DspStreamBlock decoded;
    require(DspStreamProtocol::decodeRequest(json, payload, decoded, error) && decoded.iq == b.iq, "IQ wire roundtrip changed samples");
    b.firstSample = (std::uint64_t(1)<<60)+17;
    require(DspStreamProtocol::decodeRequest(DspStreamProtocol::request(b), payload, decoded, error) && decoded.firstSample == b.firstSample, "64-bit sample clock lost precision");
    json["firstSample"] = -1;
    require(!DspStreamProtocol::decodeRequest(json, payload, decoded, error), "Negative sample position accepted");
    partial = QByteArray("OBDSP001", 8)+QByteArray(8, char(0xff));
    require(DspStreamProtocol::extract(partial, json, payload, error) == -1, "Oversized packet accepted");
    partial = QByteArray("WRONG001", 8)+QByteArray(8, char(0));
    require(DspStreamProtocol::extract(partial, json, payload, error) == -1, "Bad protocol signature accepted");
    b.iq[0] = std::numeric_limits<float>::quiet_NaN();
    require(!DspStreamProtocol::validate(b).isEmpty(), "Nonfinite samples accepted");
    b = tone(); b.ffts[0].length = 1500;
    require(!DspStreamProtocol::validate(b).isEmpty(), "Invalid FFT length accepted");
    std::cout << "Framing, finite bounds, metadata and 64-bit sample clock: passed\n";
}
void processorTests() {
    DspStreamProcessor processor;
    auto b = tone(); auto r = processor.process(b); checkTone(r);
    QByteArray response = DspStreamProtocol::response(b, r), payload;
    QJsonObject json; QString error; DspStreamResult decoded;
    require(DspStreamProtocol::extract(response, json, payload, error) == 1 &&
        DspStreamProtocol::decodeResponse(b, json, payload, decoded) && decoded.iq && *decoded.iq == b.iq, "Result roundtrip failed");
    checkTone(decoded);
    json["generation"] = "0";
    require(!DspStreamProtocol::decodeResponse(b, json, payload, decoded), "Stale generation accepted");
    processor.clear(); b = tone(1024);
    require(processor.process(b).spectra.empty(), "FFT emitted before full observation");
    b.firstSample += 1024; checkTone(processor.process(b));
    b.firstSample += 1024; b.gap = true;
    require(processor.process(b).spectra.empty(), "Gap retained old FFT history");
    b.gap = false; b.firstSample += 1024; checkTone(processor.process(b));
    b.firstSample += 1024; ++b.epoch;
    require(processor.process(b).spectra.empty(), "Epoch retained old FFT history");
    b.firstSample += 5000;
    require(processor.process(b).spectra.empty(), "Noncontiguous sample positions retained history");
    processor.clear(); b = tone(1024);
    require(processor.process(b).spectra.empty(), "Unexpected initial FFT");
    b.returnIq = false; b.firstSample += 1024;
    checkTone(processor.process(b));
    b.stream = "second"; b.firstSample = 0;
    require(processor.process(b).error.isEmpty(), "Second stream rejected");
    for (int i = 2; i < 8; ++i) {
        b.stream = QString("extra-%1").arg(i);
        require(processor.process(b).error.isEmpty(), "Eight-stream admission failed");
    }
    b.stream = "ninth";
    require(!processor.process(b).error.isEmpty(), "Ninth stream exceeded the history budget");
    processor.clear(); b = tone(4096);
    for (int i = 0; i < 19; ++i) {
        b.firstSample = std::uint64_t(i)*4096;
        r = processor.process(b);
        require(r.error.isEmpty(), "Ring buffer wrap failed");
    }
    QThread::msleep(25); b.firstSample += 4096;
    checkTone(processor.process(b));
    std::cout << "Window/FFT calibration, returned IQ, gaps, epochs and stale replies: passed\n";
}
void networkTests(const DspStreamEndpoint &endpoint, bool native = false) {
    DspStreamManager manager; manager.configure(endpoint);
    auto b = tone(); const auto remote = manager.process(b, true); checkTone(remote);
    require(remote.iq && *remote.iq == b.iq, "External IQ differs");
    auto bad = endpoint; bad.token = QString(64, 'x');
    manager.configure(bad);
    const auto rejected = manager.process(b, true);
    require(!rejected.error.isEmpty(), "Invalid session token accepted");
    if (native) require(rejected.error.contains("rejected the session token"), "Native token rejection reason was lost");
    require(manager.process(b, true).error == rejected.error, "Retry hid the authentication failure");
    manager.configure(endpoint); b.generation++;
    checkTone(manager.process(b, true));
    manager.clear();
    std::cout << "Authenticated external FFT/IQ, rejected wrong token and reconnect: passed\n";
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int ms = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}

enum class CancelPhase { Connect, Hello, Result };

// Owns only the peer sockets on the test thread. The manager and all its Qt
// client operations live on a separate thread; only its atomic flag is shared.
struct CancellationPeer {
    struct Client { QTcpSocket *socket; QByteArray buffer; int ordinal; int socks = 0; };
    QTcpServer listener;
    std::vector<std::unique_ptr<Client>> clients;
    DspStreamProcessor processor;
    CancelPhase phase;
    bool reached = false, firstClosed = false;
    QString error;

    explicit CancellationPeer(CancelPhase p) : phase(p) {
        listener.setProxy(QNetworkProxy::NoProxy);
        require(listener.listen(QHostAddress::LocalHost, 0), "Cancellation peer bind failed");
        QObject::connect(&listener, &QTcpServer::newConnection, &listener, [this] {
            auto *socket = listener.nextPendingConnection();
            socket->setReadBufferSize(DspStreamTransport::PacketLimit);
            auto client = std::make_unique<Client>();
            client->socket = socket;
            client->ordinal = int(clients.size());
            auto *state = client.get();
            clients.push_back(std::move(client));
            QObject::connect(socket, &QTcpSocket::readyRead, &listener, [this, state] { receive(*state); });
            if (!state->ordinal)
                QObject::connect(socket, &QTcpSocket::disconnected, &listener, [this] { firstClosed = true; });
            receive(*state);
        });
    }
    DspStreamEndpoint endpoint() const {
        DspStreamEndpoint endpoint;
        endpoint.enabled = true;
        endpoint.address = QStringLiteral("127.0.0.1");
        endpoint.port = listener.serverPort();
        endpoint.token = QString(64, 'c');
        return endpoint;
    }
    void receive(Client &client) {
        client.buffer.append(client.socket->read(DspStreamTransport::PacketLimit - client.buffer.size()));
        if (phase == CancelPhase::Connect) {
            // A real SOCKS5 greeting proves TCP setup is still ConnectingState.
            // Withhold the proxy reply, not the application hello.
            if (!client.ordinal) {
                if (!client.buffer.isEmpty()) {
                    if (quint8(client.buffer[0]) != 5) error = "Missing SOCKS connect greeting";
                    reached = true;
                }
                return;
            }
            // The replacement connection completes SOCKS locally, then serves
            // DSP on the same socket. No connection to the synthetic IP is made.
            if (client.socks == 0) {
                if (client.buffer.size() < 2) return;
                const int greeting = 2 + quint8(client.buffer[1]);
                if (client.buffer.size() < greeting) return;
                client.buffer.remove(0, greeting);
                client.socket->write(QByteArray::fromHex("0500"));
                client.socks = 1;
            }
            if (client.socks == 1) {
                if (client.buffer.size() < 5) return;
                const int type = quint8(client.buffer[3]);
                const int size = type == 1 ? 10 : type == 4 ? 22 : type == 3 ? 7 + quint8(client.buffer[4]) : 0;
                if (!size) { error = "Invalid SOCKS address type"; return; }
                if (client.buffer.size() < size) return;
                client.buffer.remove(0, size);
                client.socket->write(QByteArray::fromHex("050000017f0000010001"));
                client.socks = 2;
            }
        }
        while (error.isEmpty()) {
            QJsonObject header;
            QByteArray payload;
            const int parsed = DspStreamProtocol::extract(client.buffer, header, payload, error);
            if (parsed <= 0) return;
            QByteArray reply;
            const bool hello = header.value("type") == "hello";
            if (hello) {
                reply = DspStreamProtocol::encode({{"type", "hello"}, {"version", 1}});
            } else {
                DspStreamBlock block;
                if (!DspStreamProtocol::decodeRequest(header, payload, block, error)) return;
                reply = DspStreamProtocol::response(block, processor.process(block));
            }
            if (!client.ordinal && ((hello && phase == CancelPhase::Hello) || (!hello && phase == CancelPhase::Result))) {
                reply.truncate(9); // Leave an incomplete frame behind for cancellation to discard.
                reached = true;
            }
            if (client.socket->write(reply) != reply.size()) error = "Cancellation peer reply failed";
        }
    }
};

struct RestoreProxy {
    QNetworkProxy previous = QNetworkProxy::applicationProxy();
    ~RestoreProxy() { QNetworkProxy::setApplicationProxy(previous); }
};

void cancellationTests() {
    std::atomic_bool cancel{true};
    auto b = tone(1024);
    DspStreamManager local;
    const auto cancelledLocal = local.process(b, false, &cancel);
    require(cancelledLocal.error == "DSP stream request cancelled" && !cancelledLocal.iq && cancelledLocal.spectra.empty(),
            "Pre-cancelled local request entered the processor");
    cancel = false;
    require(local.process(b, false, &cancel).spectra.empty(), "Cancelled local request retained IQ history");
    b.firstSample += 1024;
    checkTone(local.process(b, false));
    cancel = true;
    const auto cancelledExternal = local.process(b, true, &cancel);
    require(cancelledExternal.error == cancelledLocal.error && !cancelledExternal.iq && cancelledExternal.spectra.empty(),
            "Pre-cancelled request did not precede endpoint validation");

    for (auto phase : {CancelPhase::Connect, CancelPhase::Hello, CancelPhase::Result}) {
        RestoreProxy restore;
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        CancellationPeer peer(phase);
        auto endpoint = peer.endpoint();
        if (phase == CancelPhase::Connect) {
            // Qt bypasses application proxies for loopback destinations. This
            // documentation-only IP stays entirely inside the local test proxy.
            endpoint.address = QStringLiteral("192.0.2.1");
            QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy,
                QStringLiteral("127.0.0.1"), peer.listener.serverPort()));
        }
        cancel = false;
        std::promise<DspStreamResult> first;
        auto firstResult = first.get_future();
        std::promise<void> resume;
        auto resumed = resume.get_future();
        auto work = std::async(std::launch::async, [&] {
            DspStreamManager manager;
            manager.configure(endpoint);
            auto block = tone();
            block.gap = true;
            first.set_value(manager.process(block, true, &cancel));
            require(resumed.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                    "Cancellation test did not release its worker");
            ++block.generation;
            return manager.process(block, true, &cancel);
        });
        if (!waitUntil([&] { return peer.reached || !peer.error.isEmpty(); }) || !peer.error.isEmpty()) {
            QString detail = QStringLiteral("Cancellation stage %1 barrier not reached; connections=%2; peer=%3")
                .arg(int(phase)).arg(peer.clients.size()).arg(peer.error);
            if (firstResult.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
                detail += QStringLiteral("; client=%1").arg(firstResult.get().error);
            cancel = true;
            resume.set_value();
            throw std::runtime_error(detail.toStdString());
        }
        require(!waitUntil([&] { return firstResult.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, 80),
                "Network wait ended before cancellation (connect budget shortened to one slice)");
        QElapsedTimer latency;
        latency.start();
        cancel.store(true, std::memory_order_release);
        require(waitUntil([&] { return firstResult.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, 400),
                "Network cancellation exceeded its slice bound");
        const auto result = firstResult.get();
        require(result.error == "DSP stream request cancelled" && !result.iq && result.spectra.empty(),
                "Cancellation returned a timeout, partial IQ or a stale spectrum");
        const auto elapsed = latency.elapsed();
        require(waitUntil([&] { return peer.firstClosed; }, 400), "Cancelled transport was not closed");
        cancel.store(false, std::memory_order_release);
        resume.set_value();
        require(waitUntil([&] { return work.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }),
                "Next generation did not finish after cancellation");
        checkTone(work.get());
        require(peer.error.isEmpty(), "Cancellation test peer failed");
        require(peer.clients.size() == 2, "Next generation reused cancelled transport");
        std::cout << "Cancellation stage " << int(phase) << ": " << elapsed
                  << " ms; fresh generation immediately reconnects: passed\n";
    }

    // The same deterministic proxy barrier also tests the uncancelled 700 ms
    // deadline without depending on a firewall or an unroutable external host.
    RestoreProxy restore;
    CancellationPeer proxy(CancelPhase::Connect);
    auto endpoint = proxy.endpoint();
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, endpoint.address, endpoint.port));
    endpoint.address = QStringLiteral("192.0.2.1");
    QElapsedTimer elapsed;
    elapsed.start();
    auto timeout = std::async(std::launch::async, [&] {
        DspStreamManager manager;
        manager.configure(endpoint);
        return manager.process(tone(), true);
    });
    require(waitUntil([&] { return timeout.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, 2000),
            "Connect did not retain a finite deadline");
    const auto result = timeout.get();
    require(proxy.reached && elapsed.elapsed() >= 650 && elapsed.elapsed() < 1600 &&
                result.error.contains("Connection timed out"), "Connect deadline no longer approximately 700 ms");
    std::cout << "Pre-cancelled local/external requests and 700 ms connect deadline: passed\n";
}
void benchmark() {
    constexpr int rounds = 2400;
    auto b = tone(4096); b.returnIq = false; b.ffts.clear();
    DspStreamProcessor direct; DspStreamManager manager;
    const auto run = [&](bool managed) {
        QElapsedTimer timer; timer.start();
        for (int i = 0; i < rounds; ++i) {
            b.firstSample = std::uint64_t(i)*4096;
            const auto r = managed ? manager.process(b, false) : direct.process(b);
            require(r.error.isEmpty(), "Benchmark stream failed");
        }
        return timer.nsecsElapsed()/1e6;
    };
    for (const bool returnIq : {false, true}) {
        b.returnIq = returnIq;
        run(false); run(true);
        std::vector<double> base, routed;
        for (int repeat = 0; repeat < 7; ++repeat) {
            if (repeat%2) { routed.push_back(run(true)); base.push_back(run(false)); }
            else { base.push_back(run(false)); routed.push_back(run(true)); }
        }
        std::sort(base.begin(), base.end()); std::sort(routed.begin(), routed.end());
        std::cout << "Local routing median (7 alternating runs, returnIQ=" << returnIq << "): " << rounds*4096
            << " complex samples, processor=" << base[3] << " ms, manager=" << routed[3]
            << " ms, ratio=" << routed[3]/base[3] << " (not a hardware RX benchmark)\n";
    }
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    std::cout << std::unitbuf;
    try {
        codecTests(); processorTests(); require(dspStreamRecorderSelfTest(), "Recorder tests failed");
        require(dspConnectionProbeSelfTest(), "Connection probe tests failed");
        cancellationTests();
        DspStreamEndpoint endpoint; endpoint.enabled = true;
        if (argc == 3) {
            endpoint.address = QString::fromLocal8Bit(argv[1]); endpoint.port = quint16(QString::fromLocal8Bit(argv[2]).toUInt());
            endpoint.token = qEnvironmentVariable("OBRII_DSP_TOKEN"); networkTests(endpoint);
        } else {
            endpoint.token = QString(64, 'a'); DspComputeServer server;
            require(server.start("127.0.0.1", 0, endpoint.token), "Server start rejected");
            for (int i = 0; i < 100 && !server.port(); ++i) QThread::msleep(10);
            endpoint.port = server.port(); require(endpoint.port != 0, "Server bind failed");
            networkTests(endpoint, true);
            DspStreamManager disconnected; disconnected.configure(endpoint);
            auto b = tone(); checkTone(disconnected.process(b, true));
            QElapsedTimer timer; timer.start(); server.stop();
            require(timer.elapsed() < 1000 && !server.running(), "Server stop not bounded");
            std::cout << "Compute worker stop: " << timer.elapsed() << " ms\n";
            timer.restart(); b.firstSample += b.iq.size()/2;
            const auto lost = disconnected.process(b, true);
            require(!lost.error.isEmpty() && timer.elapsed() < 2500,
                    "Disconnected compute worker did not fail within its deadline");
            require(disconnected.process(b, true).error == lost.error, "Retry hid the disconnection failure");
            DspStreamManager unavailable; unavailable.configure(endpoint);
            const auto refused = unavailable.process(b, true);
            require(refused.error.contains("socket error") && refused.error.contains(QString::number(endpoint.port)),
                    "TCP error lost the endpoint or system error");
            require(unavailable.process(b, true).error == refused.error, "Retry hid the TCP failure");
            b.gap = true; checkTone(disconnected.process(b, false));
            std::cout << "Disconnected external worker leaves local processing available: passed\n";
        }
        benchmark(); std::cout << "DSP stream self-test passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
