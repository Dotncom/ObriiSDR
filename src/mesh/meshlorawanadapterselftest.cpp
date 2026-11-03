#include "meshprotocoladapter.h"
#include "../lorawan/lorawan.h"
#include "../lorawan/regional.h"
#ifdef OBRII_LORAWAN_WITH_LORA
#include "../lorawan/lorawanlora.h"
#endif
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQueue>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <optional>
#include <stdexcept>

// The host adds the public declaration/dispatch; no production header is changed here.
namespace Mesh { std::unique_ptr<ProtocolAdapter> makeLoRaWanAdapter(QObject *parent); }

namespace {
namespace Lw = obrii::lorawan;
using Mesh::ProtocolAdapter;
const char *NetworkKey = "00112233445566778899aabbccddeeff";
const char *ApplicationKey = "ffeeddccbbaa99887766554433221100";

void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
void pass(const char *why) { std::printf("PASS %s\n", why); std::fflush(stdout); }
Lw::Bytes bytes(const QByteArray &b) { return {b.begin(), b.end()}; }
QByteArray bytes(const Lw::Bytes &b) { return QByteArray(reinterpret_cast<const char *>(b.data()), int(b.size())); }
QJsonObject abp(bool network) {
    return {{"role", network ? "network" : "device"}, {"activation", "abp"}, {"devAddr", "26011bda"},
        {"nwkSKeyHex", NetworkKey}, {"appSKeyHex", ApplicationKey}, {"initializeState", true},
        {"dataRate", 5}, {"frequencyHz", 868100000}, {"nbTrans", 3}};
}
QJsonObject otaa(bool network) {
    QJsonObject c{{"role", network ? "network" : "device"}, {"activation", "otaa"},
        {"joinEuiHex", "0102030405060708"}, {"devEuiHex", "8090a0b0c0d0e0f0"},
        {"appKeyHex", NetworkKey}, {"initializeState", true}, {"dataRate", 5}};
    if (network) { c.insert("devAddr", "26011bda"); c.insert("netId", "000013"); }
    return c;
}
QJsonObject readBody(const QString &root) {
    QFile f(QDir(root).filePath("lorawan-state.json"));
    require(f.open(QIODevice::ReadOnly), "Cannot inspect unit-test state");
    return QJsonDocument::fromJson(f.readAll()).object().value("body").toObject();
}
Lw::Bytes savedNative(const QString &root) {
    return bytes(QByteArray::fromBase64(readBody(root).value("native").toString().toLatin1()));
}
std::optional<Lw::SessionState> savedSession(const QString &root, const QJsonObject &config) {
    const auto native = savedNative(root);
    if (config.value("activation") == "abp") return Lw::loadSession(native);
    if (config.value("role") == "network") return Lw::loadJoinServer(native).activated;
    return Lw::loadDeviceJoin(native).activated;
}
void noSecrets(const QJsonObject &value) {
    const auto json = QJsonDocument(value).toJson(QJsonDocument::Compact).toLower();
    require(!json.contains(NetworkKey) && !json.contains(ApplicationKey) && !json.contains("nwkskeyhex") &&
        !json.contains("appskeyhex") && !json.contains("appkeyhex") && !json.contains("\"native\""),
        "Keys or private module state escaped through status/events/commands");
}
struct Tx { quint64 ticket; QByteArray wire; QJsonObject plan; };
struct Endpoint {
    QTemporaryDir root;
    std::unique_ptr<ProtocolAdapter> adapter = Mesh::makeLoRaWanAdapter(nullptr);
    QJsonObject config, plan;
    QQueue<Tx> flights;
    QList<QJsonObject> events;
    QStringList failures;

    explicit Endpoint(const QJsonObject &c) : config(c) {
        require(root.isValid(), "Cannot create private unit-test stateRoot");
        QObject::connect(adapter.get(), &ProtocolAdapter::event, adapter.get(), [this](const QJsonObject &e) {
            noSecrets(e);
            require(e.value("type") == "lorawan" && e.value("status") == e.value("event"), "Stable LoRaWAN event schema");
            events.append(e);
            if (e.value("event") == "tx_plan") plan = e;
        });
        QObject::connect(adapter.get(), &ProtocolAdapter::failed, adapter.get(), [this](const QString &e) { failures.append(e); });
        QObject::connect(adapter.get(), &ProtocolAdapter::transmit, adapter.get(),
            [this](quint64 ticket, const QString &interface, const QByteArray &wire) {
                require(interface == "radio" && !wire.isEmpty() && wire.size() <= 255, "TX must be a direct native PHYPayload");
                require(plan.value("ticket").toString().toULongLong() == ticket, "TX must publish its PHY/timing plan first");
                const auto native = savedNative(root.path());
                if (quint8(wire[0]) == 0) {
                    const auto state = Lw::loadDeviceJoin(native);
                    require(state.pendingDevNonce && state.nextDevNonce > *state.pendingDevNonce, "DevNonce was not durable before TX");
                } else if (quint8(wire[0]) == 0x20) {
                    const auto state = Lw::loadJoinServer(native);
                    require(state.nextJoinNonce > 0 && state.activated, "JoinNonce/session were not durable before JoinAccept TX");
                } else {
                    const auto state = savedSession(root.path(), config);
                    require(state && state->nextTransmit > Lw::wireCounter(bytes(wire)), "FCnt was not durable before native TX escaped");
                }
                const auto dutyBlob = QByteArray::fromBase64(readBody(root.path()).value("duty").toString().toLatin1());
                const auto duty = Lw::loadDuty(bytes(dutyBlob));
                require(duty.aggregate > 0, "Duty reservation was not durable before TX");
                flights.enqueue({ticket, wire, plan});
            });
        start();
    }
    void start() {
        QString error;
        require(adapter->start(config, {"radio"}, root.path(), &error), qPrintable(error));
        noSecrets(adapter->status());
    }
    QJsonObject command(const QJsonObject &request, qint64 now) {
        const auto result = adapter->command(request, now); noSecrets(result); return result;
    }
    int count(const char *name) const {
        int n = 0; for (const auto &e : events) if (e.value("event") == QLatin1String(name)) ++n; return n;
    }
    QJsonObject last(const char *name) const {
        for (int i = events.size() - 1; i >= 0; --i) if (events[i].value("event") == QLatin1String(name)) return events[i];
        return {};
    }
};

QJsonObject acknowledgeStart(Endpoint &sender, Tx &tx, qint64 now) {
    const auto response = sender.command({{"op", "tx_started"}, {"ticket", QString::number(tx.ticket)}, {"startMs", double(now)}}, now);
    if (response.value("ok").toBool()) {
        for (const char *key : {"startUs", "endUs", "airtimeUs"}) tx.plan.insert(key, response.value(key));
    }
    return response;
}

QByteArray throughIq(const Tx &tx) {
#ifdef OBRII_LORAWAN_WITH_LORA
    const auto rate = unsigned(tx.plan.value("dataRate").toInt());
    const bool down = tx.plan.value("modem").toObject().value("invertIq").toBool();
    const auto dir = down ? Lw::Direction::Downlink : Lw::Direction::Uplink;
    Lw::LoRaBearer modulator(rate, dir, 1), receiver(rate, dir, 1);
    const auto iq = modulator.modulate(bytes(tx.wire));
    std::vector<Lw::RadioPacket> packets;
    for (std::size_t at = 0; at < iq.size();) {
        const auto count = (std::min)(std::size_t((at / 137) % 2 ? 1024 : 137), iq.size() - at);
        const auto found = receiver.receive(iq.data() + at, count);
        packets.insert(packets.end(), found.begin(), found.end()); at += count;
    }
    require(packets.size() == 1 && bytes(packets[0].wire) == tx.wire, "LoRa IQ did not preserve the exact native LoRaWAN frame");
    if (down) require(!modulator.config().payloadCrc && modulator.config().invertIq,
        "Downlink must use inverted IQ without PHY CRC");
    return bytes(packets[0].wire);
#else
    return tx.wire;
#endif
}
struct Pair {
    Endpoint device, network;
    qint64 now = 0;
    explicit Pair(bool ot = false) : device(ot ? otaa(false) : abp(false)), network(ot ? otaa(true) : abp(true)) {}
    Tx take(Endpoint &sender, qint64 at, bool authorizeStart = true) {
        now = (std::max)(now, at); sender.adapter->step(now);
        require(!sender.flights.isEmpty(), "Expected a scheduled native TX");
        auto tx = sender.flights.dequeue();
        if (authorizeStart) require(acknowledgeStart(sender, tx, now).value("ok").toBool(), "Host tx_started callback was rejected");
        return tx;
    }
    void capture(Endpoint &receiver, const Tx &tx, const QByteArray &wire, quint32 frequency = 0) {
        const auto startUs = tx.plan.value("startUs").toString().toULongLong();
        const auto endUs = tx.plan.value("endUs").toString().toULongLong();
        const auto endMs = qint64((endUs + 999) / 1000);
        now = (std::max)(now, endMs);
        const auto context = receiver.command({{"op", "rx_context"}, {"frequencyHz", frequency ? double(frequency) : tx.plan.value("frequencyHz")},
            {"dataRate", tx.plan.value("dataRate")}, {"packetStartMs", double(startUs / 1000)}, {"packetEndMs", double(endMs)}}, now);
        require(context.value("ok").toBool(), "Host RX context was rejected");
        receiver.adapter->receive("radio", wire, now);
    }
    void deliver(Endpoint &sender, Endpoint &receiver, const Tx &tx, bool iq = false) {
        now = (std::max)(now, qint64((tx.plan.value("endUs").toString().toULongLong() + 999) / 1000));
        sender.adapter->complete(tx.ticket, true, now);
        require(sender.adapter->status().value("ready").toBool(), "Valid native TX completion disabled the sender");
        capture(receiver, tx, iq ? throughIq(tx) : tx.wire);
    }
    qint64 nextUplink() const {
        return (std::max)(now, qint64((device.adapter->status().value("nextUplinkUs").toString().toULongLong() + 999) / 1000));
    }
    qint64 window(int index = 0) const {
        const auto windows = network.adapter->status().value("windows").toArray();
        require(windows.size() == 2, "Network requires observed uplink receiver state before downlink scheduling");
        return qint64((windows[index].toObject().value("nominalUs").toString().toULongLong() + 999) / 1000);
    }
};

void abpRoundtrip() {
    Pair pair;
    const QByteArray payload("native\0up\xff", 10);
    require(pair.device.command({{"op", "send"}, {"port", 7}, {"payloadBase64", QString::fromLatin1(payload.toBase64())},
        {"confirmed", true}}, pair.now).value("ok").toBool(), "Queue confirmed ABP uplink");
    require(pair.network.command({{"op", "send"}, {"text", "downlink"}, {"confirmed", true}}, pair.now).value("ok").toBool(), "Queue Class A downlink");
    pair.network.adapter->step(pair.now);
    require(pair.network.flights.isEmpty(), "Network must not TX without an observed uplink window");
    const auto up = pair.take(pair.device, pair.now);
    require(quint8(up.wire[0]) == 0x80 && !up.wire.startsWith("OBRII"), "Uplink is not a native confirmed LoRaWAN frame");
    pair.deliver(pair.device, pair.network, up, true);
    require(pair.network.count("message") == 1 && QByteArray::fromBase64(pair.network.last("message").value("payloadBase64").toString().toLatin1()) == payload,
        "Network lost authenticated binary uplink payload");
    const auto down = pair.take(pair.network, pair.window());
    pair.deliver(pair.network, pair.device, down, true);
    require(pair.device.count("message") == 1 && pair.device.last("message").value("ack").toBool(), "Downlink did not carry the native uplink ACK");
    require(QByteArray::fromBase64(pair.device.last("message").value("payloadBase64").toString().toLatin1()) == "downlink", "Wrong downlink application bytes");
    const auto ack = pair.take(pair.device, pair.nextUplink());
    pair.deliver(pair.device, pair.network, ack);
    require(pair.network.last("message").value("ack").toBool(), "Confirmed downlink was not ACKed by the next native uplink");
    require(pair.device.failures.isEmpty() && pair.network.failures.isEmpty(), "Unexpected ABP persistence/timing failure");
    pass("two ABP adapters: native data, Class A queue, binary payloads, ACKs and directional LoRa IQ");
}

void activation() {
    Pair pair(true);
    require(!pair.device.command({{"op", "send"}, {"text", "not joined"}}, 0).value("ok").toBool(), "Unactivated device must not send data");
    require(!pair.network.command({{"op", "join"}}, 0).value("ok").toBool(), "Network must not impersonate an OTAA device");
    require(pair.device.command({{"op", "join"}}, 0).value("ok").toBool(), "OTAA device join request");
    const auto request = pair.take(pair.device, 0);
    pair.deliver(pair.device, pair.network, request, true);
    require(pair.network.count("join_requested") == 1, "Provisioned network did not authenticate JoinRequest");
    const auto accept = pair.take(pair.network, pair.window());
    pair.deliver(pair.network, pair.device, accept, true);
    require(pair.device.adapter->status().value("activated").toBool() && pair.device.count("activated") == 1,
        "OTAA device did not activate from native JoinAccept");
    const auto savedD = Lw::loadDeviceJoin(savedNative(pair.device.root.path()));
    const auto savedN = Lw::loadJoinServer(savedNative(pair.network.root.path()));
    require(savedD.activated && savedN.activated && savedD.activated->keys.application == savedN.activated->keys.application &&
        savedD.nextDevNonce == 1 && savedN.nextJoinNonce == 1, "Durable OTAA key/nonce state differs");
    require(pair.device.command({{"op", "send"}, {"text", "after OTAA"}, {"ttlMs", 3600000}}, pair.now).value("ok").toBool(), "Activated OTAA send");
    pair.deliver(pair.device, pair.network, pair.take(pair.device, pair.nextUplink()));
    require(pair.network.count("message") == 1, "OTAA-derived Session did not authenticate uplink");
    pair.device.adapter->stop(); pair.device.start();
    require(pair.device.adapter->status().value("activated").toBool(), "Reboot lost OTAA activation/counters");
    pass("OTAA native join, persist-before-TX nonces, derived sessions and reboot activation");
}

void repeatAndMic() {
    Pair pair;
    require(pair.device.command({{"op", "send"}, {"text", "repeat"}}, 0).value("ok").toBool(), "Queue first NbTrans uplink");
    const auto first = pair.take(pair.device, 0);
    pair.deliver(pair.device, pair.network, first);
    for (int i = 0; i < 2; ++i) {
        require(pair.device.command({{"op", "repeat"}}, pair.now).value("ok").toBool(), "Bounded repeat command");
        const auto repeated = pair.take(pair.device, pair.nextUplink());
        require(repeated.wire == first.wire, "NbTrans must preserve native ciphertext, MIC and FCnt");
        pair.deliver(pair.device, pair.network, repeated);
    }
    require(pair.network.count("message") == 1 && pair.network.count("duplicate") == 2 &&
        !pair.device.command({{"op", "repeat"}}, pair.now).value("ok").toBool(), "NbTrans duplicate/application-delivery bounds failed");

    // A valid CRC-less LoRa waveform can still carry a forged LoRaWAN MIC.
    require(pair.network.command({{"op", "send"}, {"text", "authenticated"}}, pair.now).value("ok").toBool(), "Queue MIC test downlink");
    const auto down = pair.take(pair.network, pair.window());
    pair.now = qint64((down.plan.value("endUs").toString().toULongLong() + 999) / 1000);
    pair.network.adapter->complete(down.ticket, true, pair.now);
    auto forged = down;
    forged.wire[forged.wire.size() - 1] = char(quint8(forged.wire.back()) ^ 1);
    const auto physicallyDecoded = throughIq(forged);
    require(physicallyDecoded == forged.wire, "CRC-less PHY must not masquerade as MIC authentication");
    pair.capture(pair.device, forged, physicallyDecoded);
    require(pair.device.count("message") == 0 && pair.device.count("rejected") > 0,
        "An inverted CRC-less downlink with forged MIC released plaintext");
    pair.capture(pair.device, down, down.wire, 868300000);
    require(pair.device.count("message") == 0, "Wrong-channel downlink bypassed RX1/RX2 metadata checks");
    pair.capture(pair.device, down, down.wire);
    require(pair.device.count("message") == 1, "Valid authenticated downlink did not recover after MIC/context rejection");
    pass("NbTrans exact repeats/dedup, CRC-less inverted IQ, MIC rejection and RX window channel checks");
}

void macAndPersistence() {
    Pair pair;
    // Six DevStatusReq commands require an 18-byte uplink answer block.
    require(pair.network.command({{"op", "send_mac"}, {"macBase64", "BgYGBgYG"}}, 0).value("ok").toBool(), "Queue native MAC requests");
    require(pair.device.command({{"op", "send"}, {"text", "open window"}}, 0).value("ok").toBool(), "Queue uplink for MAC window");
    pair.deliver(pair.device, pair.network, pair.take(pair.device, 0));
    pair.deliver(pair.network, pair.device, pair.take(pair.network, pair.window()));
    require(pair.device.command({{"op", "send"}, {"text", "application retained"}}, pair.now).value("ok").toBool(), "Queue application alongside MAC answers");
    const auto response = pair.take(pair.device, pair.nextUplink());
    pair.deliver(pair.device, pair.network, response);
    require(pair.network.last("message").value("macCommands").toInt() == 6 &&
        pair.device.adapter->status().value("queued").toInt() == 1, "MAC answer priority consumed the queued application");
    pair.deliver(pair.device, pair.network, pair.take(pair.device, pair.nextUplink()));
    require(QByteArray::fromBase64(pair.network.last("message").value("payloadBase64").toString().toLatin1()) == "application retained",
        "MAC answers silently dropped queued application data");
    const auto before = pair.device.adapter->status().value("nextTransmitCounter");
    pair.device.adapter->stop(); pair.device.start();
    require(pair.device.adapter->status().value("nextTransmitCounter") == before, "Restart rolled back FCnt");
    auto other = Mesh::makeLoRaWanAdapter(nullptr); QString error;
    require(!other->start(pair.device.config, {"radio"}, pair.device.root.path(), &error), "Concurrent session owner bypassed QLockFile");
    pair.device.adapter->stop();
    auto changed = pair.device.config; changed.insert("nwkSKeyHex", QString(32, '1'));
    require(!other->start(changed, {"radio"}, pair.device.root.path(), &error), "Different key context reused prior counter state");
    pair.device.start();
    require(!pair.device.command({{"op", "status"}, {"keys", true}}, pair.now).value("ok").toBool(), "Status must not support secret export arguments");
    pass("native EU868 MAC answers, application preservation, persistent counters, exclusive ownership and profile binding");
}

void rx2Fallback() {
    Pair pair;
    require(pair.network.command({{"op", "send"}, {"text", "RX2"}}, 0).value("ok").toBool(), "Queue RX2 downlink");
    require(pair.device.command({{"op", "send"}, {"text", "open RX2"}}, 0).value("ok").toBool(), "Queue RX2 uplink");
    pair.deliver(pair.device, pair.network, pair.take(pair.device, 0));
    const auto rx2 = pair.window(1);
    pair.now = pair.window(0) + 20;
    pair.network.adapter->step(pair.now);
    require(pair.network.flights.isEmpty(), "A missed RX1 start emitted a late downlink");
    const auto down = pair.take(pair.network, rx2);
    require(down.plan.value("dataRate").toInt() == 0 && down.plan.value("frequencyHz").toDouble() == 869525000,
        "RX2 fallback did not use the regional frequency/rate");
    pair.deliver(pair.network, pair.device, down);
    require(pair.device.count("message") == 1, "RX2 window did not accept the authenticated native downlink");
    pair.capture(pair.device, down, down.wire);
    require(pair.device.count("message") == 1 && pair.device.last("rejected").value("reason").toString().contains("outside"),
        "A consumed Class A window accepted another downlink");
    pass("missed RX1 fallback to RX2, directional rate/frequency and one downlink per Class A opportunity");
}

void delayedStart() {
    Pair pair;
    require(pair.device.command({{"op", "send"}, {"text", "prepared before samples"}}, 0).value("ok").toBool(), "Queue delayed start uplink");
    auto tx = pair.take(pair.device, 0, false);
    const auto duration = tx.plan.value("endUs").toString().toULongLong();
    const auto native = savedNative(pair.device.root.path());
    const auto cooldown = pair.device.adapter->status().value("nextUplinkUs").toString().toULongLong();
    const auto before = Lw::loadDuty(bytes(QByteArray::fromBase64(readBody(pair.device.root.path()).value("duty").toString().toLatin1())));
    require(!pair.device.command({{"op", "tx_started"}, {"ticket", QString::number(tx.ticket + 1)}, {"startMs", 50}}, 50).value("ok").toBool(),
        "Wrong tx_started ticket was accepted");
    require(!pair.device.command({{"op", "tx_started"}, {"ticket", QString::number(tx.ticket)}, {"startMs", 49}}, 50).value("ok").toBool(),
        "Backdated tx_started timestamp was accepted");
    pair.now = 50;
    const auto start = acknowledgeStart(pair.device, tx, pair.now);
    require(start.value("ok").toBool() && start.value("startUs") == "50000" &&
        start.value("endUs").toString().toULongLong() == 50000 + duration &&
        start.value("airtimeUs").toString().toULongLong() == duration &&
        !start.value("hardwareTimingVerified").toBool(), "Delayed start changed PHY airtime or claimed hardware timing");
    require(savedNative(pair.device.root.path()) == native && pair.device.count("tx_started") == 1 && pair.device.flights.isEmpty(),
        "tx_started must not consume another nonce/counter or emit another TX");
    const auto after = Lw::loadDuty(bytes(QByteArray::fromBase64(readBody(pair.device.root.path()).value("duty").toString().toLatin1())));
    require(50000 + Lw::dutyReady(after, 868100000) >= 50000 + Lw::dutyReady(before, 868100000) &&
        pair.device.adapter->status().value("nextUplinkUs").toString().toULongLong() >= cooldown + 50000,
        "Delayed start did not durably extend duty/cooldown reservations");
    require(!acknowledgeStart(pair.device, tx, pair.now).value("ok").toBool(), "Duplicate tx_started was accepted");
    pair.deliver(pair.device, pair.network, tx);
    const auto windows = pair.network.adapter->status().value("windows").toArray();
    require(pair.network.count("message") == 1 && windows.size() == 2 &&
        windows[0].toObject().value("nominalUs").toString().toULongLong() == quint64(pair.now) * 1000 + 1000000,
        "Class A RX windows did not use the acknowledged on-air end");
    require(!acknowledgeStart(pair.device, tx, pair.now).value("ok").toBool(), "Completed ticket accepted another tx_started");

    Pair downlink;
    require(downlink.network.command({{"op", "send"}, {"text", "start inside RX1"}}, 0).value("ok").toBool(), "Queue delayed RX1 downlink");
    require(downlink.device.command({{"op", "send"}, {"text", "open RX1"}}, 0).value("ok").toBool(), "Queue RX1 uplink");
    downlink.deliver(downlink.device, downlink.network, downlink.take(downlink.device, 0));
    auto down = downlink.take(downlink.network, downlink.window(), false);
    downlink.now += 5;
    require(acknowledgeStart(downlink.network, down, downlink.now).value("ok").toBool(), "In-window delayed downlink start was rejected");
    downlink.deliver(downlink.network, downlink.device, down);
    require(downlink.device.count("message") == 1, "Delayed in-window downlink was not authenticated");
    pass("one-shot delayed TX authorization, durable conservative reservations, unchanged wire counters and actual-end RX windows");
}

void rejectedStarts() {
    Pair pair;
    require(pair.network.command({{"op", "send"}, {"text", "do not move RX1"}}, 0).value("ok").toBool(), "Queue expired RX1 fixture");
    require(pair.device.command({{"op", "send"}, {"text", "window"}}, 0).value("ok").toBool(), "Queue uplink window fixture");
    pair.deliver(pair.device, pair.network, pair.take(pair.device, 0));
    auto down = pair.take(pair.network, pair.window(), false);
    pair.now += 11;
    require(!acknowledgeStart(pair.network, down, pair.now).value("ok").toBool() && pair.network.count("tx_started") == 0,
        "tx_started moved a downlink beyond the original RX1 start tolerance");
    pair.network.adapter->complete(down.ticket, false, pair.now);
    require(pair.network.adapter->status().value("ready").toBool(), "An explicitly unsent late downlink should be cancellable");

    Endpoint strict(abp(false));
    require(strict.command({{"op", "send"}, {"text", "strict complete"}}, 0).value("ok").toBool(), "Queue strict completion fixture");
    strict.adapter->step(0); auto tx = strict.flights.dequeue();
    require(acknowledgeStart(strict, tx, 50).value("ok").toBool(), "Start strict completion fixture");
    strict.adapter->complete(tx.ticket, true, qint64((tx.plan.value("endUs").toString().toULongLong() + 999) / 1000) + 11);
    require(!strict.adapter->status().value("ready").toBool() && !strict.failures.isEmpty() && strict.failures.last().contains("on-air end"),
        "tx_started relaxed late on-air completion checks");

    Endpoint expired(abp(false));
    require(expired.command({{"op", "send"}, {"text", "TTL"}, {"ttlMs", 1}}, 0).value("ok").toBool(), "Queue expired-start fixture");
    expired.adapter->step(0); auto expiredTx = expired.flights.dequeue();
    require(!acknowledgeStart(expired, expiredTx, 2).value("ok").toBool(), "Expired request was authorized before its first samples");
    expired.adapter->complete(expiredTx.ticket, false, 2);

    Endpoint storage(abp(false));
    require(storage.command({{"op", "send"}, {"text", "sync before samples"}}, 0).value("ok").toBool(), "Queue start-persistence fixture");
    storage.adapter->step(0); auto storageTx = storage.flights.dequeue();
    const auto path = QDir(storage.root.path()).filePath("lorawan-state.json");
    require(QFile::remove(path) && QDir(storage.root.path()).mkdir("lorawan-state.json"), "Cannot construct start persistence failure");
    require(!acknowledgeStart(storage, storageTx, 50).value("ok").toBool() &&
        !storage.adapter->status().value("ready").toBool() && storage.count("tx_started") == 0,
        "Failed start reservation authorized samples or did not fail closed");

    Endpoint waiting(abp(false));
    require(waiting.command({{"op", "send"}, {"text", "bounded prepare"}, {"ttlMs", 3600000}}, 0).value("ok").toBool(), "Queue preparation-time bound fixture");
    waiting.adapter->step(0); auto waitingTx = waiting.flights.dequeue();
    require(!acknowledgeStart(waiting, waitingTx, 120001).value("ok").toBool(), "Unbounded late preparation extended a flight indefinitely");
    pass("late RX slot/TTL/preparation start rejection, unchanged strict completion and fail-closed start persistence");
}

void failuresAndBounds() {
    Endpoint node(abp(false));
    for (int i = 0; i < 16; ++i)
        require(node.command({{"op", "send"}, {"text", "bounded"}}, 0).value("ok").toBool(), "Queue capacity below limit");
    require(!node.command({{"op", "send"}, {"text", "overflow"}}, 0).value("ok").toBool(), "Queue overflow was accepted");
    node.command({{"op", "cancel"}}, 0);
    require(!node.command({{"op", "send"}, {"port", 0}, {"text", "reserved"}}, 0).value("ok").toBool() &&
        !node.command({{"op", "send"}, {"text", QString(243, 'x')}}, 0).value("ok").toBool() &&
        !node.command({{"op", "send"}, {"text", "x"}, {"ttlMs", 0}}, 0).value("ok").toBool(), "Port/MTU/QoS bounds failed");
    node.adapter->receive("radio", QByteArray(20, 'x'), 0);
    require(node.count("rejected") == 1 && node.last("rejected").value("reason").toString().contains("rx_context"),
        "Missing receiver metadata must be an explicit error");
    require(node.command({{"op", "send"}, {"text", "expire"}, {"ttlMs", 1}}, 0).value("ok").toBool(), "Queue expiry fixture");
    node.adapter->step(2);
    require(node.flights.isEmpty() && node.count("expired") == 1, "Expired request transmitted");
    require(node.command({{"op", "send"}, {"text", "no persistence no TX"}}, 2).value("ok").toBool(), "Queue persistence failure fixture");
    const auto statePath = QDir(node.root.path()).filePath("lorawan-state.json");
    require(QFile::remove(statePath) && QDir(node.root.path()).mkdir("lorawan-state.json"), "Cannot construct bounded storage-failure fixture");
    node.adapter->step(2);
    require(node.flights.isEmpty() && !node.adapter->status().value("ready").toBool() && !node.failures.isEmpty(),
        "Persistence failure did not disable TX before bytes escaped");

    Endpoint backwards(abp(false));
    backwards.adapter->step(10); backwards.adapter->step(9);
    require(!backwards.adapter->status().value("ready").toBool(), "Backwards monotonic host time did not fail closed");
    Endpoint timeout(abp(false));
    require(timeout.command({{"op", "send"}, {"text", "missing completion"}}, 0).value("ok").toBool(), "Queue completion timeout fixture");
    timeout.adapter->step(0); timeout.adapter->step(180001);
    require(!timeout.adapter->status().value("ready").toBool() && savedSession(timeout.root.path(), timeout.config)->nextTransmit == 1,
        "Unknown TX outcome must disable the adapter without reusing the reserved counter");
    Endpoint corrupt(abp(false)); corrupt.adapter->stop();
    QFile f(QDir(corrupt.root.path()).filePath("lorawan-state.json"));
    require(f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write("corrupt") == 7, "Cannot construct corrupt state fixture"); f.close();
    QString error;
    require(!corrupt.adapter->start(corrupt.config, {"radio"}, corrupt.root.path(), &error), "Corrupt state silently reset counters");
    require(QFile::remove(f.fileName()), "Cannot remove corrupt unit-test state fixture");
    require(!corrupt.adapter->start(corrupt.config, {"radio"}, corrupt.root.path(), &error), "Initialized marker allowed missing-state counter reset");
    QTemporaryDir blank;
    auto invalid = Mesh::makeLoRaWanAdapter(nullptr);
    require(!invalid->start({{"role", "device"}, {"activation", "abp"}}, {"radio"}, blank.path(), &error), "Missing explicit keys acquired defaults");
    auto noInit = abp(false); noInit.remove("initializeState");
    require(!invalid->start(noInit, {"radio"}, blank.path(), &error) &&
        !invalid->start(abp(false), {"radio"}, ".", &error), "Missing initialization consent or relative stateRoot accepted");
    pass("bounded queue/QoS/MTU, required RX state, expiry, fail-closed persistence, clock and corrupt/missing state");
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        abpRoundtrip(); activation(); repeatAndMic(); macAndPersistence(); rx2Fallback(); delayedStart(); rejectedStarts(); failuresAndBounds();
        std::printf("LoRaWAN adapter self-test passed; explicit local test keys only, no RF or network opened\n");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
