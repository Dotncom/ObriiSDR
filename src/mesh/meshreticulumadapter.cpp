#include "meshprotocoladapter.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <optional>

namespace Mesh {
namespace {
constexpr int MaxLine = 32768, MaxQueue = 32, NormalQueue = 24, RpcTimeout = 3000;
constexpr int DefaultStartupTimeout = 15000;

QJsonObject reject(const QString &reason) { return {{"ok", false}, {"error", reason}}; }

bool onlyKeys(const QJsonObject &object, const QStringList &names) {
    for (auto i = object.begin(); i != object.end(); ++i)
        if (!names.contains(i.key())) return false;
    return true;
}
bool number(const QJsonValue &value, double minimum, double maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) &&
           std::floor(value.toDouble()) == value.toDouble() &&
           value.toDouble() >= minimum && value.toDouble() <= maximum;
}
bool hashValue(const QJsonValue &value) {
    static const QRegularExpression expression("^[0-9a-fA-F]{32}$");
    return value.isString() && expression.match(value.toString()).hasMatch();
}
bool decode64(const QJsonValue &value, int limit, QByteArray *decoded = nullptr) {
    if (!value.isString() || value.toString().size() > 4 * ((limit + 2) / 3)) return false;
    const auto encoded = value.toString().toLatin1();
    const auto data = QByteArray::fromBase64(encoded);
    if (data.size() > limit || data.toBase64() != encoded) return false;
    if (decoded) *decoded = data;
    return true;
}

class ReticulumAdapter final : public ProtocolAdapter {
public:
    explicit ReticulumAdapter(QObject *parent) : ProtocolAdapter(parent), process_(this) {
        clock_.start();
        connect(&process_, &QProcess::started, this, [this] {
            if (active_) enqueue("status");
        });
        connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { readOutput(); });
        connect(&process_, &QProcess::readyReadStandardError, this, [this] {
            process_.setReadChannel(QProcess::StandardError);
            const bool hasData = !process_.read(4096).isEmpty();
            process_.setReadChannel(QProcess::StandardOutput);
            if (active_ && hasData) fail("Reticulum backend diagnostics reported a failure (details redacted)");
        });
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            if (active_) fail("Reticulum backend process failed (runtime details redacted)");
        });
        connect(&process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int, QProcess::ExitStatus) {
            if (active_) fail("Reticulum backend exited unexpectedly (details redacted)");
        });
    }
    ~ReticulumAdapter() override { stop(); }

    bool start(const QJsonObject &configuration, const QStringList &interfaceIds,
               const QString &localStateDirectory, QString *error) override {
        stop();
        fault_.clear();
        address_.clear();
        backendStatus_ = {};
        auto invalid = [&](const QString &message) {
            fault_ = message;
            if (error) *error = message;
            return false;
        };
        if (!onlyKeys(configuration, {"python", "moduleDirectory", "interfaces", "framing", "transport", "bitrate", "mtu", "startupTimeoutMs"}))
            return invalid("Unsupported Reticulum configuration field; runtime paths must be locally approved");
        if (!configuration.value("python").isString() || !configuration.value("moduleDirectory").isString())
            return invalid("Reticulum requires locally approved python and moduleDirectory paths");
        const QFileInfo python(configuration.value("python").toString());
        const QFileInfo modules(configuration.value("moduleDirectory").toString());
        const QFileInfo state(localStateDirectory);
        if (!python.isAbsolute() || !python.isFile() || !python.isExecutable() ||
            !modules.isAbsolute() || !modules.isDir() || !state.isAbsolute() || !state.isDir() || state.isSymLink())
            return invalid("Reticulum local runtime or state directory is unavailable");
        const QDir moduleDirectory(modules.canonicalFilePath());
        for (const auto &file : {"__init__.py", "service.py", "node.py", "transport.py", "fragments.py", "rnode.py", "access.py"}) {
            const QFileInfo module(moduleDirectory.filePath(QString("obrii_reticulum/") + file));
            if (!module.isFile() || module.isSymLink()) return invalid("Reticulum local sidecar module is incomplete");
        }
        interfaces_ = interfaceIds;
        QStringList configured;
        if (configuration.contains("interfaces")) {
            if (!configuration.value("interfaces").isArray()) return invalid("Reticulum interfaces must be an array");
            for (const auto &value : configuration.value("interfaces").toArray()) {
                if (!value.isString()) return invalid("Reticulum interface IDs must be strings");
                configured << value.toString();
            }
            if (!interfaces_.isEmpty() && configured != interfaces_) return invalid("Reticulum interface IDs do not match host bindings");
            if (interfaces_.isEmpty()) interfaces_ = configured;
        } else if (interfaces_.isEmpty()) interfaces_ << "radio";
        if (interfaces_.isEmpty() || interfaces_.size() > 8) return invalid("Reticulum requires 1..8 interfaces");
        QSet<QString> unique;
        const QRegularExpression validInterface("^[A-Za-z0-9_.-]{1,32}$");
        for (const auto &name : interfaces_) {
            if (!validInterface.match(name).hasMatch() || unique.contains(name)) return invalid("Invalid or duplicate Reticulum interface ID");
            unique.insert(name);
        }
        framing_ = configuration.value("framing").toString("obrn");
        if ((configuration.contains("framing") && !configuration.value("framing").isString()) ||
            (framing_ != "obrn" && framing_ != "rnode")) return invalid("Reticulum framing must be obrn or rnode");
        if (configuration.contains("transport") && !configuration.value("transport").isBool()) return invalid("transport must be boolean");
        transport_ = configuration.value("transport").toBool(false);
        if (configuration.contains("bitrate") && !number(configuration.value("bitrate"), 5, 10000000)) return invalid("Invalid Reticulum bitrate");
        bitrate_ = configuration.value("bitrate").toInt(1100);
        if (configuration.contains("mtu") && !number(configuration.value("mtu"), 64, 255)) return invalid("Invalid Reticulum radio MTU");
        mtu_ = configuration.value("mtu").toInt(255);
        if (framing_ == "rnode" && mtu_ != 255) return invalid("RNode framing requires MTU 255");
        if (configuration.contains("startupTimeoutMs") && !number(configuration.value("startupTimeoutMs"), 1000, 60000))
            return invalid("Reticulum startupTimeoutMs must be an integer in 1000..60000");
        startupTimeoutMs_ = configuration.value("startupTimeoutMs").toInt(DefaultStartupTimeout);
        lock_ = std::make_unique<QLockFile>(QDir(state.canonicalFilePath()).filePath("reticulum-adapter.lock"));
        lock_->setStaleLockTime(0);
        if (!lock_->tryLock(0)) { lock_.reset(); return invalid("Reticulum state is already owned or unavailable"); }

        // -I ignores PYTHON* and user-site imports. Paths are argv data, never code.
        // The only executable snippet is this fixed bootstrap for the local package.
        const QString bootstrap = "import runpy,sys; p=sys.argv.pop(1); sys.path.insert(0,p); "
                                  "sys.argv[0]='obrii_reticulum.service'; "
                                  "runpy.run_module('obrii_reticulum.service',run_name='__main__')";
        QStringList arguments{"-I", "-u", "-c", bootstrap, modules.canonicalFilePath(),
                              "--state-dir", state.canonicalFilePath(), "--framing", framing_,
                              "--mtu", QString::number(mtu_), "--bitrate", QString::number(bitrate_)};
        if (transport_) arguments << "--transport";
        for (const auto &name : interfaces_) arguments << "--interface" << name;
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto &name : environment.keys())
            if (name.startsWith("PYTHON", Qt::CaseInsensitive)) environment.remove(name);
        process_.setProcessEnvironment(environment);
        process_.setWorkingDirectory(modules.canonicalFilePath());
        process_.setProcessChannelMode(QProcess::SeparateChannels);
        process_.setProgram(python.canonicalFilePath());
        process_.setArguments(arguments);
        active_ = true;
        ready_ = false;
        acceptedAt_ = clock_.elapsed();
        nextPoll_ = nextStatus_ = acceptedAt_;
        process_.start(QIODevice::ReadWrite);
        if (error) error->clear();
        return true;
    }

    QJsonObject command(const QJsonObject &request, qint64) override {
        const auto op = request.value("op").toString();
        if (op == "status") return {{"ok", true}, {"status", status()}};
        if (!active_ || !ready_) return reject("Reticulum adapter is not ready");
        QStringList allowed{"op"};
        if (op == "send" || op == "link_send" || op == "request_echo") allowed << "payloadBase64" << "data" << "text";
        if (op == "announce") allowed << "payloadBase64" << "data" << "text";
        if (op == "send" || op == "request_path" || op == "path" || op == "link_open") allowed << "destination";
        if (op == "send" || op == "link_open") allowed << "expected_identity";
        if (op == "link_close" || op == "link_send" || op == "request_echo") allowed << "link";
        if (!QStringList{"send", "announce", "request_path", "path", "link_open", "link_close", "link_send", "request_echo"}.contains(op))
            return reject("Unsupported Reticulum operation");
        if (!onlyKeys(request, allowed)) return reject("Unsupported Reticulum command argument");
        QJsonObject arguments;
        if (allowed.contains("destination")) {
            if (!hashValue(request.value("destination"))) return reject("Expected 16-byte destination hash in hex");
            arguments.insert("destination", request.value("destination").toString().toLower());
        }
        if (allowed.contains("link")) {
            if (!hashValue(request.value("link"))) return reject("Expected 16-byte link hash in hex");
            arguments.insert("link", request.value("link").toString().toLower());
        }
        if (request.contains("expected_identity")) {
            if (!hashValue(request.value("expected_identity"))) return reject("Expected 16-byte identity fingerprint in hex");
            arguments.insert("expected_identity", request.value("expected_identity").toString().toLower());
        }
        if (allowed.contains("data")) {
            const int fields = int(request.contains("payloadBase64")) + int(request.contains("data")) + int(request.contains("text"));
            if (fields > 1 || (op != "announce" && !fields)) return reject("Specify exactly one payload field");
            QJsonValue value = request.contains("payloadBase64") ? request.value("payloadBase64") : request.value("data");
            if (request.contains("text")) {
                if (!request.value("text").isString() || request.value("text").toString().size() > 4096) return reject("Invalid text payload");
                value = QString::fromLatin1(request.value("text").toString().toUtf8().toBase64());
            }
            if (op == "announce" && !fields) value = QString();
            const int limit = op == "announce" ? 128 : 4096;
            if (!decode64(value, limit)) return reject("Invalid or oversized canonical base64 payload");
            arguments.insert("data", value);
        }
        const auto id = enqueue(op, arguments, {}, true);
        if (id.isEmpty()) return reject("Reticulum RPC queue full");
        return {{"ok", true}, {"pending", true}, {"id", id}};
    }

    void receive(const QString &interfaceId, const QByteArray &wire, qint64) override {
        if (!active_ || !ready_ || !interfaces_.contains(interfaceId)) return;
        if (wire.isEmpty() || wire.size() > mtu_) {
            publish({{"event", "rejected"}, {"protocol", "reticulum"}, {"reason", "radio_mtu"}});
            return;
        }
        if (enqueue("rx", {{"data", QString::fromLatin1(wire.toBase64())}}, interfaceId).isEmpty())
            publish({{"event", "rx_dropped"}, {"protocol", "reticulum"}, {"reason", "queue_full"}});
    }

    void step(qint64) override {
        if (!active_) return;
        const auto now = clock_.elapsed();
        if (!ready_ && now - acceptedAt_ >= startupTimeoutMs_) {
            fail("Reticulum backend startup readiness timeout");
            return;
        }
        if (ready_ && ((inFlight_ && now - inFlight_->accepted >= RpcTimeout) ||
                      (!queue_.isEmpty() && now - queue_.head().accepted >= RpcTimeout))) {
            fail("Reticulum backend RPC timeout");
            return;
        }
        if (!ready_ || now < nextPoll_) return;
        nextPoll_ = now + 50;
        // Four maximum-size echo responses still fit in one 32768-byte line.
        if (!hasPending("events")) enqueue("events", {{"limit", 4}});
        for (const auto &name : interfaces_)
            if (!busyInterfaces_.contains(name) && !hasPending("poll_tx", name)) enqueue("poll_tx", {}, name);
        if (now >= nextStatus_ && !hasPending("status")) {
            nextStatus_ = now + 1000;
            enqueue("status");
        }
    }

    void complete(quint64 ticket, bool sent, qint64) override {
        if (!active_ || !ready_) return;
        const auto found = transmissions_.find(ticket);
        if (found == transmissions_.end()) return;
        const auto tx = found.value();
        transmissions_.erase(found);
        if (enqueue("tx_done", {{"ticket", tx.nativeTicket}, {"sent", sent}}, tx.interface, false, true).isEmpty())
            fail("Reticulum TX completion queue exhausted");
    }

    QJsonObject status() const override {
        QJsonArray names;
        for (const auto &name : interfaces_) names.append(name);
        QJsonObject result{{"protocol", "reticulum"}, {"ready", active_ && ready_}, {"running", active_},
                           {"nativeAddress", address_}, {"interfaces", names}, {"framing", framing_},
                           {"transport", transport_}, {"bitrate", bitrate_}, {"mtu", mtu_},
                           {"startupTimeoutMs", startupTimeoutMs_}, {"rpcTimeoutMs", RpcTimeout},
                           {"queuedRpcs", queue_.size() + int(inFlight_.has_value())},
                           {"txPending", transmissions_.size()}, {"error", fault_}};
        for (const auto &key : {"rns_version", "single_mdu", "receipts_pending", "requests_pending", "dropped_events"})
            if (backendStatus_.contains(key)) result.insert(key, backendStatus_.value(key));
        return result;
    }

    void stop() override {
        active_ = ready_ = false;
        queue_.clear();
        inFlight_.reset();
        transmissions_.clear();
        busyInterfaces_.clear();
        output_.clear();
        if (process_.state() != QProcess::NotRunning) {
            if (process_.state() == QProcess::Running) {
                process_.write("{\"id\":\"stop\",\"op\":\"shutdown\"}\n");
                process_.closeWriteChannel();
            }
            if (!process_.waitForFinished(200)) {
                process_.terminate();
                if (!process_.waitForFinished(200)) {
                    process_.kill();
                    process_.waitForFinished(300);
                }
            }
        }
        process_.closeReadChannel(QProcess::StandardOutput);
        process_.closeReadChannel(QProcess::StandardError);
        lock_.reset();
    }

private:
    struct Rpc {
        QString id, op, interface;
        QByteArray wire;
        qint64 accepted = 0;
        bool publicReply = false;
    };
    struct Tx { QString interface; double nativeTicket = 0; };
    QProcess process_;
    QElapsedTimer clock_;
    QQueue<Rpc> queue_;
    std::optional<Rpc> inFlight_;
    QMap<quint64, Tx> transmissions_;
    QSet<QString> busyInterfaces_;
    std::unique_ptr<QLockFile> lock_;
    QByteArray output_;
    QStringList interfaces_;
    QString framing_ = "obrn", address_, fault_;
    QJsonObject backendStatus_;
    bool active_ = false, ready_ = false, transport_ = false;
    int bitrate_ = 1100, mtu_ = 255, startupTimeoutMs_ = DefaultStartupTimeout;
    quint64 nextId_ = 1, nextTicket_ = 1;
    qint64 acceptedAt_ = 0, nextPoll_ = 0, nextStatus_ = 0;

    void publish(QJsonObject value) {
        value.insert("status", value.value("event").toString());
        emit event(value);
    }

    bool hasPending(const QString &op, const QString &interface = {}) const {
        if (inFlight_ && inFlight_->op == op && inFlight_->interface == interface) return true;
        for (const auto &rpc : queue_) if (rpc.op == op && rpc.interface == interface) return true;
        return false;
    }

    QString enqueue(const QString &op, QJsonObject arguments = {}, const QString &interface = {},
                    bool publicReply = false, bool receipt = false) {
        if (!active_ || queue_.size() + int(inFlight_.has_value()) >= (receipt ? MaxQueue : NormalQueue)) return {};
        const auto id = QString::number(nextId_++);
        arguments.insert("id", id);
        arguments.insert("op", op);
        if (!interface.isEmpty()) arguments.insert("interface", interface);
        const auto wire = QJsonDocument(arguments).toJson(QJsonDocument::Compact) + '\n';
        if (wire.size() > MaxLine) return {};
        queue_.enqueue({id, op, interface, wire, clock_.elapsed(), publicReply});
        pump();
        return id;
    }

    void pump() {
        if (!active_ || inFlight_ || queue_.isEmpty() || process_.state() != QProcess::Running) return;
        inFlight_ = queue_.dequeue();
        if (process_.write(inFlight_->wire) != inFlight_->wire.size()) fail("Reticulum backend input failed");
    }

    void fail(const QString &reason) {
        if (!active_) return;
        fault_ = reason;
        active_ = ready_ = false;
        if (inFlight_ && inFlight_->publicReply) emitReply(*inFlight_, false, {});
        for (const auto &rpc : queue_) if (rpc.publicReply) emitReply(rpc, false, {});
        queue_.clear();
        inFlight_.reset();
        for (auto i = transmissions_.begin(); i != transmissions_.end(); ++i)
            publish({{"event", "tx_failed"}, {"protocol", "reticulum"}, {"ticket", QString::number(i.key())},
                     {"interface", i.value().interface}, {"reason", "backend_unavailable"}});
        transmissions_.clear();
        busyInterfaces_.clear();
        output_.clear();
        process_.kill();
        emit failed(reason);
    }

    void readOutput() {
        if (!active_) { process_.read(4096); return; }
        if (process_.bytesAvailable() > MaxLine * 2) { fail("Reticulum backend output limit exceeded"); return; }
        int budget = MaxLine * 2;
        while (active_ && process_.bytesAvailable() > 0 && budget > 0) {
            const auto chunk = process_.read(qMin(4096, budget));
            budget -= chunk.size();
            output_ += chunk;
            int newline;
            while (active_ && (newline = output_.indexOf('\n')) >= 0) {
                if (newline + 1 > MaxLine) { fail("Reticulum backend JSON line limit exceeded"); return; }
                const auto line = output_.left(newline);
                output_.remove(0, newline + 1);
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(line, &error);
                if (error.error != QJsonParseError::NoError || !document.isObject() || !inFlight_ ||
                    document.object().value("id").toString() != inFlight_->id ||
                    !document.object().value("ok").isBool()) {
                    fail("Reticulum backend returned an invalid RPC reply"); return;
                }
                const auto rpc = *inFlight_;
                inFlight_.reset();
                handleReply(rpc, document.object());
                pump();
            }
            if (output_.size() >= MaxLine) { fail("Reticulum backend unterminated JSON line limit exceeded"); return; }
        }
    }

    void emitReply(const Rpc &rpc, bool ok, const QJsonValue &result) {
        QJsonObject value{{"event", "reply"}, {"protocol", "reticulum"}, {"id", rpc.id}, {"op", rpc.op}, {"ok", ok}};
        if (ok) {
            value.insert("result", result);
            const auto native = result.toObject();
            for (const auto &key : {"packet", "request", "link"})
                if (native.contains(key)) { value.insert("nativeId", native.value(key)); break; }
        } else value.insert("error", "Reticulum command rejected or backend unavailable (details redacted)");
        publish(value);
    }

    void handleReply(const Rpc &rpc, const QJsonObject &reply) {
        if (!active_) return;
        const auto now = clock_.elapsed();
        if (!ready_ && now - acceptedAt_ >= startupTimeoutMs_) {
            fail("Reticulum backend startup readiness timeout");
            return;
        }
        if (ready_ && now - rpc.accepted >= RpcTimeout) {
            if (rpc.publicReply) emitReply(rpc, false, {});
            fail("Reticulum backend RPC timeout");
            return;
        }
        const bool ok = reply.value("ok").toBool();
        const auto result = reply.value("result");
        if (rpc.publicReply) { emitReply(rpc, ok, result); return; }
        if (!ok) {
            if (rpc.op == "rx") {
                publish({{"event", "rejected"}, {"protocol", "reticulum"}, {"interface", rpc.interface}});
            } else fail("Reticulum backend control operation failed (details redacted)");
            return;
        }
        if (rpc.op == "status") {
            const auto status = result.toObject();
            if (!hashValue(status.value("destination")) || status.value("rns_version").toString() != "1.5.5") {
                fail("Reticulum backend readiness/version check failed"); return;
            }
            address_ = status.value("destination").toString();
            backendStatus_ = {};
            for (const auto &key : {"rns_version", "single_mdu", "receipts_pending", "requests_pending", "dropped_events"})
                if (status.contains(key)) backendStatus_.insert(key, status.value(key));
            if (!ready_) {
                ready_ = true;
                publish({{"event", "ready"}, {"protocol", "reticulum"}, {"nativeAddress", address_}});
            }
        } else if (rpc.op == "events") {
            if (!result.isArray() || result.toArray().size() > 4) { fail("Reticulum backend event batch limit exceeded"); return; }
            for (const auto &value : result.toArray()) {
                if (!value.isObject()) { fail("Reticulum backend returned an invalid event"); return; }
                forwardEvent(value.toObject());
            }
        } else if (rpc.op == "poll_tx") {
            if (!result.isObject() || !result.toObject().contains("tx")) { fail("Reticulum backend returned invalid TX"); return; }
            const auto tx = result.toObject().value("tx");
            if (tx.isNull()) return;
            const auto object = tx.toObject();
            QByteArray wire;
            if (!tx.isObject() || !number(object.value("ticket"), 1, 9007199254740991.0) ||
                !decode64(object.value("data"), mtu_, &wire) || wire.isEmpty() || busyInterfaces_.contains(rpc.interface)) {
                fail("Reticulum backend returned invalid TX"); return;
            }
            const auto ticket = nextTicket_++;
            busyInterfaces_.insert(rpc.interface);
            transmissions_.insert(ticket, {rpc.interface, object.value("ticket").toDouble()});
            emit transmit(ticket, rpc.interface, wire);
        } else if (rpc.op == "tx_done") {
            if (!result.toObject().value("completed").toBool()) { fail("Reticulum backend did not accept TX receipt"); return; }
            busyInterfaces_.remove(rpc.interface);
        }
    }

    void forwardEvent(const QJsonObject &native) {
        const auto name = native.value("event").toString();
        static const QStringList allowed{"announce", "packet_received", "link_data", "delivered", "delivery_failed",
                                         "tx_failed", "tx_rejected", "interface_state", "link_active", "link_closed",
                                         "peer_identified", "response", "request_failed"};
        if (!allowed.contains(name)) return;
        QString normalized = name;
        if (name == "packet_received" || name == "link_data") normalized = "message";
        else if (name == "delivered") normalized = "acknowledged";
        else if (name == "delivery_failed" || name == "tx_rejected") normalized = "tx_failed";
        QJsonObject value{{"event", normalized}, {"protocol", "reticulum"}, {"backendEvent", name}};
        for (const auto &key : {"packet", "request", "link", "destination", "identity", "peer_identity", "interface",
                                "encrypted", "sender_authenticated", "signature_valid", "initiator", "online", "mdu", "hops", "ticket"})
            if (native.contains(key)) value.insert(key, native.value(key));
        for (const auto &key : {"packet", "request", "ticket"})
            if (native.contains(key)) { value.insert("nativeId", native.value(key)); break; }
        if (native.contains("data") && decode64(native.value("data"), 4096)) value.insert("payloadBase64", native.value("data"));
        if (native.contains("app_data") && decode64(native.value("app_data"), 128)) value.insert("appDataBase64", native.value("app_data"));
        if (normalized == "message") {
            value.insert("to", address_);
            // RNS SINGLE ciphertext does not reveal or authenticate the sender.
            if (hashValue(native.value("peer_identity"))) value.insert("from", native.value("peer_identity"));
        }
        publish(value);
    }
};
}

std::unique_ptr<ProtocolAdapter> makeReticulumAdapter(QObject *parent) {
    return std::make_unique<ReticulumAdapter>(parent);
}
}
