#include "commandcontrolserver.h"
#include "commanddispatcher.h"
#include "../src/cli/clicommandadapter.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr int RequestLimit = 64 * 1024;
constexpr int ResponseLimit = 1024 * 1024;

void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void pump() {
    QCoreApplication::processEvents();
    QThread::msleep(1);
}
void until(const std::function<bool()> &condition, int timeoutMs = 4000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        check(timer.elapsed() < timeoutMs, "event-loop test timed out");
        pump();
    }
}
void pumpFor(int durationMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < durationMs) pump();
}
void connectClient(QTcpSocket &socket, const CommandControlServer &server) {
    socket.setReadBufferSize(ResponseLimit + 1);
    socket.connectToHost(QHostAddress::LocalHost, server.port());
    until([&] { return socket.state() == QAbstractSocket::ConnectedState; });
}
QByteArray envelope(const QString &token, const QJsonObject &request) {
    return QJsonDocument(QJsonObject{{"token", token}, {"request", request}}).toJson(QJsonDocument::Compact) + '\n';
}
QJsonObject request(const QString &name, const QJsonObject &args = {}) {
    return {{"command", name}, {"args", args}};
}
struct Reply {
    QByteArray bytes;
    QJsonObject object;
};
Reply collect(QTcpSocket &socket, bool requireResponse = true) {
    Reply reply;
    until([&] {
        reply.bytes += socket.readAll();
        check(reply.bytes.size() <= ResponseLimit, "response exceeded byte limit");
        return socket.state() == QAbstractSocket::UnconnectedState;
    }, 6000);
    reply.bytes += socket.readAll();
    check(reply.bytes.size() <= ResponseLimit, "final response exceeded byte limit");
    if (reply.bytes.isEmpty() && !requireResponse) return reply;
    check(reply.bytes.endsWith('\n') && reply.bytes.count('\n') == 1, "expected one JSON response line");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(reply.bytes, &error);
    check(error.error == QJsonParseError::NoError && document.isObject(), "response is not JSON");
    reply.object = document.object();
    return reply;
}
Reply exchange(CommandControlServer &server, const QByteArray &line, bool requireResponse = true) {
    QTcpSocket socket;
    connectClient(socket, server);
    check(socket.write(line) == line.size(), "client write failed");
    return collect(socket, requireResponse);
}
QJsonObject call(CommandControlServer &server, const QString &name, const QJsonObject &args = {}) {
    return exchange(server, envelope(server.token(), request(name, args))).object;
}
void code(const QJsonObject &reply, const char *expected) {
    if (reply.value("ok").toBool() || reply.value("errorCode") != expected) {
        const auto actual = reply.value("ok").toBool() ? QStringLiteral("<success>") : reply.value("errorCode").toString();
        throw std::runtime_error(std::string("expected ") + expected + ", got " + actual.toStdString());
    }
}
void add(CommandDispatcher &dispatcher, const QString &name, CommandDispatcher::Access access,
         CommandDispatcher::Handler handler, const QJsonObject &schema = {}) {
    CommandDispatcher::Spec spec;
    spec.name = name;
    spec.description = QStringLiteral("Synthetic transport test");
    spec.access = access;
    spec.inputSchema = schema;
    check(dispatcher.add(spec, std::move(handler)), "test command registration failed");
}
bool catalogHas(const QJsonObject &reply, const QString &name) {
    for (const auto &entry : reply.value("commands").toArray())
        if (entry.toObject().value("name") == name) return true;
    return false;
}
void stopClean(CommandControlServer &server) {
    server.stop();
    // Flush deferred deletions only outside socket callbacks/handler reentry.
    until([&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return server.findChildren<QTcpSocket *>().isEmpty() && server.findChildren<QTcpServer *>().isEmpty();
    });
    check(!server.isListening() && !server.port() && server.token().isEmpty() &&
          server.status().value("clients").toInt() == 0, "stop leaked active state");
}

void legacyReplyCompatibility() {
    CommandDispatcher dispatcher(QStringLiteral("legacy-reply-test"));
    const QJsonObject original{{"ok", true}, {"id", "fft-branch-2"},
        {"schemaVersion", 17}, {"command", "original-result"}, {"levelsDbfs", QJsonArray{-95, -12}}};
    int calls = 0;
    registerCliCommandHandlers(dispatcher, [&](const QJsonObject &) { ++calls; return original; }, true);
    const QJsonObject input{{"command", "cli.legacy"}, {"args", QJsonObject{{"request", QJsonObject{{"action", "graph"}}}}},
        {"id", "request-123"}};
    const auto typed = dispatcher.execute(input, CommandDispatcher::Context::human());
    check(typed.value("id") == QJsonValue("request-123") && typed.value("schemaVersion") == QJsonValue(1),
          "typed dispatcher metadata collided with legacy result");
    check(typed.value("legacyResult").toObject() == original && calls == 1, "legacy result fields changed");
    check(cliCommandWireReply(typed, true) == original, "old action-based reply was not preserved");
    check(cliCommandWireReply(typed, false) == typed, "typed compatibility request lost its metadata");
    const auto denied = dispatcher.execute(input, {});
    code(denied, "permission_denied");
    check(cliCommandWireReply(denied, true) == denied && calls == 1, "denied adapter invoked legacy handler");
    const auto basic = dispatcher.execute(request("status"), {});
    check(basic.value("id").isString() && basic.value("command") == QJsonValue("status"),
          "ordinary typed command metadata changed");
}

void authenticationAndPermissions() {
    CommandDispatcher dispatcher(QStringLiteral("control-selftest"));
    int reads = 0, controls = 0, files = 0, transmit = 0, changes = 0;
    auto *owner = QThread::currentThread();
    check(dispatcher.addStandard("status", [&](const QJsonObject &) {
        check(QThread::currentThread() == owner, "handler moved off owner thread");
        ++reads;
        return QJsonObject{{"value", 42}};
    }), "status registration");
    add(dispatcher, "mutate", CommandDispatcher::Access::Control, [&](const QJsonObject &) {
        ++controls; return QJsonObject{{"changed", true}};
    });
    add(dispatcher, "files", CommandDispatcher::Access::Files, [&](const QJsonObject &) {
        ++files; return QJsonObject{};
    });
    add(dispatcher, "transmit", CommandDispatcher::Access::Transmit, [&](const QJsonObject &) {
        ++transmit; return QJsonObject{};
    });
    CommandControlServer server(&dispatcher);
    QObject::connect(&server, &CommandControlServer::statusChanged, [&] { ++changes; });
    check(!server.isListening() && !server.port() && server.token().isEmpty() && !server.allowControl(), "server not idle/read-only by default");
    check(server.findChildren<QTcpServer *>().isEmpty() && server.findChildren<QTcpSocket *>().isEmpty(), "constructor allocated sockets/listeners");
    QString error;
    QTcpServer occupied;
    check(occupied.listen(QHostAddress::LocalHost, 0), "test port reservation failed");
    check(!server.start(occupied.serverPort(), &error) && !error.isEmpty() &&
          !server.isListening() && !server.port() && server.token().isEmpty() &&
          server.findChildren<QTcpServer *>().isEmpty(), "failed listen leaked listener or token");
    occupied.close();
    check(server.start(0, &error) && server.port() && error.isEmpty(), "start failed");
    const auto listeners = server.findChildren<QTcpServer *>();
    check(listeners.size() == 1 && listeners[0]->serverAddress() == QHostAddress(QHostAddress::LocalHost) &&
          listeners[0]->serverAddress().protocol() == QAbstractSocket::IPv4Protocol, "listener is not IPv4 loopback only");
    const QString firstToken = server.token();
    check(firstToken.size() == 64 && firstToken.toLatin1() == QByteArray::fromHex(firstToken.toLatin1()).toHex(), "token is not 256-bit hex");
    check(!QJsonDocument(server.status()).toJson().contains(firstToken.toLatin1()) && !server.status().contains("token"), "status leaked token");
    check(!server.start(0, &error) && server.token() == firstToken, "duplicate start changed session");

    QString badToken = firstToken;
    badToken[0] = badToken[0] == '0' ? '1' : '0';
    const auto denied = exchange(server, envelope(badToken, request("commands"))).object;
    code(denied, "unauthorized");
    check(!denied.contains("commands") && !denied.contains("instanceId") && reads == 0 &&
          server.status().value("authFailures").toInt() == 1 && server.status().value("requests").toInt() == 0,
          "bad token reached dispatcher or exposed catalog");
    code(exchange(server, envelope(firstToken.left(1), request("status"))).object, "unauthorized");
    code(exchange(server, envelope(firstToken + "0", request("status"))).object, "unauthorized");

    auto withId = request("status");
    withId.insert("id", "synthetic-id");
    withId.insert("targetInstanceId", dispatcher.instanceId());
    const auto success = exchange(server, envelope(firstToken, withId)).object;
    check(success.value("ok").toBool() && success.value("value").toInt() == 42 &&
          success.value("id") == "synthetic-id" && success.value("instanceId") == dispatcher.instanceId() && reads == 1,
          "authenticated read failed");
    const auto readCatalog = call(server, "commands");
    check(catalogHas(readCatalog, "status") && !catalogHas(readCatalog, "mutate") &&
          !catalogHas(readCatalog, "files") && !catalogHas(readCatalog, "transmit"), "read catalog exposed privileged commands");
    code(call(server, "mutate"), "permission_denied");
    code(call(server, "files", {{"allowFiles", true}}), "permission_denied");
    code(call(server, "transmit", {{"allowTransmit", true}}), "permission_denied");
    check(!controls && !files && !transmit, "read-only context invoked privileged handler");

    server.setAllowControl(true);
    check(server.allowControl() && call(server, "mutate").value("ok").toBool() && controls == 1, "manual Control permission failed");
    const auto controlCatalog = call(server, "commands");
    check(catalogHas(controlCatalog, "mutate") && !catalogHas(controlCatalog, "files") &&
          !catalogHas(controlCatalog, "transmit"), "Control catalog exposed Files/Transmit");
    code(call(server, "files"), "permission_denied");
    code(call(server, "transmit"), "permission_denied");
    check(!files && !transmit, "Control permission granted Files/Transmit");
    server.setAllowControl(false);
    code(call(server, "mutate"), "permission_denied");
    check(controls == 1, "Control revocation failed");
    const auto audit = call(server, "audit");
    check(!audit.value("events").toArray().isEmpty(), "missing dispatcher audit");
    for (const auto &event : audit.value("events").toArray())
        check(event.toObject().value("origin") == "agent", "request context origin was not agent");
    check(changes > 0 && server.status().value("rejected").toInt() >= 8, "status counters/signals missing");
    stopClean(server);
    check(server.start(), "restart failed");
    check(server.token() != firstToken && !server.status().value("requests").toInt(), "token/counters did not rotate on start");
    code(exchange(server, envelope(firstToken, request("status"))).object, "unauthorized");
    check(call(server, "status").value("ok").toBool(), "fresh token not accepted");
    stopClean(server);
    CommandControlServer missing(nullptr);
    check(!missing.start(0, &error) && !missing.isListening() && !error.isEmpty(), "null dispatcher started listener");
}

void framingAndLimits() {
    CommandDispatcher dispatcher(QStringLiteral("framing-selftest"));
    int calls = 0;
    add(dispatcher, "read", CommandDispatcher::Access::Read, [&](const QJsonObject &) {
        ++calls; return QJsonObject{};
    });
    const QJsonObject echoSchema{{"properties", QJsonObject{{"payload", QJsonObject{{"type", "string"}, {"maxLength", RequestLimit}}}}}};
    add(dispatcher, "echo", CommandDispatcher::Access::Read, [&](const QJsonObject &args) {
        ++calls; return QJsonObject{{"length", args.value("payload").toString().size()}};
    }, echoSchema);
    CommandControlServer server(&dispatcher);
    check(server.start(), "framing server start");
    code(exchange(server, "{broken}\n").object, "invalid_json");
    code(exchange(server, "[]\n").object, "invalid_json");
    code(exchange(server, "\n").object, "invalid_json");
    auto invalid = QJsonObject{{"token", server.token()}, {"request", request("read")}, {"allowControl", true}};
    code(exchange(server, QJsonDocument(invalid).toJson(QJsonDocument::Compact) + '\n').object, "invalid_envelope");
    invalid.remove("allowControl"); invalid.insert("request", "read");
    code(exchange(server, QJsonDocument(invalid).toJson(QJsonDocument::Compact) + '\n').object, "invalid_request");
    auto badRequest = request("read"); badRequest.insert("context", QJsonObject{{"allowFiles", true}});
    code(exchange(server, envelope(server.token(), badRequest)).object, "invalid_request");
    badRequest = request("read"); badRequest.insert("args", QJsonArray{});
    code(exchange(server, envelope(server.token(), badRequest)).object, "invalid_request");
    badRequest = request("read"); badRequest.insert("id", QJsonObject{});
    code(exchange(server, envelope(server.token(), badRequest)).object, "invalid_request");
    badRequest = request("read"); badRequest.insert("origin", "human");
    code(exchange(server, envelope(server.token(), badRequest)).object, "invalid_request");
    code(exchange(server, envelope(server.token(), request("read")) + envelope(server.token(), request("read"))).object,
         "invalid_envelope");
    check(calls == 0, "malformed envelope reached handler");

    QTcpSocket partial;
    connectClient(partial, server);
    const auto line = envelope(server.token(), request("read"));
    partial.write(line.left(7));
    pumpFor(20);
    check(calls == 0 && partial.bytesAvailable() == 0, "partial request dispatched early");
    partial.write(line.mid(7, line.size() - 8));
    pumpFor(20);
    check(calls == 0, "request without newline dispatched");
    partial.write("\n", 1);
    check(collect(partial).object.value("ok").toBool() && calls == 1, "partial request did not assemble");

    const int overhead = envelope(server.token(), request("echo", {{"payload", ""}})).size() - 1;
    const QString payload(RequestLimit - overhead, 'x');
    const auto exact = envelope(server.token(), request("echo", {{"payload", payload}}));
    check(exact.size() == RequestLimit + 1, "boundary fixture size");
    check(exchange(server, exact).object.value("length").toInt() == payload.size() && calls == 2,
          "exact request boundary rejected");
    code(exchange(server, envelope(server.token(), request("echo", {{"payload", payload + 'x'}}))).object, "request_too_large");
    code(exchange(server, QByteArray(RequestLimit + 1, 'x')).object, "request_too_large");
    check(calls == 2, "oversized request invoked handler");
    stopClean(server);
}

void queuedPoliciesAndStop() {
    CommandDispatcher dispatcher(QStringLiteral("queued-policy-selftest"));
    CommandControlServer server(&dispatcher);
    int calls = 0;
    add(dispatcher, "mutate", CommandDispatcher::Access::Control, [&](const QJsonObject &) {
        ++calls;
        return QJsonObject{};
    });
    check(server.start(), "queued-policy server start");
    const auto atNativeRead = [&](const std::function<void()> &action, bool requireResponse = true) {
        QTcpSocket client;
        connectClient(client, server);
        QPointer<QTcpSocket> accepted;
        until([&] {
            for (auto *socket : server.findChildren<QTcpSocket *>()) {
                if (socket->state() == QAbstractSocket::ConnectedState && socket->peerPort() == client.localPort()) {
                    accepted = socket;
                    return true;
                }
            }
            return false;
        });
        QObject::connect(accepted, &QIODevice::channelReadyRead, &server, [action](int) { action(); });
        const auto line = envelope(server.token(), request("mutate"));
        check(client.write(line) == line.size(), "queued policy write");
        return collect(client, requireResponse);
    };
    code(atNativeRead([&] { server.setAllowControl(true); }).object, "permission_denied");
    check(calls == 0 && server.allowControl(), "queued request gained permission after parsing");
    code(atNativeRead([&] { server.setAllowControl(false); }).object, "permission_denied");
    check(calls == 0 && !server.allowControl(), "queued request retained revoked Control");
    server.setAllowControl(true);
    check(call(server, "mutate").value("ok").toBool() && calls == 1, "ordinary allowed queued request failed");
    const auto stopped = atNativeRead([&] { server.stop(); }, false);
    pumpFor(10);
    check(stopped.bytes.isEmpty() && !server.isListening() && calls == 1, "stopped queued request invoked handler");
    stopClean(server);
}

void clientsDeadlinesAndRate() {
    std::cout << "  client capacity\n" << std::flush;
    CommandDispatcher dispatcher(QStringLiteral("limits-selftest"));
    int calls = 0;
    add(dispatcher, "read", CommandDispatcher::Access::Read, [&](const QJsonObject &) {
        ++calls; return QJsonObject{};
    });
    CommandControlServer server(&dispatcher);
    check(server.start(), "client-cap server start");
    std::vector<std::unique_ptr<QTcpSocket>> sockets;
    for (int i = 0; i < 8; ++i) {
        auto socket = std::make_unique<QTcpSocket>();
        connectClient(*socket, server);
        sockets.push_back(std::move(socket));
    }
    until([&] { return server.status().value("clients").toInt() == 8; });
    QTcpSocket extra;
    extra.connectToHost(QHostAddress::LocalHost, server.port());
    until([&] { return server.status().value("rejected").toInt() >= 1 && extra.state() == QAbstractSocket::UnconnectedState; });
    check(server.status().value("clients").toInt() == 8 && !calls, "client cap exceeded or handler called");
    server.stop();
    for (const auto &socket : sockets) until([&] { return socket->state() == QAbstractSocket::UnconnectedState; });
    stopClean(server);

    std::cout << "  partial-input deadline\n" << std::flush;
    check(server.start(), "timeout server start");
    QTcpSocket idle;
    connectClient(idle, server);
    idle.write("{\"token\":", 9);
    QElapsedTimer elapsed; elapsed.start();
    const auto timeout = collect(idle, false);
    check(elapsed.elapsed() >= 1800 && elapsed.elapsed() < 4000 && !calls, "request deadline did not close idle client");
    if (!timeout.bytes.isEmpty()) code(timeout.object, "timeout");
    stopClean(server);

    std::cout << "  rolling rate window\n" << std::flush;
    check(server.start(), "rate server start");
    QElapsedTimer burst;
    burst.start();
    for (int remaining = 20; remaining > 0;) {
        std::vector<std::unique_ptr<QTcpSocket>> batch;
        const int size = remaining < 8 ? remaining : 8;
        for (int i = 0; i < size; ++i) {
            auto socket = std::make_unique<QTcpSocket>();
            connectClient(*socket, server);
            batch.push_back(std::move(socket));
        }
        const auto line = envelope(server.token(), request("read"));
        for (const auto &socket : batch) check(socket->write(line) == line.size(), "rate burst write failed");
        for (const auto &socket : batch) check(collect(*socket).object.value("ok").toBool(), "allowed rate burst rejected");
        // A peer can observe EOF before the server processes its disconnect.
        // Wait for that batch to drain before opening another full-capacity batch.
        until([&] { return server.status().value("clients").toInt() == 0; });
        remaining -= size;
    }
    const auto limited = call(server, "read");
    check(burst.elapsed() < 1000, "test command burst exceeded one rate window");
    code(limited, "rate_limited");
    check(calls == 20 && server.status().value("requests").toInt() == 20, "global rate limit invoked handler");
    pumpFor(1050);
    check(call(server, "read").value("ok").toBool() && calls == 21, "sliding rate window did not recover");
    stopClean(server);
}

void responsesAndReentry() {
    CommandDispatcher dispatcher(QStringLiteral("response-selftest"));
    CommandControlServer server(&dispatcher);
    int nestedCalls = 0;
    bool nativeReadFinished = false;
    bool timeoutSocketDestroyed = false;
    QPointer<QTcpSocket> slowSocket;
    add(dispatcher, "large", CommandDispatcher::Access::Read, [](const QJsonObject &) {
        return QJsonObject{{"text", QString(900000, 'x')}};
    });
    add(dispatcher, "oversized", CommandDispatcher::Access::Read, [](const QJsonObject &) {
        return QJsonObject{{"text", QString(ResponseLimit + 1, 'x')}};
    });
    add(dispatcher, "escaped", CommandDispatcher::Access::Read, [](const QJsonObject &) {
        return QJsonObject{{"text", QString(200000, QChar(1))}};
    });
    add(dispatcher, "deep", CommandDispatcher::Access::Read, [](const QJsonObject &) {
        QJsonValue value("leaf");
        for (int i = 0; i < 70; ++i) value = QJsonArray{value};
        return QJsonObject{{"nested", value}};
    });
    add(dispatcher, "nested", CommandDispatcher::Access::Read, [&](const QJsonObject &) {
        ++nestedCalls; return QJsonObject{};
    });
    add(dispatcher, "reenter", CommandDispatcher::Access::Read, [&](const QJsonObject &) {
        const CommandDispatcher::Context context{QStringLiteral("agent"), false, false, false};
        code(dispatcher.execute(request("nested"), context), "busy");
        const auto nested = exchange(server, envelope(server.token(), request("nested"))).object;
        code(nested, "busy");
        return QJsonObject{{"nestedBlocked", true}};
    });
    add(dispatcher, "slow", CommandDispatcher::Access::Read, [&](const QJsonObject &) {
        check(nativeReadFinished, "handler ran before native channelReadyRead unwound");
        QEventLoop nested;
        QTimer::singleShot(2100, &nested, &QEventLoop::quit);
        nested.exec();
        // The handler is now a server MetaCall, not a socket signal callback.
        // Exercise actual reclamation during dispatch, not just a closed socket.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        timeoutSocketDestroyed = slowSocket.isNull();
        check(timeoutSocketDestroyed, "timeout did not reclaim socket during queued handler");
        return QJsonObject{{"late", true}};
    });
    add(dispatcher, "stop-server", CommandDispatcher::Access::Read, [&](const QJsonObject &) {
        server.stop();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return QJsonObject{};
    });
    check(server.start(), "response server start");
    std::cout << "  bounded large response\n" << std::flush;
    const auto large = call(server, "large");
    check(large.value("ok").toBool() && large.value("text").toString().size() == 900000, "bounded large reply failed");
    std::cout << "  oversized response\n" << std::flush;
    code(call(server, "oversized"), "response_too_large");
    std::cout << "  escaped response\n" << std::flush;
    code(call(server, "escaped"), "response_too_large");
    std::cout << "  deep response\n" << std::flush;
    code(call(server, "deep"), "response_too_large");
    std::cout << "  dispatcher and socket reentry\n" << std::flush;
    check(call(server, "reenter").value("nestedBlocked").toBool() && !nestedCalls, "reentrant request executed");
    std::cout << "  long-handler deadline\n" << std::flush;
    QTcpSocket slowClient;
    connectClient(slowClient, server);
    until([&] {
        for (auto *socket : server.findChildren<QTcpSocket *>()) {
            if (socket->state() == QAbstractSocket::ConnectedState && socket->peerPort() == slowClient.localPort()) {
                slowSocket = socket;
                return true;
            }
        }
        return false;
    });
    QObject::connect(slowSocket, &QIODevice::channelReadyRead, &server, [&](int) { nativeReadFinished = true; });
    const auto slowLine = envelope(server.token(), request("slow"));
    check(slowClient.write(slowLine) == slowLine.size(), "long handler request write");
    const auto slow = collect(slowClient, false);
    check(nativeReadFinished && timeoutSocketDestroyed, "queued native-emitter lifetime regression");
    check(!slow.object.value("late").toBool(), "late handler response was sent after timeout");
    if (!slow.bytes.isEmpty()) code(slow.object, "timeout");
    std::cout << "  stop during handler\n" << std::flush;
    const auto stopped = exchange(server, envelope(server.token(), request("stop-server")), false);
    check(stopped.bytes.isEmpty() && !server.isListening(), "reentrant stop sent stale response");
    stopClean(server);
}
} // namespace

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QCoreApplication app(argc, argv);
    try {
        std::cout << "command_control_selftest: legacy result compatibility\n" << std::flush;
        legacyReplyCompatibility();
        std::cout << "command_control_selftest: authentication and permissions\n" << std::flush;
        authenticationAndPermissions();
        std::cout << "command_control_selftest: framing and limits\n" << std::flush;
        framingAndLimits();
        std::cout << "command_control_selftest: queued grant, revoke and stop\n" << std::flush;
        queuedPoliciesAndStop();
        std::cout << "command_control_selftest: clients, deadlines and rate\n" << std::flush;
        clientsDeadlinesAndRate();
        std::cout << "command_control_selftest: responses and reentry\n" << std::flush;
        responsesAndReentry();
        std::cout << "command_control_selftest: PASS (loopback auth, permissions, framing, limits, deadlines, cleanup)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "command_control_selftest: FAIL: " << error.what() << '\n';
        return 1;
    }
}
