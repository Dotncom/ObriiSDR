#include "commandcontrolserver.h"

#include "commanddispatcher.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {
constexpr int MaxRequestBytes = 64 * 1024;
constexpr int MaxResponseBytes = 1024 * 1024;
constexpr int MaxClients = 8;
constexpr int TimeoutMs = 2000;
constexpr int MaxJsonDepth = 64;

void increment(quint64 &count) {
    if (count != std::numeric_limits<quint64>::max()) ++count;
}
QJsonObject failure(const QString &code, const QString &message) {
    auto result = CommandDispatcher::failure(code, message);
    result.insert(QStringLiteral("schemaVersion"), 1);
    return result;
}
QString freshToken() {
    char entropy[32];
    for (std::size_t offset = 0; offset < sizeof(entropy); offset += sizeof(quint32)) {
        const quint32 word = QRandomGenerator::system()->generate();
        std::memcpy(entropy + offset, &word, sizeof(word));
    }
    return QString::fromLatin1(QByteArray(entropy, sizeof(entropy)).toHex());
}
bool validCommand(const QString &command) {
    if (command.isEmpty() || command.size() > 64 || command[0] < 'a' || command[0] > 'z') return false;
    for (QChar c : command) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') continue;
        return false;
    }
    return true;
}
bool validRequest(const QJsonObject &request) {
    if (!request.value(QStringLiteral("command")).isString() ||
        !validCommand(request.value(QStringLiteral("command")).toString())) return false;
    for (auto it = request.begin(); it != request.end(); ++it) {
        if (it.key() == QLatin1String("command")) continue;
        if (it.key() == QLatin1String("args") && it.value().isObject()) continue;
        if (it.key() == QLatin1String("id") &&
            ((it.value().isString() && it.value().toString().size() <= 128) ||
             (it.value().isDouble() && std::isfinite(it.value().toDouble())))) continue;
        if (it.key() == QLatin1String("targetInstanceId") && it.value().isString() &&
            it.value().toString().size() <= 128) continue;
        return false;
    }
    return true;
}

bool spend(qint64 &remaining, qint64 size) {
    if (size < 0 || size > remaining) return false;
    remaining -= size;
    return true;
}
bool stringFits(const QString &value, qint64 &remaining) {
    if (!spend(remaining, 2)) return false;
    for (int i = 0; i < value.size(); ++i) {
        const ushort c = value[i].unicode();
        int bytes = 1;
        if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') bytes = 2;
        else if (c < 0x20) bytes = 6;
        else if (c < 0x80) bytes = 1;
        else if (c < 0x800) bytes = 2;
        else if (QChar::isHighSurrogate(c) && i + 1 < value.size() && QChar::isLowSurrogate(value[i + 1].unicode())) {
            bytes = 4;
            ++i;
        } else if (QChar::isSurrogate(c)) bytes = 6;
        else bytes = 3;
        if (!spend(remaining, bytes)) return false;
    }
    return true;
}
// Bound serialization before allocating a JSON byte buffer. Numbers reserve
// their worst-case width; large/deep replies fail before building JSON bytes.
bool jsonFits(const QJsonValue &value, qint64 &remaining, int depth = 0) {
    if (depth > MaxJsonDepth) return false;
    if (value.isString()) return stringFits(value.toString(), remaining);
    if (value.isDouble()) return spend(remaining, 32);
    if (value.isBool()) return spend(remaining, value.toBool() ? 4 : 5);
    if (value.isArray()) {
        if (!spend(remaining, 2)) return false;
        const auto array = value.toArray();
        bool first = true;
        for (const auto &element : array) {
            if ((!first && !spend(remaining, 1)) || !jsonFits(element, remaining, depth + 1)) return false;
            first = false;
        }
        return true;
    }
    if (value.isObject()) {
        if (!spend(remaining, 2)) return false;
        const auto object = value.toObject();
        bool first = true;
        for (auto it = object.begin(); it != object.end(); ++it) {
            if ((!first && !spend(remaining, 1)) || !stringFits(it.key(), remaining) ||
                !spend(remaining, 1) || !jsonFits(it.value(), remaining, depth + 1)) return false;
            first = false;
        }
        return true;
    }
    return spend(remaining, 4);
}
} // namespace

CommandControlServer::CommandControlServer(CommandDispatcher *dispatcher, QObject *parent)
    : QObject(parent), dispatcher_(dispatcher) {
}

CommandControlServer::~CommandControlServer() {
    stop();
}

bool CommandControlServer::start(quint16 requestedPort, QString *error) {
    if (error) error->clear();
    if (QThread::currentThread() != thread() || !dispatcher_ || isListening()) {
        if (error) *error = QStringLiteral("Command control requires its owning thread, a dispatcher, and a stopped server");
        return false;
    }
    auto *listener = new QTcpServer(this);
    listener->setMaxPendingConnections(MaxClients);
    if (!listener->listen(QHostAddress::LocalHost, requestedPort)) {
        if (error) *error = listener->errorString();
        delete listener;
        return false;
    }
    token_ = freshToken();
    server_ = listener;
    ++generation_;
    requests_ = authFailures_ = rejected_ = 0;
    rateHead_ = rateCount_ = 0;
    clock_.start();
    connect(listener, &QTcpServer::newConnection, this, &CommandControlServer::acceptConnections);
    emit statusChanged();
    return true;
}

void CommandControlServer::stop() {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool changed = server_ || !clients_.isEmpty() || !token_.isEmpty();
    ++generation_;
    auto *listener = server_;
    server_ = nullptr;
    token_.clear();
    if (listener) {
        listener->disconnect(this);
        listener->close();
        listener->deleteLater();
    }
    const auto clients = std::move(clients_);
    clients_.clear();
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        it.value().timer->stop();
        it.value().timer->disconnect(this);
        it.key()->disconnect(this);
        it.key()->abort();
        it.key()->deleteLater();
    }
    rateHead_ = rateCount_ = 0;
    if (changed) emit statusChanged();
}

bool CommandControlServer::isListening() const { return server_ && server_->isListening(); }
quint16 CommandControlServer::port() const { return isListening() ? server_->serverPort() : 0; }
QString CommandControlServer::token() const { return token_; }
bool CommandControlServer::allowControl() const { return allowControl_; }

void CommandControlServer::setAllowControl(bool allow) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (allowControl_ == allow) return;
    allowControl_ = allow;
    emit statusChanged();
}

QJsonObject CommandControlServer::status() const {
    return {{"schemaVersion", 1}, {"transport", "obrii-jsonl-v1"}, {"mcp", false},
        {"listening", isListening()}, {"address", "127.0.0.1"}, {"port", int(port())},
        {"instanceId", dispatcher_ ? dispatcher_->instanceId() : QString()},
        {"allowControl", allowControl_}, {"allowFiles", false}, {"allowTransmit", false},
        {"clients", clients_.size()}, {"maxClients", MaxClients},
        {"maxRequestBytes", MaxRequestBytes}, {"maxResponseBytes", MaxResponseBytes},
        {"timeoutMs", TimeoutMs}, {"maxCommandsPerSecond", int(commandTimes_.size())},
        {"requests", double(requests_)}, {"authFailures", double(authFailures_)}, {"rejected", double(rejected_)}};
}

bool CommandControlServer::authentic(const QString &supplied) const {
    if (token_.size() != 64) return false;
    volatile quint32 difference = quint32(supplied.size()) ^ 64U;
    for (int i = 0; i < 64; ++i) {
        const ushort value = i < supplied.size() ? supplied[i].unicode() : 0;
        difference |= quint32(value ^ token_[i].unicode());
    }
    return difference == 0;
}

bool CommandControlServer::admitCommand() {
    const qint64 now = clock_.elapsed();
    while (rateCount_ && now - commandTimes_[rateHead_] >= 1000) {
        rateHead_ = (rateHead_ + 1) % commandTimes_.size();
        --rateCount_;
    }
    if (rateCount_ == commandTimes_.size()) return false;
    commandTimes_[(rateHead_ + rateCount_) % commandTimes_.size()] = now;
    ++rateCount_;
    return true;
}

void CommandControlServer::acceptConnections() {
    QPointer<CommandControlServer> alive(this);
    const quint64 generation = generation_;
    while (server_ && server_->hasPendingConnections()) {
        auto *socket = server_->nextPendingConnection();
        if (!socket) continue;
        socket->setParent(this);
        socket->setReadBufferSize(MaxRequestBytes + 1);
        if (clients_.size() >= MaxClients || !socket->peerAddress().isLoopback() ||
            socket->peerAddress().protocol() != QAbstractSocket::IPv4Protocol) {
            increment(rejected_);
            socket->abort();
            socket->deleteLater();
            emit statusChanged();
            if (!alive || generation != generation_) return;
            continue;
        }
        Client client;
        client.input.reserve(MaxRequestBytes + 1);
        client.deadlineMs = clock_.elapsed() + TimeoutMs;
        client.timer = new QTimer(socket);
        client.timer->setSingleShot(true);
        client.timer->setTimerType(Qt::PreciseTimer);
        connect(client.timer, &QTimer::timeout, this, [this, socket] { expireClient(socket); });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readClient(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { removeClient(socket); });
        connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) { removeClient(socket); });
        clients_.insert(socket, client);
        client.timer->start(TimeoutMs);
        emit statusChanged();
        if (!alive || generation != generation_) return;
        if (clients_.contains(socket) && socket->bytesAvailable()) readClient(socket);
        if (!alive || generation != generation_) return;
    }
}

void CommandControlServer::readClient(QTcpSocket *socket) {
    auto it = clients_.find(socket);
    if (it == clients_.end() || it->responding || it->processing) return;
    if (clock_.elapsed() >= it->deadlineMs) { expireClient(socket); return; }
    auto &input = it->input;
    const int before = input.size();
    const qint64 take = (std::min)(socket->bytesAvailable(), qint64(MaxRequestBytes + 1 - before));
    input.resize(before + int(take));
    const qint64 got = socket->read(input.data() + before, take);
    if (got < 0) { removeClient(socket); return; }
    input.resize(before + int(got));
    const int newline = input.indexOf('\n');
    if (newline < 0) {
        if (input.size() > MaxRequestBytes) reject(socket, "request_too_large", "Request exceeds 64 KiB");
        return;
    }
    if (newline > MaxRequestBytes) { reject(socket, "request_too_large", "Request exceeds 64 KiB"); return; }
    if (newline + 1 != input.size() || socket->bytesAvailable()) {
        reject(socket, "invalid_envelope", "Send exactly one JSON line per connection");
        return;
    }
    input.chop(1);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(input, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        reject(socket, "invalid_json", "Expected one JSON object"); return;
    }
    const auto envelope = document.object();
    if (envelope.size() != 2 || !envelope.contains("token") || !envelope.contains("request")) {
        reject(socket, "invalid_envelope", "Expected only token and request fields"); return;
    }
    if (!envelope.value("token").isString() || !authentic(envelope.value("token").toString())) {
        increment(authFailures_);
        reject(socket, "unauthorized", "Authentication failed"); return;
    }
    if (!envelope.value("request").isObject() || !validRequest(envelope.value("request").toObject())) {
        reject(socket, "invalid_request", "Invalid command request fields or types"); return;
    }
    if (dispatching_) { reject(socket, "busy", "A command is already executing"); return; }
    const auto request = envelope.value("request").toObject();
    const quint64 generation = generation_;
    const bool controlAllowed = allowControl_;
    QPointer<QTcpSocket> socketAlive(socket);
    it->processing = true;
    // A handler may pump nested events. Never run it on the native socket's
    // readyRead stack: timeout/stop can then delete that active Qt emitter.
    QMetaObject::invokeMethod(this, [this, socketAlive, request, controlAllowed, generation] {
        if (socketAlive && generation == generation_)
            dispatchRequest(socketAlive, request, controlAllowed, generation);
    }, Qt::QueuedConnection);
}

void CommandControlServer::dispatchRequest(QTcpSocket *socket, const QJsonObject &request,
                                           bool controlAllowed, quint64 generation) {
    auto it = clients_.find(socket);
    if (it == clients_.end() || it->responding || generation != generation_) return;
    if (clock_.elapsed() >= it->deadlineMs) { expireClient(socket); return; }
    if (dispatching_) { reject(socket, "busy", "A command is already executing"); return; }
    if (!admitCommand()) { reject(socket, "rate_limited", "Command rate limit reached"); return; }
    const CommandDispatcher::Context context{QStringLiteral("agent"), controlAllowed && allowControl_, false, false};
    QPointer<CommandControlServer> alive(this);
    QPointer<QTcpSocket> socketAlive(socket);
    dispatching_ = true;
    increment(requests_);
    const auto response = dispatcher_->execute(request, context);
    if (!alive) return;
    dispatching_ = false;
    if (!socketAlive || generation != generation_) return;
    it = clients_.find(socket);
    if (it == clients_.end() || it->responding) return;
    if (clock_.elapsed() >= it->deadlineMs) { expireClient(socket); return; }
    if (!response.value("ok").toBool()) markRejected(socket);
    respond(socket, response);
}

void CommandControlServer::expireClient(QTcpSocket *socket) {
    const auto it = clients_.constFind(socket);
    if (it == clients_.constEnd()) return;
    if (it->responding) {
        markRejected(socket);
        removeClient(socket);
    } else {
        const quint64 generation = generation_;
        QPointer<CommandControlServer> alive(this);
        QPointer<QTcpSocket> socketAlive(socket);
        reject(socket, "timeout", "Request deadline exceeded");
        if (alive && socketAlive && generation == generation_) {
            const auto pending = clients_.constFind(socket);
            if (pending != clients_.constEnd()) pending->timer->start(1);
        }
    }
}

void CommandControlServer::reject(QTcpSocket *socket, const QString &code, const QString &message) {
    if (!clients_.contains(socket)) return;
    markRejected(socket);
    respond(socket, failure(code, message));
}

void CommandControlServer::markRejected(QTcpSocket *socket) {
    auto it = clients_.find(socket);
    if (it == clients_.end() || it->rejected) return;
    it->rejected = true;
    increment(rejected_);
}

void CommandControlServer::respond(QTcpSocket *socket, const QJsonObject &response) {
    auto it = clients_.find(socket);
    if (it == clients_.end() || it->responding) return;
    qint64 remaining = MaxResponseBytes - 1;
    QByteArray bytes;
    if (jsonFits(response, remaining)) bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
    if (bytes.isEmpty() || bytes.size() >= MaxResponseBytes) {
        markRejected(socket);
        bytes = QJsonDocument(failure("response_too_large", "Response exceeds transport size or depth limits"))
                    .toJson(QJsonDocument::Compact);
    }
    bytes.append('\n');
    it->responding = true;
    it->input.clear();
    QPointer<CommandControlServer> alive(this);
    QPointer<QTcpSocket> socketAlive(socket);
    if (socket->bytesToWrite() || socket->write(bytes) != bytes.size()) {
        if (alive && socketAlive) {
            markRejected(socket);
            removeClient(socket);
        }
        return;
    }
    if (alive && socketAlive && clients_.contains(socket)) socket->disconnectFromHost();
}

void CommandControlServer::removeClient(QTcpSocket *socket) {
    const auto it = clients_.find(socket);
    if (it == clients_.end()) return;
    auto *timer = it->timer;
    clients_.erase(it);
    timer->stop();
    timer->disconnect(this);
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    emit statusChanged();
}
