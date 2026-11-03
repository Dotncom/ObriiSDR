#include "meshnodedirectory.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;
int checks = 0;
const QString PrivateToken = "test-private-directory-token-NEVER-PUBLISH";
const QString PrivateOwner = "test-private-owner-NEVER-PUBLISH";

void check(bool okay, const char *description)
{
    ++checks;
    std::printf("%s %s\n", okay ? "PASS" : "FAIL", description);
    std::fflush(stdout);
    if (!okay) ++failures;
}

QString id(int number = 1)
{
    return QString("12345678-1234-4234-8234-%1").arg(number, 12, 16, QLatin1Char('0'));
}

QJsonObject entry(int number = 1)
{
    return {{"protocol", "obrii-mesh-node"}, {"version", 1}, {"instanceId", id(number)},
            {"host", "192.0.2.7"}, {"port", 45454}, {"name", "Directory helper test"},
            {"active", false}, {"availableBearers", QJsonArray{"data", "iq"}}};
}

bool okay(const QJsonObject &object) { return object.value("ok") == true; }

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 2500)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

void pump(int milliseconds = 30)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}

bool safeStatus(const QJsonObject &status)
{
    const auto bytes = QJsonDocument(status).toJson(QJsonDocument::Compact);
    return status.size() == 4 && status.value("configured").isBool() && status.value("registered").isBool() &&
           status.value("lastSuccessUtcMs").isDouble() && status.value("error").isString() &&
           !bytes.contains(PrivateToken.toUtf8()) && !bytes.contains(PrivateOwner.toUtf8()) &&
           !status.contains("host") && !status.contains("port");
}

void metadataTests()
{
    const auto original = entry();
    check(Mesh::publicDiscoveryEntry(original) == original, "public metadata roundtrips");
    auto raw = original;
    for (const auto &key : {"token", "session", "sessionKey", "privateKey", "path", "credentials", "owner"})
        raw[key] = PrivateToken;
    raw["nested"] = QJsonObject{{"secret", PrivateOwner}};
    check(Mesh::publicDiscoveryEntry(raw) == original, "normalizer whitelists public fields only");
    raw = original;
    raw.remove("host");
    raw.remove("protocol");
    raw.remove("version");
    check(Mesh::publicDiscoveryEntry(raw, "192.0.2.7") == original,
          "raw callback metadata gets envelope and explicit host fallback");
    check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "missing host without fallback fails closed");
    raw["host"] = "0.0.0.0";
    check(Mesh::publicDiscoveryEntry(raw, "192.0.2.7").isEmpty(), "invalid explicit host does not use fallback");
    raw = original;
    raw["instanceId"] = "{" + id().toUpper() + "}";
    raw["availableBearers"] = QJsonArray{"iq", "data"};
    check(Mesh::publicDiscoveryEntry(raw) == original, "UUID and bearer ordering are canonicalized");

    for (const auto &key : {"instanceId", "host", "port", "name", "active", "availableBearers"}) {
        raw = original;
        raw.remove(key);
        check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "missing required public field rejected");
        for (const auto &bad : {QJsonValue(QJsonValue::Null), QJsonValue(QJsonArray{}), QJsonValue(QJsonObject{})}) {
            raw = original;
            raw[key] = bad;
            check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "wrong public field container/null type rejected");
        }
    }
    for (const auto &host : {"", "localhost", "example.org", "::1", "::ffff:192.0.2.1", "0.0.0.0",
                             "0.1.2.3", "224.0.0.1", "239.255.79.82", "255.255.255.255", "240.0.0.1",
                             "192.0.2", "192.000.2.7", "3221225991", "0xc0.0.2.7", "192.0.2.7 ",
                             "192.0.2.7/24", "192.0.2.256", "192.0.2.7:80"}) {
        raw = original;
        raw["host"] = host;
        check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "non-numeric/noncanonical/non-unicast host rejected");
    }
    for (const auto &host : {"127.0.0.1", "10.2.3.4", "169.254.1.2", "223.255.255.254"}) {
        raw = original;
        raw["host"] = host;
        check(!Mesh::publicDiscoveryEntry(raw).isEmpty(), "numeric unicast IPv4 accepted including loopback");
    }
    for (const auto &port : {QJsonValue(0), QJsonValue(65536), QJsonValue(-1), QJsonValue(1.5),
                             QJsonValue(true), QJsonValue("1234"),
                             QJsonValue(std::numeric_limits<double>::infinity())}) {
        raw = original;
        raw["port"] = port;
        check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "port range/type/integer validation");
    }
    for (const int port : {1, 65535}) {
        raw = original;
        raw["port"] = port;
        check(!Mesh::publicDiscoveryEntry(raw).isEmpty(), "port boundaries accepted");
    }
    for (const auto &value : {QJsonValue("not-a-uuid"), QJsonValue("00000000-0000-0000-0000-000000000000"),
                              QJsonValue(123), QJsonValue(id() + "suffix")}) {
        raw = original;
        raw["instanceId"] = value;
        check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "UUID must be a non-null complete string");
    }
    for (const auto &bearers : {QJsonArray{}, QJsonArray{"data", "data"}, QJsonArray{"rf"},
                                QJsonArray{true}, QJsonArray{"data", "iq", "data"}}) {
        raw = original;
        raw["availableBearers"] = bearers;
        check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "empty/duplicate/unknown/oversized bearers rejected");
    }
    for (const auto &bearers : {QJsonArray{"data"}, QJsonArray{"iq"}}) {
        raw = original;
        raw["availableBearers"] = bearers;
        check(!Mesh::publicDiscoveryEntry(raw).isEmpty(), "single bearer accepted");
    }
    for (const auto &key : {"protocol", "version", "name", "active", "directoryVersion"}) {
        raw = original;
        raw[key] = QJsonValue::Null;
        check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "explicit null envelope/public option rejected");
    }
    raw = original;
    raw["name"] = QString(128, QChar(0x0406));
    check(!Mesh::publicDiscoveryEntry(raw).isEmpty(), "128-character Unicode name accepted");
    raw["name"] = QString(129, 'x');
    check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "129-character name rejected");
    raw = original;
    raw["active"] = "false";
    check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "active is strictly boolean");
    for (const auto &version : {QJsonValue(-1), QJsonValue(2), QJsonValue(true), QJsonValue(0.5), QJsonValue("1")}) {
        raw = original;
        raw["directoryVersion"] = version;
        check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "directory capability limited to integer 0/1");
    }
    for (int version : {0, 1}) {
        raw = original;
        raw["directoryVersion"] = version;
        check(Mesh::publicDiscoveryEntry(raw) == raw, "optional directory version roundtrips");
    }
    raw = original;
    raw["protocol"] = "obrii-mesh-discovery";
    check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "wrong explicit protocol rejected");
    raw = original;
    raw["version"] = true;
    check(Mesh::publicDiscoveryEntry(raw).isEmpty(), "envelope version is not boolean");
}

void directoryTests()
{
    Mesh::NodeDirectory directory;
    check(!directory.enabled() && directory.entries(0).isEmpty() && !okay(directory.put(entry(), PrivateOwner, 0)),
          "directory defaults empty and disabled");
    directory.setEnabled(true);
    check(!okay(directory.put(entry(), {}, 0)) && !okay(directory.put(entry(), PrivateOwner, -1)),
          "empty private owner and negative clock rejected");
    for (const auto &key : {"token", "owner", "session", "path", "keys", "unexpected", "op", "requestId"}) {
        auto raw = entry();
        raw[key] = PrivateToken;
        check(!okay(directory.put(raw, PrivateOwner, 0)) && directory.count(0) == 0,
              "directory put strictly rejects extraneous fields without inserting");
    }
    auto raw = entry();
    raw.remove("protocol");
    check(!okay(directory.put(raw, PrivateOwner, 0)), "directory requires normalized protocol envelope");
    raw = entry();
    raw.remove("version");
    check(!okay(directory.put(raw, PrivateOwner, 0)), "directory requires normalized version envelope");
    auto result = directory.put(entry(), PrivateOwner, 100);
    check(okay(result) && result.value("registered") == true && result.value("expiresInMs") == 90000 &&
          directory.count(100) == 1, "directory registers with exact 90-second TTL");
    const auto listed = directory.entries(100);
    check(listed == QJsonArray{entry()} && !QJsonDocument(listed).toJson().contains(PrivateOwner.toUtf8()) &&
          !QJsonDocument(result).toJson().contains(PrivateOwner.toUtf8()), "private owner never returned");
    raw = entry();
    raw["instanceId"] = "{" + id().toUpper() + "}";
    raw["name"] = "Hostile replacement";
    check(!okay(directory.put(raw, "other-owner", 101)) && directory.entries(101) == listed,
          "different owner cannot replace UUID including alternate UUID spelling");
    raw["name"] = "Refreshed";
    check(okay(directory.put(raw, PrivateOwner, 5000)) && directory.count(94999) == 1 && directory.count(95000) == 0,
          "same owner refresh resets TTL; exact expiry boundary frees UUID");
    check(okay(directory.put(entry(), "other-owner", 95000)), "expired UUID can be claimed by another owner");
    directory.revoke(PrivateOwner);
    check(directory.count(95000) == 1, "revoke does not remove another owner's UUID");
    directory.put(entry(2), "other-owner", 95000);
    directory.put(entry(3), PrivateOwner, 95000);
    directory.revoke("other-owner");
    check(directory.entries(95000) == QJsonArray{entry(3)}, "revoke removes every UUID owned by credential");
    directory.setEnabled(false);
    directory.setEnabled(true);
    check(directory.count(95000) == 0, "disable clears entries permanently");

    bool allInserted = true;
    for (int n = 1; n <= 64; ++n) allInserted &= okay(directory.put(entry(n), PrivateOwner, 100000));
    check(allInserted && directory.count(100000) == 64 && !okay(directory.put(entry(65), PrivateOwner, 100000)),
          "directory cap is 64 UUIDs");
    check(okay(directory.put(entry(1), PrivateOwner, 100001)), "existing owner can renew while directory is full");
    check(okay(directory.put(entry(65), "new-owner", 190000)) && directory.count(190000) == 2,
          "expiry frees capacity while renewed entries survive");
    check(directory.count(280000) == 0, "all records eventually expire via count");
    const qint64 nearLimit = std::numeric_limits<qint64>::max() - 100;
    check(okay(directory.put(entry(), PrivateOwner, nearLimit)) &&
          directory.count(std::numeric_limits<qint64>::max()) == 1, "TTL age comparison avoids signed deadline overflow");
}

struct Catalog : QObject {
    QTcpServer server;
    QVector<QJsonObject> requests;
    QVector<int> connectionIds;
    QVector<QPointer<QTcpSocket>> peers;
    std::function<void(QTcpSocket *, const QJsonObject &)> handler;
    int accepted = 0;

    Catalog()
    {
        check(server.listen(QHostAddress::LocalHost, 0), "mock catalog listens on numeric loopback");
        QObject::connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *peer = server.nextPendingConnection();
                peer->setParent(this);
                peers.append(peer);
                peer->setProperty("serial", ++accepted);
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(peer, &QTcpSocket::readyRead, this, [this, peer, buffer] {
                    *buffer += peer->readAll();
                    const int newline = buffer->indexOf('\n');
                    if (newline < 0) return;
                    const auto document = QJsonDocument::fromJson(buffer->left(newline));
                    buffer->remove(0, newline + 1);
                    const auto request = document.object();
                    requests.append(request);
                    connectionIds.append(peer->property("serial").toInt());
                    if (handler) handler(peer, request);
                });
            }
        });
        handler = [this](QTcpSocket *peer, const QJsonObject &request) { send(peer, reply(request)); };
    }

    QJsonObject config() const
    {
        return {{"host", "127.0.0.1"}, {"port", server.serverPort()}, {"token", PrivateToken}};
    }

    QJsonObject reply(const QJsonObject &request) const
    {
        if (request.value("op") == "discover") {
            auto response = entry(900);
            response["protocol"] = "obrii-mesh-discovery";
            response["op"] = "instance";
            response["nonce"] = request.value("nonce");
            response["requestId"] = request.value("requestId");
            response["directoryVersion"] = 1;
            response["ok"] = true;
            return response;
        }
        return {{"ok", true}, {"requestId", request.value("requestId")}, {"registered", true},
                {"expiresInMs", 90000}, {"instanceId", id(900)}};
    }

    static void send(QTcpSocket *peer, const QJsonObject &reply)
    {
        peer->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
    }
};

void configurationTests()
{
    QString error = PrivateToken;
    check(Mesh::DirectoryAnnouncer::validateConfiguration({}, &error) && error.isEmpty(),
          "empty private configuration disables and clears stale error");
    const QJsonObject original{{"host", "127.0.0.1"}, {"port", 45454}, {"token", PrivateToken}};
    check(Mesh::DirectoryAnnouncer::validateConfiguration(original), "exact private configuration accepted");
    for (const auto &key : {"host", "port", "token"}) {
        auto bad = original;
        bad.remove(key);
        check(!Mesh::DirectoryAnnouncer::validateConfiguration(bad, &error) && error == "invalid_configuration",
              "missing private field rejected with non-sensitive error class");
        for (const auto &value : {QJsonValue(QJsonValue::Null), QJsonValue(true), QJsonValue(QJsonObject{})}) {
            bad = original;
            bad[key] = value;
            check(!Mesh::DirectoryAnnouncer::validateConfiguration(bad), "wrong private field types rejected");
        }
    }
    for (const auto &key : {"path", "scan", "enabled", "protocol", "version", "unexpected"}) {
        auto bad = original;
        bad[key] = PrivateToken;
        check(!Mesh::DirectoryAnnouncer::validateConfiguration(bad), "unknown private configuration key rejected");
    }
    for (const auto &host : {"example.org", "::1", "0.0.0.0", "255.255.255.255", "224.0.0.1", "192.0.2"}) {
        auto bad = original;
        bad["host"] = host;
        auto badAdvertisement = original;
        badAdvertisement["advertiseHost"] = host;
        check(!Mesh::DirectoryAnnouncer::validateConfiguration(bad) &&
              !Mesh::DirectoryAnnouncer::validateConfiguration(badAdvertisement),
              "catalog and advertisement reject DNS/non-unicast/non-numeric addresses");
    }
    for (const auto &token : {QString{}, QString(4097, 'x')}) {
        auto bad = original;
        bad["token"] = token;
        check(!Mesh::DirectoryAnnouncer::validateConfiguration(bad), "token outside 1..4096 characters rejected");
    }
    for (int size : {1, 4096}) {
        auto good = original;
        good["token"] = QString(size, 'x');
        good["advertiseHost"] = "198.51.100.7";
        check(Mesh::DirectoryAnnouncer::validateConfiguration(good), "token length boundaries and explicit advertisement accepted");
    }
    for (const auto &port : {QJsonValue(0), QJsonValue(65536), QJsonValue(2.5), QJsonValue("1234")}) {
        auto bad = original;
        bad["port"] = port;
        check(!Mesh::DirectoryAnnouncer::validateConfiguration(bad), "private port strictly validated");
    }
}

void successfulAnnouncerTests()
{
    Catalog catalog;
    int metadataCalls = 0;
    Mesh::DirectoryAnnouncer announcer([&] {
        ++metadataCalls;
        auto raw = entry();
        raw.remove("host");
        if (metadataCalls > 1) raw["host"] = "";
        raw.remove("protocol");
        raw.remove("version");
        raw["session"] = PrivateOwner;
        raw["token"] = PrivateToken;
        return raw;
    });
    pump();
    check(catalog.accepted == 0 && metadataCalls == 0 && !announcer.status().value("configured").toBool() &&
          safeStatus(announcer.status()), "no automatic registration, callback or listener without private configuration");
    check(announcer.configure(catalog.config()) &&
          waitUntil([&] { return announcer.status().value("registered").toBool(); }), "two-stage asynchronous registration succeeds");
    check(catalog.requests.size() == 2 && metadataCalls == 1 && catalog.connectionIds[0] != catalog.connectionIds[1],
          "discovery and authenticated registration use different TCP connections");
    if (catalog.requests.size() < 2) return;
    const auto discover = catalog.requests[0];
    const auto registration = catalog.requests[1];
    check(discover.size() == 5 && discover.value("op") == "discover" && discover.value("protocol") == "obrii-mesh-discovery" &&
          discover.value("version") == 1 && !QUuid(discover.value("nonce").toString()).isNull() &&
          !QUuid(discover.value("requestId").toString()).isNull() && !discover.contains("token"),
          "public discovery request uses UUID correlations and no credential");
    auto expected = entry();
    expected["host"] = "127.0.0.1";
    check(registration.size() == 5 && registration.value("op") == "directoryRegister" &&
          registration.value("token") == PrivateToken && registration.value("instanceId") == id(900) &&
          registration.value("requestId") != discover.value("requestId") &&
          registration.value("entry").toObject() == expected, "registration pins catalog identity and uses socket-local host with whitelisted metadata");
    check(safeStatus(announcer.status()) && announcer.status().value("lastSuccessUtcMs").toDouble() > 0 &&
          announcer.status().value("error") == "", "successful status contains only four non-sensitive fields");

    // Drive the existing production refresh timer without waiting 30 seconds.
    QTimer *refresh = nullptr;
    for (auto *timer : announcer.findChildren<QTimer *>())
        if (timer->interval() == 30000 && timer->isActive()) refresh = timer;
    check(refresh != nullptr, "configured announcer has a 30-second refresh timer");
    if (refresh) QMetaObject::invokeMethod(refresh, "timeout", Qt::DirectConnection);
    if (refresh) QMetaObject::invokeMethod(refresh, "timeout", Qt::DirectConnection);
    check(waitUntil([&] { return catalog.requests.size() == 4 && announcer.status().value("registered").toBool(); }) &&
          metadataCalls == 2, "production timer refreshes by rediscovering and ignores overlapping attempts");
    if (catalog.requests.size() >= 4) {
        check(catalog.requests[2].value("nonce") != discover.value("nonce") &&
              catalog.requests[2].value("requestId") != discover.value("requestId"), "refresh uses fresh nonce and request identity");
        check(catalog.requests[3].value("entry").toObject() == expected,
              "empty callback host from any-address listener falls back to socket-local IPv4");
    }

    auto configured = catalog.config();
    configured["advertiseHost"] = "198.51.100.77";
    const int previous = catalog.requests.size();
    check(announcer.configure(configured) && waitUntil([&] { return catalog.requests.size() >= previous + 2 &&
                          announcer.status().value("registered").toBool(); }), "reconfiguration registers explicit advertisement");
    check(catalog.requests.last().value("entry").toObject().value("host") == "198.51.100.77",
          "private advertiseHost overrides callback and socket address");
    auto invalid = configured;
    invalid["host"] = "do-not-resolve.example";
    QString error;
    check(!announcer.configure(invalid, &error) && error == "invalid_configuration" &&
          announcer.status().value("configured") == true && announcer.status().value("registered") == true,
          "invalid reconfiguration is atomic and returns only an error class");
    const auto staleRefresh = QPointer<QTimer>(refresh);
    announcer.configure({});
    if (staleRefresh) QMetaObject::invokeMethod(staleRefresh, "timeout", Qt::DirectConnection);
    pump();
    check(!announcer.status().value("configured").toBool() && !announcer.status().value("registered").toBool() &&
          safeStatus(announcer.status()) && catalog.requests.size() == previous + 2,
          "empty configuration clears status and stale refresh cannot reconnect");
}

void replyFailureTest(const QString &stage, const std::function<void(QJsonObject &)> &mutate,
                      const char *description, const QString &expectedError = "invalid_reply")
{
    Catalog catalog;
    catalog.handler = [&](QTcpSocket *peer, const QJsonObject &request) {
        auto reply = catalog.reply(request);
        if (request.value("op") == stage) mutate(reply);
        Catalog::send(peer, reply);
    };
    Mesh::DirectoryAnnouncer announcer([] { return entry(); });
    announcer.configure(catalog.config());
    check(waitUntil([&] { return !announcer.status().value("error").toString().isEmpty(); }) &&
          announcer.status().value("error") == expectedError && !announcer.status().value("registered").toBool() &&
          safeStatus(announcer.status()) && (stage != "discover" || catalog.requests.size() == 1), description);
}

void announcerFailureTests()
{
    replyFailureTest("discover", [](QJsonObject &reply) { reply["requestId"] = id(777); }, "discovery requestId mismatch blocks credential");
    replyFailureTest("discover", [](QJsonObject &reply) { reply["nonce"] = id(777); }, "discovery nonce mismatch blocks credential");
    replyFailureTest("discover", [](QJsonObject &reply) { reply["instanceId"] = "bad"; }, "discovery invalid UUID blocks credential");
    replyFailureTest("discover", [](QJsonObject &reply) { reply["directoryVersion"] = 0; }, "non-directory discovery blocks credential");
    replyFailureTest("discover", [](QJsonObject &reply) { reply.remove("directoryVersion"); }, "missing directory capability blocks credential");
    replyFailureTest("discover", [](QJsonObject &reply) { reply["protocol"] = "obrii-mesh-node"; }, "wrong discovery protocol blocks credential");
    replyFailureTest("discover", [](QJsonObject &reply) { reply["version"] = true; }, "wrong discovery version type blocks credential");
    replyFailureTest("discover", [](QJsonObject &reply) { reply["op"] = "status"; }, "wrong discovery operation blocks credential");
    replyFailureTest("discover", [](QJsonObject &reply) { reply["host"] = "catalog.example"; }, "discovery DNS host blocks credential");
    replyFailureTest("discover", [](QJsonObject &reply) { reply["active"] = 1; }, "discovery wrong metadata type blocks credential");
    replyFailureTest("discover", [](QJsonObject &reply) { reply["ok"] = false; reply["error"] = PrivateToken; },
                     "untrusted discovery error is not exposed", "discovery_rejected");
    replyFailureTest("directoryRegister", [](QJsonObject &reply) { reply["requestId"] = id(777); }, "registration requestId mismatch rejected");
    replyFailureTest("directoryRegister", [](QJsonObject &reply) { reply["instanceId"] = id(777); }, "registration catalog identity mismatch rejected");
    replyFailureTest("directoryRegister", [](QJsonObject &reply) { reply.remove("instanceId"); }, "registration missing catalog identity rejected");
    replyFailureTest("directoryRegister", [](QJsonObject &reply) { reply["expiresInMs"] = 89999; }, "registration requires exact TTL contract");
    replyFailureTest("directoryRegister", [](QJsonObject &reply) { reply["registered"] = 1; }, "registration acknowledgement is strictly boolean");
    replyFailureTest("directoryRegister", [](QJsonObject &reply) { reply["ok"] = false; reply["error"] = PrivateToken; },
                     "untrusted authentication error is not exposed", "registration_rejected");

    for (const auto &stage : {QString("discover"), QString("directoryRegister")}) {
        for (const auto &bytes : {QByteArray("not-json\n"), QByteArray("[]\n"), QByteArray("{}\n{}\n"),
                                  QByteArray(65537, 'x')}) {
            Catalog catalog;
            catalog.handler = [&](QTcpSocket *peer, const QJsonObject &request) {
                if (request.value("op") == stage) peer->write(bytes);
                else Catalog::send(peer, catalog.reply(request));
            };
            Mesh::DirectoryAnnouncer announcer([] { return entry(); });
            announcer.configure(catalog.config());
            const auto expected = bytes.size() > 65536 ? "reply_too_large" : "invalid_reply";
            check(waitUntil([&] { return !announcer.status().value("error").toString().isEmpty(); }) &&
                  announcer.status().value("error") == expected && safeStatus(announcer.status()),
                  "both stages reject malformed/multiple/oversized TCP replies without exposing data");
        }
        Catalog catalog;
        catalog.handler = [&](QTcpSocket *peer, const QJsonObject &request) {
            if (request.value("op") != stage) Catalog::send(peer, catalog.reply(request));
        };
        Mesh::DirectoryAnnouncer announcer([] { return entry(); });
        QElapsedTimer elapsed;
        elapsed.start();
        announcer.configure(catalog.config());
        check(waitUntil([&] { return announcer.status().value("error") == "timeout"; }, 3000) &&
              elapsed.elapsed() >= 1400 && elapsed.elapsed() < 2800 && safeStatus(announcer.status()),
              "each TCP stage has an independent 1500ms wall timeout");
    }
    {
        Catalog catalog;
        Mesh::DirectoryAnnouncer announcer([] { auto raw = entry(); raw["port"] = 0; return raw; });
        announcer.configure(catalog.config());
        check(waitUntil([&] { return announcer.status().value("error") == "invalid_metadata"; }) &&
              catalog.requests.size() == 1 && safeStatus(announcer.status()), "invalid local metadata prevents sending authenticated registration");
    }
    {
        Catalog catalog;
        auto configuration = catalog.config();
        catalog.server.close();
        Mesh::DirectoryAnnouncer announcer([] { return entry(); });
        announcer.configure(configuration);
        check(waitUntil([&] { return !announcer.status().value("error").toString().isEmpty(); }) &&
              (announcer.status().value("error") == "transport_error" || announcer.status().value("error") == "timeout") &&
              safeStatus(announcer.status()), "refused TCP connection yields bounded sanitized transport error or timeout");
    }
}

void fragmentedReplyTests()
{
    Catalog catalog;
    catalog.handler = [&](QTcpSocket *peer, const QJsonObject &request) {
        const auto bytes = QJsonDocument(catalog.reply(request)).toJson(QJsonDocument::Compact) + '\n';
        peer->write(bytes.left(13));
        QTimer::singleShot(10, peer, [peer, bytes] { peer->write(bytes.mid(13)); peer->disconnectFromHost(); });
    };
    Mesh::DirectoryAnnouncer announcer([] { return entry(); });
    announcer.configure(catalog.config());
    check(waitUntil([&] { return announcer.status().value("registered").toBool(); }),
          "fragmented replies followed by peer close succeed in both stages");
}

void replyBoundaryAndRetryTests()
{
    {
        Catalog catalog;
        catalog.handler = [&](QTcpSocket *peer, const QJsonObject &request) {
            auto reply = catalog.reply(request);
            reply["padding"] = "";
            const int size = QJsonDocument(reply).toJson(QJsonDocument::Compact).size() + 1;
            reply["padding"] = QString(65536 - size, 'x');
            Catalog::send(peer, reply);
        };
        Mesh::DirectoryAnnouncer announcer([] { return entry(); });
        announcer.configure(catalog.config());
        check(waitUntil([&] { return announcer.status().value("registered").toBool(); }) &&
              safeStatus(announcer.status()), "exactly 64KiB JSON replies accepted in both stages without forwarding extra fields");
    }
    {
        Catalog catalog;
        bool reject = true;
        catalog.handler = [&](QTcpSocket *peer, const QJsonObject &request) {
            auto reply = catalog.reply(request);
            if (request.value("op") == "directoryRegister" && reject) {
                reply["ok"] = false;
                reply["error"] = PrivateToken;
            }
            Catalog::send(peer, reply);
        };
        Mesh::DirectoryAnnouncer announcer([] { return entry(); });
        announcer.configure(catalog.config());
        check(waitUntil([&] { return announcer.status().value("error") == "registration_rejected"; }),
              "retry test reaches sanitized rejection");
        reject = false;
        for (auto *timer : announcer.findChildren<QTimer *>())
            if (timer->interval() == 30000) QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
        check(waitUntil([&] { return announcer.status().value("registered").toBool(); }) &&
              catalog.requests.size() == 4 && announcer.status().value("error") == "" && safeStatus(announcer.status()),
              "configured announcer retries on next refresh and clears previous sanitized error");
    }
}

void cancellationTests()
{
    for (const auto &stage : {QString("discover"), QString("directoryRegister")}) {
        Catalog oldCatalog;
        QPointer<QTcpSocket> delayed;
        QJsonObject delayedReply;
        oldCatalog.handler = [&](QTcpSocket *peer, const QJsonObject &request) {
            if (request.value("op") == stage) { delayed = peer; delayedReply = oldCatalog.reply(request); }
            else Catalog::send(peer, oldCatalog.reply(request));
        };
        Mesh::DirectoryAnnouncer announcer([] { return entry(); });
        announcer.configure(oldCatalog.config());
        check(waitUntil([&] { return !delayed.isNull(); }), "cancellation test reaches held stage");
        auto *oldClient = announcer.findChild<QTcpSocket *>();
        if (oldClient) {
            // Queue signal deliveries before cancelling; their old epoch must
            // not change the replacement configuration or its status.
            QMetaObject::invokeMethod(oldClient, "readyRead", Qt::QueuedConnection);
            QMetaObject::invokeMethod(oldClient, "disconnected", Qt::QueuedConnection);
            for (auto *timer : oldClient->findChildren<QTimer *>())
                QMetaObject::invokeMethod(timer, "timeout", Qt::QueuedConnection);
        }
        Catalog replacement;
        announcer.configure(replacement.config());
        if (delayed && delayed->state() == QAbstractSocket::ConnectedState) Catalog::send(delayed, delayedReply);
        check(waitUntil([&] { return announcer.status().value("registered").toBool(); }) &&
              replacement.requests.size() == 2 && safeStatus(announcer.status()),
              "reconfigure cancels old stage and stale callbacks cannot corrupt new success");
        announcer.stop();
        pump();
        check(!announcer.status().value("configured").toBool() && !announcer.status().value("registered").toBool() &&
              announcer.status().value("lastSuccessUtcMs") == 0 && safeStatus(announcer.status()),
              "stop clears private configuration, active registration and status");
    }
    for (const auto &stage : {QString("discover"), QString("directoryRegister")}) {
        Catalog catalog;
        QPointer<QTcpSocket> delayed;
        QJsonObject delayedReply;
        catalog.handler = [&](QTcpSocket *peer, const QJsonObject &request) {
            if (request.value("op") == stage) { delayed = peer; delayedReply = catalog.reply(request); }
            else Catalog::send(peer, catalog.reply(request));
        };
        Mesh::DirectoryAnnouncer announcer([] { return entry(); });
        announcer.configure(catalog.config());
        check(waitUntil([&] { return !delayed.isNull(); }), "stop test reaches pending stage");
        const int previous = catalog.requests.size();
        announcer.stop();
        if (delayed && delayed->state() == QAbstractSocket::ConnectedState) Catalog::send(delayed, delayedReply);
        for (auto *timer : announcer.findChildren<QTimer *>())
            if (timer->interval() == 30000) QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
        pump();
        check(catalog.requests.size() == previous && !announcer.status().value("configured").toBool() &&
              !announcer.status().value("registered").toBool() && announcer.status().value("error") == "" &&
              safeStatus(announcer.status()), "stop in either pending stage ignores late reply and stale refresh");
    }
    {
        Catalog catalog;
        catalog.handler = [](QTcpSocket *, const QJsonObject &) {};
        auto *announcer = new Mesh::DirectoryAnnouncer([] { return entry(); });
        announcer->configure(catalog.config());
        check(waitUntil([&] { return catalog.requests.size() == 1; }), "destruction test reaches pending discovery");
        QPointer<QTcpSocket> client = announcer->findChild<QTcpSocket *>();
        delete announcer;
        pump();
        check(client.isNull() && catalog.requests.size() == 1, "destruction cancels pending socket and no late registration occurs");
    }
    {
        Catalog catalog;
        Mesh::DirectoryAnnouncer *self = nullptr;
        Mesh::DirectoryAnnouncer announcer([&] { self->stop(); return entry(); });
        self = &announcer;
        announcer.configure(catalog.config());
        check(waitUntil([&] { return catalog.requests.size() == 1 && !announcer.status().value("configured").toBool(); }) &&
              catalog.requests.size() == 1 && safeStatus(announcer.status()), "metadata callback stop is epoch guarded before credential use");
    }
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    metadataTests();
    directoryTests();
    configurationTests();
    successfulAnnouncerTests();
    announcerFailureTests();
    fragmentedReplyTests();
    replyBoundaryAndRetryTests();
    cancellationTests();
    std::printf("Directory helper self-test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
