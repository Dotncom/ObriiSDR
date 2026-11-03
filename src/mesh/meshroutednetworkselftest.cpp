#include "meshdiscovery.h"
#include "meshnode.h"
#include "meshnodedirectory.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {
constexpr int RpcTimeoutMs = 2500;
constexpr int SearchTimeoutMs = 6000;
int failures = 0;
int checks = 0;

void check(bool okay, const char *description)
{
    ++checks;
    std::printf("%s %s\n", okay ? "PASS" : "FAIL", description);
    std::fflush(stdout);
    if (!okay) ++failures;
}

QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

bool waitFor(const std::function<bool()> &condition, int milliseconds = RpcTimeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return condition();
}

void drainFor(int milliseconds)
{
    waitFor([] { return false; }, milliseconds);
}

QJsonObject publicRequest(const QString &operation = "discover")
{
    return {{"op", operation}, {"protocol", "obrii-mesh-discovery"}, {"version", 1},
            {"nonce", uuid()}, {"requestId", uuid()}};
}

struct Reply {
    QJsonObject object;
    QByteArray bytes;
    bool connected = false;
    bool completed = false;
};

// Event-driven waiting also services the fake TCP peers in this same thread.
Reply exchange(quint16 port, const QByteArray &wire)
{
    Reply reply;
    QTcpSocket socket;
    socket.setReadBufferSize(256 * 1024);
    bool ended = false;
    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&] {
        reply.connected = true;
        socket.write(wire);
    });
    QObject::connect(&socket, &QTcpSocket::readyRead, &socket, [&] { reply.bytes += socket.readAll(); });
    QObject::connect(&socket, &QTcpSocket::disconnected, &socket, [&] {
        reply.bytes += socket.readAll();
        ended = true;
    });
    QObject::connect(&socket, &QAbstractSocket::errorOccurred, &socket,
                     [&](QAbstractSocket::SocketError) {
        if (socket.state() == QAbstractSocket::UnconnectedState) ended = true;
    });
    socket.connectToHost(QHostAddress::LocalHost, port);
    reply.completed = waitFor([&] { return ended || reply.bytes.contains('\n'); });
    const int newline = reply.bytes.indexOf('\n');
    if (newline >= 0) reply.object = QJsonDocument::fromJson(reply.bytes.left(newline)).object();
    socket.abort();
    return reply;
}

Reply rpc(quint16 port, const QJsonObject &request)
{
    return exchange(port, QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
}

Reply control(const QJsonObject &service, QJsonObject request, const QString &token = {})
{
    request["instanceId"] = service.value("instanceId");
    request["requestId"] = uuid();
    request["token"] = token.isEmpty() ? service.value("token") : QJsonValue(token);
    return rpc(quint16(service.value("port").toInt()), request);
}

QJsonObject start(Mesh::NodeService &service, const QString &root, bool directory = false,
                  const QJsonObject &directoryClient = {})
{
    QJsonObject options{{"operation", "start"}, {"bind", "127.0.0.1"}, {"port", 0},
                        {"directoryEnabled", directory}, {"protocolStateDirectory", root}};
    if (!directoryClient.isEmpty()) options["directoryClient"] = directoryClient;
    return service.command(options);
}

bool started(const QJsonObject &service)
{
    return service.value("ok") == true && service.value("listening") == true &&
           service.value("port").toInt() > 0 && !QUuid(service.value("instanceId").toString()).isNull();
}

QJsonObject nodeEntry(const QJsonObject &service, const QString &name = "Test leaf")
{
    return {{"protocol", "obrii-mesh-node"}, {"version", 1},
            {"instanceId", service.value("instanceId")}, {"host", "127.0.0.1"},
            {"port", service.value("port")}, {"name", name}, {"active", false},
            {"availableBearers", QJsonArray{"data", "iq"}}};
}

QJsonObject endpoint(quint16 port, const QString &role = "seed", bool enabled = true)
{
    return {{"enabled", enabled}, {"role", role}, {"name", "Loopback test"},
            {"host", "127.0.0.1"}, {"port", port}};
}

bool containsId(const QJsonArray &entries, const QString &id)
{
    for (const auto &value : entries)
        if (QUuid(value.toObject().value("instanceId").toString()) == QUuid(id)) return true;
    return false;
}

QJsonObject instance(const Mesh::DiscoveryClient &client, const QString &id)
{
    for (const auto &entry : client.instances())
        if (QUuid(entry.value("instanceId").toString()) == QUuid(id)) return entry;
    return {};
}

bool publicOnly(const QJsonValue &value)
{
    static const QSet<QString> forbidden{"token", "key", "secret", "pairingSecret", "password",
        "credentials", "session", "sessionKey", "ownerToken", "peers", "topology", "results",
        "payloadBase64", "protocolStateDirectory", "stateDirectory", "directoryClient", "registrations",
        "radioTxProfiles", "reticulum", "python", "moduleDirectory", "path", "directory"};
    if (value.isArray()) {
        for (const auto &item : value.toArray()) if (!publicOnly(item)) return false;
    } else if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            if (forbidden.contains(it.key()) || !publicOnly(it.value())) return false;
    }
    return true;
}

bool omits(const QJsonObject &object, const QStringList &secrets)
{
    if (!publicOnly(object)) return false;
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    for (const auto &secret : secrets)
        if (!secret.isEmpty() && bytes.contains(secret.toUtf8())) return false;
    return true;
}

bool matchesPublicReply(const Reply &reply, const QJsonObject &request, bool directory = false)
{
    const auto object = reply.object;
    if (!reply.connected || !reply.completed || object.value("ok") != true ||
        object.value("protocol") != "obrii-mesh-discovery" || object.value("version") != 1 ||
        object.value("op") != (directory ? "directory" : "instance") ||
        object.value("nonce") != request.value("nonce") ||
        object.value("requestId") != request.value("requestId")) return false;
    auto metadata = object;
    metadata["protocol"] = "obrii-mesh-node";
    for (const auto key : {"ok", "op", "nonce", "requestId", "entries"}) metadata.remove(key);
    if (Mesh::publicDiscoveryEntry(metadata) != metadata || !publicOnly(object)) return false;
    if (directory) {
        if (object.value("directoryVersion") != 1 || !object.value("entries").isArray() ||
            object.value("entries").toArray().size() > 64) return false;
        for (const auto &entry : object.value("entries").toArray())
            if (!entry.isObject() || Mesh::publicDiscoveryEntry(entry.toObject()) != entry.toObject()) return false;
    }
    return true;
}

struct Registration {
    QString clientId;
    QString token;
};

Registration pair(const QJsonObject &service, const QString &clientId = {})
{
    Registration registration{clientId.isEmpty() ? uuid() : clientId, {}};
    auto request = QJsonObject{{"op", "pair"}, {"instanceId", service.value("instanceId")},
                              {"requestId", uuid()}, {"code", service.value("pairingSecret")},
                              {"clientId", registration.clientId}, {"clientName", "Routed self-test"}};
    const auto reply = rpc(quint16(service.value("port").toInt()), request).object;
    if (reply.value("ok") == true) registration.token = reply.value("token").toString();
    return registration;
}

bool noRf(Mesh::NodeService &service)
{
    return service.status().value("rfEnabled") == false && !Mesh::NodeService::radioCaptureActive();
}

// No UDP responder exists for this identity, so finding it proves TCP fallback.
class FakePublicPeer : public QObject {
public:
    enum Mode { Honest, WrongProtocol, WrongNonce, WrongRequestId, Oversized, Silent, TooManyEntries };
    QTcpServer server;
    QString id = uuid();
    Mode mode = Honest;
    int delayMs = 0;
    int requests = 0;
    int replyAttempts = 0;
    bool validRequests = true;
    QJsonArray entries;
    QJsonObject lastRequest;

    FakePublicPeer()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto socket = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, handled] {
                    if (*handled) return;
                    *buffer += socket->readAll();
                    const int newline = buffer->indexOf('\n');
                    if (newline < 0) return;
                    *handled = true;
                    const auto request = QJsonDocument::fromJson(buffer->left(newline)).object();
                    lastRequest = request;
                    ++requests;
                    const auto op = request.value("op").toString();
                    validRequests &= request.size() == 5 && (op == "discover" || op == "directoryList") &&
                        request.value("protocol") == "obrii-mesh-discovery" && request.value("version") == 1 &&
                        !QUuid(request.value("nonce").toString()).isNull() &&
                        !QUuid(request.value("requestId").toString()).isNull() && !request.contains("token");
                    if (mode == Silent) return;
                    auto response = QJsonObject{{"ok", true}, {"protocol", "obrii-mesh-discovery"},
                        {"version", 1}, {"op", op == "directoryList" ? "directory" : "instance"},
                        {"nonce", request.value("nonce")}, {"requestId", request.value("requestId")},
                        {"instanceId", id}, {"host", "127.0.0.1"}, {"port", server.serverPort()},
                        {"name", "TCP-only test peer"}, {"active", false},
                        {"availableBearers", QJsonArray{"data", "iq"}}, {"directoryVersion", 1}};
                    if (op == "directoryList") response["entries"] = entries;
                    if (mode == WrongProtocol) response["protocol"] = "not-mesh-discovery";
                    if (mode == WrongNonce) response["nonce"] = uuid();
                    if (mode == WrongRequestId) response["requestId"] = uuid();
                    if (mode == TooManyEntries) {
                        QJsonArray many;
                        for (int i = 0; i < 65; ++i) many.append(nodeEntry(response));
                        response["entries"] = many;
                    }
                    const auto wire = mode == Oversized ? QByteArray(65537, 'x') + '\n' :
                        QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
                    const QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(delayMs, this, [this, guard, wire] {
                        ++replyAttempts;
                        if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
                        guard->write(wire);
                        guard->disconnectFromHost();
                    });
                });
            }
        });
    }

    ~FakePublicPeer() override
    {
        // Connections belong to the server; prevent callbacks during member teardown.
        for (auto socket : server.findChildren<QTcpSocket *>()) {
            socket->disconnect(this);
            socket->abort();
        }
    }
};

void publicTcpTests(const QString &root)
{
    Mesh::NodeService service;
    const auto local = start(service, QDir(root).filePath("public"), true);
    check(started(local), "public TCP fixture starts on an ephemeral loopback port");
    if (!started(local)) return;
    const auto port = quint16(local.value("port").toInt());
    QStringList secrets{local.value("token").toString(), local.value("pairingSecret").toString(), root};
    auto request = publicRequest();
    auto reply = rpc(port, request);
    check(matchesPublicReply(reply, request) && reply.object.value("active") == false && omits(reply.object, secrets),
          "unauthenticated TCP discovery echoes nonce/request ID and exposes only inactive public metadata");
    request = publicRequest("directoryList");
    reply = rpc(port, request);
    check(matchesPublicReply(reply, request, true) && omits(reply.object, secrets),
          "public directory list has v1 bounded normalized entries and no startup credentials");

    const QString session = uuid();
    const QString sessionKey = uuid() + uuid();
    secrets << session << sessionKey;
    QJsonObject topology{{"nodes", QJsonArray{QJsonObject{{"id", "A"}, {"name", "Active TCP test"}}}},
                         {"links", QJsonArray{}}};
    QJsonObject peers{{"A", QJsonObject{{"host", "127.0.0.1"}, {"port", port},
                                       {"instanceId", local.value("instanceId")}}}};
    const auto active = control(local, {{"op", "configure"}, {"session", session}, {"key", sessionKey},
        {"nodeId", "A"}, {"transport", "data"}, {"topology", topology}, {"peers", peers}}).object;
    check(active.value("ok") == true && active.value("active") == true && active.value("rfEnabled") == false,
          "single-node laboratory data session becomes active without RF");
    request = publicRequest();
    reply = rpc(port, request);
    check(matchesPublicReply(reply, request) && reply.object.value("active") == true && omits(reply.object, secrets),
          "active public TCP metadata does not expose session, key, peers, payloads or private paths");
    request = publicRequest("directoryList");
    reply = rpc(port, request);
    check(matchesPublicReply(reply, request, true) && omits(reply.object, secrets),
          "active directory catalogue also contains public metadata only");

    for (const auto &field : QStringList{"protocol", "nonce", "requestId", "version", "token"}) {
        auto malformed = publicRequest();
        if (field == "version") malformed[field] = 2;
        else malformed[field] = field == "token" ? local.value("token") : QJsonValue("invalid-envelope");
        const auto denied = rpc(port, malformed);
        check(denied.connected && denied.completed && denied.bytes.isEmpty(),
              qPrintable("public TCP rejects malformed/private envelope field: " + field));
    }
    auto missingNonce = publicRequest();
    missingNonce.remove("nonce");
    check(rpc(port, missingNonce).bytes.isEmpty(), "public TCP rejects missing nonce");
    auto oversized = QJsonDocument(publicRequest()).toJson(QJsonDocument::Compact);
    oversized += QByteArray(4096, ' ') + '\n';
    const auto denied = exchange(port, oversized);
    check(denied.connected && denied.completed && denied.bytes.isEmpty(),
          "oversized public TCP request is rejected before replying");
    check(control(local, {{"op", "release"}, {"session", session}}).object.value("ok") == true,
          "active secrecy fixture releases its data-only session");
    check(waitFor([&] { return noRf(service); }), "public TCP operations never enable RF or receiver capture");
}

void rateGuardTest(const QString &root)
{
    Mesh::NodeService service;
    const auto local = start(service, QDir(root).filePath("rate"));
    check(started(local), "rate-limit fixture starts independently");
    if (!started(local)) return;
    int completed = 0;
    int accepted = 0;
    bool matched = true;
    std::vector<std::unique_ptr<QTcpSocket>> sockets;
    // Below the listener's 32-connection cap: rejection here exercises the
    // public per-source rate guard, not just connection exhaustion.
    for (int i = 0; i < 24; ++i) {
        auto socket = std::make_unique<QTcpSocket>();
        auto raw = socket.get();
        auto bytes = std::make_shared<QByteArray>();
        auto done = std::make_shared<bool>(false);
        const auto request = publicRequest();
        QObject::connect(raw, &QTcpSocket::connected, raw, [raw, request] {
            raw->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        });
        const auto finish = [&, raw, bytes, done, request] {
            if (*done) return;
            *bytes += raw->readAll();
            if (!bytes->contains('\n') && raw->state() != QAbstractSocket::UnconnectedState) return;
            *done = true;
            ++completed;
            if (bytes->contains('\n')) {
                ++accepted;
                const auto reply = QJsonDocument::fromJson(bytes->left(bytes->indexOf('\n'))).object();
                matched &= reply.value("ok") == true && reply.value("nonce") == request.value("nonce") &&
                           reply.value("requestId") == request.value("requestId");
            }
        };
        QObject::connect(raw, &QTcpSocket::readyRead, raw, finish);
        QObject::connect(raw, &QTcpSocket::disconnected, raw, finish);
        QObject::connect(raw, &QAbstractSocket::errorOccurred, raw,
                         [finish](QAbstractSocket::SocketError) { finish(); });
        raw->connectToHost(QHostAddress::LocalHost, quint16(local.value("port").toInt()));
        sockets.push_back(std::move(socket));
    }
    const bool finished = waitFor([&] { return completed == 24; });
    check(finished && accepted > 0 && accepted <= 16 && completed > accepted && matched,
          "24 concurrent public requests are bounded to 16 replies per loopback source");
    sockets.clear();
    drainFor(1050);
    const auto request = publicRequest();
    check(matchesPublicReply(rpc(quint16(local.value("port").toInt()), request), request),
          "public TCP rate guard recovers after its one-second window");
    check(noRf(service), "rate-limited discovery never enables RF");
}

void seedAndReplyTests()
{
    FakePublicPeer seed;
    check(seed.server.isListening(), "TCP-only seed listens on ephemeral loopback port without UDP responder");
    if (!seed.server.isListening()) return;
    Mesh::DiscoveryClient client;
    int finishes = 0;
    QObject::connect(&client, &Mesh::DiscoveryClient::finished, &client, [&] { ++finishes; });
    check(client.setEndpoints({endpoint(seed.server.serverPort())}), "production client accepts saved TCP seed");
    QElapsedTimer elapsed;
    elapsed.start();
    const bool running = client.start();
    check(running && waitFor([&] { return finishes == 1; }, SearchTimeoutMs), "saved seed search finishes within bounded timeout");
    const auto found = instance(client, seed.id);
    check(!client.searching() && elapsed.elapsed() <= SearchTimeoutMs && seed.requests > 0 && seed.validRequests &&
          !found.isEmpty() && found.value("discoveryMethods").toArray().contains("tcp") &&
          !found.value("discoveryMethods").toArray().contains("udp") && client.statistics().value("tcpReplies").toInt() > 0,
          "unique fake seed identity is found through actual TCP fallback, independent of LAN UDP responses");
    check(publicOnly(found), "TCP seed result contains no credential or control fields");

    const std::vector<std::pair<FakePublicPeer::Mode, const char *>> cases{
        {FakePublicPeer::WrongProtocol, "client rejects wrong-protocol TCP reply"},
        {FakePublicPeer::WrongNonce, "client rejects mismatched nonce TCP reply"},
        {FakePublicPeer::WrongRequestId, "client rejects mismatched request-ID TCP reply"},
        {FakePublicPeer::Oversized, "client rejects over-64KiB TCP reply"},
        {FakePublicPeer::TooManyEntries, "client rejects directory reply containing 65 entries"},
        {FakePublicPeer::Silent, "silent TCP endpoint times out within the bounded search"}};
    for (const auto &test : cases) {
        FakePublicPeer bad;
        bad.mode = test.first;
        Mesh::DiscoveryClient probe;
        bool done = false;
        QObject::connect(&probe, &Mesh::DiscoveryClient::finished, &probe, [&] { done = true; });
        const bool directory = test.first == FakePublicPeer::TooManyEntries;
        const bool configured = probe.setEndpoints({endpoint(bad.server.serverPort(), directory ? "directory" : "seed")});
        QElapsedTimer clock;
        clock.start();
        const bool launched = configured && probe.start();
        check(launched && waitFor([&] { return done; }, SearchTimeoutMs) && !probe.searching() &&
              clock.elapsed() <= SearchTimeoutMs && bad.requests > 0 && instance(probe, bad.id).isEmpty() &&
              probe.statistics().value("failedProbes").toInt() > 0, test.second);
    }
    FakePublicPeer disabled;
    const int previousFinishes = finishes;
    check(client.setEndpoints({endpoint(disabled.server.serverPort(), "seed", false)}) && client.start() &&
          waitFor([&] { return finishes == previousFinishes + 1; }, SearchTimeoutMs) && disabled.requests == 0 &&
          instance(client, disabled.id).isEmpty(), "disabled saved seed is not contacted or imported");
}

void directoryTests(const QString &root)
{
    Mesh::NodeService catalogue;
    Mesh::NodeService leaf;
    const auto local = start(catalogue, QDir(root).filePath("catalogue"));
    const auto remote = start(leaf, QDir(root).filePath("leaf"));
    check(started(local) && started(remote) && local.value("directoryEnabled") == false,
          "catalogue and leaf use separate ephemeral loopback services; catalogue is opt-in");
    if (!started(local) || !started(remote)) return;
    const auto port = quint16(local.value("port").toInt());
    const auto disabled = rpc(port, publicRequest("directoryList"));
    check(disabled.completed && disabled.bytes.isEmpty(), "directory list is unavailable before local opt-in");
    check(catalogue.command({{"operation", "directory-on"}}).value("directoryEnabled") == true,
          "local directory-on enables the catalogue without changing RF authorization");
    const auto registration = pair(local);
    const auto second = pair(local);
    check(!registration.token.isEmpty() && !second.token.isEmpty() && registration.token != second.token,
          "catalogue grants separate registered credentials through existing pairing");
    if (registration.token.isEmpty() || second.token.isEmpty()) return;
    const auto advertised = nodeEntry(remote, "Untrusted catalogue label");
    auto unauthenticated = QJsonObject{{"op", "directoryRegister"}, {"requestId", uuid()},
        {"instanceId", local.value("instanceId")}, {"entry", advertised}};
    check(rpc(port, unauthenticated).bytes.isEmpty(), "unauthenticated directory registration is rejected");
    unauthenticated["token"] = uuid();
    check(rpc(port, unauthenticated).bytes.isEmpty(), "unknown directory credential is rejected");
    auto staleIdentity = unauthenticated;
    staleIdentity["token"] = registration.token;
    staleIdentity["instanceId"] = uuid();
    check(rpc(port, staleIdentity).object.value("ok") != true, "registration requires current catalogue instance identity");
    const auto accepted = control(local, {{"op", "directoryRegister"}, {"entry", advertised}}, registration.token).object;
    check(accepted.value("ok") == true && accepted.value("registered") == true &&
          accepted.value("expiresInMs").toInt() == 90000,
          "registered credential authorizes public leaf registration with a 90-second lease");
    check(control(local, {{"op", "directoryRegister"}, {"entry", advertised}}, second.token).object.value("ok") != true,
          "another registered client cannot overwrite an existing owner's catalogue entry");
    auto privateEntry = advertised;
    privateEntry["token"] = remote.value("token");
    check(control(local, {{"op", "directoryRegister"}, {"entry", privateEntry}}, registration.token).object.value("ok") != true,
          "authenticated registration still rejects private fields in advertised metadata");

    QTcpSocket reserved;
    const bool reservedPort = reserved.bind(QHostAddress(QHostAddress::LocalHost), quint16(0));
    auto unreachable = advertised;
    const auto unreachableId = uuid();
    unreachable["instanceId"] = unreachableId;
    unreachable["port"] = reserved.localPort();
    check(reservedPort && control(local, {{"op", "directoryRegister"}, {"entry", unreachable}}).object.value("ok") == true,
          "admin may catalogue an unreachable identity; its TCP port is reserved but not listening");
    auto impostor = advertised;
    const auto impostorId = uuid();
    impostor["instanceId"] = impostorId;
    check(control(local, {{"op", "directoryRegister"}, {"entry", impostor}}).object.value("ok") == true,
          "catalogue can contain a claimed UUID whose reachable endpoint has a different identity");
    auto request = publicRequest("directoryList");
    auto listing = rpc(port, request);
    const QStringList secrets{local.value("token").toString(), local.value("pairingSecret").toString(),
        remote.value("token").toString(), remote.value("pairingSecret").toString(), registration.token, second.token, root};
    check(matchesPublicReply(listing, request, true) &&
          containsId(listing.object.value("entries").toArray(), remote.value("instanceId").toString()) &&
          containsId(listing.object.value("entries").toArray(), unreachableId) && omits(listing.object, secrets),
          "public catalogue lists registered public metadata, never registration or node secrets");

    Mesh::DiscoveryClient discovery;
    bool finished = false;
    QObject::connect(&discovery, &Mesh::DiscoveryClient::finished, &discovery, [&] { finished = true; });
    check(discovery.setEndpoints({endpoint(port, "directory")}) && discovery.start() &&
          waitFor([&] { return finished; }, SearchTimeoutMs), "production directory discovery completes within bounded timeout");
    const auto discovered = instance(discovery, remote.value("instanceId").toString());
    check(discovered.value("discoveryMethods").toArray().contains("directory") &&
          discovered.value("name") != advertised.value("name") &&
          discovery.statistics().value("directoryReplies").toInt() > 0 &&
          discovery.statistics().value("tcpReplies").toInt() > 0 && omits(discovered, secrets),
          "directory origin is recorded after actual leaf TCP verification, even if UDP also finds that leaf");
    check(instance(discovery, unreachableId).isEmpty() && instance(discovery, impostorId).isEmpty(),
          "directory discovery excludes unreachable UUID and reachable endpoint with mismatched UUID");
    check(catalogue.command({{"operation", "revoke"}, {"clientId", registration.clientId}}).value("ok") == true,
          "local revocation removes the registered catalogue owner");
    listing = rpc(port, publicRequest("directoryList"));
    check(!containsId(listing.object.value("entries").toArray(), remote.value("instanceId").toString()) &&
          control(local, {{"op", "directoryRegister"}, {"entry", advertised}}, registration.token).bytes.isEmpty(),
          "revocation withdraws owner's leaf and rejects reuse of revoked token");

    check(catalogue.command({{"operation", "directory-off"}}).value("directoryEnabled") == false &&
          catalogue.command({{"operation", "directory-on"}}).value("directoryEntries").toInt() == 0,
          "directory-off clears catalogue records before re-enabling");
    bool filled = true;
    for (int i = 0; i < 64; ++i) {
        auto entry = advertised;
        entry["instanceId"] = uuid();
        filled &= control(local, {{"op", "directoryRegister"}, {"entry", entry}}).object.value("registered") == true;
    }
    auto overflow = advertised;
    overflow["instanceId"] = uuid();
    const auto overflowReply = control(local, {{"op", "directoryRegister"}, {"entry", overflow}}).object;
    request = publicRequest("directoryList");
    listing = rpc(port, request);
    check(filled && overflowReply.value("ok") == false && matchesPublicReply(listing, request, true) &&
          listing.object.value("entries").toArray().size() == 64,
          "actual catalogue admits 64 registrations, rejects the 65th and never lists more than 64");
    check(noRf(catalogue) && noRf(leaf), "catalogue registration, discovery and revocation do not enable RF");
}

void autoRegistrationTest(const QString &root)
{
    Mesh::NodeService catalogue;
    Mesh::NodeService leaf;
    const auto local = start(catalogue, QDir(root).filePath("auto-catalogue"), true);
    check(started(local), "automatic-registration catalogue starts with directoryEnabled");
    if (!started(local)) return;
    const auto registration = pair(local);
    check(!registration.token.isEmpty(), "automatic announcer receives a dedicated registered credential");
    if (registration.token.isEmpty()) return;
    const QJsonObject configuration{{"host", "127.0.0.1"}, {"port", local.value("port")},
                                    {"token", registration.token}, {"advertiseHost", "127.0.0.1"}};
    const auto remote = start(leaf, QDir(root).filePath("auto-leaf"), false, configuration);
    check(started(remote), "leaf starts with private directoryClient configuration");
    if (!started(remote)) return;
    check(waitFor([&] { return leaf.status().value("directoryClient").toObject().value("registered") == true; }, 4500),
          "actual DirectoryAnnouncer registers leaf and heartbeat publishes successful status");
    auto request = publicRequest("directoryList");
    auto reply = rpc(quint16(local.value("port").toInt()), request);
    const QStringList secrets{registration.token, local.value("token").toString(), local.value("pairingSecret").toString(),
        remote.value("token").toString(), remote.value("pairingSecret").toString(), root};
    check(matchesPublicReply(reply, request, true) &&
          containsId(reply.object.value("entries").toArray(), remote.value("instanceId").toString()) &&
          omits(reply.object, secrets) && !QJsonDocument(leaf.status()).toJson().contains(registration.token.toUtf8()),
          "automatic public catalogue and ordinary leaf status omit private announcer credentials");
    check(catalogue.command({{"operation", "revoke"}, {"clientId", registration.clientId}}).value("ok") == true,
          "automatic announcer credential can be revoked locally");
    reply = rpc(quint16(local.value("port").toInt()), publicRequest("directoryList"));
    check(!containsId(reply.object.value("entries").toArray(), remote.value("instanceId").toString()),
          "revoking automatic announcer immediately withdraws its leaf");
    leaf.command({{"operation", "directory-client"}, {"directoryClient", configuration}});
    check(waitFor([&] {
        const auto state = leaf.status().value("directoryClient").toObject();
        return state.value("registered") == false && !state.value("error").toString().isEmpty();
    }, 4500), "announcer retry with revoked credential fails without a false success callback");
    const auto stopped = leaf.command({{"operation", "directory-client"}, {"directoryClient", QJsonObject{}}});
    check(stopped.value("directoryClient").toObject().value("configured") == false && noRf(catalogue) && noRf(leaf),
          "empty local directory-client configuration stops announcer without RF side effects");
}

void pairingRotationTest(const QString &root)
{
    Mesh::NodeService catalogue;
    const auto local = start(catalogue, QDir(root).filePath("pair-rotation"), true);
    check(started(local), "credential-rotation catalogue starts on ephemeral loopback port");
    if (!started(local)) return;
    const auto owner = pair(local);
    const auto other = pair(local);
    check(!owner.token.isEmpty() && !other.token.isEmpty(), "rotation fixture pairs two independent catalogue owners");
    if (owner.token.isEmpty() || other.token.isEmpty()) return;
    auto first = nodeEntry(local, "Rotating owner first leaf");
    auto second = nodeEntry(local, "Rotating owner second leaf");
    auto unrelated = nodeEntry(local, "Unrelated owner's leaf");
    first["instanceId"] = uuid();
    second["instanceId"] = uuid();
    unrelated["instanceId"] = uuid();
    const auto registerEntry = [&](const QJsonObject &entry, const QString &token) {
        return control(local, {{"op", "directoryRegister"}, {"entry", entry}}, token).object.value("registered") == true;
    };
    check(registerEntry(first, owner.token) && registerEntry(second, owner.token) && registerEntry(unrelated, other.token),
          "old credential owns two catalogue records alongside an unrelated owner's record");
    const auto rotated = pair(local, owner.clientId);
    check(rotated.clientId == owner.clientId && !rotated.token.isEmpty() && rotated.token != owner.token,
          "re-pairing the same client rotates its credential");
    if (rotated.token.isEmpty()) return;
    const auto port = quint16(local.value("port").toInt());
    auto request = publicRequest("directoryList");
    auto listing = rpc(port, request);
    const auto entries = listing.object.value("entries").toArray();
    check(matchesPublicReply(listing, request, true) &&
          !containsId(entries, first.value("instanceId").toString()) &&
          !containsId(entries, second.value("instanceId").toString()) &&
          containsId(entries, unrelated.value("instanceId").toString()),
          "pair rotation immediately withdraws all old-token records, preserving other owners without waiting for TTL");
    const auto stale = control(local, {{"op", "directoryRegister"}, {"entry", first}}, owner.token);
    check(stale.connected && stale.completed && stale.bytes.isEmpty(), "rotated-out token cannot register catalogue entries");
    check(registerEntry(first, rotated.token) && registerEntry(second, rotated.token),
          "replacement token immediately reclaims the same leaf UUIDs without orphaned owner_conflict");
    check(catalogue.command({{"operation", "revoke"}, {"clientId", owner.clientId}}).value("ok") == true,
          "rotated registration remains locally revocable under the same client ID");
    request = publicRequest("directoryList");
    listing = rpc(port, request);
    check(matchesPublicReply(listing, request, true) &&
          !containsId(listing.object.value("entries").toArray(), first.value("instanceId").toString()) &&
          !containsId(listing.object.value("entries").toArray(), second.value("instanceId").toString()) &&
          containsId(listing.object.value("entries").toArray(), unrelated.value("instanceId").toString()) &&
          omits(listing.object, {owner.token, rotated.token, other.token, local.value("token").toString()}) && noRf(catalogue),
          "revoking replacement credential clears its records only, with no secret or RF exposure");
}

void reentrantCatalogueRestartTest()
{
    FakePublicPeer catalogue;
    FakePublicPeer oldLeaf;
    FakePublicPeer anotherOldLeaf;
    FakePublicPeer replacement;
    const QJsonObject firstService{{"instanceId", oldLeaf.id}, {"port", oldLeaf.server.serverPort()}};
    const QJsonObject secondService{{"instanceId", anotherOldLeaf.id}, {"port", anotherOldLeaf.server.serverPort()}};
    catalogue.entries = {nodeEntry(firstService), nodeEntry(secondService)};
    Mesh::DiscoveryClient client;
    bool reentered = false;
    bool restarted = false;
    int finishes = 0;
    int staleLeafCallbacks = 0;
    QObject::connect(&client, &Mesh::DiscoveryClient::finished, &client, [&] { ++finishes; });
    QObject::connect(&client, &Mesh::DiscoveryClient::foundInstance, &client, [&](const QJsonObject &entry) {
        const auto id = entry.value("instanceId").toString();
        if (id == oldLeaf.id || id == anotherOldLeaf.id) ++staleLeafCallbacks;
        if (id != catalogue.id || reentered) return;
        reentered = true;
        // This is deliberately synchronous: the old catalogue callback has
        // not yet iterated its entries when the next scan starts.
        client.cancel();
        restarted = client.setEndpoints({endpoint(replacement.server.serverPort())}) && client.start();
    });
    const bool configured = client.setEndpoints({endpoint(catalogue.server.serverPort(), "directory")});
    check(configured && client.start() && waitFor([&] { return reentered; }),
          "catalogue foundInstance callback synchronously cancels and restarts production client");
    check(restarted && waitFor([&] { return finishes == 1; }, SearchTimeoutMs) &&
          instance(client, replacement.id).value("discoveryMethods").toArray().contains("tcp"),
          "reentrant restart completes only the replacement scan and finds its TCP seed");
    check(catalogue.requests == 1 && oldLeaf.requests == 0 && anotherOldLeaf.requests == 0 &&
          staleLeafCallbacks == 0 && instance(client, oldLeaf.id).isEmpty() &&
          instance(client, anotherOldLeaf.id).isEmpty() && instance(client, catalogue.id).isEmpty() &&
          client.statistics().value("directoryReplies").toInt() == 0,
          "old catalogue entries never enqueue, connect or surface in the reentrant replacement generation");
    const auto statistics = client.statistics();
    drainFor(300);
    check(finishes == 1 && !client.searching() && staleLeafCallbacks == 0 && client.statistics() == statistics,
          "reentrant catalogue restart has no late completion or stale statistics updates");
}

void udpBearerValidationTest()
{
    FakePublicPeer nonceSource;
    nonceSource.mode = FakePublicPeer::Silent;
    Mesh::DiscoveryClient client;
    bool finished = false;
    QObject::connect(&client, &Mesh::DiscoveryClient::finished, &client, [&] { finished = true; });
    const bool configured = client.setEndpoints({endpoint(nonceSource.server.serverPort())});
    const bool running = configured && client.start();
    const bool observed = running && waitFor([&] { return nonceSource.requests > 0; });
    // Read the public nonce from the TCP request, not private client state.
    // QObject exposes the real ephemeral UDP child socket used by this scan.
    auto receiver = client.findChild<QUdpSocket *>();
    QUdpSocket sender;
    const bool bound = sender.bind(QHostAddress(QHostAddress::LocalHost), quint16(0));
    check(observed && receiver && receiver->localPort() > 0 && bound,
          "UDP bearer fixture uses public TCP nonce and ephemeral loopback datagram ports");
    if (!observed || !receiver || !receiver->localPort() || !bound) return;
    const auto response = [&](const QString &id) {
        return QJsonObject{{"protocol", "obrii-mesh-discovery"}, {"version", 1}, {"op", "instance"},
            {"nonce", nonceSource.lastRequest.value("nonce")}, {"instanceId", id}, {"host", "127.0.0.1"},
            {"port", nonceSource.server.serverPort()}, {"name", "UDP capability test"}, {"active", false}};
    };
    bool sent = true;
    const auto send = [&](const QJsonObject &object) {
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        sent &= sender.writeDatagram(bytes, QHostAddress::LocalHost, receiver->localPort()) == bytes.size();
    };
    QStringList invalidIds;
    const QList<QJsonValue> invalidBearers{QJsonValue(QJsonValue::Null), QJsonValue("data"), QJsonArray{},
        QJsonArray{"unknown"}, QJsonArray{"data", "data"}, QJsonArray{"data", 42}, QJsonArray{"data", "iq", "data"}};
    for (const auto &bearers : invalidBearers) {
        const auto id = uuid();
        invalidIds.append(id);
        auto object = response(id);
        object["availableBearers"] = bearers;
        send(object);
    }
    const auto missingId = uuid();
    invalidIds.append(missingId);
    send(response(missingId));
    const auto dataId = uuid();
    auto data = response(dataId);
    data["availableBearers"] = QJsonArray{"data"};
    send(data);
    const auto iqId = uuid();
    auto iq = response(iqId);
    iq["availableBearers"] = QJsonArray{"iq"};
    send(iq);
    check(sent && waitFor([&] { return !instance(client, dataId).isEmpty() && !instance(client, iqId).isEmpty(); }),
          "matching-nonce UDP positive controls are received by the active production scan");
    check(waitFor([&] { return finished; }, SearchTimeoutMs), "UDP capability validation scan finishes within bound");
    bool rejected = true;
    for (const auto &id : invalidIds) rejected &= instance(client, id).isEmpty();
    check(rejected, "UDP discovery rejects missing, null, scalar, empty, unknown, duplicate and malformed bearer arrays");
    check(instance(client, dataId).value("availableBearers").toArray() == QJsonArray{"data"} &&
          instance(client, iqId).value("availableBearers").toArray() == QJsonArray{"iq"} &&
          instance(client, dataId).value("discoveryMethods").toArray().contains("udp"),
          "UDP discovery preserves advertised single-bearer capabilities without fabricating data or IQ support");
}

void cancellationTest()
{
    FakePublicPeer delayed;
    FakePublicPeer replacement;
    delayed.delayMs = 900;
    Mesh::DiscoveryClient client;
    int finishes = 0;
    int oldCallbacks = 0;
    int callbacks = 0;
    QObject::connect(&client, &Mesh::DiscoveryClient::finished, &client, [&] { ++finishes; });
    QObject::connect(&client, &Mesh::DiscoveryClient::foundInstance, &client, [&](const QJsonObject &entry) {
        ++callbacks;
        if (entry.value("instanceId") == delayed.id) ++oldCallbacks;
    });
    const bool configured = client.setEndpoints({endpoint(delayed.server.serverPort())});
    check(configured && client.start() && waitFor([&] { return delayed.requests > 0; }),
          "cancellation fixture has a real pending TCP discovery reply");
    client.cancel();
    const int atCancel = callbacks;
    check(!client.searching() && finishes == 0, "cancel ends search without emitting finished");
    check(client.setEndpoints({}) && client.start(), "cancelled client immediately restarts with empty saved endpoints");
    client.probeEndpoint("127.0.0.1", replacement.server.serverPort(), false);
    check(waitFor([&] { return finishes == 1; }, SearchTimeoutMs) &&
          instance(client, replacement.id).value("discoveryMethods").toArray().contains("tcp") &&
          instance(client, delayed.id).isEmpty() && oldCallbacks == 0 && delayed.replyAttempts > 0,
          "restart accepts new explicit TCP probe but ignores delayed reply from cancelled generation");
    const auto finalStatistics = client.statistics();
    const int finalCallbacks = callbacks;
    drainFor(1100);
    check(!client.searching() && finishes == 1 && callbacks == finalCallbacks && callbacks >= atCancel &&
          oldCallbacks == 0 && client.statistics() == finalStatistics,
          "completed restarted client emits no late callbacks and retains stable statistics");
    client.cancel();
    client.cancel();
    check(!client.searching() && finishes == 1, "repeated cancellation is safe and emits no extra completion");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("Routed network self-test");
    QTemporaryDir temporary;
    check(temporary.isValid(), "isolated temporary protocol-state root is available");
    if (!temporary.isValid()) return 1;
    check(!Mesh::NodeService::radioCaptureActive(), "test begins without receiver capture");
    publicTcpTests(temporary.path());
    rateGuardTest(temporary.path());
    seedAndReplyTests();
    directoryTests(temporary.path());
    autoRegistrationTest(temporary.path());
    pairingRotationTest(temporary.path());
    reentrantCatalogueRestartTest();
    udpBearerValidationTest();
    cancellationTest();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(!Mesh::NodeService::radioCaptureActive(), "all routed network fixtures finish without RF capture");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
