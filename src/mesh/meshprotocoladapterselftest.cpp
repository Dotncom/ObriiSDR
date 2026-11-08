#include "meshprotocoladapter.h"
#include "../meshtastic/meshtastic.h"
#include "../meshtastic/meshpki.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQueue>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Mesh::ProtocolAdapter;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
struct Flight { ProtocolAdapter *sender; quint64 ticket; QString interface; QByteArray wire; };

struct Harness {
    QQueue<Flight> flights;
    std::vector<ProtocolAdapter *> nodes;
    QMap<ProtocolAdapter *, QList<QJsonObject>> events;
    QMap<QString, int> interfaceFlights;
    QList<QString> failures;
    qint64 now = 0;

    void add(ProtocolAdapter *node) {
        nodes.push_back(node);
        QObject::connect(node, &ProtocolAdapter::transmit, node,
            [this, node](quint64 ticket, const QString &interface, const QByteArray &wire) {
                ++interfaceFlights[interface];
                flights.enqueue({node, ticket, interface, wire});
            });
        QObject::connect(node, &ProtocolAdapter::event, node,
                         [this, node](const QJsonObject &event) {
                require(event.value("status").isString() && event.value("status") == event.value("event"), "stable event.status string");
                events[node].append(event);
            });
        QObject::connect(node, &ProtocolAdapter::failed, node, [this](const QString &error) { failures.append(error); });
    }
    int count(ProtocolAdapter *node, const QString &name) const {
        int result = 0;
        for (const auto &event : events.value(node)) if (event.value("event").toString() == name) ++result;
        return result;
    }
    void tick(bool relayTopology = false) {
        for (auto node : nodes) node->step(now);
        int budget = 128;
        while (!flights.isEmpty()) {
            require(--budget > 0, "wire loop exceeded bounded flight budget");
            const auto flight = flights.dequeue();
            require(!flight.wire.isEmpty() && flight.wire.size() <= 255, "wire outside native radio MTU");
            flight.sender->complete(flight.ticket, true, now);
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i] == flight.sender) continue;
                if (relayTopology && nodes.size() == 3 &&
                    ((flight.sender == nodes[0] && i == 2) || (flight.sender == nodes[2] && i == 0))) continue;
                nodes[i]->receive(flight.interface, flight.wire, now);
            }
        }
        QCoreApplication::processEvents();
        ++now;
    }
    bool wait(const std::function<bool()> &predicate, int timeoutMs) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!predicate() && elapsed.elapsed() < timeoutMs) {
            tick();
            QThread::msleep(5);
        }
        if (!predicate())
            for (const auto &failure : failures) std::cerr << "Backend failure: " << failure.toStdString() << '\n';
        return predicate();
    }
};

QJsonObject meshtastic(quint32 id) {
    return {{"nodeNumber", double(id)}, {"relayDelayMs", 0}, {"relayJitterMs", 0},
            {"ackTimeoutMs", 80}, {"retryDelayMs", 10}, {"maxRetries", 1}, {"historyTtlMs", 1000}};
}

void testMeshtastic() {
    QTemporaryDir stateA, stateB, stateRelay;
    require(stateA.isValid() && stateB.isValid() && stateRelay.isValid(), "temporary state directories");
    auto a = Mesh::makeMeshtasticAdapter(), b = Mesh::makeMeshtasticAdapter(), relay = Mesh::makeMeshtasticAdapter();
    Harness harness;
    harness.add(a.get()); harness.add(relay.get()); harness.add(b.get());
    QString error;
    const auto configA = meshtastic(0xa0000011), configB = meshtastic(0xa0000022), configRelay = meshtastic(0xa0000033);
    require(a->start(configA, {"radio"}, stateA.path(), &error), "start Meshtastic A");
    require(b->start(configB, {"radio"}, stateB.path(), &error), "start Meshtastic B");
    require(relay->start(configRelay, {"radio"}, stateRelay.path(), &error), "start Meshtastic relay");
    auto competing = Mesh::makeMeshtasticAdapter();
    require(!competing->start(configA, {"radio"}, stateA.path(), &error), "state lock must exclude a second owner");
    require(!competing->start(configA, {"radio", "other"}, stateA.path(), &error), "reject multiple Meshtastic interfaces");
    require(a->status().value("nativeAddress").toString() == QString::number(0xa0000011u), "stable native node address");
    const QJsonObject send{{"op", "send"}, {"destination", "!a0000022"}, {"text", "native wire"}, {"reliable", true}};
    const auto first = a->command(send, harness.now);
    require(first.value("ok").toBool(), "queue reliable native send");
    a->step(harness.now++);
    require(harness.flights.size() == 1, "exactly one native TX flight");
    const auto held = harness.flights.dequeue();
    const obrii::meshtastic::Bytes native(held.wire.begin(), held.wire.end());
    const auto decoded = obrii::meshtastic::decodeRadio(native);
    require(decoded.header.from == 0xa0000011u && decoded.header.to == 0xa0000022u, "native Meshtastic header, not host wrapper");
    for (int i = 0; i < 500; ++i) a->step(harness.now++);
    require(harness.count(a.get(), "tx_failed") == 0 && a->status().value("pending").toInt() == 1,
            "ACK timer must not begin before actual TX completion");
    a->complete(held.ticket, false, harness.now++);
    require(harness.count(a.get(), "tx_failed") == 1, "failed aggregate TX reaches native engine");
    require(a->command(send, harness.now).value("ok").toBool(), "queue early-ACK fanout timing test");
    a->step(harness.now++);
    require(harness.flights.size() == 1, "early ACK original flight");
    const auto original = harness.flights.dequeue();
    b->receive("radio", original.wire, harness.now);
    b->step(harness.now++);
    require(harness.flights.size() == 1, "native ACK queued on fast peer");
    const auto earlyAck = harness.flights.dequeue();
    b->complete(earlyAck.ticket, true, harness.now);
    a->receive("radio", earlyAck.wire, harness.now++);
    require(harness.count(a.get(), "acknowledged") == 0 && a->status().value("deferredRx").toInt() == 1,
            "ACK waits for all aggregate TX fanout links to settle");
    a->complete(original.ticket, true, harness.now++);
    require(harness.count(a.get(), "acknowledged") == 1 && a->status().value("deferredRx").toInt() == 0,
            "early native ACK is processed after aggregate TX success");
    const auto messagesBeforeRelay = harness.count(b.get(), "message");
    const auto acksBeforeRelay = harness.count(a.get(), "acknowledged");
    const auto second = a->command(send, harness.now);
    require(second.value("ok").toBool(), "queue next reliable native send");
    for (int i = 0; i < 40; ++i) harness.tick(true);
    require(harness.count(b.get(), "message") == messagesBeforeRelay + 1, "relay delivers exactly one message");
    require(harness.count(a.get(), "acknowledged") == acksBeforeRelay + 1, "native ACK traverses relay back to sender");
    const auto previousId = second.value("nativeId").toDouble();
    a->stop();
    require(a->start(configA, {"radio"}, stateA.path(), &error), "restart persisted Meshtastic node");
    const auto afterRestart = a->command(send, harness.now);
    require(afterRestart.value("ok").toBool() && afterRestart.value("nativeId").toDouble() > previousId,
            "packet ID must advance after restart");
    a->step(harness.now++);
    require(!harness.flights.isEmpty(), "restarted sender emits native wire");
    const auto restarted = harness.flights.dequeue();
    const auto restartedPacket = obrii::meshtastic::decodeRadio(obrii::meshtastic::Bytes(restarted.wire.begin(), restarted.wire.end()));
    require(decoded.body != restartedPacket.body, "same plaintext must not reuse AES-CTR ciphertext after restart");
    a->complete(restarted.ticket, true, harness.now++);
    const auto retryWire = restarted.wire;
    harness.now += 100;
    a->step(harness.now);
    harness.now += 10;
    a->step(harness.now++);
    require(!harness.flights.isEmpty() && harness.flights.head().wire == retryWire, "retry preserves native packet ID and ciphertext");
    const auto retry = harness.flights.dequeue();
    a->complete(retry.ticket, false, harness.now++);

    // Channel renaming/reordering must not create a fresh sequence for an old key.
    a->stop();
    auto renamed = configA;
    renamed.insert("channels", QJsonArray{QJsonObject{{"name", "Other"}, {"pskBase64", "Ag=="}},
                                         QJsonObject{{"name", "Renamed"}, {"pskBase64", "AQ=="}}});
    require(a->start(renamed, {"radio"}, stateA.path(), &error), "start changed channel set");
    auto reorderedSend = send;
    reorderedSend.insert("channel", 1);
    const auto reordered = a->command(reorderedSend, harness.now);
    require(reordered.value("ok").toBool() && reordered.value("nativeId").toDouble() > afterRestart.value("nativeId").toDouble(),
            "effective-key namespace survives channel reconfiguration");
    const auto exported = QJsonDocument(a->status()).toJson(QJsonDocument::Compact);
    require(!exported.contains("pskBase64") && !exported.contains("keyHash") && !exported.contains(stateA.path().toUtf8()), "status excludes key material and state paths");
    a->stop();

    const auto counters = QDir(stateA.path()).entryList({"meshtastic-*.json"}, QDir::Files);
    require(!counters.isEmpty(), "persistent counters exist");
    for (const auto &name : counters) {
        QSaveFile file(QDir(stateA.path()).filePath(name));
        require(file.open(QIODevice::WriteOnly) && file.write("corrupt") == 7 && file.commit(), "corrupt temporary test counters");
    }
    require(!a->start(configA, {"radio"}, stateA.path(), &error), "corrupt nonce state must fail closed");
    require(!a->status().value("ready").toBool(), "corrupt state leaves TX disabled");
    b->stop(); relay->stop();

    QTemporaryDir failingState;
    require(a->start(configA, {"radio"}, failingState.path(), &error), "start persistence-failure test");
    const auto counter = QDir(failingState.path()).entryList({"meshtastic-*.json"}, QDir::Files).first();
    const auto counterPath = QDir(failingState.path()).filePath(counter);
    require(QFile::remove(counterPath) && QDir().mkdir(counterPath), "replace temporary counter with unwriteable target");
    const int oldFlights = harness.flights.size();
    require(!a->command(send, harness.now).value("ok").toBool(), "failed persistence rejects send before TX");
    a->step(harness.now++);
    require(!a->status().value("ready").toBool() && harness.flights.size() == oldFlights && !harness.failures.isEmpty(),
            "persistence failure disables adapter and emits no native bytes");
    a->stop();
    std::cout << "Meshtastic: native relay/ACK, TX failure, completion clock, retry, restart/key namespaces, lock and fail-closed persistence passed\n";
}

void testMeshtasticPki() {
    namespace Mt = obrii::meshtastic;
    QTemporaryDir stateA, stateB;
    auto a = Mesh::makeMeshtasticAdapter(), b = Mesh::makeMeshtasticAdapter();
    Harness harness;
    harness.add(a.get()); harness.add(b.get());
    auto configA = meshtastic(0xb0000011), configB = meshtastic(0xb0000022);
    configA.insert("pki", true); configB.insert("pki", true);
    QString error;
    require(a->start(configA, {"radio"}, stateA.path(), &error), "provision PKI identity A");
    require(b->start(configB, {"radio"}, stateB.path(), &error), "provision PKI identity B");
    const auto keyA = a->status().value("publicKeyHex").toString();
    const auto keyB = b->status().value("publicKeyHex").toString();
    const auto fingerprintA = a->status().value("nativeFingerprint").toString();
    require(keyA.size() == 64 && keyB.size() == 64 && keyA != keyB && fingerprintA.size() == 64, "distinct CSPRNG native identities and public fingerprints");
    require(fingerprintA == QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromHex(keyA.toLatin1()), QCryptographicHash::Sha256).toHex()), "fingerprint is SHA256 of the native public key");
    require(a->status().value("pki").toBool() && !a->status().value("admin").toBool() && !a->status().value("pkiAdmin").toBool(), "PKI enabled, all admin execution explicitly unsupported");
    require(a->status().value("pkiPeerCount").toInt() == 0 && b->status().value("pkiPeerCount").toInt() == 0, "identity provisioning never implies peer trust");
    auto competing = Mesh::makeMeshtasticAdapter();
    require(!competing->start(configA, {"radio"}, stateA.path(), &error), "identity/pin file is exclusively owned");
    const QJsonObject send{{"op", "send_pki"}, {"destination", "!b0000022"}, {"text", "authenticated native payload"}, {"reliable", true}};
    require(!a->command(send, harness.now).value("ok").toBool(), "unconfigured peer cannot receive PKI sends");
    a->stop();
    configA.insert("pkiPeers", QJsonArray{QJsonObject{{"nodeNumber", "!b0000022"}, {"publicKeyHex", keyB}}});
    require(a->start(configA, {"radio"}, stateA.path(), &error), "operator profile explicitly pins B");
    require(a->status().value("publicKeyHex").toString() == keyA, "adding peer pins preserves own private identity");

    // A valid NodeInfo public-key claim is still not an explicit operator pin.
    Mt::User user;
    user.id = "!b0000011";
    const auto publicA = QByteArray::fromHex(keyA.toLatin1());
    user.publicKey.assign(publicA.begin(), publicA.end());
    const auto nodeInfo = Mt::encodeUser(user);
    require(a->command({{"op", "send_native"}, {"destination", "!b0000022"},
                        {"payloadBase64", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(nodeInfo.data()), int(nodeInfo.size())).toBase64())},
                        {"data", QJsonObject{{"port", double(Mt::NodeInfoPort)}}}}, harness.now).value("ok").toBool(), "queue native NodeInfo claim");
    for (int i = 0; i < 4; ++i) harness.tick();
    require(b->status().value("pkiPeerCount").toInt() == 0, "NodeInfo must not implement trust-on-first-use");
    const auto untrusted = a->command(send, harness.now);
    require(untrusted.value("ok").toBool(), "sender has an explicit destination pin");
    for (int i = 0; i < 4; ++i) harness.tick();
    require(harness.count(b.get(), "rejected") > 0 && b->status().value("pkiPeerCount").toInt() == 0,
            "untrusted encrypted sender is rejected despite prior NodeInfo claim");
    require(a->command({{"op", "cancel"}, {"nativeId", untrusted.value("nativeId")}}, harness.now).value("ok").toBool(), "cancel intentionally untrusted send");
    b->stop();
    configB.insert("pkiPeers", QJsonArray{QJsonObject{{"nodeNumber", "!b0000011"}, {"publicKeyHex", keyA}}});
    require(b->start(configB, {"radio"}, stateB.path(), &error), "operator profile explicitly pins A");
    require(b->status().value("publicKeyHex").toString() == keyB, "B identity survives explicit pin provisioning");
    auto admin = send;
    admin.insert("data", QJsonObject{{"port", double(Mt::AdminPort)}});
    require(!a->command(admin, harness.now).value("ok").toBool(), "PKI admin commands remain unsupported");
    auto channel = send;
    channel.insert("channel", 0);
    require(!a->command(channel, harness.now).value("ok").toBool(), "PKI cannot silently select a PSK channel");
    require(!a->command({{"op", "pin_peer"}, {"nodeNumber", "!b0000022"}, {"publicKeyHex", keyB}}, harness.now).value("ok").toBool(), "no remote command can mutate trust pins");

    const auto reliable = a->command(send, harness.now);
    require(reliable.value("ok").toBool(), "queue mutually pinned PKI packet");
    a->step(harness.now++);
    require(harness.flights.size() == 1, "one PKI radio packet");
    const auto packet = harness.flights.dequeue();
    const auto native = Mt::decodeRadio(Mt::Bytes(packet.wire.begin(), packet.wire.end()));
    require(native.header.channelHash == 0, "PKI uses native channel hash zero");
    a->complete(packet.ticket, true, harness.now++);

    Mt::Routing routing;
    routing.kind = Mt::Routing::Kind::Error;
    Mt::Data fakeAck;
    fakeAck.port = Mt::RoutingPort;
    fakeAck.requestId = quint32(reliable.value("nativeId").toDouble());
    fakeAck.payload = Mt::encodeRouting(routing);
    Mt::Header header;
    header.from = 0xb0000022; header.to = 0xb0000011; header.id = 0xf1000001;
    const auto forged = Mt::seal(header, fakeAck, Mt::Channel{});
    a->receive("radio", QByteArray(reinterpret_cast<const char *>(forged.data()), int(forged.size())), harness.now++);
    require(harness.count(a.get(), "acknowledged") == 0 && a->status().value("pending").toInt() == 1,
            "unauthenticated PSK ACK cannot clear a native PKI send");
    b->receive("radio", packet.wire, harness.now);
    b->step(harness.now++);
    require(harness.flights.size() == 1, "native PKI receiver queues authenticated routing proof");
    const auto ack = harness.flights.dequeue();
    b->complete(ack.ticket, true, harness.now);
    a->receive("radio", ack.wire, harness.now++);
    require(harness.count(a.get(), "acknowledged") == 1, "pairwise authenticated native ACK accepted");
    bool authenticatedMessage = false, authenticatedAck = false, pkiSent = false;
    for (const auto &event : harness.events.value(b.get()))
        if (event.value("event").toString() == "message" && event.value("security").toString() == "pki")
            authenticatedMessage |= event.value("authenticated").toBool() && event.value("senderPublicKeyHex").toString() == keyA;
    for (const auto &event : harness.events.value(a.get())) {
        if (event.value("event").toString() == "acknowledged")
            authenticatedAck |= event.value("authenticated").toBool() && event.value("senderPublicKeyHex").toString() == keyB;
        if (event.value("event").toString() == "sent") pkiSent |= event.value("security").toString() == "pki";
    }
    require(authenticatedMessage && authenticatedAck && pkiSent, "native PKI TX/message/ACK events carry accurate security and authenticated public sender identity");

    a->stop(); b->stop();
    auto persistedA = configA, persistedB = configB;
    persistedA.remove("pkiPeers"); persistedB.remove("pkiPeers");
    require(a->start(persistedA, {"radio"}, stateA.path(), &error) && b->start(persistedB, {"radio"}, stateB.path(), &error), "reload durable private identities and peer pins");
    require(a->status().value("publicKeyHex").toString() == keyA && b->status().value("publicKeyHex").toString() == keyB &&
            a->status().value("nativeFingerprint").toString() == fingerprintA && a->status().value("pkiPeerCount").toInt() == 1,
            "private identity, fingerprint and explicit pins survive restart without TOFU");
    const auto restarted = a->command(send, harness.now);
    require(restarted.value("ok").toBool() && restarted.value("nativeId").toDouble() > reliable.value("nativeId").toDouble(), "PKI packet ID ledger advances across restart");
    for (int i = 0; i < 8; ++i) harness.tick();
    require(harness.count(a.get(), "acknowledged") == 2, "persisted identities and pins still authenticate native ACKs");

    a->stop();
    auto pskOnly = persistedA;
    pskOnly.insert("pki", false);
    require(a->start(pskOnly, {"radio"}, stateA.path(), &error), "temporarily disable PKI without destroying its identity");
    auto pskSend = send;
    pskSend.insert("op", "send");
    const auto psk = a->command(pskSend, harness.now);
    require(psk.value("ok").toBool() && psk.value("nativeId").toDouble() > restarted.value("nativeId").toDouble(), "PSK sequence advances after PKI use");
    require(a->command({{"op", "cancel"}, {"nativeId", psk.value("nativeId")}}, harness.now).value("ok").toBool(), "cancel mode-transition test packet before TX");
    a->stop();
    require(a->start(persistedA, {"radio"}, stateA.path(), &error), "re-enable stored PKI after PSK-only operation");
    const auto reenabled = a->command(send, harness.now);
    require(reenabled.value("ok").toBool() && reenabled.value("nativeId").toDouble() > psk.value("nativeId").toDouble(), "combined key namespaces prevent reuse across PKI mode changes");
    for (int i = 0; i < 8; ++i) harness.tick();
    require(harness.count(a.get(), "acknowledged") == 3, "PKI identity and pins remain valid after mode changes");

    const auto identityPath = QDir(stateA.path()).filePath(QDir(stateA.path()).entryList({"meshtastic-*.pki"}, QDir::Files).first());
    QFile identity(identityPath);
    require(identity.open(QIODevice::ReadOnly), "read temporary test identity fixture");
    QByteArray identityBytes = identity.read(10000);
    identity.close();
    struct WipeFixture { QByteArray &value; ~WipeFixture() { Mt::secureErase(value.data(), std::size_t(value.size())); } } wipeFixture{identityBytes};
    auto privateHex = QByteArray::fromRawData(identityBytes.constData() + 12, 32).toHex();
    const auto exported = QJsonDocument(a->status()).toJson(QJsonDocument::Compact);
    require(!exported.contains(privateHex) && !exported.contains("privateKey") && !exported.contains(identityPath.toUtf8()), "status exports only public identity, not private key or paths");
    Mt::secureErase(privateHex.data(), std::size_t(privateHex.size()));
    a->stop();
    auto mismatch = configA;
    mismatch.insert("pkiPeers", QJsonArray{QJsonObject{{"nodeNumber", "!b0000022"}, {"publicKeyHex", keyA}}});
    require(!a->start(mismatch, {"radio"}, stateA.path(), &error) && !a->status().value("ready").toBool(), "profile pin mismatch fails closed");
    require(identity.open(QIODevice::ReadOnly), "read identity after pin mismatch");
    auto afterMismatch = identity.read(10000);
    identity.close();
    require(afterMismatch == identityBytes, "pin mismatch must not overwrite durable identity or old pin");
    Mt::secureErase(afterMismatch.data(), std::size_t(afterMismatch.size()));
    require(a->start(persistedA, {"radio"}, stateA.path(), &error), "old explicit pins remain usable after rejected replacement");
    a->stop();

    const auto pkiCounters = QDir(stateA.path()).entryList({"meshtastic-*-pki-*.json"}, QDir::Files);
    require(pkiCounters.size() == 1, "own PKI identity has a dedicated durable nonce namespace");
    const auto counterPath = QDir(stateA.path()).filePath(pkiCounters.first());
    require(QFile::rename(counterPath, counterPath + ".saved"), "temporarily remove PKI nonce ledger");
    require(!a->start(persistedA, {"radio"}, stateA.path(), &error), "missing PKI nonce ledger cannot be silently reset");
    require(QFile::rename(counterPath + ".saved", counterPath), "restore temporary nonce ledger");
    QSaveFile corrupt(identityPath);
    require(corrupt.open(QIODevice::WriteOnly) && corrupt.write("corrupt") == 7 && corrupt.commit(), "corrupt temporary private-identity state");
    require(!a->start(persistedA, {"radio"}, stateA.path(), &error), "corrupt private-key state fails closed without generating a replacement");
    require(QFile::remove(identityPath), "remove corrupt temporary identity fixture");
    require(!a->start(persistedA, {"radio"}, stateA.path(), &error), "lost private identity beside an existing nonce ledger fails closed");
    require(harness.flights.isEmpty() && harness.failures.isEmpty(), "failed PKI startup cannot release native TX");
    a->stop(); b->stop();
    QTemporaryDir boundedState;
    auto bounded = meshtastic(0xc0000011);
    bounded.insert("pki", true);
    QJsonArray manyPins;
    for (quint32 i = 0; i < 256; ++i)
        manyPins.append(QJsonObject{{"nodeNumber", double(0xd0000100u + i)}, {"publicKeyHex", keyB}});
    bounded.insert("pkiPeers", manyPins);
    require(a->start(bounded, {"radio"}, boundedState.path(), &error) && a->status().value("pkiPeerCount").toInt() == 256,
            "full 256-entry explicit pin table is bounded and supported");
    a->stop();
    manyPins.append(QJsonObject{{"nodeNumber", "!d0000300"}, {"publicKeyHex", keyB}});
    bounded.insert("pkiPeers", manyPins);
    require(!a->start(bounded, {"radio"}, boundedState.path(), &error), "reject 257th peer pin");
    bounded.insert("pkiPeers", QJsonArray{QJsonObject{{"nodeNumber", "!d0000300"}, {"publicKeyHex", QString(64, '0')}}});
    QTemporaryDir weakState;
    require(!a->start(bounded, {"radio"}, weakState.path(), &error), "native crypto rejects weak/low-order public peer keys");
    bounded.remove("pkiPeers");
    bounded.insert("privateKeyHex", keyA);
    require(!a->start(bounded, {"radio"}, boundedState.path(), &error), "private keys cannot be imported through command/config JSON");
    std::cout << "Meshtastic PKI: two actual adapters, explicit pins/no TOFU, native authenticated message/ACK, forged PSK ACK rejection, durable identity/counters and fail-closed pin/key/ledger mismatch passed\n";
}

void testMeshtasticBinaryContract() {
    QTemporaryDir stateA, stateB, stateC;
    auto a = Mesh::makeMeshtasticAdapter(), b = Mesh::makeMeshtasticAdapter(), c = Mesh::makeMeshtasticAdapter();
    Harness harness;
    harness.add(a.get()); harness.add(b.get()); harness.add(c.get());
    QString error;
    auto start = [&](ProtocolAdapter *adapter, quint32 number, const QString &state) {
        auto config = meshtastic(number);
        config.insert("relayRole", "router");
        config.insert("channels", QJsonArray{QJsonObject{{"name", "PortTest"}, {"pskBase64", "AQ=="}, {"enabled", true}}});
        return adapter->start(config, {"radio"}, state, &error);
    };
    require(start(a.get(), 256, stateA.path()) && start(b.get(), 257, stateB.path()) && start(c.get(), 258, stateC.path()),
            "binary event-contract adapter setup");
    const auto payload = QByteArray::fromHex("004e41544956452d4c6f5261ff");
    const auto sent = a->command({{"op", "send"}, {"destination", 258},
                                 {"payloadBase64", QString::fromLatin1(payload.toBase64())}, {"reliable", true}}, harness.now);
    require(sent.value("ok").toBool(), "queue exact integration binary payload");
    for (int i = 0; i < 40; ++i) harness.tick(true);
    bool message = false, acknowledged = false;
    for (const auto &event : harness.events.value(c.get()))
        if (event.value("status") == "message" && event.value("nativeId") == sent.value("nativeId"))
            message |= event.value("packetId") == sent.value("nativeId") && event.value("from").toString() == "256" &&
                       event.value("to").toString() == "258" && event.value("payloadBase64").toString() == QString::fromLatin1(payload.toBase64());
    for (const auto &event : harness.events.value(a.get()))
        acknowledged |= event.value("status") == "acknowledged" && event.value("nativeId") == sent.value("nativeId");
    require(message && acknowledged, "binary payload and matching native message/ACK event IDs survive relay");
    a->stop(); b->stop(); c->stop();
    std::cout << "Meshtastic event contract: exact PortTest binary payload, nodes 256/257/258, message/ACK IDs and addresses passed\n";
}

void testReticulum(const QString &python, const QString &modules, const QString &framing, const QStringList &interfaces) {
    if (!QFileInfo(python).isFile() || !QFileInfo(modules).isDir()) {
        std::cout << "Reticulum: SKIPPED (pass --python and --modules for the isolated rns==1.5.5 runtime)\n";
        return;
    }
    QTemporaryDir stateA, stateB;
    auto a = Mesh::makeReticulumAdapter(), b = Mesh::makeReticulumAdapter();
    Harness harness;
    harness.add(a.get()); harness.add(b.get());
    QString error;
    const QJsonObject config{{"python", python}, {"moduleDirectory", modules}, {"framing", framing}, {"bitrate", 1000000}};
    require(a->start(config, interfaces, stateA.path(), &error), "accept Reticulum startup A");
    require(!a->status().value("ready").toBool(), "Reticulum accepted is not ready before status RPC");
    require(b->start(config, interfaces, stateB.path(), &error), "accept Reticulum startup B");
    QElapsedTimer startup;
    startup.start();
    while ((!a->status().value("ready").toBool() || !b->status().value("ready").toBool()) && startup.elapsed() < 20000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    require(a->status().value("ready").toBool() && b->status().value("ready").toBool(), "two real sidecars become ready without TX polling");
    require(a->status().value("startupTimeoutMs").toInt() == 15000 && a->status().value("rpcTimeoutMs").toInt() == 3000,
            "startup readiness budget is distinct from normal RPC timeout");
    const auto destination = b->status().value("nativeAddress").toString();
    require(destination.size() == 32, "native Reticulum destination hash");
    auto competing = Mesh::makeReticulumAdapter();
    require(!competing->start(config, interfaces, stateA.path(), &error), "Reticulum state has exclusive ownership");
    require(harness.flights.isEmpty(), "startup/readiness must never transmit without step polling");
    require(b->command({{"op", "announce"}}, harness.now).value("pending").toBool(), "announce is asynchronous RPC");
    for (int i = 0; i < 10; ++i) { QCoreApplication::processEvents(); QThread::msleep(5); }
    require(harness.flights.isEmpty(), "queued native announce does not transmit until step polls TX");
    require(harness.wait([&] { return harness.count(a.get(), "announce") > 0; }, 15000), "native RNS announce crosses radio fragments");
    require(a->command({{"op", "request_path"}, {"destination", destination}}, harness.now).value("pending").toBool(), "typed request_path RPC");
    const auto sent = a->command({{"op", "send"}, {"destination", destination}, {"text", "reticulum native bytes"}}, harness.now);
    require(sent.value("pending").toBool(), "native RNS send returns pending ID");
    require(harness.wait([&] { return harness.count(b.get(), "message") > 0 && harness.count(a.get(), "acknowledged") > 0; }, 20000), "native RNS encrypted message and delivery proof");
    const auto opened = a->command({{"op", "link_open"}, {"destination", destination}}, harness.now);
    require(opened.value("pending").toBool(), "link_open returns pending ID");
    require(harness.wait([&] { return harness.count(a.get(), "link_active") > 0 && harness.count(b.get(), "link_active") > 0; }, 20000), "native RNS link handshake");
    QString link;
    for (const auto &event : harness.events.value(a.get()))
        if (event.value("event").toString() == "link_active") link = event.value("link").toString();
    require(link.size() == 32, "native link hash");
    require(a->command({{"op", "request_echo"}, {"link", link}, {"text", "echo"}}, harness.now).value("pending").toBool(), "typed echo request");
    require(harness.wait([&] { return harness.count(a.get(), "response") > 0; }, 20000), "actual RNS request/reply callbacks");
    require(harness.failures.isEmpty(), "no unexpected sidecar failures");
    const auto nativeAddress = a->status().value("nativeAddress").toString();
    const auto exported = QJsonDocument(a->status()).toJson(QJsonDocument::Compact);
    require(!exported.contains(python.toUtf8()) && !exported.contains(stateA.path().toUtf8()) && !exported.contains("public_key"), "Reticulum status redacts runtime and key material");
    a->stop();
    require(a->start(config, interfaces, stateA.path(), &error), "restart real sidecar");
    require(harness.wait([&] { return a->status().value("ready").toBool(); }, 20000), "restarted sidecar readiness");
    require(a->status().value("nativeAddress").toString() == nativeAddress, "persistent RNS identity/destination");
    a->stop(); b->stop();

    auto invalid = config;
    invalid.insert("arguments", "--arbitrary");
    require(!a->start(invalid, {"radio"}, stateA.path(), &error), "reject arbitrary runtime arguments");
    invalid = config;
    invalid.insert("python", "python -c arbitrary");
    require(!a->start(invalid, {"radio"}, stateA.path(), &error), "reject shell strings as executable");
    require(!a->start(config, {"radio", "radio"}, stateA.path(), &error), "reject duplicate interfaces");
    require(!a->start(config, {"a", "b", "c", "d", "e", "f", "g", "h", "i"}, stateA.path(), &error), "reject ninth interface");
    require(a->start(config, {"radio"}, stateA.path(), &error), "start queue test sidecar");
    require(harness.wait([&] { return a->status().value("ready").toBool(); }, 20000), "queue test readiness");
    int accepted = 0;
    for (int i = 0; i < 64; ++i)
        if (a->command({{"op", "path"}, {"destination", destination}}, harness.now).value("ok").toBool()) ++accepted;
    require(accepted > 0 && accepted <= 24 && a->status().value("queuedRpcs").toInt() <= 32, "bounded async RPC queue");
    a->stop();
    for (const auto &interface : interfaces) require(harness.interfaceFlights.value(interface) > 0, "each native interface is polled without host rerouting");
    std::cout << "Reticulum: " << framing.toStdString() << ", " << interfaces.size()
              << " interfaces, two real rns==1.5.5 sidecars, announce/message/proof, links/echo, identity, lock and bounded RPC passed\n";
}

void testReticulumFailures(const QString &python) {
    if (!QFileInfo(python).isFile()) return;
    QTemporaryDir modules, state;
    require(QDir(modules.path()).mkdir("obrii_reticulum"), "create temporary fault-fixture package");
    const auto package = QDir(modules.path()).filePath("obrii_reticulum");
    auto fixture = [&](const QByteArray &source) {
        for (const auto &name : {"__init__.py", "service.py", "node.py", "transport.py", "fragments.py", "rnode.py", "access.py"}) {
            QSaveFile file(QDir(package).filePath(name));
            const auto content = QString(name) == "service.py" ? source : QByteArray("\n");
            require(file.open(QIODevice::WriteOnly) && file.write(content) == content.size() && file.commit(), "write local fault fixture");
        }
    };
    auto adapter = Mesh::makeReticulumAdapter();
    Harness harness;
    harness.add(adapter.get());
    QString error;
    const QJsonObject config{{"python", python}, {"moduleDirectory", modules.path()}};
    fixture("import sys\nsys.stderr.write('credential=do-not-export /private/runtime/path\\n')\nsys.stderr.flush()\n");
    require(adapter->start(config, {"radio"}, state.path(), &error), "startup stderr fixture accepted asynchronously");
    require(harness.wait([&] { return !harness.failures.isEmpty(); }, 4000), "startup stderr emits failed");
    require(!adapter->status().value("ready").toBool(), "startup stderr never produces readiness");
    auto exported = QJsonDocument(adapter->status()).toJson(QJsonDocument::Compact);
    require(!exported.contains("do-not-export") && !exported.contains("/private/runtime/path") && !exported.contains(modules.path().toUtf8()), "stderr credentials and paths are redacted");
    adapter->stop();
    harness.failures.clear();

    fixture("import sys,time\nsys.stdout.write('x'*32769)\nsys.stdout.flush()\ntime.sleep(10)\n");
    require(adapter->start(config, {"radio"}, state.path(), &error), "oversized stdout fixture accepted");
    require(harness.wait([&] { return !harness.failures.isEmpty(); }, 4000), "oversized stdout fails closed");
    require(!adapter->status().value("ready").toBool(), "oversized stdout never produces readiness");
    adapter->stop();
    harness.failures.clear();

    fixture("import time\ntime.sleep(10)\n");
    auto shortStartup = config;
    shortStartup.insert("startupTimeoutMs", 1200);
    QElapsedTimer deadline;
    deadline.start();
    require(adapter->start(shortStartup, {"radio"}, state.path(), &error), "unresponsive backend accepted asynchronously");
    require(harness.wait([&] { return !harness.failures.isEmpty(); }, 2500), "configured startup/status readiness deadline is enforced");
    require(deadline.elapsed() >= 1100 && deadline.elapsed() < 2500, "startup deadline is independently bounded by local configuration");
    adapter->stop();
    harness.failures.clear();

    fixture("import sys,json,time\nr=json.loads(sys.stdin.readline())\ntime.sleep(3.4)\n"
            "print(json.dumps({'id':r['id'],'ok':True,'result':{'destination':'11'*16,'rns_version':'1.5.5'}}),flush=True)\n"
            "for line in sys.stdin:\n time.sleep(10)\n");
    require(adapter->start(config, {"radio"}, state.path(), &error), "normal RPC timeout fixture accepted");
    QElapsedTimer fixtureStartup;
    fixtureStartup.start();
    require(harness.wait([&] { return adapter->status().value("ready").toBool(); }, 10000), "cold backend may take more than three seconds for initial status readiness");
    require(fixtureStartup.elapsed() >= 3300 && harness.failures.isEmpty(), "initial status RPC uses startup budget rather than normal RPC deadline");
    deadline.restart();
    require(adapter->command({{"op", "path"}, {"destination", QString(32, '2')}}, harness.now).value("pending").toBool(), "normal typed RPC accepted after readiness");
    require(harness.wait([&] { return !harness.failures.isEmpty(); }, 4500), "normal RPC still fails closed after three seconds");
    require(deadline.elapsed() >= 2900 && deadline.elapsed() < 4500 &&
            adapter->status().value("startupTimeoutMs").toInt() == 15000, "startup budget never extends ordinary RPC deadline");
    adapter->stop();
    harness.failures.clear();

    auto invalid = config;
    invalid.insert("startupTimeoutMs", 60001);
    require(!adapter->start(invalid, {"radio"}, state.path(), &error), "unbounded startup timeout is rejected");
    invalid.insert("startupTimeoutMs", true);
    require(!adapter->start(invalid, {"radio"}, state.path(), &error), "startup timeout must be a typed integer");
    invalid = config;
    invalid.insert("stateDirectory", state.path());
    require(!adapter->start(invalid, {"radio"}, state.path(), &error), "remote configuration cannot override state directory");
    invalid = config;
    invalid.insert("moduleDirectory", QDir(modules.path()).filePath("missing"));
    require(!adapter->start(invalid, {"radio"}, state.path(), &error), "missing backend rejected without process startup");
    std::cout << "Reticulum failures: stderr redaction, bounded JSON, independently bounded startup readiness/three-second RPC deadlines, state override and missing backend rejection passed\n";
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    auto option = [&](const QString &name) {
        const int index = arguments.indexOf(name);
        return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString();
    };
    try {
        testMeshtastic();
        testMeshtasticPki();
        testMeshtasticBinaryContract();
        testReticulum(option("--python"), option("--modules"), "rnode", {"radio"});
        testReticulum(option("--python"), option("--modules"), "obrn", {"a", "b", "c", "d", "e", "f", "g", "h"});
        testReticulumFailures(option("--python"));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
