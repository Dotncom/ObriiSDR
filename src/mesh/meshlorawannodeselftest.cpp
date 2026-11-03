#include "meshnode.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr int ReplyLimit = 128 * 1024;
const char *NetworkKey = "00112233445566778899aabbccddeeff";
const char *ApplicationKey = "ffeeddccbbaa99887766554433221100";
const char *DeviceEui = "8090a0b0c0d0e0f0";
int failures = 0;

QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString runKey() { auto s = uuid() + uuid(); s.remove('-'); return s; }
void events() {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
void pass(const char *why) { std::printf("PASS %s\n", why); std::fflush(stdout); }
quint64 count(const QJsonValue &v) { return v.isString() ? v.toString().toULongLong() : quint64(v.toDouble()); }
QString scalar(const QJsonValue &v) { return v.isString() ? v.toString() : QString::number(v.toDouble(), 'f', 0); }

QJsonObject rpc(int port, const QString &token, QJsonObject request, int timeoutMs = 3000) {
    const auto id = uuid(); request["requestId"] = id;
    if (request.value("op") != "pair") request["token"] = token;
    QTcpSocket socket; socket.setReadBufferSize(ReplyLimit + 1);
    bool failed = false;
    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&] {
        socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    });
    QObject::connect(&socket, &QTcpSocket::errorOccurred, &socket, [&](QAbstractSocket::SocketError error) {
        if (error != QAbstractSocket::RemoteHostClosedError) failed = true;
    });
    socket.connectToHost("127.0.0.1", quint16(port));
    QElapsedTimer timer; timer.start(); QByteArray buffer;
    while (timer.elapsed() < timeoutMs) {
        events(); buffer += socket.readAll();
        if (buffer.size() > ReplyLimit) return {};
        if (buffer.contains('\n') || failed) break;
        QThread::msleep(2);
    }
    const int end = buffer.indexOf('\n'); if (end < 0) return {};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(buffer.left(end), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() || document.object().value("requestId") != id) return {};
    return document.object();
}

QJsonObject profile(bool network, bool otaa, int tolerance = -1) {
    QJsonObject p{{"role", network ? "network" : "device"}, {"activation", otaa ? "otaa" : "abp"},
        {"initializeState", true}, {"frequencyHz", 868100000}, {"dataRate", 5}, {"rx1Offset", 1}, {"nbTrans", 1}};
    if (tolerance >= 0) p["clockToleranceMs"] = tolerance;
    if (otaa) {
        p["joinEuiHex"] = "0102030405060708"; p["devEuiHex"] = DeviceEui; p["appKeyHex"] = NetworkKey;
        if (network) { p["devAddr"] = "26011bda"; p["netId"] = "000013"; }
    } else { p["devAddr"] = "26011bda"; p["nwkSKeyHex"] = NetworkKey; p["appSKeyHex"] = ApplicationKey; }
    return p;
}
QJsonObject port(const QString &direction) {
    return {{"id", "radio"}, {"direction", direction}, {"backend", "network"}, {"enabled", true}};
}
QJsonObject node(const QString &id) {
    return {{"id", id}, {"name", id}, {"enabled", true}, {"ports", QJsonArray{port("rx"), port("tx")}},
        {"protocol", QJsonObject{{"type", "lorawan"}, {"profileId", "lw"}}}};
}
QJsonObject link(const QString &from, const QString &to, const QString &bearer) {
    // Deliberately not the LoRaWAN PHY. Each native radio plan must override it.
    const QJsonObject wrongStatic{{"type", "lora"}, {"sf", 7}, {"bw", 125000}, {"cr", 1}, {"os", 1},
        {"preamble", 16}, {"syncWord", 43}, {"invertIq", from == "A"}, {"payloadCrc", from != "A"}};
    return {{"tx", from}, {"rx", to}, {"txPort", "radio"}, {"rxPort", "radio"}, {"bearer", bearer},
        {"enabled", true}, {"delayMs", 2}, {"gainDb", 0}, {"modem", wrongStatic}};
}
QJsonObject topology(const QString &bearer) {
    return {{"nodes", QJsonArray{node("A"), node("B")}}, {"links", QJsonArray{link("A", "B", bearer), link("B", "A", bearer)}}};
}
QList<QJsonObject> outcomes(const QJsonObject &state, const QString &name, const QString &nativeId = {}) {
    QList<QJsonObject> result;
    for (const auto &v : state.value("results").toArray()) {
        const auto e = v.toObject();
        if (e.value("protocol") == "lorawan" && e.value("status") == name &&
            (nativeId.isEmpty() || scalar(e.value("nativeId")) == nativeId)) result.append(e);
    }
    return result;
}
QList<QJsonObject> messages(const QJsonObject &state, const QByteArray &payload, int portNumber) {
    QList<QJsonObject> result;
    for (const auto &e : outcomes(state, "message"))
        if (e.value("port").toInt(-1) == portNumber &&
            QByteArray::fromBase64(e.value("payloadBase64").toString().toLatin1()) == payload) result.append(e);
    return result;
}

class Rig {
public:
    struct Endpoint {
        QString id, admin, token, pairing, instance, directory;
        int port = 0;
        std::unique_ptr<Mesh::NodeService> service;
    };
    QTemporaryDir state;
    std::vector<std::unique_ptr<Endpoint>> endpoints;
    QString client = uuid(), session, secret;
    QVector<QJsonObject> latest;
    bool captureBaseline = Mesh::NodeService::radioCaptureActive();

    Rig(const QJsonObject &device, const QJsonObject &network) {
        require(state.isValid(), "Create private endpoint test root");
        add("A", device); add("B", network);
    }
    ~Rig() {
        try { release(); } catch (...) { /* Stop only this fixture's services after a failed privacy/RPC assertion. */ }
        for (auto &e : endpoints) e->service->command({{"operation", "stop"}});
        events();
    }
    QString redacted(QString s) const {
        for (const auto *key : {NetworkKey, ApplicationKey}) s.replace(QLatin1String(key), "[profile-key]", Qt::CaseInsensitive);
        for (const auto &e : endpoints) {
            for (const auto &value : {e->admin, e->token, e->pairing, e->directory}) if (!value.isEmpty()) s.replace(value, "[private]");
            s.replace(QDir::toNativeSeparators(e->directory), "[state]");
        }
        if (!secret.isEmpty()) s.replace(secret, "[run-key]");
        return s.left(600);
    }
    void privacy(const QJsonValue &value) const {
        if (value.isObject()) {
            const auto o = value.toObject();
            for (auto i = o.begin(); i != o.end(); ++i) {
                const auto key = i.key().toLower();
                require(!QStringList{"nwkskeyhex", "appskeyhex", "appkeyhex", "protocolprofiles", "protocolstatedirectory", "pairingsecret"}.contains(key),
                    "Public reply leaked a private profile field");
                privacy(i.value());
            }
        } else if (value.isArray()) { for (const auto &v : value.toArray()) privacy(v); }
        else if (value.isString()) {
            const auto s = value.toString();
            require(!s.contains(NetworkKey, Qt::CaseInsensitive) && !s.contains(ApplicationKey, Qt::CaseInsensitive), "Public reply leaked a profile key value");
            if (!secret.isEmpty()) require(!s.contains(secret), "Public reply leaked a session run key");
            for (const auto &e : endpoints) {
                for (const auto &token : {e->admin, e->token})
                    if (!token.isEmpty()) require(!s.contains(token), "Public reply leaked a service/pairing credential");
                require(s != e->pairing, "Public reply leaked a pairing PIN");
                require(!QDir::fromNativeSeparators(s).contains(QDir::fromNativeSeparators(e->directory)), "Public reply leaked a private state path");
            }
        }
    }
    QJsonObject call(int index, QJsonObject request) const {
        const auto &e = *endpoints.at(std::size_t(index)); request["instanceId"] = e.instance;
        if (!session.isEmpty() && !request.contains("session")) request["session"] = session;
        const auto reply = rpc(e.port, e.token, request);
        if (!reply.isEmpty()) {
            require((!reply.value("ok").toBool() && reply.value("instanceId").isUndefined()) || reply.value("instanceId") == e.instance,
                "Endpoint instance identity changed or disappeared");
            privacy(reply);
        }
        return reply;
    }
    QJsonObject protocol(int index, const QJsonObject &args) const { return call(index, {{"op", "protocol"}, {"args", args}}); }
    void begin() {
        require(release(), "Release prior owned session"); session = uuid(); secret = runKey();
    }
    QJsonObject configureOne(int index, const QJsonObject &graph) const {
        QJsonObject peers;
        for (const auto &e : endpoints) peers[e->id] = QJsonObject{{"host", "127.0.0.1"}, {"port", e->port}, {"instanceId", e->instance}};
        return call(index, {{"op", "configure"}, {"key", secret}, {"nodeId", endpoints[std::size_t(index)]->id},
            {"transport", "data"}, {"topology", graph}, {"peers", peers}});
    }
    void configure(const QJsonObject &graph) {
        begin();
        for (int i = 0; i < 2; ++i) {
            const auto reply = configureOne(i, graph);
            if (!reply.value("ok").toBool()) std::fprintf(stderr, "configure %s: %s\n", endpoints[std::size_t(i)]->id.toUtf8().constData(), redacted(reply.value("error").toString("no bounded RPC reply")).toUtf8().constData());
            require(reply.value("ok").toBool(), "Configure paired NodeServices with a local LoRaWAN profile");
        }
        latest = poll();
        for (int i = 0; i < 2; ++i) {
            const auto &s = latest[i]; const auto p = s.value("nativeProtocol").toObject();
            require(s.value("active").toBool() && s.value("session") == session && p.value("type") == "lorawan" &&
                p.value("ready").toBool() && p.value("interfaceId") == "radio" && p.value("role") == (i ? "network" : "device"),
                "Endpoint did not instantiate the correct LoRaWAN role/interface");
            require(p.value("timingSource") == "simulated-transport" && p.value("hardwareTimingVerified").isBool() &&
                !p.value("hardwareTimingVerified").toBool(), "Endpoint did not disclose modeled timing provenance");
            const auto bindings = s.value("ports").toArray();
            require(bindings.size() == 2, "Endpoint requires exactly one radio RX/TX interface");
            for (const auto &v : bindings) require(v.toObject().value("id") == "radio" && v.toObject().value("backend") == "network", "Native lab endpoint opened a non-network port");
        }
        noRf();
    }
    QVector<QJsonObject> poll() const {
        QVector<QJsonObject> states;
        for (int i = 0; i < 2; ++i) states.append(call(i, {{"op", "status"}}));
        return states;
    }
    bool await(const std::function<bool(const QVector<QJsonObject> &)> &predicate, int timeoutMs) {
        QElapsedTimer timer; timer.start();
        do {
            latest = poll();
            for (const auto &s : latest) {
                const auto p = s.value("nativeProtocol").toObject();
                if (!s.value("ok").toBool() || !s.value("active").toBool() || !p.value("ready").toBool() || !p.value("error").toString().isEmpty()) {
                    diagnose(); return false;
                }
            }
            if (predicate(latest)) return true;
            events(); QThread::msleep(15);
        } while (timer.elapsed() < timeoutMs);
        diagnose(); return false;
    }
    bool release() {
        if (session.isEmpty()) return true;
        bool ok = true;
        for (int i = 0; i < int(endpoints.size()); ++i) {
            const auto s = call(i, {{"op", "status"}});
            if (!s.value("ok").toBool()) { ok = false; continue; }
            if (!s.value("active").toBool()) continue;
            if (s.value("session") != session) { ok = false; continue; }
            const auto reply = call(i, {{"op", "release"}});
            ok &= reply.value("ok").toBool() && !reply.value("active").toBool();
        }
        session.clear(); secret.fill('0'); secret.clear(); return ok;
    }
    void noRf() const {
        for (const auto &s : poll()) require(s.value("ok").toBool() && s.value("listening").toBool() &&
            !s.value("rfEnabled").toBool() && !s.value("radioTxAllowed").toBool() &&
            s.value("radioIngress").toArray().isEmpty() && s.value("radioEgress").toArray().isEmpty(), "Test service opened a physical radio path");
        require(Mesh::NodeService::radioCaptureActive() == captureBaseline, "Endpoint test changed unrelated radio-capture ownership");
    }
private:
    void add(const QString &id, const QJsonObject &p) {
        auto e = std::make_unique<Endpoint>(); e->id = id; e->directory = state.filePath("host-" + id);
        require(QDir().mkpath(e->directory) && QFile::setPermissions(e->directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "Create private service-local profile state directory");
        e->service = std::make_unique<Mesh::NodeService>();
        const auto started = e->service->command({{"operation", "start"}, {"bind", "127.0.0.1"}, {"port", 0},
            {"protocolProfiles", QJsonObject{{"lw", p}}}, {"protocolStateDirectory", e->directory}});
        require(started.value("ok").toBool(), "Start test-owned loopback NodeService");
        e->admin = started.value("token").toString(); e->pairing = started.value("pairingSecret").toString();
        e->instance = started.value("instanceId").toString(); e->port = started.value("port").toInt();
        const auto pin = rpc(e->port, e->admin, {{"op", "status"}});
        require(pin.value("protocol") == "obrii-mesh-node" && pin.value("version") == 1 &&
            pin.value("instanceId") == e->instance && !pin.value("active").toBool(), "Pin isolated node instance identity");
        const auto paired = rpc(e->port, {}, {{"op", "pair"}, {"instanceId", e->instance}, {"clientId", client},
            {"clientName", "LoRaWAN node integration test"}, {"code", e->pairing}});
        require(paired.value("ok").toBool() && paired.value("instanceId") == e->instance && !paired.value("token").toString().isEmpty(), "Pair a private endpoint test client");
        e->token = paired.value("token").toString(); endpoints.push_back(std::move(e));
    }
    void diagnose() const {
        for (int i = 0; i < latest.size(); ++i) {
            const auto &s = latest[i]; const auto p = s.value("nativeProtocol").toObject();
            QJsonObject summary{{"node", endpoints[std::size_t(i)]->id}, {"active", s.value("active")},
                {"ready", p.value("ready")}, {"activated", p.value("activated")}, {"queued", p.value("queued")},
                {"pending", p.value("pending")}, {"error", p.value("error")}, {"tx", s.value("tx")}, {"rx", s.value("rx")},
                {"dropped", s.value("dropped")}, {"rejected", p.value("rejected")},
                {"iqTxSamples", s.value("iqTxSamples")}, {"iqRxSamples", s.value("iqRxSamples")}};
            QJsonArray tail; const auto all = s.value("results").toArray();
            for (int at = (std::max)(0, all.size() - 8); at < all.size(); ++at) {
                const auto e = all[at].toObject(); QJsonObject item;
                for (const char *name : {"status", "event", "nativeId", "ticket", "counter", "reason", "authenticated", "timingSource"})
                    if (e.contains(name)) item[name] = e.value(name);
                tail.append(item);
            }
            summary["events"] = tail;
            std::fprintf(stderr, "%s\n", redacted(QString::fromUtf8(QJsonDocument(summary).toJson(QJsonDocument::Compact))).toUtf8().constData());
        }
    }
};

void checkRadioEvents(const QJsonObject &state, const QString &nativeId, bool downlink) {
    const auto plans = outcomes(state, "tx_plan", nativeId), starts = outcomes(state, "tx_started", nativeId), sent = outcomes(state, "sent", nativeId);
    require(plans.size() == 1 && starts.size() == 1 && sent.size() == 1, "Native request did not produce one plan, start authorization and on-air completion");
    const auto &p = plans.first(); const auto &s = starts.first();
    require(p.value("ticket") == s.value("ticket") && s.value("ticket") == sent.first().value("ticket") &&
        count(s.value("startUs")) >= count(p.value("startUs")) && count(s.value("startUs")) <= count(p.value("startDeadlineUs")) &&
        count(s.value("endUs")) - count(s.value("startUs")) == count(p.value("airtimeUs")) && p.value("airtimeUs") == s.value("airtimeUs"),
        "Transport start metadata lost ticket, deadline or native airtime consistency");
    const auto modem = s.value("modem").toObject(); const int rate = s.value("dataRate").toInt(-1);
    require(modem.value("type") == "lora" && modem.value("cr") == 1 && modem.value("os") == 1 && modem.value("bw") == 125000 &&
        modem.value("syncWord") == 52 && modem.value("preamble") == 8 && modem.value("invertIq").toBool() == downlink &&
        modem.value("payloadCrc").toBool() != downlink, "Per-frame PHY was replaced by the deliberately wrong static link modem");
    require(downlink ? ((rate == 4 && modem.value("sf") == 8) || (rate == 0 && modem.value("sf") == 12)) :
        (rate == 5 && modem.value("sf") == 7), "RX1/RX2 downlink rate did not differ from the configured uplink");
    for (const auto &e : {p, s, sent.first()}) require(e.value("timingSource") == "simulated-transport", "Native event omitted simulated transport provenance");
    require(s.value("hardwareTimingVerified").isBool() && !s.value("hardwareTimingVerified").toBool(), "Lab transport claimed qualified hardware timing");
}

void roundtrip(const QString &bearer, bool otaa) {
    // ABP omits tolerance to exercise the host default. OTAA explicitly supplies it.
    Rig rig(profile(false, otaa, otaa ? 250 : -1), profile(true, otaa, otaa ? 250 : -1));
    rig.configure(topology(bearer));
    const auto blockedStart = rig.protocol(0, {{"op", "tx_started"}, {"ticket", "1"}, {"startMs", 0}});
    const auto blockedContext = rig.protocol(0, {{"op", "rx_context"}, {"frequencyHz", 868100000}, {"dataRate", 5}, {"packetStartMs", 0}, {"packetEndMs", 1}});
    require(!blockedStart.value("ok").toBool() && blockedStart.value("error").toString().contains("host-only") &&
        !blockedContext.value("ok").toBool() && blockedContext.value("error").toString().contains("host-only"), "Remote application RPC forged radio timing callbacks");
    if (otaa) {
        require(!rig.latest[0].value("nativeProtocol").toObject().value("activated").toBool() &&
            rig.latest[0].value("nativeProtocol").toObject().value("nativeAddress") == DeviceEui, "OTAA device invented an activation before join");
        require(!rig.protocol(0, {{"op", "send"}, {"payloadBase64", "AA=="}, {"confirmed", true}}).value("ok").toBool(), "Unjoined OTAA device sent native application traffic");
        const auto join = rig.protocol(0, {{"op", "join"}, {"ttlMs", 60000}});
        require(join.value("ok").toBool() && join.value("protocolReply").toObject().value("queued").toBool(), "Explicit native OTAA join was not queued");
        require(rig.await([](const QVector<QJsonObject> &states) {
            return states[0].value("nativeProtocol").toObject().value("activated").toBool() &&
                states[1].value("nativeProtocol").toObject().value("activated").toBool() &&
                outcomes(states[0], "activated").size() == 1 && outcomes(states[1], "join_requested").size() == 1;
        }, 20000), "Paired services did not complete native OTAA over the selected link bearer");
    }
    const QByteArray uplink = QByteArray::fromHex("004c572d5550ff"), downlink = QByteArray::fromHex("ff4c572d444e00");
    const auto queuedDown = rig.protocol(1, {{"op", "send"}, {"port", 11}, {"payloadBase64", QString::fromLatin1(downlink.toBase64())}, {"confirmed", true}, {"ttlMs", 60000}});
    require(queuedDown.value("ok").toBool(), "Queue native network downlink without a mesh destination");
    const auto downId = scalar(queuedDown.value("protocolReply").toObject().value("nativeId"));
    require(!downId.isEmpty() && downId != "0" && outcomes(queuedDown, "tx_plan", downId).isEmpty(), "Network transmitted an application downlink without a data-uplink window");
    const auto queuedUp = rig.protocol(0, {{"op", "send"}, {"port", 10}, {"payloadBase64", QString::fromLatin1(uplink.toBase64())}, {"confirmed", true}, {"ttlMs", 60000}});
    const auto upId = scalar(queuedUp.value("protocolReply").toObject().value("nativeId"));
    require(queuedUp.value("ok").toBool() && !upId.isEmpty() && upId != "0", "Queue native confirmed device uplink without a destination");
    require(rig.await([&](const QVector<QJsonObject> &states) {
        return messages(states[1], uplink, 10).size() == 1 && messages(states[0], downlink, 11).size() == 1 &&
            !outcomes(states[0], "acknowledged").isEmpty() && !outcomes(states[1], "acknowledged").isEmpty() &&
            !states[0].value("nativeProtocol").toObject().value("pending").toBool() &&
            !states[1].value("nativeProtocol").toObject().value("pending").toBool();
    }, 25000), "NodeService native payloads/ACKs did not complete across the modeled LoRaWAN link");
    const auto up = messages(rig.latest[1], uplink, 10).first(), down = messages(rig.latest[0], downlink, 11).first();
    require(up.value("authenticated").toBool() && down.value("authenticated").toBool() && up.value("confirmed").toBool() &&
        down.value("confirmed").toBool() && down.value("ack").toBool(), "Native MIC/confirmed ACK semantics were lost at the endpoint boundary");
    for (const auto &e : {up, down}) require(!e.contains("path") && !e.contains("source") && !e.contains("destination") &&
        e.value("timingSource") == "simulated-transport", "Native LoRaWAN traffic was replaced by a lab mesh route or unqualified RF claim");
    for (const auto &s : rig.latest) require(s.value("nativeProtocol").toObject().value("devAddr") == "26011bda" &&
        count(s.value("forwarded")) == 0, "Single-device LoRaWAN invented mesh forwarding or a different native address");
    checkRadioEvents(rig.latest[0], upId, false); checkRadioEvents(rig.latest[1], downId, true);
    for (const auto &s : rig.latest) {
        require(count(s.value("dropped")) == 0 && count(s.value("nativeProtocol").toObject().value("rejected")) == 0,
            "Clean modeled link dropped or rejected a native frame");
        const auto links = s.value("links").toArray(); require(links.size() == 1 && links.first().toObject().value("bearer") == bearer, "Endpoint did not retain exactly one directed outgoing link");
        if (bearer == "iq") require(count(s.value("iqTxSamples")) > 0 && count(s.value("iqRxSamples")) > 0 &&
            count(links.first().toObject().value("samples")) > 0, "IQ test delivered through a DATA bypass instead of CF32 modulation/demodulation");
        else require(count(s.value("iqTxSamples")) == 0 && count(s.value("iqRxSamples")) == 0 &&
            count(links.first().toObject().value("samples")) == 0, "DATA modeled-airtime test unexpectedly performed IQ DSP");
    }
    rig.noRf();
    const auto beforeA = rig.latest[0].value("nativeProtocol").toObject().value("nextTransmitCounter");
    const auto beforeB = rig.latest[1].value("nativeProtocol").toObject().value("nextTransmitCounter");
    require(rig.release(), "Release paired native sessions without stopping listeners");
    rig.configure(topology(bearer));
    require(rig.latest[0].value("nativeProtocol").toObject().value("nextTransmitCounter") == beforeA &&
        rig.latest[1].value("nativeProtocol").toObject().value("nextTransmitCounter") == beforeB &&
        rig.latest[0].value("nativeProtocol").toObject().value("activated").toBool(), "Session reconfigure lost durable FCnt or OTAA activation");
    require(rig.release(), "Release reopened private native state ownership");
    pass(otaa ? "OTAA endpoint join/data/ACK/reconfigure preserves native bytes and private state" :
        "ABP endpoint data/ACK/reconfigure preserves native bytes and private state");
}

void hostGates() {
    Rig rig(profile(false, false), profile(true, false));
    auto graph = topology("data"); auto nodes = graph.value("nodes").toArray(); auto a = nodes[0].toObject();
    auto publicProtocol = a.value("protocol").toObject(); publicProtocol["nwkSKeyHex"] = NetworkKey;
    a["protocol"] = publicProtocol; nodes[0] = a; graph["nodes"] = nodes;
    rig.begin(); const auto secret = rig.configureOne(0, graph);
    require(!secret.value("ok").toBool() && secret.value("error").toString().contains("locally approved profile"), "Remote topology installed LoRaWAN RF/session keys");
    graph = topology("data"); nodes = graph.value("nodes").toArray(); a = nodes[0].toObject();
    auto bindings = a.value("ports").toArray(); bindings.append(QJsonObject{{"id", "observer"}, {"direction", "rx"}, {"backend", "radio"}, {"enabled", true}});
    a["ports"] = bindings; nodes[0] = a; graph["nodes"] = nodes;
    rig.begin(); const auto physical = rig.configureOne(0, graph);
    require(!physical.value("ok").toBool() && physical.value("error").toString().contains("qualified timestamped"), "Physical LoRaWAN was silently enabled without qualified timed SDR support");
    graph = topology("data"); graph["links"] = QJsonArray{link("B", "A", "data")};
    rig.begin(); const auto noPeer = rig.configureOne(0, graph);
    require(!noPeer.value("ok").toBool() && noPeer.value("error").toString().contains("one outgoing"), "Native LoRaWAN accepted a missing outgoing peer");
    rig.noRf(); require(rig.release(), "Release rejected configuration attempts");
    Rig explicitProfile(profile(false, false), profile(true, false, 251));
    explicitProfile.begin(); const auto invalid = explicitProfile.configureOne(1, topology("data"));
    require(!invalid.value("ok").toBool(), "Host overwrote an explicit invalid profile clock tolerance with its default");
    require(explicitProfile.release(), "Release explicit invalid-profile attempt");
    pass("private-profile enforcement, host-only RF qualification, one peer and explicit tolerance override validation");
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); app.setApplicationName("Obrii LoRaWAN node integration self-test");
    const auto args = app.arguments();
    for (int i = 1; i < args.size(); ++i) if (args[i] != "--data-only" && args[i] != "--iq-only") {
        std::fprintf(stderr, "Usage: ObriiMeshLoRaWanNodeSelfTest [--data-only | --iq-only]\n"); return 2;
    }
    if (args.contains("--data-only") && args.contains("--iq-only")) return 2;
    auto run = [](const char *name, const std::function<void()> &test) {
        std::printf("CASE %s\n", name); std::fflush(stdout);
        try { test(); } catch (const std::exception &e) { ++failures; std::fprintf(stderr, "FAIL %s: %s\n", name, e.what()); }
    };
    run("LoRaWAN host policy gates", hostGates);
    if (!args.contains("--iq-only")) {
        run("paired ABP DATA modeled airtime", [] { roundtrip("data", false); });
        run("paired OTAA DATA modeled airtime", [] { roundtrip("data", true); });
    }
    if (!args.contains("--data-only")) {
        run("paired ABP IQ dynamic PHY", [] { roundtrip("iq", false); });
        run("paired OTAA IQ dynamic PHY", [] { roundtrip("iq", true); });
    }
    std::printf("%d failure(s); test-owned loopback services only, no physical LoRaWAN RF\n", failures);
    return failures ? 1 : 0;
}
