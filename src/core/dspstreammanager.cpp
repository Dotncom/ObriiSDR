#include "dspstreammanager.h"
#include "diagnosticlogging.h"
#include "spectrumcomputesession.h"
#include "dspconnectionprobe.h"
#include "dspstreamtransport_p.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <array>
#include <functional>

namespace {
using namespace DspStreamTransport;
class ComputeListener final : public QTcpServer {
public:
    std::function<void(qintptr)> accepted;
protected:
    void incomingConnection(qintptr descriptor) override { accepted(descriptor); }
};
bool validToken(const QString &token) {
    if (token.size() < 32 || token.size() > 128) return false;
    for (QChar c : token) if (c.unicode() < 33 || c.unicode() > 126) return false;
    return true;
}
bool tokenMatches(const QString &a, const QString &b) {
    if (a.size() != b.size()) return false;
    ushort difference = 0;
    for (int i = 0; i < a.size(); ++i) difference |= a[i].unicode() ^ b[i].unicode();
    return difference == 0;
}
bool connectClient(QTcpSocket &socket, const QHostAddress &address, quint16 port,
                   const std::atomic_bool *cancel) {
    // waitForConnected() aborts its attempt on timeout; polling it at 25 ms
    // would incorrectly shorten the 700 ms connect budget to the first slice.
    QEventLoop loop;
    QTimer slice;
    slice.setInterval(25);
    slice.setTimerType(Qt::PreciseTimer);
    QElapsedTimer deadline;
    QObject::connect(&socket, &QTcpSocket::stateChanged, &loop, [&loop](QAbstractSocket::SocketState state) {
        if (state == QAbstractSocket::ConnectedState || state == QAbstractSocket::UnconnectedState) loop.quit();
    });
    QObject::connect(&slice, &QTimer::timeout, &loop, [&] {
        if ((cancel && cancel->load(std::memory_order_acquire)) || deadline.elapsed() >= 700) loop.quit();
    });
    if (cancel && cancel->load(std::memory_order_acquire)) return false;
    deadline.start();
    socket.connectToHost(address, port);
    if (socket.state() != QAbstractSocket::ConnectedState && socket.state() != QAbstractSocket::UnconnectedState) {
        slice.start();
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    return socket.state() == QAbstractSocket::ConnectedState;
}
}

struct DspStreamManager::Impl {
    DspStreamEndpoint endpoint;
    DspStreamProcessor local;
    std::unique_ptr<QTcpSocket> socket;
    QByteArray buffer;
    QString lastError;
    std::chrono::steady_clock::time_point retry{};
    void disconnect() { if (socket) socket->abort(); socket.reset(); buffer.clear(); }
};
DspStreamManager::DspStreamManager() : impl(std::make_unique<Impl>()) {}
DspStreamManager::~DspStreamManager() = default;
void DspStreamManager::clear() {
    impl->local.clear(); impl->disconnect(); impl->retry = {}; impl->lastError.clear();
}
void DspStreamManager::configure(const DspStreamEndpoint &endpoint) {
    if (impl->endpoint == endpoint) return;
    clear(); impl->endpoint = endpoint;
}
DspStreamResult DspStreamManager::process(const DspStreamBlock &block, bool external,
                                         const std::atomic_bool *cancel) {
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_acquire); };
    const auto cancellation = [this] {
        impl->disconnect();
        impl->retry = {};
        impl->lastError.clear();
        DspStreamResult result;
        result.error = QStringLiteral("DSP stream request cancelled");
        return result;
    };
    if (cancelled()) return cancellation();
    if (!external) return impl->local.process(block);
    DspStreamResult result;
    result.error = DspStreamProtocol::validate(block);
    if (cancelled()) return cancellation();
    if (!result.error.isEmpty()) return result;
    const auto &endpoint = impl->endpoint;
    QHostAddress address;
    if (!endpoint.enabled || !validToken(endpoint.token) || !endpoint.port || !address.setAddress(endpoint.address)) {
        result.error = "Configure and enable the external DSP connection"; return result;
    }
    const auto failure = [this, &result, &cancelled, &cancellation](const QString &reason) {
        if (cancelled()) return cancellation();
        if (impl->lastError != reason && fobosVerboseLoggingEnabled())
            qDebug().noquote() << "[DSP stream]" << impl->endpoint.address << impl->endpoint.port << reason;
        impl->lastError = reason;
        impl->disconnect(); impl->retry = std::chrono::steady_clock::now()+std::chrono::seconds(1);
        result.error = reason; return result;
    };
    // A new IQ block arrives much faster than the retry interval. Keep the cause
    // visible instead of replacing it with an uninformative waiting message.
    if (std::chrono::steady_clock::now() < impl->retry) { result.error = impl->lastError; return result; }
    bool connected = false;
    const auto write = [&](const QByteArray &packet) {
        return !cancelled() && send(*impl->socket, packet, cancel);
    };
    if (!impl->socket || impl->socket->state() != QAbstractSocket::ConnectedState) {
        impl->disconnect(); impl->socket = std::make_unique<QTcpSocket>();
        impl->socket->setReadBufferSize(PacketLimit);
        const bool opened = connectClient(*impl->socket, address, endpoint.port, cancel);
        if (cancelled()) return cancellation();
        if (!opened) return failure(
            QStringLiteral("External DSP TCP %1:%2: %3 (socket error %4)")
                .arg(endpoint.address).arg(endpoint.port)
                .arg(impl->socket->state() == QAbstractSocket::ConnectingState
                    ? QStringLiteral("Connection timed out") : impl->socket->errorString())
                .arg(int(impl->socket->state() == QAbstractSocket::ConnectingState
                    ? QAbstractSocket::SocketTimeoutError : impl->socket->error())));
        impl->socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        QJsonObject hello; QByteArray payload;
        if (!write(DspStreamProtocol::encode({{"type", "hello"}, {"version", 1}, {"token", endpoint.token}})) ||
            !receive(*impl->socket, impl->buffer, hello, payload, 1000, cancel))
            return failure("External DSP TCP connected, but handshake failed: check the compute service and session token");
        if (cancelled()) return cancellation();
        if (hello.value("type") == "error" && hello.value("code") == "authentication_failed")
            return failure("External DSP rejected the session token; copy the current token from the compute PC");
        if (hello.value("type") == "error" && hello.value("code") == "busy")
            return failure("External DSP worker is busy with a connection test; retry after the test");
        if (!payload.isEmpty() || hello.value("type") != "hello" || hello.value("version").toInt() != 1)
            return failure("External DSP authentication/protocol failed");
        if (fobosVerboseLoggingEnabled())
            qDebug().noquote() << "[DSP stream] authenticated" << endpoint.address << endpoint.port;
        impl->lastError.clear();
        connected = true;
    }
    auto request = DspStreamProtocol::request(block);
    request["gap"] = block.gap || connected;
    QJsonObject response; QByteArray payload;
    if (!write(DspStreamProtocol::encode(request, DspStreamProtocol::floats(block.iq))) ||
        !receive(*impl->socket, impl->buffer, response, payload, 1500, cancel)) return failure("External DSP timeout/disconnection; stream interrupted");
    if (cancelled()) return cancellation();
    if (!DspStreamProtocol::decodeResponse(block, response, payload, result)) return failure("External DSP returned invalid or stale data");
    if (cancelled()) return cancellation();
    return result;
}

struct DspComputeServer::Impl {
    std::atomic<bool> stopping{false}, active{false};
    std::atomic<quint16> boundPort{0};
    std::thread thread;
    mutable std::mutex mutex;
    QString message = QStringLiteral("Stopped");
    QString lastDiagnosticMessage;
    std::atomic<bool> mainFftBusy{false};
    std::mutex admissionMutex;
    int computeSessions = 0;
    bool probeActive = false;
    void status(const QString &text) {
        std::lock_guard<std::mutex> lock(mutex);
        message = text;
        if (!text.startsWith("Blocks ") && lastDiagnosticMessage != text) {
            lastDiagnosticMessage = text;
            if (fobosVerboseLoggingEnabled()) qDebug().noquote() << "[DSP compute]" << text;
        }
    }
    void serve(qintptr descriptor, const QString &token, int memoryMiB) {
        QTcpSocket socket;
        if (!socket.setSocketDescriptor(descriptor)) return;
        QTcpSocket *peer = &socket;
        struct MainLease {
            std::atomic<bool> &busy; bool owns = false;
            ~MainLease() { if (owns) busy = false; }
        } lease{mainFftBusy};
        struct Admission {
            Impl &owner;
            bool acquired = false, probe = false;
            ~Admission() {
                if (!acquired) return;
                std::lock_guard<std::mutex> lock(owner.admissionMutex);
                if (probe) owner.probeActive = false;
                else --owner.computeSessions;
            }
        } admission{*this};
        try {
            peer->setReadBufferSize(PacketLimit); peer->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            QByteArray buffer, payload; QJsonObject json;
            if (!receive(*peer, buffer, json, payload, 1500, &stopping) || !payload.isEmpty() ||
                json.value("type") != "hello" || json.value("version").toInt() != 1) {
                status("Listening; rejected an incomplete or incompatible handshake"); return;
            }
            if (!tokenMatches(json.value("token").toString(), token)) {
                send(*peer, DspStreamProtocol::encode({{"type", "error"}, {"code", "authentication_failed"},
                    {"error", "Session token rejected"}}), &stopping);
                status("Listening; rejected a client with a different session token"); return;
            }
            admission.probe = json.value("purpose") == "connection_probe";
            {
                std::lock_guard<std::mutex> lock(admissionMutex);
                if (!probeActive && (!admission.probe || computeSessions == 0)) {
                    admission.acquired = true;
                    if (admission.probe) probeActive = true;
                    else ++computeSessions;
                }
            }
            if (!admission.acquired) {
                send(*peer, DspStreamProtocol::encode({{"type", "error"}, {"code", "busy"},
                    {"error", "Stop active compute sessions or wait for the connection test"}}), &stopping);
                return;
            }
            if (!send(*peer, DspStreamProtocol::encode({{"type", "hello"}, {"version", 1},
                {"connectionProbeVersion", DspConnectionProbeProtocol::Version},
                {"mainFftPipelineChunks", SpectrumComputeProtocol::MaxPipelineChunks},
                {"mainFftVersion", 1}, {"mainFftMaxLength", SpectrumComputeProtocol::maxLengthForBudget(memoryMiB)}}), &stopping)) {
                status("Listening; handshake reply failed"); return;
            }
            if (admission.probe) {
                status("Connection test; synthetic data only");
                DspConnectionProbeProtocol::serve(socket, buffer, stopping);
                status("Listening; previous stream ended");
                return;
            }
            status("Client connected");
            DspStreamProcessor processor;
            std::unique_ptr<SpectrumComputeSession> spectrum;
            quint64 blocks = 0, samples = 0;
            QElapsedTimer report; report.start();
            while (!stopping && receive(*peer, buffer, json, payload, 5000, &stopping)) {
                if (json.value("type").toString().startsWith("main_fft_")) {
                    if (!spectrum) {
                        bool available = false;
                        if (!mainFftBusy.compare_exchange_strong(available, true)) {
                            send(*peer, DspStreamProtocol::encode({{"type", "error"}, {"code", "busy"}, {"error", "Main FFT worker is busy"}}), &stopping); break;
                        }
                        lease.owns = true; spectrum = std::make_unique<SpectrumComputeSession>(memoryMiB);
                    }
                    const auto reply = [this, peer](const QJsonObject &header, const QByteArray &bytes) {
                        return !stopping && send(*peer, DspStreamProtocol::encode(header, bytes), &stopping);
                    };
                    if (!spectrum->handle(json, payload, reply)) break;
                    if (json.value("type") == "main_fft_iq") { samples += payload.size()/8; ++blocks; }
                    if (report.elapsed() >= 250) {
                        status(QStringLiteral("Blocks %1 | samples %2 | main FFT").arg(blocks).arg(samples)); report.restart();
                    }
                    continue;
                }
                DspStreamBlock block; QString error;
                if (!DspStreamProtocol::decodeRequest(json, payload, block, error)) {
                    send(*peer, DspStreamProtocol::encode({{"type", "error"}, {"error", error}}), &stopping); break;
                }
                auto result = processor.process(block);
                if (!send(*peer, DspStreamProtocol::response(block, result), &stopping)) break;
                samples += block.iq.size()/2U; ++blocks;
                if (blocks == 1 || report.elapsed() >= 250) {
                    status(QStringLiteral("Blocks %1 | samples %2 | DSP %3 ms").arg(blocks).arg(samples).arg(result.processingMs, 0, 'f', 2));
                    report.restart();
                }
            }
        } catch (const std::exception &) { status("Compute service resource limit"); }
        peer->abort();
    }
    void run(QString address, quint16 port, QString token, int memoryMiB) {
        ComputeListener server;
        struct Client { std::thread thread; std::atomic<bool> done{false}; ~Client() { if (thread.joinable()) thread.join(); } };
        std::array<std::unique_ptr<Client>, 2> clients;
        struct StopClientsOnExit {
            std::atomic<bool> &stop;
            ~StopClientsOnExit() { stop = true; }
        } stopClientsOnExit{stopping};
        server.accepted = [&](qintptr descriptor) {
            // Retuning reconnects while the old socket may still be unwinding its
            // short receive wait. Hold only this descriptor, never another worker.
            QElapsedTimer grace; grace.start();
            while (!stopping) {
                for (auto &client : clients) if (client && client->done) client.reset();
                for (auto &client : clients) if (!client) {
                    client = std::make_unique<Client>(); auto *state = client.get();
                    try {
                        state->thread = std::thread([this, descriptor, token, memoryMiB, state]() {
                            serve(descriptor, token, memoryMiB); state->done = true;
                        });
                    } catch (...) { client.reset(); QTcpSocket rejected; rejected.setSocketDescriptor(descriptor); }
                    return;
                }
                if (grace.elapsed() >= 100) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            QTcpSocket rejected; rejected.setSocketDescriptor(descriptor); rejected.abort();
        };
        if (!server.listen(QHostAddress(address), port)) {
            status(QStringLiteral("Cannot bind compute service: %1:%2: %3")
                .arg(address).arg(port).arg(server.errorString())); active = false; return;
        }
        boundPort = server.serverPort(); status("Listening; no receiver is opened");
        while (!stopping) {
            server.waitForNewConnection(100);
            for (auto &client : clients) if (client && client->done) client.reset();
        }
        server.close(); boundPort = 0;
        for (auto &client : clients) client.reset();
        active = false; status("Stopped");
    }
};
DspComputeServer::DspComputeServer() : impl(std::make_unique<Impl>()) {}
DspComputeServer::~DspComputeServer() { stop(); }
bool DspComputeServer::start(const QString &address, quint16 port, const QString &token, int mainFftMemoryMiB) {
    QHostAddress parsed;
    if (running() || !parsed.setAddress(address) || !validToken(token) || parsed.isMulticast()) return false;
    stop(); impl->stopping = false; impl->active = true; impl->status("Starting");
    try {
        impl->thread = std::thread([this, address, port, token, mainFftMemoryMiB]() {
            try { impl->run(address, port, token, mainFftMemoryMiB); }
            catch (const std::exception &) {
                impl->boundPort = 0; impl->active = false;
                impl->status("Compute service resource limit");
            }
        });
    } catch (const std::exception &) {
        impl->active = false; impl->status("Cannot start compute service"); return false;
    }
    return true;
}
void DspComputeServer::stop() {
    impl->stopping = true;
    if (impl->thread.joinable()) impl->thread.join();
    impl->active = false; impl->boundPort = 0;
}
void DspComputeServer::requestStop() { impl->stopping = true; impl->status("Stopping"); }
bool DspComputeServer::running() const { return impl->active.load(); }
quint16 DspComputeServer::port() const { return impl->boundPort.load(); }
QString DspComputeServer::status() const { std::lock_guard<std::mutex> lock(impl->mutex); return impl->message; }
