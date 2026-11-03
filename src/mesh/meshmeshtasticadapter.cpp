#include "meshprotocoladapter.h"
#include "../meshtastic/meshtastic.h"
#include "../meshtastic/meshpki.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QQueue>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace Mesh {
namespace {
namespace Mt = obrii::meshtastic;
constexpr int MaxPkiPeers = 256, PkiStateBaseBytes = 112, MaxPkiStateBytes = PkiStateBaseBytes + 36 * MaxPkiPeers;
using Pins = std::map<quint32, Mt::Key32>;

struct WipeKey {
    Mt::Key32 &key;
    ~WipeKey() { Mt::secureErase(key.data(), key.size()); }
};
struct WipeArray {
    QByteArray &value;
    ~WipeArray() { Mt::secureErase(value.data(), std::size_t(value.size())); }
};

class PkiStateFailure : public std::runtime_error {
public:
    PkiStateFailure() : std::runtime_error("Meshtastic PKI local identity/pin state is invalid or unavailable; it was not replaced") {}
};

QJsonObject rejected(const QString &reason) {
    return {{"ok", false}, {"error", reason}};
}

bool only(const QJsonObject &object, const QStringList &names) {
    for (auto i = object.begin(); i != object.end(); ++i)
        if (!names.contains(i.key())) return false;
    return true;
}

quint64 integer(const QJsonValue &value, quint64 low, quint64 high) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble() ||
        value.toDouble() < double(low) || value.toDouble() > double(high))
        throw std::invalid_argument("Invalid integer configuration/command field");
    return quint64(value.toDouble());
}

quint32 address(const QJsonValue &value) {
    if (!value.isString()) return quint32(integer(value, 0, 0xffffffffULL));
    QString hex = value.toString();
    if (hex.startsWith('!')) hex.remove(0, 1);
    if (hex.startsWith("0x", Qt::CaseInsensitive)) hex.remove(0, 2);
    if (hex.isEmpty() || hex.size() > 8) throw std::invalid_argument("Invalid hexadecimal node number");
    for (const QChar c : hex)
        if (!((c >= '0' && c <= '9') || (c.toLower() >= 'a' && c.toLower() <= 'f')))
            throw std::invalid_argument("Invalid hexadecimal node number");
    bool ok = false;
    const auto result = hex.toUInt(&ok, 16);
    if (!ok) throw std::invalid_argument("Invalid hexadecimal node number");
    return result;
}

bool boolean(const QJsonObject &object, const QString &name, bool fallback) {
    if (!object.contains(name)) return fallback;
    if (!object.value(name).isBool()) throw std::invalid_argument("Expected boolean field");
    return object.value(name).toBool();
}

QByteArray base64(const QJsonValue &value, int limit) {
    if (!value.isString()) throw std::invalid_argument("Expected base64 string");
    if (value.toString().size() > 4 * ((limit + 2) / 3)) throw std::invalid_argument("Payload exceeds limit");
    const auto encoded = value.toString().toLatin1();
    const auto decoded = QByteArray::fromBase64(encoded);
    if (decoded.size() > limit || decoded.toBase64() != encoded)
        throw std::invalid_argument("Expected canonical padded base64");
    return decoded;
}

Mt::Bytes bytes(const QByteArray &value) {
    return Mt::Bytes(value.begin(), value.end());
}
QByteArray bytes(const Mt::Bytes &value) {
    return QByteArray(reinterpret_cast<const char *>(value.data()), int(value.size()));
}

QString publicHex(const Mt::Key32 &key) {
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(key.data()), int(key.size())).toHex());
}
QString fingerprint(const Mt::Key32 &key) {
    return QString::fromLatin1(QCryptographicHash::hash(
        QByteArray(reinterpret_cast<const char *>(key.data()), int(key.size())), QCryptographicHash::Sha256).toHex());
}
Mt::Key32 parsePublicKey(const QJsonValue &value) {
    if (!value.isString() || value.toString().size() != 64)
        throw std::invalid_argument("PKI publicKeyHex must contain exactly 64 hexadecimal characters");
    const auto hex = value.toString();
    for (const QChar c : hex)
        if (!((c >= '0' && c <= '9') || (c.toLower() >= 'a' && c.toLower() <= 'f')))
            throw std::invalid_argument("Invalid PKI publicKeyHex");
    const auto raw = QByteArray::fromHex(hex.toLatin1());
    Mt::Key32 result{};
    std::copy(raw.begin(), raw.end(), result.begin());
    return result;
}

Pins profilePins(const QJsonObject &configuration, quint32 localNode, bool enabled) {
    Pins pins;
    if (!configuration.contains("pkiPeers")) return pins;
    if (!enabled || !configuration.value("pkiPeers").isArray())
        throw std::invalid_argument("pkiPeers requires pki:true and an explicitly configured peer array");
    const auto peers = configuration.value("pkiPeers").toArray();
    if (peers.size() > MaxPkiPeers) throw std::invalid_argument("PKI supports at most 256 pinned peers");
    for (const auto &value : peers) {
        if (!value.isObject() || !only(value.toObject(), {"nodeNumber", "publicKeyHex"}))
            throw std::invalid_argument("PKI peer requires only nodeNumber and publicKeyHex");
        const auto peer = value.toObject();
        const auto id = address(peer.value("nodeNumber"));
        if (id <= 3 || id == Mt::Broadcast || id == localNode || pins.count(id))
            throw std::invalid_argument("Reserved, local or duplicate PKI peer node number");
        pins.emplace(id, parsePublicKey(peer.value("publicKeyHex")));
    }
    return pins;
}

class StateFailure : public std::runtime_error {
public:
    StateFailure() : std::runtime_error("Meshtastic packet-ID persistence failed; TX disabled") {}
};

QString eventName(Mt::EventKind kind) {
    switch (kind) {
    case Mt::EventKind::Message: return "message";
    case Mt::EventKind::Observed: return "observed";
    case Mt::EventKind::Duplicate: return "duplicate";
    case Mt::EventKind::Rejected: return "rejected";
    case Mt::EventKind::UnknownChannel: return "unknown_channel";
    case Mt::EventKind::AmbiguousChannel: return "ambiguous_channel";
    case Mt::EventKind::Sent: return "sent";
    case Mt::EventKind::Relayed: return "relayed";
    case Mt::EventKind::Acknowledged: return "acknowledged";
    case Mt::EventKind::NegativeAck: return "negative_ack";
    case Mt::EventKind::Retrying: return "retrying";
    case Mt::EventKind::TimedOut: return "tx_failed";
    case Mt::EventKind::TxFailed: return "tx_failed";
    case Mt::EventKind::QueueFull: return "queue_full";
    case Mt::EventKind::IdentityConflict: return "identity_conflict";
    case Mt::EventKind::Cancelled: return "cancelled";
    }
    return "rejected";
}

class MeshtasticAdapter final : public ProtocolAdapter {
public:
    using ProtocolAdapter::ProtocolAdapter;
    ~MeshtasticAdapter() override { stop(); }

    bool start(const QJsonObject &configuration, const QStringList &interfaces,
               const QString &localStateDirectory, QString *error) override {
        stop();
        fault_.clear();
        publicKeyHex_.clear();
        nativeFingerprint_.clear();
        nodeNumber_ = 0;
        lastNow_ = 0;
        try {
            if (!only(configuration, {"nodeNumber", "channels", "relayRole", "hopLimit", "interfaceId",
                                      "relayUnknownChannels", "relayPki", "relayDelayMs", "relayJitterMs",
                                      "ackTimeoutMs", "retryDelayMs", "maxRetries", "historyTtlMs", "pki", "pkiPeers"}))
                throw std::invalid_argument("Unsupported Meshtastic configuration field");
            QStringList selected = interfaces;
            if (selected.isEmpty()) selected << configuration.value("interfaceId").toString("radio");
            if (selected.size() != 1 || selected.first().isEmpty() || selected.first().size() > 32 ||
                (configuration.contains("interfaceId") &&
                 (!configuration.value("interfaceId").isString() ||
                  configuration.value("interfaceId").toString() != selected.first())))
                throw std::invalid_argument("Meshtastic requires exactly one matching radio interface");
            interface_ = selected.first();
            Mt::NodeConfig config;
            config.nodeId = address(configuration.value("nodeNumber"));
            nodeNumber_ = config.nodeId;
            const bool enablePki = boolean(configuration, "pki", false);
            const auto requestedPins = profilePins(configuration, config.nodeId, enablePki);
            if (configuration.contains("channels")) {
                if (!configuration.value("channels").isArray()) throw std::invalid_argument("Expected channels array");
                const auto channels = configuration.value("channels").toArray();
                if (channels.isEmpty() || channels.size() > 8) throw std::invalid_argument("Configure 1..8 channels");
                config.channels.clear();
                for (const auto &value : channels) {
                    if (!value.isObject()) throw std::invalid_argument("Expected channel object");
                    const auto object = value.toObject();
                    if (!only(object, {"name", "pskBase64", "enabled"}))
                        throw std::invalid_argument("Unsupported channel field");
                    Mt::Channel channel;
                    if (object.contains("name")) {
                        if (!object.value("name").isString() || object.value("name").toString().size() > 64)
                            throw std::invalid_argument("Expected channel name of 1..64 UTF-8 bytes");
                        channel.name = object.value("name").toString().toUtf8().toStdString();
                    }
                    if (object.contains("pskBase64")) channel.psk = bytes(base64(object.value("pskBase64"), 32));
                    channel.enabled = boolean(object, "enabled", true);
                    channel.validate();
                    config.channels.push_back(std::move(channel));
                }
            }
            const auto role = configuration.value("relayRole").toString("client");
            if (configuration.contains("relayRole") && !configuration.value("relayRole").isString())
                throw std::invalid_argument("Expected relayRole string");
            if (role == "off") config.relay = Mt::RelayMode::Off;
            else if (role == "client") config.relay = Mt::RelayMode::Client;
            else if (role == "router") config.relay = Mt::RelayMode::Router;
            else throw std::invalid_argument("relayRole must be off, client or router");
            relayRole_ = role;
            auto setting = [&](const char *name, quint32 &field, quint64 low, quint64 high) {
                if (configuration.contains(name)) field = quint32(integer(configuration.value(name), low, high));
            };
            quint32 hops = config.defaultHopLimit;
            setting("hopLimit", hops, 0, 7);
            config.defaultHopLimit = quint8(hops);
            hopLimit_ = int(hops);
            setting("relayDelayMs", config.relayDelayMs, 0, 86400000);
            setting("relayJitterMs", config.relayJitterMs, 0, 86400000);
            setting("ackTimeoutMs", config.ackTimeoutMs, 1, 86400000);
            setting("retryDelayMs", config.retryDelayMs, 1, 86400000);
            setting("historyTtlMs", config.historyTtlMs, 1, 86400000);
            if (configuration.contains("maxRetries")) config.maxRetries = int(integer(configuration.value("maxRetries"), 0, 10));
            config.relayUnknownChannels = boolean(configuration, "relayUnknownChannels", false);
            config.relayPki = boolean(configuration, "relayPki", true);
            config.maxQueue = 32;
            config.maxPending = 16;
            config.maxEvents = 128;
            config.validate();
            const QFileInfo directory(localStateDirectory);
            if (!directory.isAbsolute() || !directory.isDir() || directory.isSymLink())
                throw std::invalid_argument("A local validated state directory is required");
            const QDir state(directory.canonicalFilePath());
            lock_ = std::make_unique<QLockFile>(state.filePath(QString("meshtastic-%1.lock").arg(config.nodeId)));
            lock_->setStaleLockTime(0);
            if (!lock_->tryLock(0)) throw std::invalid_argument("Meshtastic state is already owned or unavailable");
            bool existingPki = false;
            if (enablePki) {
                pki_ = provisionPki(state, requestedPins, &existingPki);
                config.pki = pki_;
                config.validate();
                publicKeyHex_ = publicHex(pki_->publicKey());
                nativeFingerprint_ = fingerprint(pki_->publicKey());
            }

            // Separate key namespaces survive channel reorder, subsets and key rotation.
            // The maximum is reserved in every enabled namespace before Node sees an ID.
            nextId_ = 1;
            QSet<QString> seen;
            auto loadCounter = [&](const QString &hash, bool required = false) {
                const auto name = QString("meshtastic-%1-%2.json").arg(config.nodeId).arg(hash);
                if (seen.contains(name)) return;
                seen.insert(name);
                Counter counter{state.filePath(name), hash};
                const QFileInfo fileInfo(counter.path);
                if (fileInfo.isSymLink() || (required && !fileInfo.exists())) throw StateFailure();
                if (fileInfo.exists()) {
                    QFile file(counter.path);
                    if (!fileInfo.isFile() || fileInfo.size() > 1024 || !file.open(QIODevice::ReadOnly)) throw StateFailure();
                    QJsonParseError parse;
                    const auto document = QJsonDocument::fromJson(file.read(1025), &parse);
                    const auto saved = document.object();
                    bool valid = false;
                    const auto next = saved.value("next").toString().toULongLong(&valid);
                    if (parse.error != QJsonParseError::NoError || !document.isObject() ||
                        saved.value("version").toInt() != 1 || saved.value("keyHash").toString() != hash ||
                        saved.value("nodeNumber").toString() != QString::number(config.nodeId) ||
                        !valid || !next || next > 0x100000000ULL) throw StateFailure();
                    nextId_ = qMax(nextId_, next);
                }
                counters_.push_back(std::move(counter));
            };
            for (const auto &channel : config.channels) {
                if (!channel.enabled) continue;
                loadCounter(QString::fromLatin1(QCryptographicHash::hash(bytes(channel.effectiveKey()),
                                                QCryptographicHash::Sha256).toHex()));
            }
            if (counters_.empty()) throw std::invalid_argument("At least one channel must be enabled");
            if (pki_) loadCounter("pki-" + nativeFingerprint_, existingPki);
            if (nextId_ > 0xffffffffULL) throw StateFailure();
            saveCounters(nextId_);
            channelCount_ = int(config.channels.size());
            node_ = std::make_unique<Mt::Node>(std::move(config), [this] {
                if (nextId_ > 0xffffffffULL) throw StateFailure();
                const auto id = quint32(nextId_);
                saveCounters(nextId_ + 1);
                ++nextId_;
                return id;
            });
            if (error) error->clear();
            return true;
        } catch (const std::exception &exception) {
            fault_ = QString::fromUtf8(exception.what());
            stop();
            if (error) *error = fault_;
            return false;
        }
    }

    QJsonObject command(const QJsonObject &request, qint64 nowMs) override {
        const auto op = request.value("op").toString();
        if (op == "status") return {{"ok", true}, {"status", status()}};
        if (!node_) return rejected("Meshtastic adapter is not ready");
        try {
            const auto now = clock(nowMs);
            if (op == "cancel") {
                if (!only(request, {"op", "nativeId"})) return rejected("Unsupported cancel argument");
                const auto id = quint32(integer(request.value("nativeId"), 1, 0xffffffffULL));
                node_->cancel(id, now);
                drainEvents();
                return {{"ok", true}, {"nativeId", double(id)}};
            }
            if (op != "send" && op != "send_native" && op != "send_pki") return rejected("Unsupported Meshtastic operation");
            if (!only(request, {"op", "destination", "payloadBase64", "text", "reliable", "channel", "data"}))
                return rejected("Unsupported send argument");
            const auto to = address(request.value("destination"));
            if (op == "send_pki" && !pki_) return rejected("Meshtastic PKI is not enabled by the local profile");
            if (op == "send_pki" && request.contains("channel")) return rejected("PKI uses native channel hash zero; do not specify a PSK channel");
            const auto channel = request.contains("channel") ? integer(request.value("channel"), 0, 7) : 0;
            Mt::Data data;
            data.port = Mt::TextPort;
            if (request.contains("payloadBase64")) {
                if (request.contains("text")) return rejected("Specify text or payloadBase64, not both");
                data.payload = bytes(base64(request.value("payloadBase64"), int(Mt::DataMtu)));
            } else if (request.value("text").isString()) {
                if (request.value("text").toString().size() > int(Mt::DataMtu)) return rejected("Payload exceeds Meshtastic Data MTU");
                const auto text = request.value("text").toString().toUtf8();
                if (text.size() > int(Mt::DataMtu)) return rejected("Payload exceeds Meshtastic Data MTU");
                data.payload = bytes(text);
            } else return rejected("Expected payloadBase64 or text");
            if (op == "send_native" || (op == "send_pki" && request.contains("data"))) {
                if (!request.value("data").isObject()) return rejected("Expected native Data object");
                const auto native = request.value("data").toObject();
                if (!only(native, {"port", "wantResponse", "dest", "source", "requestId", "replyId", "emoji", "bitfield"}))
                    return rejected("Unsupported native Data field");
                data.port = quint32(integer(native.value("port"), 1, 511));
                if (data.port == Mt::AdminPort) return rejected("Admin client/server is not wired by this adapter");
                data.wantResponse = boolean(native, "wantResponse", false);
                auto field = [&](const char *name, quint32 &target) {
                    if (native.contains(name)) target = quint32(integer(native.value(name), 0, 0xffffffffULL));
                };
                field("dest", data.dest); field("source", data.source); field("requestId", data.requestId);
                field("replyId", data.replyId); field("emoji", data.emoji);
                if (native.contains("bitfield")) data.bitfield = quint32(integer(native.value("bitfield"), 0, 0xffffffffULL));
            } else if (request.contains("data")) return rejected("Use send_native or send_pki for application Data fields");
            const bool reliable = boolean(request, "reliable", false);
            const auto id = op == "send_pki" ? node_->sendPki(data, to, reliable, now)
                                             : node_->send(data, std::size_t(channel), to, reliable, now);
            drainEvents();
            return {{"ok", true}, {"queued", true}, {"nativeId", double(id)}};
        } catch (const StateFailure &) {
            failPersistence();
            return rejected(fault_);
        } catch (const std::exception &exception) {
            return rejected(QString::fromUtf8(exception.what()));
        }
    }

    void receive(const QString &interfaceId, const QByteArray &wire, qint64 nowMs) override {
        if (!node_ || interfaceId != interface_ || wire.size() <= int(Mt::RadioHeaderBytes) || wire.size() > int(Mt::RadioMtu)) return;
        try {
            const auto now = clock(nowMs);
            if (activeTicket_ && awaitingCompletionTo_) {
                const auto header = Mt::decodeRadio(bytes(wire)).header;
                if (header.to == nodeNumber_ && header.from == awaitingCompletionTo_) {
                    // A faster fanout link can deliver the native ACK before the
                    // aggregate TX receipt. Node accepts ACKs only after completion.
                    if (deferredRx_.size() < 32) deferredRx_.enqueue(wire);
                    else publish({{"event", "rx_dropped"}, {"protocol", "meshtastic"}, {"reason", "completion_backlog"}});
                    return;
                }
            }
            node_->receive(bytes(wire), now);
            drainEvents();
        }
        catch (const StateFailure &) { failPersistence(); }
        catch (const std::exception &) { publish({{"event", "rejected"}, {"protocol", "meshtastic"}}); }
    }

    void step(qint64 nowMs) override {
        if (!node_) return;
        try {
            const auto tx = node_->poll(clock(nowMs));
            drainEvents();
            if (!tx || !node_) return;
            activeNativeTicket_ = tx->ticket;
            activeTicket_ = nextTicket_++;
            const auto header = Mt::decodeRadio(tx->radioBytes).header;
            awaitingCompletionTo_ = header.wantAck && header.from == nodeNumber_ ? header.to : 0;
            emit transmit(activeTicket_, interface_, bytes(tx->radioBytes));
        } catch (const StateFailure &) { failPersistence(); }
        catch (const std::exception &) { publish({{"event", "rejected"}, {"protocol", "meshtastic"}}); }
    }

    void complete(quint64 ticket, bool sent, qint64 nowMs) override {
        if (!node_ || !ticket || ticket != activeTicket_) return;
        try {
            const auto now = clock(nowMs);
            node_->completeTransmission(activeNativeTicket_, sent, now);
            activeTicket_ = activeNativeTicket_ = 0;
            awaitingCompletionTo_ = 0;
            const auto deferred = std::move(deferredRx_);
            deferredRx_.clear();
            for (const auto &wire : deferred) node_->receive(bytes(wire), now);
            drainEvents();
        } catch (const StateFailure &) { failPersistence(); }
        catch (const std::exception &) { publish({{"event", "rejected"}, {"protocol", "meshtastic"}}); }
    }

    QJsonObject status() const override {
        return {{"protocol", "meshtastic"}, {"ready", bool(node_)}, {"nativeAddress", QString::number(nodeNumber_)},
                {"nodeNumber", QString::number(nodeNumber_)}, {"interfaceId", interface_},
                {"channels", channelCount_}, {"relayRole", relayRole_}, {"hopLimit", hopLimit_},
                {"queued", node_ ? int(node_->queuedCount()) : 0}, {"pending", node_ ? int(node_->pendingCount()) : 0},
                {"deferredRx", deferredRx_.size()},
                {"droppedEvents", node_ ? double(node_->droppedEventCount()) : 0},
                {"psk", true}, {"pki", bool(pki_)}, {"pkiPeerCount", pkiPeerCount_},
                {"publicKeyHex", publicKeyHex_}, {"nativeFingerprint", nativeFingerprint_},
                {"admin", false}, {"pkiAdmin", false}, {"error", fault_}};
    }

    void stop() override {
        node_.reset();
        pki_.reset();
        pkiPeerCount_ = 0;
        activeTicket_ = activeNativeTicket_ = 0;
        awaitingCompletionTo_ = 0;
        deferredRx_.clear();
        counters_.clear();
        lock_.reset();
    }

private:
    struct Counter { QString path, hash; };
    std::unique_ptr<Mt::Node> node_;
    std::shared_ptr<Mt::PkiContext> pki_;
    std::unique_ptr<QLockFile> lock_;
    std::vector<Counter> counters_;
    QQueue<QByteArray> deferredRx_;
    quint64 nextId_ = 1, nextTicket_ = 1, activeTicket_ = 0, activeNativeTicket_ = 0;
    quint32 nodeNumber_ = 0;
    quint32 awaitingCompletionTo_ = 0;
    quint64 lastNow_ = 0;
    int channelCount_ = 0, hopLimit_ = 3, pkiPeerCount_ = 0;
    QString interface_, relayRole_, fault_, publicKeyHex_, nativeFingerprint_;

    std::shared_ptr<Mt::PkiContext> provisionPki(const QDir &state, const Pins &requested, bool *existing) {
        const auto path = state.filePath(QString("meshtastic-%1.pki").arg(nodeNumber_));
        const QFileInfo info(path);
        *existing = info.exists();
        if (info.isSymLink()) throw PkiStateFailure();
        Mt::Key32 privateKey{}, storedPublic{};
        WipeKey wipePrivate{privateKey};
        Pins pins;
        auto provider = Mt::platformCrypto();
        if (*existing) {
            QFile file(path);
            if (!info.isFile() || info.size() < PkiStateBaseBytes || info.size() > MaxPkiStateBytes ||
                !file.open(QIODevice::ReadOnly)) throw PkiStateFailure();
#ifndef Q_OS_WIN
            if (file.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                      QFileDevice::ReadOther | QFileDevice::WriteOther)) throw PkiStateFailure();
#endif
            QByteArray wire = file.read(MaxPkiStateBytes + 1);
            WipeArray wipeWire{wire};
            if (wire.size() != info.size()) throw PkiStateFailure();
            QCryptographicHash checksum(QCryptographicHash::Sha256);
            checksum.addData(wire.constData(), wire.size() - 32);
            if (checksum.result() != QByteArray::fromRawData(wire.constData() + wire.size() - 32, 32)) throw PkiStateFailure();
            QDataStream input(&wire, QIODevice::ReadOnly);
            input.setVersion(QDataStream::Qt_5_12);
            quint32 magic = 0, version = 0, node = 0, count = 0;
            input >> magic >> version >> node;
            input.readRawData(reinterpret_cast<char *>(privateKey.data()), 32);
            input.readRawData(reinterpret_cast<char *>(storedPublic.data()), 32);
            input >> count;
            if (magic != 0x4d504b49 || version != 1 || node != nodeNumber_ || count > MaxPkiPeers ||
                wire.size() != PkiStateBaseBytes + int(count) * 36) throw PkiStateFailure();
            for (quint32 i = 0; i < count; ++i) {
                quint32 id = 0;
                Mt::Key32 key{};
                input >> id;
                input.readRawData(reinterpret_cast<char *>(key.data()), 32);
                if (id <= 3 || id == Mt::Broadcast || id == nodeNumber_ || !pins.emplace(id, key).second)
                    throw PkiStateFailure();
            }
            if (input.status() != QDataStream::Ok || input.device()->pos() != wire.size() - 32) throw PkiStateFailure();
        } else {
            // A missing identity beside an existing nonce ledger is loss, not a
            // request to silently rotate an identity and discard its trust pins.
            if (!state.entryList({QString("meshtastic-%1-pki-*.json").arg(nodeNumber_)},
                                 QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System).isEmpty()) throw PkiStateFailure();
            privateKey = Mt::generatePrivateKey(*provider);
        }
        auto context = std::make_shared<Mt::PkiContext>(std::move(provider), privateKey, MaxPkiPeers);
        if (*existing && !Mt::constantTimeEqual(context->publicKey(), storedPublic)) throw PkiStateFailure();
        for (const auto &pin : pins) context->pinPeer(pin.first, pin.second);
        bool changed = !*existing;
        for (const auto &pin : requested) {
            // pinPeer rejects both changed pins and low-order keys; the profile
            // can explicitly add/confirm pins, but cannot silently replace one.
            context->pinPeer(pin.first, pin.second);
            changed |= pins.emplace(pin.first, pin.second).second;
        }
        if (changed) {
            QByteArray wire;
            wire.reserve(MaxPkiStateBytes);
            WipeArray wipeWire{wire};
            {
                QDataStream output(&wire, QIODevice::WriteOnly);
                output.setVersion(QDataStream::Qt_5_12);
                output << quint32(0x4d504b49) << quint32(1) << nodeNumber_;
                output.writeRawData(reinterpret_cast<const char *>(privateKey.data()), 32);
                output.writeRawData(reinterpret_cast<const char *>(context->publicKey().data()), 32);
                output << quint32(pins.size());
                for (const auto &pin : pins) {
                    output << pin.first;
                    output.writeRawData(reinterpret_cast<const char *>(pin.second.data()), 32);
                }
                if (output.status() != QDataStream::Ok) throw PkiStateFailure();
            }
            QCryptographicHash checksum(QCryptographicHash::Sha256);
            checksum.addData(wire.constData(), wire.size());
            wire += checksum.result();
            if (QFileInfo(path).isSymLink()) throw PkiStateFailure();
            QSaveFile file(path);
            file.setDirectWriteFallback(false);
            if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
                file.write(wire) != wire.size() || !file.commit()) throw PkiStateFailure();
        }
        pkiPeerCount_ = int(pins.size());
        return context;
    }

    void publish(QJsonObject value) {
        value.insert("status", value.value("event").toString());
        emit event(value);
    }

    quint64 clock(qint64 now) {
        if (now < 0 || quint64(now) < lastNow_) throw std::invalid_argument("Meshtastic time must be monotonic");
        lastNow_ = quint64(now);
        return lastNow_;
    }

    void saveCounters(quint64 next) {
        for (const auto &counter : counters_) {
            if (QFileInfo(counter.path).isSymLink()) throw StateFailure();
            QSaveFile file(counter.path);
            file.setDirectWriteFallback(false);
            const auto wire = QJsonDocument(QJsonObject{{"version", 1}, {"nodeNumber", QString::number(nodeNumber_)},
                              {"keyHash", counter.hash}, {"next", QString::number(next)}}).toJson(QJsonDocument::Compact);
            if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
                file.write(wire) != wire.size() || !file.commit()) throw StateFailure();
        }
    }

    void failPersistence() {
        fault_ = "Meshtastic packet-ID persistence failed; TX disabled";
        stop();
        emit failed(fault_);
    }

    void drainEvents() {
        if (!node_) return;
        const auto events = node_->takeEvents();
        for (const auto &native : events) {
            auto security = native.security;
            // Core TX lifecycle events leave Security at its PSK default. On a
            // PKI-enabled local sender, channel zero is reserved for PKI frames.
            if (pki_ && native.header.from == nodeNumber_ && native.header.channelHash == 0 &&
                (native.kind == Mt::EventKind::Sent || native.kind == Mt::EventKind::TxFailed ||
                 native.kind == Mt::EventKind::Retrying || native.kind == Mt::EventKind::TimedOut)) security = Mt::Security::Pki;
            QJsonObject value{{"event", eventName(native.kind)}, {"protocol", "meshtastic"},
                              {"nativeId", double(native.requestId ? native.requestId : native.header.id)},
                              {"packetId", double(native.header.id)}, {"authenticated", native.authenticated},
                              {"security", security == Mt::Security::Pki ? "pki" : "psk"}};
            if (native.header.from > 3) value.insert("from", QString::number(native.header.from));
            if (native.header.to > 3) value.insert("to", QString::number(native.header.to));
            if (native.senderPublicKey) {
                value.insert("senderPublicKeyHex", publicHex(*native.senderPublicKey));
                value.insert("senderFingerprint", fingerprint(*native.senderPublicKey));
            }
            if (native.channelIndex) value.insert("channel", int(*native.channelIndex));
            if (native.data) {
                value.insert("payloadBase64", QString::fromLatin1(bytes(native.data->payload).toBase64()));
                value.insert("port", double(native.data->port));
                if (native.data->port == Mt::TextPort) {
                    try { value.insert("text", QString::fromStdString(Mt::text(*native.data))); }
                    catch (const std::exception &) {}
                }
            }
            if (native.kind == Mt::EventKind::TimedOut) value.insert("reason", "ack_timeout");
            if (native.kind == Mt::EventKind::NegativeAck) value.insert("routingError", double(native.routingError));
            publish(value);
        }
    }
};
}

std::unique_ptr<ProtocolAdapter> makeMeshtasticAdapter(QObject *parent) {
    return std::make_unique<MeshtasticAdapter>(parent);
}
}
