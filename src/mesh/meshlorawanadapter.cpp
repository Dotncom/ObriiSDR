#include "meshprotocoladapter.h"
#include "../lorawan/lorawan.h"
#include "../lorawan/regional.h"
#include "../lorawan/lorawanlora.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QQueue>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#ifdef Q_OS_WIN
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Mesh {
namespace {
namespace Lw = obrii::lorawan;
constexpr qint64 MaxClockMs = 9000000000000LL;
constexpr quint64 MaxDelayUs = 30ULL * 86400 * 1000000;
constexpr int MaxStateBytes = 16384, MaxQueue = 16;

struct StateFailure : std::runtime_error {
    StateFailure() : std::runtime_error("LoRaWAN durable state unavailable; adapter disabled, reload required") {}
};
void require(bool ok, const char *why) { if (!ok) throw std::invalid_argument(why); }
void only(const QJsonObject &o, const QStringList &fields) {
    for (auto i = o.begin(); i != o.end(); ++i)
        require(fields.contains(i.key()), "Unsupported LoRaWAN field");
}
quint64 number(const QJsonValue &v, quint64 low, quint64 high) {
    require(v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= double(low) &&
        v.toDouble() <= double(high) && std::floor(v.toDouble()) == v.toDouble(), "Invalid LoRaWAN integer field");
    return quint64(v.toDouble());
}
quint64 setting(const QJsonObject &o, const char *key, quint64 def, quint64 low, quint64 high) {
    return o.contains(key) ? number(o.value(key), low, high) : def;
}
bool boolean(const QJsonObject &o, const char *key, bool def) {
    if (!o.contains(key)) return def;
    require(o.value(key).isBool(), "Expected LoRaWAN boolean field");
    return o.value(key).toBool();
}
QByteArray hex(const QJsonValue &v, int count) {
    require(v.isString() && v.toString().size() == count * 2, "Expected an exact-length LoRaWAN hexadecimal field");
    const auto s = v.toString().toLatin1();
    for (char c : s) require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F'), "Invalid LoRaWAN hexadecimal field");
    return QByteArray::fromHex(s);
}
template<std::size_t N> std::array<std::uint8_t, N> fixed(const QJsonValue &v) {
    const auto b = hex(v, int(N));
    std::array<std::uint8_t, N> a{};
    std::copy(b.begin(), b.end(), a.begin());
    return a;
}
quint32 address(const QJsonValue &v, int digits = 8) {
    if (!v.isString()) return quint32(number(v, digits == 8 ? 1 : 0, digits == 8 ? 0xffffffffULL : 0xffffffULL));
    const auto b = hex(v, digits / 2);
    quint32 result = 0;
    for (char c : b) result = (result << 8) | quint8(c);
    require(digits != 8 || result != 0, "LoRaWAN DevAddr must not be zero");
    return result;
}
QString addressText(quint32 v) { return QString("%1").arg(v, 8, 16, QLatin1Char('0')); }
QByteArray base64(const QJsonValue &v, int limit) {
    require(v.isString() && v.toString().size() <= 4 * ((limit + 2) / 3), "Invalid or oversized base64 field");
    const auto encoded = v.toString().toLatin1(), decoded = QByteArray::fromBase64(encoded);
    require(decoded.size() <= limit && decoded.toBase64() == encoded, "Expected canonical padded base64");
    return decoded;
}
Lw::Bytes bytes(const QByteArray &b) { return {b.begin(), b.end()}; }
QByteArray bytes(const Lw::Bytes &b) { return QByteArray(reinterpret_cast<const char *>(b.data()), int(b.size())); }
QString b64(const Lw::Bytes &b) { return QString::fromLatin1(bytes(b).toBase64()); }
quint64 storedNumber(const QJsonValue &v, quint64 high) {
    require(v.isString() && !v.toString().isEmpty() && v.toString().size() <= 20, "Invalid saved LoRaWAN number");
    for (QChar c : v.toString()) require(c >= '0' && c <= '9', "Invalid saved LoRaWAN number");
    bool ok = false;
    const auto n = v.toString().toULongLong(&ok);
    require(ok && n <= high && QString::number(n) == v.toString(), "Invalid saved LoRaWAN number");
    return n;
}
void increment(quint64 &v) { if (v != (std::numeric_limits<quint64>::max)()) ++v; }
QJsonObject rejected(const QString &why) { return {{"ok", false}, {"error", why.left(256)}}; }

QJsonObject phy(unsigned rate, Lw::Direction dir) {
    const auto c = Lw::phyConfig(rate, dir, 1);
    return {{"type", "lora"}, {"sf", c.spreadingFactor}, {"bw", c.bandwidthHz}, {"cr", c.codingRate},
        {"os", 1}, {"preamble", c.preambleSymbols}, {"syncWord", int(c.syncWord)},
        {"invertIq", c.invertIq}, {"payloadCrc", c.payloadCrc}, {"ldro", "auto"}};
}
quint64 airtime(int size, unsigned rate, Lw::Direction dir) {
    const auto c = Lw::phyConfig(rate, dir, 1);
    const obrii::lora::PacketCodec codec(c);
    const auto count = (c.preambleSymbols + 4 + quint64(codec.symbolCount(size))) * c.samplesPerSymbol() + c.samplesPerSymbol() / 4;
    require(count <= 2u * 1024 * 1024, "LoRaWAN waveform exceeds host sample budget");
    const auto us = quint64(std::ceil(codec.timeOnAirSeconds(size) * 1000000));
    require(us && us <= 120000000, "LoRaWAN airtime exceeds host duration budget");
    return us;
}

struct Snapshot {
    Lw::Bytes native, mac;
    Lw::Eu868 region;
    Lw::DutyState duty;
    quint64 cooldown = 0, uptime = 0;
};
struct RxContext { quint32 frequency; unsigned rate; quint64 begin, end, receivedAt; };
enum class Kind { Data, Repeat, Join, JoinAccept };
struct Pending {
    Kind kind = Kind::Data;
    Lw::DataFrame frame;
    quint64 id = 0, expires = 0;
};
struct Flight {
    quint64 ticket = 0, id = 0, begin = 0, duration = 0, timeout = 0;
    Kind kind = Kind::Data;
    bool confirmed = false;
    quint32 frequency = 0;
    unsigned rate = 0;
    quint64 expires = 0, latestStart = 0;
    bool started = false;
};

class LoRaWanAdapter final : public ProtocolAdapter {
public:
    using ProtocolAdapter::ProtocolAdapter;
    ~LoRaWanAdapter() override { stop(); }

    bool start(const QJsonObject &c, const QStringList &interfaces, const QString &stateRoot, QString *error) override {
        stop();
        fault_.clear(); clocked_ = false; now_ = boot_ = lastNow_ = 0;
        rxCount_ = txCount_ = rejectCount_ = 0;
        try {
            only(c, {"role", "activation", "interfaceId", "devAddr", "netId", "nwkSKeyHex", "appSKeyHex",
                "joinEuiHex", "devEuiHex", "appKeyHex", "initializeState", "initialFCntUp", "initialFCntDown",
                "frequencyHz", "dataRate", "nbTrans", "rx1Offset", "rx2DataRate", "rx2FrequencyHz", "receiveDelay",
                "adrEnabled", "clockToleranceMs", "rxContextMaxAgeMs", "txCompleteTimeoutMs"});
            require(c.value("role") == "device" || c.value("role") == "network", "LoRaWAN role must be device or network");
            require(c.value("activation") == "abp" || c.value("activation") == "otaa", "LoRaWAN activation must be abp or otaa");
            network_ = c.value("role") == "network";
            otaa_ = c.value("activation") == "otaa";
            const bool initialize = boolean(c, "initializeState", false);
            const auto initialUp = setting(c, "initialFCntUp", 0, 0, 65535);
            const auto initialDown = setting(c, "initialFCntDown", 0, 0, 65535);
            role_ = network_ ? Lw::Role::Network : Lw::Role::Device;
            interface_ = c.value("interfaceId").toString(interfaces.isEmpty() ? QStringLiteral("radio") : interfaces.first());
            require(!c.contains("interfaceId") || c.value("interfaceId").isString(), "Expected interfaceId string");
            require(!interface_.isEmpty() && interface_.size() <= 32 &&
                (interfaces.isEmpty() || (interfaces.size() == 1 && interfaces.first() == interface_)),
                "LoRaWAN requires one matching radio interface (default radio)");
            for (QChar ch : interface_) require(ch.isLetterOrNumber() || ch == '_' || ch == '-', "Invalid radio interface identifier");
            configuredAddress_ = (!otaa_ || network_) ? address(c.value("devAddr")) : 0;
            Lw::SessionKeys keys;
            credentials_ = {};
            QByteArray identity = network_ ? "network:" : "device:";
            identity += otaa_ ? "otaa:" : "abp:";
            if (otaa_) {
                require(!c.contains("nwkSKeyHex") && !c.contains("appSKeyHex") &&
                    !c.contains("initialFCntUp") && !c.contains("initialFCntDown"), "OTAA cannot import ABP session fields");
                require(network_ || (!c.contains("devAddr") && !c.contains("netId")), "OTAA device address is assigned by JoinAccept");
                credentials_.joinEui = fixed<8>(c.value("joinEuiHex"));
                credentials_.devEui = fixed<8>(c.value("devEuiHex"));
                credentials_.appKey = fixed<16>(c.value("appKeyHex"));
                eui_ = QString::fromLatin1(hex(c.value("devEuiHex"), 8).toHex());
                identity += hex(c.value("joinEuiHex"), 8) + hex(c.value("devEuiHex"), 8) + hex(c.value("appKeyHex"), 16);
                joinParameters_ = {};
                if (network_) {
                    joinParameters_.devAddr = configuredAddress_;
                    joinParameters_.netId = address(c.value("netId"), 6);
                    identity += QByteArray::number(configuredAddress_) + ':' + QByteArray::number(joinParameters_.netId);
                }
            } else {
                require(!c.contains("joinEuiHex") && !c.contains("devEuiHex") && !c.contains("appKeyHex") && !c.contains("netId"),
                    "ABP requires session keys, not OTAA credentials");
                keys.network = fixed<16>(c.value("nwkSKeyHex")); keys.application = fixed<16>(c.value("appSKeyHex"));
                identity += QByteArray::number(configuredAddress_) + ':' + hex(c.value("nwkSKeyHex"), 16) + hex(c.value("appSKeyHex"), 16);
                eui_.clear();
            }
            contextHash_ = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
            frequency_ = quint32(setting(c, "frequencyHz", 868100000, 863000000, 870000000));
            tolerance_ = setting(c, "clockToleranceMs", 10, 1, 250) * 1000;
            contextAge_ = setting(c, "rxContextMaxAgeMs", 10000, 1, 120000) * 1000;
            completeTimeout_ = setting(c, "txCompleteTimeoutMs", 120000, 1000, 180000) * 1000;
            store_ = {};
            auto &r = store_.region;
            r.dataRate = quint8(setting(c, "dataRate", 5, 0, 5));
            r.nbTrans = quint8(setting(c, "nbTrans", 1, 1, 15));
            r.rx1Offset = quint8(setting(c, "rx1Offset", 0, 0, 5));
            r.rx2DataRate = quint8(setting(c, "rx2DataRate", 0, 0, 6));
            r.rx2FrequencyHz = quint32(setting(c, "rx2FrequencyHz", 869525000, 863000000, 870000000));
            r.receiveDelay = quint8(setting(c, "receiveDelay", 1, 1, 15));
            r.adrEnabled = boolean(c, "adrEnabled", true);
            r.validate(); r.channelIndex(frequency_, r.dataRate);
            root_ = checkedRoot(stateRoot);
            statePath_ = QDir(root_).filePath("lorawan-state.json");
            markerPath_ = QDir(root_).filePath("lorawan-initialized");
            const auto lockPath = QDir(root_).filePath("lorawan.lock");
            require(!QFileInfo(lockPath).isSymLink(), "LoRaWAN lock must not be a symbolic link");
            lock_ = std::make_unique<QLockFile>(lockPath);
            lock_->setStaleLockTime(0);
            require(lock_->tryLock(0), "LoRaWAN state is already owned or unavailable");
            const bool exists = QFileInfo::exists(statePath_);
            if (exists) {
                load();
                if (QFileInfo::exists(markerPath_) && readFile(markerPath_, 128) != contextHash_) throw StateFailure();
            } else {
                require(!QFileInfo::exists(markerPath_), "LoRaWAN state is missing from an initialized directory; refusing counter reset");
                require(initialize, "Fresh LoRaWAN state requires explicit initializeState for a new key/nonce context");
                if (otaa_) store_.native = network_ ? Lw::saveJoinServer({}) : Lw::saveDeviceJoin({});
                else {
                    Lw::SessionState initial; initial.role = role_; initial.devAddr = configuredAddress_; initial.keys = keys;
                    initial.maxUplinkCopies = r.nbTrans;
                    initial.nextTransmit = network_ ? initialDown : initialUp;
                    store_.native = Lw::saveSession(initial);
                }
                persist(store_);
            }
            baseUptime_ = store_.uptime;
            store_.region.channelIndex(frequency_, store_.region.dataRate);
            if (otaa_) {
                if (network_) {
                    server_ = std::make_unique<Lw::JoinServer>(credentials_, Lw::loadJoinServer(store_.native),
                        [this](const auto &s) { commitNative(Lw::saveJoinServer(s)); });
                    if (server_->state().activated) attach(*server_->state().activated);
                } else {
                    device_ = std::make_unique<Lw::OtaaDevice>(credentials_, Lw::loadDeviceJoin(store_.native),
                        [this](const auto &s) { commitNative(Lw::saveDeviceJoin(s)); });
                    if (device_->state().activated) attach(*device_->state().activated);
                }
            } else {
                const auto state = Lw::loadSession(store_.native);
                require(state.devAddr == configuredAddress_ && state.keys.network == keys.network && state.keys.application == keys.application,
                    "ABP state does not match the local operator profile");
                attach(state);
            }
            if (!QFileInfo::exists(markerPath_)) writeFile(markerPath_, contextHash_);
            ready_ = true;
            if (error) error->clear();
            return true;
        } catch (const std::exception &e) {
            fault_ = QString::fromLatin1(e.what()).left(256); stop();
            if (error) *error = fault_;
            return false;
        }
    }

    QJsonObject command(const QJsonObject &request, qint64 nowMs) override {
        try {
            const auto op = request.value("op").toString();
            if (op == "status") { only(request, {"op"}); return {{"ok", true}, {"status", status()}}; }
            require(ready_, "LoRaWAN adapter is not ready; inspect local state/profile error");
            clock(nowMs);
            if (op == "tx_started") return startFlight(request);
            if (op == "rx_context") {
                only(request, {"op", "frequencyHz", "dataRate", "packetStartMs", "packetEndMs"});
                const auto begin = number(request.value("packetStartMs"), 0, quint64(MaxClockMs)) * 1000;
                const auto end = number(request.value("packetEndMs"), 0, quint64(MaxClockMs)) * 1000;
                require(begin <= end && end <= now_ && now_ - end <= contextAge_ && end - begin <= 120000000,
                    "Invalid/stale LoRaWAN RX packet timing context");
                const auto hz = quint32(number(request.value("frequencyHz"), 863000000, 870000000));
                const auto rate = unsigned(number(request.value("dataRate"), 0, 6));
                require(Lw::Eu868::validFrequency(hz), "Unsupported EU868 receive frequency");
                rxContext_ = RxContext{hz, rate, begin, end, now_};
                return {{"ok", true}};
            }
            if (op == "cancel") {
                only(request, {"op"});
                const int count = queue_.size(); queue_.clear();
                return {{"ok", true}, {"cancelled", count}};
            }
            require(op == "join" || op == "send" || op == "send_mac" || op == "repeat", "Unsupported LoRaWAN operation");
            require(queue_.size() < MaxQueue, "LoRaWAN queue is full (16 requests)");
            Pending p;
            p.expires = now_ + setting(request, "ttlMs", 60000, 1, 3600000) * 1000;
            if (op == "join") {
                only(request, {"op", "ttlMs"});
                require(!network_ && bool(device_), "join requires an OTAA device role");
                require(!session_ && queue_.isEmpty() && !flight_.ticket, "OTAA join is already active or the device is activated");
                p.kind = Kind::Join;
            } else if (op == "repeat") {
                only(request, {"op", "ttlMs"});
                require(!network_ && bool(session_), "repeat requires an activated device session");
                require(session_->state().mayRepeat && session_->state().transmitCopies < session_->state().maxUplinkCopies,
                    "No uplink repetition remains within NbTrans");
                require(queue_.isEmpty() && !flight_.ticket, "repeat requires an empty TX queue");
                p.kind = Kind::Repeat;
            } else {
                require(bool(session_), "send requires an activated ABP/OTAA session");
                p.frame.type = network_ ? Lw::FrameType::UnconfirmedDown : Lw::FrameType::UnconfirmedUp;
                if (op == "send_mac") {
                    only(request, {"op", "macBase64", "ttlMs"});
                    require(network_, "send_mac requires a network role");
                    const auto mac = bytes(base64(request.value("macBase64"), 242));
                    const auto parsed = Lw::parseMac(Lw::Direction::Downlink, mac);
                    require(!mac.empty() && parsed.unparsed.empty(), "MAC request contains empty or unsupported commands");
                    for (const auto &command : parsed.commands)
                        require(command.cid == 2 || command.cid == 6 || command.cid == 13,
                            "Network MAC parameter negotiation is not implemented; use LinkCheckAns, DevStatusReq or DeviceTimeAns");
                    p.frame.port = 0; p.frame.payload = mac;
                } else {
                    only(request, {"op", "port", "payloadBase64", "text", "confirmed", "ttlMs"});
                    p.frame.port = quint8(setting(request, "port", 1, 1, 223));
                    const bool confirmed = boolean(request, "confirmed", false);
                    if (confirmed) p.frame.type = network_ ? Lw::FrameType::ConfirmedDown : Lw::FrameType::ConfirmedUp;
                    require(request.contains("payloadBase64") != request.contains("text"), "Specify exactly one of text or payloadBase64");
                    if (request.contains("text")) {
                        require(request.value("text").isString() && request.value("text").toString().size() <= 242, "Invalid or oversized text payload");
                        const auto b = request.value("text").toString().toUtf8();
                        require(b.size() <= 242, "UTF-8 payload exceeds LoRaWAN limit"); p.frame.payload = bytes(b);
                    } else p.frame.payload = bytes(base64(request.value("payloadBase64"), 242));
                }
                require(13 + p.frame.payload.size() <= Lw::Eu868::maxMacPayload(store_.region.dataRate) + 5,
                    "Payload exceeds the current EU868 data-rate budget");
            }
            require(nextId_ != (std::numeric_limits<quint64>::max)(), "LoRaWAN request ID exhausted");
            p.id = nextId_++;
            queue_.enqueue(p);
            return {{"ok", true}, {"queued", true}, {"nativeId", QString::number(p.id)}};
        } catch (const StateFailure &) { failClosed(QString::fromLatin1(StateFailure().what())); return rejected(fault_); }
        catch (const std::exception &e) { return rejected(QString::fromLatin1(e.what())); }
    }

    void receive(const QString &interfaceId, const QByteArray &wire, qint64 nowMs) override {
        if (!ready_) return;
        try {
            clock(nowMs);
            require(interfaceId == interface_, "LoRaWAN packet arrived on an unconfigured interface");
            const auto context = std::exchange(rxContext_, std::nullopt);
            require(bool(context), "LoRaWAN receive requires a one-shot rx_context with frequency, rate and capture timestamps");
            require(now_ >= context->receivedAt && now_ - context->receivedAt <= contextAge_, "LoRaWAN RX context expired");
            require(wire.size() >= 12 && wire.size() <= 255, "LoRaWAN PHYPayload length must be 12..255 bytes");
            const auto dir = network_ ? Lw::Direction::Uplink : Lw::Direction::Downlink;
            const auto duration = airtime(wire.size(), context->rate, dir);
            const auto measured = context->end - context->begin;
            require((duration > measured ? duration - measured : measured - duration) <= tolerance_ + 2000,
                "LoRaWAN capture duration does not match frequency/rate context");
            if (network_) store_.region.channelIndex(context->frequency, context->rate);
            else {
                bool matched = false;
                if (windows_) for (const auto &w : *windows_)
                    matched |= w.containsStart(context->begin) && w.frequencyHz == context->frequency && w.dataRate == context->rate;
                require(matched, "Downlink is outside device RX1/RX2 or has the wrong frequency/rate");
            }
            const auto native = bytes(wire);
            if (quint8(wire[0]) == 0) {
                require(network_ && bool(server_), "JoinRequest requires a provisioned OTAA network role");
                require(!flight_.ticket && queue_.isEmpty() && joinAccept_.isEmpty(), "Network join receiver is busy");
                auto parameters = joinParameters_;
                parameters.rx1Offset = store_.region.rx1Offset; parameters.rx2DataRate = store_.region.rx2DataRate;
                parameters.rxDelay = store_.region.receiveDelay;
                const auto response = server_->accept(native, parameters);
                attach(response.session);
                joinAccept_ = bytes(response.wire);
                windows_ = Lw::receiveWindows(store_.region, context->end, context->frequency, context->rate, true, quint32(tolerance_));
                publish({{"event", "join_requested"}, {"authenticated", true}});
            } else if (quint8(wire[0]) == 0x20) {
                require(!network_ && bool(device_) && !session_, "JoinAccept requires a pending OTAA device activation");
                auto next = store_;
                device_->accept(native, [&](const auto &a) { next.region.applyJoin(a); staged_ = &next; });
                staged_ = nullptr;
                attach(*device_->state().activated);
                session_->setNbTrans(store_.region.nbTrans);
                windows_.reset();
                publish({{"event", "activated"}, {"authenticated", true}});
            } else {
                require(bool(session_), "Data receive requires an activated session for this role");
                Lw::Eu868::checkPayload(native, context->rate);
                auto next = store_;
                std::optional<Lw::DataFrame> inspected;
                if (!network_) {
                    // Inspect with the same FCnt reconstruction, but do not release data until Session commits.
                    const auto &state = session_->state();
                    quint64 count = Lw::wireCounter(native);
                    if (state.lastReceived) {
                        count |= (*state.lastReceived & 0xffff0000U);
                        if (count <= *state.lastReceived) count += 0x10000;
                    }
                    require(count <= 0xffffffffULL, "LoRaWAN receive counter exhausted");
                    inspected = Lw::decodeData(native, state.keys, quint32(count));
                    const auto result = next.region.processDownlink(*inspected);
                    next.mac = result.answers;
                }
                staged_ = &next;
                const auto received = session_->receive(native);
                staged_ = nullptr;
                if (network_) {
                    windows_ = Lw::receiveWindows(store_.region, context->end, context->frequency, context->rate, false, quint32(tolerance_));
                } else {
                    session_->setNbTrans(store_.region.nbTrans);
                    windows_.reset();
                }
                const auto &f = received.frame;
                QJsonObject value{{"event", received.duplicate ? "duplicate" : "message"}, {"authenticated", true},
                    {"counter", QString::number(f.counter)}, {"confirmed", Lw::confirmed(f.type)}, {"ack", bool(f.control & 0x20)}};
                if (!received.duplicate && f.port && *f.port != 0) {
                    value.insert("port", int(*f.port)); value.insert("payloadBase64", b64(f.payload));
                }
                const auto mac = f.port && *f.port == 0 ? f.payload : f.options;
                if (!mac.empty()) {
                    const auto parsed = Lw::parseMac(dir, mac);
                    value.insert("macCommands", int(parsed.commands.size()));
                    value.insert("unparsedMacBytes", int(parsed.unparsed.size()));
                }
                publish(value);
                if (!received.duplicate && (f.control & 0x20))
                    publish({{"event", "acknowledged"}, {"authenticated", true}, {"counter", QString::number(f.counter)}});
            }
            increment(rxCount_);
        } catch (const StateFailure &) { staged_ = nullptr; failClosed(QString::fromLatin1(StateFailure().what())); }
        catch (const std::exception &e) { staged_ = nullptr; reject(QString::fromLatin1(e.what())); }
    }

    void step(qint64 nowMs) override {
        if (!ready_) return;
        try {
            clock(nowMs);
            if (flight_.ticket) {
                if (now_ > flight_.timeout) failClosed("LoRaWAN TX completion timed out; radio outcome unknown, reload required");
                return;
            }
            while (!queue_.isEmpty() && queue_.head().expires < now_) {
                publish({{"event", "expired"}, {"nativeId", QString::number(queue_.dequeue().id)}});
            }
            if (network_) sendDownlink();
            else sendUplink();
        } catch (const StateFailure &) { staged_ = nullptr; failClosed(QString::fromLatin1(StateFailure().what())); }
        catch (const std::exception &e) {
            staged_ = nullptr;
            if (!queue_.isEmpty()) queue_.dequeue();
            reject(QString::fromLatin1(e.what()));
        }
    }

    void complete(quint64 ticket, bool sent, qint64 nowMs) override {
        if (!ready_ || !ticket || ticket != flight_.ticket) return;
        try {
            clock(nowMs);
            const auto f = std::exchange(flight_, Flight{});
            const auto expected = f.begin + f.duration;
            if (sent && (now_ + tolerance_ < expected || now_ > expected + tolerance_)) {
                failClosed("LoRaWAN completion is not the planned on-air end; timing validity unknown, reload required");
                return;
            }
            if (!network_ && sent) {
                windows_ = Lw::receiveWindows(store_.region, now_, f.frequency, f.rate, f.kind == Kind::Join, quint32(tolerance_));
                auto next = store_;
                next.cooldown = (std::max)(next.cooldown, Lw::nextUplinkTime(*windows_, f.confirmed));
                persist(next); store_ = std::move(next);
                publish({{"event", "rx_windows"}, {"windows", windowJson()}});
            }
            publish({{"event", sent ? "sent" : "tx_failed"}, {"nativeId", QString::number(f.id)},
                {"ticket", QString::number(ticket)}, {"confirmed", f.confirmed}});
        } catch (const StateFailure &) { failClosed(QString::fromLatin1(StateFailure().what())); }
        catch (const std::exception &e) { failClosed(QString::fromLatin1(e.what())); }
    }

    QJsonObject status() const override {
        const auto addr = session_ ? session_->state().devAddr : configuredAddress_;
        QJsonObject s{{"type", "lorawan"}, {"protocol", "lorawan"}, {"role", network_ ? "network" : "device"},
            {"activation", otaa_ ? "otaa" : "abp"}, {"ready", ready_}, {"activated", bool(session_)},
            {"nativeAddress", addr ? addressText(addr) : eui_}, {"devAddr", addr ? addressText(addr) : QString()},
            {"devEui", eui_}, {"interfaceId", interface_}, {"region", "EU868"}, {"queued", queue_.size()},
            {"pending", flight_.ticket != 0}, {"received", QString::number(rxCount_)}, {"transmitted", QString::number(txCount_)},
            {"rejected", QString::number(rejectCount_)}, {"hardwareTimingVerified", false}, {"networkServer", false},
            {"rxContextRequired", true}, {"automaticRetries", false}, {"error", fault_}};
        s.insert("txStartedSupported", true);
        s.insert("txStartAcknowledged", flight_.ticket != 0 && flight_.started);
        if (session_) {
            s.insert("nextTransmitCounter", QString::number(session_->state().nextTransmit));
            s.insert("nbTrans", int(session_->state().maxUplinkCopies));
            s.insert("ackPending", session_->state().acknowledgmentPending);
        }
        if (ready_) {
            s.insert("frequencyHz", double(frequency_));
            s.insert("dataRate", int(store_.region.dataRate));
            s.insert("clockAnchored", clocked_);
            s.insert("nextUplinkUs", QString::number((std::max)(store_.cooldown, Lw::dutyReady(store_.duty, frequency_))));
            s.insert("uplinkModem", phy(store_.region.dataRate, Lw::Direction::Uplink));
            s.insert("downlinkModem", phy(store_.region.dataRate, Lw::Direction::Downlink));
            s.insert("windows", windowJson());
        }
        return s;
    }

    void stop() override {
        ready_ = false; staged_ = nullptr;
        session_.reset(); device_.reset(); server_.reset();
        queue_.clear(); joinAccept_.clear(); flight_ = {};
        windows_.reset(); rxContext_.reset(); lock_.reset();
        credentials_ = {}; store_ = {}; contextHash_.clear();
    }

private:
    bool ready_ = false, network_ = false, otaa_ = false, clocked_ = false;
    Lw::Role role_ = Lw::Role::Device;
    QString interface_, root_, statePath_, markerPath_, eui_, fault_;
    QByteArray contextHash_, joinAccept_;
    quint32 configuredAddress_ = 0, frequency_ = 868100000;
    quint64 now_ = 0, boot_ = 0, lastNow_ = 0, baseUptime_ = 0;
    quint64 tolerance_ = 10000, contextAge_ = 10000000, completeTimeout_ = 120000000;
    quint64 nextId_ = 1, nextTicket_ = 1, rxCount_ = 0, txCount_ = 0, rejectCount_ = 0;
    Snapshot store_;
    Snapshot *staged_ = nullptr;
    Lw::Credentials credentials_;
    Lw::JoinAccept joinParameters_;
    std::unique_ptr<Lw::Session> session_;
    std::unique_ptr<Lw::OtaaDevice> device_;
    std::unique_ptr<Lw::JoinServer> server_;
    std::unique_ptr<QLockFile> lock_;
    QQueue<Pending> queue_;
    std::optional<RxContext> rxContext_;
    std::optional<std::array<Lw::RxWindow, 2>> windows_;
    Flight flight_;

    void publish(QJsonObject v) {
        v.insert("protocol", "lorawan"); v.insert("type", "lorawan");
        v.insert("status", v.value("event"));
        emit event(v);
    }
    void reject(const QString &reason) {
        increment(rejectCount_); publish({{"event", "rejected"}, {"reason", reason.left(256)}});
    }
    void failClosed(const QString &why) {
        fault_ = why.left(256); ready_ = false; queue_.clear(); joinAccept_.clear(); flight_ = {};
        rxContext_.reset(); windows_.reset();
        emit failed(fault_);
    }
    void clock(qint64 ms) {
        if (ms < 0 || ms > MaxClockMs || (clocked_ && quint64(ms) < lastNow_)) {
            failClosed("LoRaWAN host time must be bounded and monotonic; reload required");
            throw std::invalid_argument("LoRaWAN host time must be bounded and monotonic");
        }
        lastNow_ = quint64(ms); now_ = quint64(ms) * 1000;
        if (!clocked_) {
            boot_ = now_;
            for (auto &deadline : store_.duty.bands) deadline += now_;
            store_.duty.aggregate += now_; store_.cooldown += now_;
            clocked_ = true;
        }
    }
    QString checkedRoot(const QString &path) const {
        const QFileInfo info(path);
        require(info.isAbsolute() && info.isDir() && !info.isSymLink() && !info.canonicalFilePath().isEmpty(),
            "LoRaWAN requires an existing absolute private stateRoot directory");
        const auto canonical = QDir::fromNativeSeparators(info.canonicalFilePath());
        const auto absolute = QDir::cleanPath(QDir::fromNativeSeparators(info.absoluteFilePath()));
#ifdef Q_OS_WIN
        require(canonical.compare(absolute, Qt::CaseInsensitive) == 0, "LoRaWAN stateRoot must not traverse links/junctions");
#else
        require(canonical == absolute, "LoRaWAN stateRoot must not traverse symbolic links");
        require(!(info.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
            QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)), "LoRaWAN stateRoot must be private (0700)");
#endif
        return canonical;
    }
    void checkFile(const QString &path) const {
        try {
            if (checkedRoot(root_) != root_ || !lock_ || !lock_->isLocked()) throw StateFailure();
        } catch (const std::exception &) { throw StateFailure(); }
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && !info.isFile())) throw StateFailure();
    }
    QByteArray readFile(const QString &path, int limit) const {
        checkFile(path);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly) || f.size() < 1 || f.size() > limit) throw StateFailure();
        const auto data = f.read(limit + 1);
        if (data.size() != f.size()) throw StateFailure();
        return data;
    }
    void writeFile(const QString &path, const QByteArray &data) {
        checkFile(path);
        QSaveFile file(path); file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
            file.write(data) != data.size() || !file.flush()) throw StateFailure();
#ifdef Q_OS_WIN
        if (_commit(file.handle()) != 0) throw StateFailure();
#else
        if (::fsync(file.handle()) != 0) throw StateFailure();
#endif
        if (!file.commit()) throw StateFailure();
#ifdef Q_OS_WIN
        QFile committed(path);
        if (!committed.open(QIODevice::ReadWrite) || _commit(committed.handle()) != 0) throw StateFailure();
#else
        const int directory = ::open(QFile::encodeName(root_).constData(), O_RDONLY | O_DIRECTORY);
        if (directory < 0) throw StateFailure();
        const bool synced = ::fsync(directory) == 0; ::close(directory);
        if (!synced) throw StateFailure();
#endif
    }
    quint64 remaining(quint64 deadline) const { return clocked_ ? (deadline > now_ ? deadline - now_ : 0) : deadline; }
    void persist(const Snapshot &s) {
        auto duty = s.duty;
        for (auto &t : duty.bands) t = remaining(t);
        duty.aggregate = remaining(duty.aggregate);
        const auto uptime = clocked_ ? baseUptime_ + now_ - boot_ : s.uptime;
        QJsonObject body{{"version", 1}, {"context", QString::fromLatin1(contextHash_)}, {"native", b64(s.native)},
            {"region", b64(Lw::saveRegion(s.region))}, {"duty", b64(Lw::saveDuty(duty))}, {"mac", b64(s.mac)},
            {"cooldownUs", QString::number(remaining(s.cooldown))}, {"uptimeUs", QString::number(uptime)}};
        const auto hash = QCryptographicHash::hash(QJsonDocument(body).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
        const auto data = QJsonDocument(QJsonObject{{"body", body}, {"sha256", QString::fromLatin1(hash)}}).toJson(QJsonDocument::Compact);
        if (data.size() > MaxStateBytes) throw StateFailure();
        writeFile(statePath_, data);
    }
    void load() {
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(readFile(statePath_, MaxStateBytes), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) throw StateFailure();
        const auto envelope = doc.object(); only(envelope, {"body", "sha256"});
        require(envelope.value("body").isObject(), "Invalid saved LoRaWAN state body");
        const auto body = envelope.value("body").toObject();
        only(body, {"version", "context", "native", "region", "duty", "mac", "cooldownUs", "uptimeUs"});
        const auto hash = QCryptographicHash::hash(QJsonDocument(body).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
        if (body.value("version") != 1 || body.value("context") != QString::fromLatin1(contextHash_) ||
            envelope.value("sha256") != QString::fromLatin1(hash)) throw StateFailure();
        store_.native = bytes(base64(body.value("native"), 2048));
        store_.region = Lw::loadRegion(bytes(base64(body.value("region"), 2048)));
        store_.duty = Lw::loadDuty(bytes(base64(body.value("duty"), 2048)));
        store_.mac = bytes(base64(body.value("mac"), 242));
        for (auto t : store_.duty.bands) require(t <= MaxDelayUs, "Invalid saved duty delay");
        require(store_.duty.aggregate <= MaxDelayUs, "Invalid saved duty delay");
        store_.cooldown = storedNumber(body.value("cooldownUs"), MaxDelayUs);
        store_.uptime = storedNumber(body.value("uptimeUs"), quint64(MaxClockMs) * 1000);
    }
    void commitNative(Lw::Bytes native) {
        try {
            auto next = staged_ ? *staged_ : store_;
            next.native = std::move(native);
            persist(next); store_ = std::move(next);
        } catch (const std::exception &) { throw StateFailure(); }
    }
    void attach(const Lw::SessionState &s) {
        require(s.role == role_ && (!network_ || s.devAddr == configuredAddress_), "Restored LoRaWAN session has the wrong role/address");
        session_ = std::make_unique<Lw::Session>(role_, s, [this](const auto &next) {
            if (device_) device_->updateSession(next);
            else if (server_) server_->updateSession(next);
            else commitNative(Lw::saveSession(next));
        });
    }
    QJsonArray windowJson() const {
        QJsonArray result;
        if (windows_) for (const auto &w : *windows_) result.append(QJsonObject{{"index", int(w.index)},
            {"opensUs", QString::number(w.opensUs)}, {"nominalUs", QString::number(w.nominalUs)},
            {"closesUs", QString::number(w.closesUs)}, {"frequencyHz", double(w.frequencyHz)},
            {"dataRate", int(w.dataRate)}, {"modem", phy(w.dataRate, Lw::Direction::Downlink)}});
        return result;
    }
    Lw::Bytes preview(Lw::DataFrame f) const {
        require(bool(session_) && session_->state().nextTransmit <= 0xffffffffULL, "LoRaWAN TX counter exhausted or session unavailable");
        f.devAddr = session_->state().devAddr; f.counter = quint32(session_->state().nextTransmit);
        if (session_->state().acknowledgmentPending) f.control |= 0x20;
        return Lw::encodeData(f, session_->state().keys);
    }
    void reserve(Snapshot &next, quint32 frequency, quint64 duration, bool join) {
        next.duty = join ? Lw::reserveBackoff(next.duty, frequency, now_, duration, baseUptime_ + now_ - boot_, next.region.dutyExponent) :
            Lw::reserveDuty(next.duty, frequency, now_, duration, next.region.dutyExponent);
    }
    quint64 startDeadline() const {
        auto deadline = flight_.begin + completeTimeout_;
        if (flight_.expires) deadline = (std::min)(deadline, flight_.expires);
        if (flight_.latestStart) deadline = (std::min)(deadline, flight_.latestStart);
        return deadline;
    }
    QJsonObject startFlight(const QJsonObject &request) {
        only(request, {"op", "ticket", "startMs"});
        const auto ticket = storedNumber(request.value("ticket"), (std::numeric_limits<quint64>::max)());
        require(ticket && ticket == flight_.ticket, "tx_started requires the active adapter ticket as a decimal string");
        require(!flight_.started, "tx_started was already acknowledged for this ticket");
        const auto begin = number(request.value("startMs"), 0, quint64(MaxClockMs)) * 1000;
        require(begin == now_, "tx_started startMs must equal the current host callback nowMs");
        require(begin >= flight_.begin, "tx_started cannot move a transmission earlier than its plan");
        require(begin <= flight_.begin + completeTimeout_, "tx_started exceeded the transport preparation time budget");
        require(!flight_.expires || begin <= flight_.expires, "tx_started request expired before the first samples");
        require(!network_ || (flight_.latestStart && begin <= flight_.latestStart),
            "tx_started is outside the original RX1/RX2 start opportunity; cancel this unsent job");
        const auto deadline = startDeadline();

        auto next = store_;
        // The dispatch reservation already includes this packet. Recompute its
        // later reservation independently, then merge without shortening any deadline.
        const auto reservation = flight_.kind == Kind::Join ?
            Lw::reserveBackoff({}, flight_.frequency, begin, flight_.duration,
                baseUptime_ + now_ - boot_, next.region.dutyExponent) :
            Lw::reserveDuty({}, flight_.frequency, begin, flight_.duration, next.region.dutyExponent);
        for (std::size_t i = 0; i < next.duty.bands.size(); ++i)
            next.duty.bands[i] = (std::max)(next.duty.bands[i], reservation.bands[i]);
        next.duty.aggregate = (std::max)(next.duty.aggregate, reservation.aggregate);
        if (!network_) {
            const auto windows = Lw::receiveWindows(next.region, begin + flight_.duration,
                flight_.frequency, flight_.rate, flight_.kind == Kind::Join, quint32(tolerance_));
            next.cooldown = (std::max)(next.cooldown, Lw::nextUplinkTime(windows, flight_.confirmed));
        }
        try { persist(next); } catch (const std::exception &) { throw StateFailure(); }
        store_ = std::move(next);
        flight_.begin = begin;
        flight_.timeout = begin + flight_.duration + completeTimeout_;
        flight_.started = true;
        QJsonObject result{{"ticket", QString::number(ticket)}, {"nativeId", QString::number(flight_.id)},
            {"startUs", QString::number(begin)}, {"endUs", QString::number(begin + flight_.duration)},
            {"airtimeUs", QString::number(flight_.duration)}, {"startDeadlineUs", QString::number(deadline)},
            {"frequencyHz", double(flight_.frequency)},
            {"dataRate", int(flight_.rate)},
            {"modem", phy(flight_.rate, network_ ? Lw::Direction::Downlink : Lw::Direction::Uplink)},
            {"hardwareTimingVerified", false}};
        auto value = result; value.insert("event", "tx_started"); publish(value);
        require(ready_ && flight_.ticket == ticket && flight_.started, "Adapter stopped before tx_started authorization returned");
        result.insert("ok", true);
        return result;
    }
    void launch(const Pending &p, const Lw::Bytes &wire, quint32 frequency, unsigned rate, quint64 duration,
                quint64 latestStart = 0) {
        require(nextTicket_ != (std::numeric_limits<quint64>::max)(), "LoRaWAN transport ticket exhausted");
        flight_ = {nextTicket_++, p.id, now_, duration, now_ + duration + completeTimeout_, p.kind,
            (p.kind == Kind::Data || p.kind == Kind::Repeat) && Lw::confirmed(p.frame.type), frequency, rate,
            p.expires, latestStart, false};
        increment(txCount_);
        const auto ticket = flight_.ticket;
        publish({{"event", "tx_plan"}, {"ticket", QString::number(ticket)}, {"nativeId", QString::number(p.id)},
            {"startUs", QString::number(now_)}, {"endUs", QString::number(now_ + duration)},
            {"airtimeUs", QString::number(duration)},
            {"startDeadlineUs", QString::number(startDeadline())},
            {"frequencyHz", double(frequency)}, {"dataRate", int(rate)},
            {"modem", phy(rate, network_ ? Lw::Direction::Downlink : Lw::Direction::Uplink)},
            {"hardwareTimingVerified", false}});
        // A synchronous host callback may stop the adapter while processing tx_plan.
        if (ready_ && flight_.ticket == ticket) emit transmit(ticket, interface_, bytes(wire));
    }
    void sendUplink() {
        const bool automatic = queue_.isEmpty();
        if (automatic && (!session_ || (store_.mac.empty() && !session_->state().acknowledgmentPending))) return;
        if (now_ < store_.cooldown || now_ < Lw::dutyReady(store_.duty, frequency_)) return;
        Pending p;
        if (!automatic) p = queue_.head();
        bool consume = !automatic;
        auto next = store_;
        Lw::Bytes wire;
        if (p.kind == Kind::Join) {
            const Lw::JoinRequest request{credentials_.joinEui, credentials_.devEui, 0};
            wire = Lw::encodeJoinRequest(request, credentials_.appKey);
        } else if (p.kind == Kind::Repeat) {
            wire = session_->state().lastTransmitted;
            require(!wire.empty(), "No cached uplink to repeat");
            p.frame.type = Lw::FrameType(wire[0] >> 5);
        }
        else {
            p.frame.control |= next.region.onNewUplink();
            const auto allMac = !next.mac.empty() ? next.mac : next.region.stickyAnswers;
            Lw::Bytes mac;
            const auto capacity = Lw::Eu868::maxMacPayload(next.region.dataRate) - 8;
            const auto parsed = Lw::parseMac(Lw::Direction::Uplink, allMac);
            require(parsed.unparsed.empty(), "Unsupported pending uplink MAC answer");
            std::size_t take = 0;
            for (const auto &command : parsed.commands) {
                const auto end = take + 1 + command.data.size();
                if (end > capacity) break;
                take = end;
            }
            mac.assign(allMac.begin(), allMac.begin() + take);
            if (!mac.empty()) {
                if (mac.size() <= 15) p.frame.options = mac;
                else {
                    p.frame.port = 0; p.frame.payload = mac;
                    p.frame.type = Lw::FrameType::UnconfirmedUp;
                    consume = false; p.id = 0;
                }
            }
            wire = preview(p.frame);
            if (!mac.empty() && wire.size() > Lw::Eu868::maxMacPayload(next.region.dataRate) + 5) {
                // Send MAC answers first without silently consuming an application request.
                p.frame.payload.clear(); p.frame.port.reset();
                p.frame.type = Lw::FrameType::UnconfirmedUp; p.id = 0; consume = false;
                wire = preview(p.frame);
            }
            Lw::Eu868::checkPayload(wire, next.region.dataRate);
            next.mac.assign(allMac.begin() + take, allMac.end());
        }
        const auto rate = next.region.dataRate;
        next.region.channelIndex(frequency_, rate);
        Lw::Eu868::checkPayload(wire, rate);
        const auto duration = airtime(int(wire.size()), rate, Lw::Direction::Uplink);
        reserve(next, frequency_, duration, p.kind == Kind::Join);
        const auto windows = Lw::receiveWindows(next.region, now_ + duration, frequency_, rate, p.kind == Kind::Join, quint32(tolerance_));
        next.cooldown = Lw::nextUplinkTime(windows, (p.kind == Kind::Data || p.kind == Kind::Repeat) && Lw::confirmed(p.frame.type));
        if (session_ && session_->state().maxUplinkCopies != next.region.nbTrans) session_->setNbTrans(next.region.nbTrans);
        staged_ = &next;
        if (p.kind == Kind::Join) wire = device_->request();
        else if (p.kind == Kind::Repeat) wire = session_->repeatUplink();
        else wire = session_->transmit(p.frame);
        staged_ = nullptr;
        if (consume) queue_.dequeue();
        launch(p, wire, frequency_, rate, duration);
    }
    void sendDownlink() {
        if (!windows_) return;
        if (now_ > windows_->back().closesUs) {
            windows_.reset(); joinAccept_.clear(); return;
        }
        if (joinAccept_.isEmpty() && queue_.isEmpty() && (!session_ || !session_->state().acknowledgmentPending)) return;
        for (const auto &w : *windows_) {
            if (now_ < w.nominalUs || now_ > w.nominalUs + tolerance_ || now_ < Lw::dutyReady(store_.duty, w.frequencyHz)) continue;
            Pending p; p.id = 0;
            const bool joining = !joinAccept_.isEmpty();
            if (joining) p.kind = Kind::JoinAccept;
            else if (!queue_.isEmpty()) p = queue_.head();
            else p.frame.type = Lw::FrameType::UnconfirmedDown;
            const auto wire = joining ? bytes(joinAccept_) : preview(p.frame);
            Lw::Eu868::checkPayload(wire, w.dataRate);
            const auto duration = airtime(int(wire.size()), w.dataRate, Lw::Direction::Downlink);
            auto next = store_; reserve(next, w.frequencyHz, duration, false);
            if (joining) { persist(next); store_ = std::move(next); }
            else {
                staged_ = &next;
                const auto committedWire = session_->transmit(p.frame);
                staged_ = nullptr;
                if (committedWire != wire) throw StateFailure();
            }
            if (!joining && !queue_.isEmpty()) queue_.dequeue();
            const auto frequency = w.frequencyHz; const auto rate = w.dataRate;
            const auto latestStart = w.nominalUs + tolerance_;
            windows_.reset(); joinAccept_.clear();
            launch(p, wire, frequency, rate, duration, latestStart);
            return;
        }
    }
};
}

std::unique_ptr<ProtocolAdapter> makeLoRaWanAdapter(QObject *parent) {
    return std::make_unique<LoRaWanAdapter>(parent);
}
}
