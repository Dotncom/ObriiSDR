#include "networkcontroller.h"

#include <algorithm>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonValue>
#include <QVariant>
#include <utility>

namespace {
constexpr int CONTROL_HANDSHAKE_TIMEOUT_MS = 3000;
constexpr int CONTROL_HEARTBEAT_INTERVAL_MS = 1000;
constexpr int CONTROL_HEARTBEAT_TIMEOUT_MS = 5000;
const QByteArray HELLO_MESSAGE = "FOBOSAPP_HELLO 1\n";
const QByteArray OK_MESSAGE = "FOBOSAPP_OK 1\n";
const QByteArray PING_MESSAGE = "FOBOSAPP_PING 1\n";
const QByteArray PONG_MESSAGE = "FOBOSAPP_PONG 1\n";
}

NetworkController::NetworkController(QObject *parent)
    : QObject(parent),
      server(new QTcpServer(this)),
      handshakeTimer(new QTimer(this)),
      heartbeatTimer(new QTimer(this)) {
    handshakeTimer->setSingleShot(true);
    heartbeatTimer->setInterval(CONTROL_HEARTBEAT_INTERVAL_MS);

    connect(server, &QTcpServer::newConnection, this, [this]() {
        while (server->hasPendingConnections()) {
            QTcpSocket *socket = server->nextPendingConnection();
            if (!socket) {
                continue;
            }

            const QString id = QString("client-%1").arg(nextPeerId++);
            const QString label = QString("%1:%2")
                                      .arg(socket->peerAddress().toString())
                                      .arg(socket->peerPort());
            const QString priorityKey = socket->peerAddress().toString();

            peerSockets.append(socket);
            peerIds.insert(socket, id);
            peerLabels.insert(socket, label);
            peerPriorityKeys.insert(socket, priorityKey);
            peerStreamProfiles.insert(socket, QStringLiteral("legacy"));
            peerReadBuffers.insert(socket, QByteArray());
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

            setStatus(QString("Client connected from %1").arg(label));

            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                if (!peerSockets.contains(socket)) {
                    return;
                }
                if (processingPeerReads.contains(socket)) {
                    peerReadBuffers[socket].append(socket->readAll());
                    return;
                }
                processingPeerReads.insert(socket);
                QByteArray buffer = peerReadBuffers.take(socket);
                while (true) {
                    processSocketData(socket, buffer);
                    if (!peerSockets.contains(socket)) break;
                    const QByteArray reentrantBytes = peerReadBuffers.take(socket);
                    if (reentrantBytes.isEmpty()) break;
                    buffer.append(reentrantBytes);
                }
                if (peerSockets.contains(socket))
                    peerReadBuffers.insert(socket, std::move(buffer));
                processingPeerReads.remove(socket);
            });

            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                const QString label = peerLabel(socket);
                const bool wasController = socket == peerSocket;
                closePeerSocket(socket);
                promoteControllerIfNeeded();
                if (currentMode == NetworkMode::Server && server->isListening()) {
                    const QString message = QString("%1 disconnected%2; server listening on %3:%4")
                                                .arg(label)
                                                .arg(wasController ? QStringLiteral(" (controller)") : QString())
                                                .arg(server->serverAddress().toString())
                                                .arg(server->serverPort());
                    setStatus(message);
                    emit channelError(message);
                }
            });

            connect(socket,
                    &QTcpSocket::errorOccurred,
                    this,
                    [this, socket](QAbstractSocket::SocketError) {
                        const QString message = QString("Client %1 socket error: %2")
                                                    .arg(peerLabel(socket))
                                                    .arg(socket ? socket->errorString() : QString("socket closed"));
                        closePeerSocket(socket);
                        promoteControllerIfNeeded();
                        setStatus(message);
                        emit channelError(message);
                    });
        }
    });

    connect(handshakeTimer, &QTimer::timeout, this, [this]() {
        if (currentMode == NetworkMode::Client &&
            clientSocket &&
            !controlReady) {
            const QString message = "Control channel timeout";
            closeClientSocket();
            setStatus(message);
            emit channelError(message);
        }
    });

    connect(heartbeatTimer, &QTimer::timeout, this, [this]() {
        if (currentMode == NetworkMode::Client) {
            if (!controlReady) {
                stopHeartbeat();
                return;
            }

            QTcpSocket *socket = activeSocket();
            if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
                handleConnectionLost("Control channel lost");
                return;
            }

            if (lastMessageTimer.isValid() &&
                lastMessageTimer.elapsed() > CONTROL_HEARTBEAT_TIMEOUT_MS) {
                handleConnectionLost("Control channel heartbeat timeout");
                return;
            }

            sendProtocolLine(socket, PING_MESSAGE);
            return;
        }

        if (currentMode != NetworkMode::Server) {
            stopHeartbeat();
            return;
        }

        const QVector<QTcpSocket*> sockets = peerSockets;
        for (QTcpSocket *socket : sockets) {
            if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
                continue;
            }
            QElapsedTimer &timer = peerLastMessageTimers[socket];
            if (timer.isValid() && timer.elapsed() > CONTROL_HEARTBEAT_TIMEOUT_MS) {
                const QString message = QString("Client %1 heartbeat timeout").arg(peerLabel(socket));
                closePeerSocket(socket);
                promoteControllerIfNeeded();
                setStatus(message);
                emit channelError(message);
                return;
            }
            sendProtocolLine(socket, PING_MESSAGE);
        }
    });
}

NetworkController::~NetworkController() {
    stop();
}

NetworkMode NetworkController::mode() const {
    return currentMode;
}

QString NetworkController::statusText() const {
    return currentStatus;
}

bool NetworkController::isControlReady() const {
    if (currentMode == NetworkMode::Server) {
        return !readyPeerSockets.isEmpty();
    }
    return controlReady;
}

qint64 NetworkController::pendingBytes() const {
    if (currentMode == NetworkMode::Client) {
        return clientSocket ? clientSocket->bytesToWrite() : 0;
    }

    qint64 maxPending = 0;
    if (currentMode == NetworkMode::Server) {
        for (QTcpSocket *socket : readyPeerSockets) {
            if (socket && acceptsLegacyStream(socket)) {
                maxPending = (std::max)(maxPending, socket->bytesToWrite());
            }
        }
    }
    return maxPending;
}

bool NetworkController::clientHasControl() const {
    return currentMode != NetworkMode::Client || localClientHasControl;
}

QString NetworkController::controllerPeerId() const {
    return peerId(peerSocket);
}

quint16 NetworkController::listeningPort() const {
    return server && server->isListening() ? server->serverPort() : 0;
}

QStringList NetworkController::readyPeerIds() const {
    QStringList result;
    for (QTcpSocket *socket : peerSockets) {
        if (isReadyPeer(socket)) result.append(peerId(socket));
    }
    return result;
}

QString NetworkController::peerLabelForId(const QString &peerIdValue) const {
    return peerLabel(peerForId(peerIdValue));
}

qint64 NetworkController::pendingBytesForPeer(const QString &peerIdValue) const {
    QTcpSocket *socket = peerForId(peerIdValue);
    return isReadyPeer(socket) ? socket->bytesToWrite() : -1;
}

bool NetworkController::peerSupportsCompactSpectrum(const QString &peerIdValue) const {
    return compactSpectrumPeers.contains(peerForId(peerIdValue));
}

bool NetworkController::peerSupportsPackedSpectrum(const QString &peerIdValue) const {
    return packedSpectrumPeers.contains(peerForId(peerIdValue));
}

bool NetworkController::peerSupportsBinaryAudio(const QString &peerIdValue) const {
    return binaryAudioPeers.contains(peerForId(peerIdValue));
}

bool NetworkController::peerSupportsUhdRxSpectrum(const QString &peerIdValue) const {
    return uhdRxSpectrumPeers.contains(peerForId(peerIdValue));
}

void NetworkController::stop() {
    handshakeTimer->stop();
    stopHeartbeat();
    controlReady = false;
    localClientHasControl = true;
    closeClientSocket();
    closePeerSocket();
    if (server->isListening()) {
        server->close();
    }
    currentMode = NetworkMode::Disabled;
    setStatus("Network disabled");
}

void NetworkController::startServer(const QString &bindAddress, quint16 port) {
    handshakeTimer->stop();
    stopHeartbeat();
    controlReady = false;
    localClientHasControl = true;
    closeClientSocket();
    closePeerSocket();
    if (server->isListening()) {
        server->close();
    }

    const QHostAddress address(bindAddress.trimmed().isEmpty() ? QStringLiteral("0.0.0.0") : bindAddress.trimmed());
    if (!server->listen(address, port)) {
        currentMode = NetworkMode::Disabled;
        const QString message = QString("Server listen failed: %1").arg(server->errorString());
        setStatus(message);
        emit channelError(message);
        return;
    }

    currentMode = NetworkMode::Server;
    setStatus(QString("Server listening on %1:%2")
                  .arg(server->serverAddress().toString())
                  .arg(server->serverPort()));
}

void NetworkController::testClientConnection(const QString &serverAddress, quint16 port) {
    handshakeTimer->stop();
    stopHeartbeat();
    if (server->isListening()) {
        server->close();
    }
    closeClientSocket();
    closePeerSocket();
    controlReady = false;
    localClientHasControl = true;

    clientSocket = new QTcpSocket(this);
    clientSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    currentMode = NetworkMode::Client;
    setStatus(QString("Connecting to %1:%2").arg(serverAddress).arg(port));

    connect(clientSocket, &QTcpSocket::connected, this, [this]() {
        sendHello();
        setStatus("Connected, waiting for server handshake");
    });

    connect(clientSocket, &QTcpSocket::readyRead, this, [this]() {
        QTcpSocket *socket = clientSocket;
        if (!socket) return;
        if (processingClientRead) {
            clientReadBuffer.append(socket->readAll());
            return;
        }
        processingClientRead = true;
        QByteArray buffer = std::move(clientReadBuffer);
        while (true) {
            processSocketData(socket, buffer);
            if (clientSocket != socket) break;
            const QByteArray reentrantBytes = std::move(clientReadBuffer);
            clientReadBuffer.clear();
            if (reentrantBytes.isEmpty()) break;
            buffer.append(reentrantBytes);
        }
        if (clientSocket == socket) clientReadBuffer = std::move(buffer);
        processingClientRead = false;
    });

    connect(clientSocket, &QTcpSocket::disconnected, this, [this]() {
        handshakeTimer->stop();
        stopHeartbeat();
        controlReady = false;
        closeClientSocket();
        if (currentMode == NetworkMode::Client) {
            const QString message = "Disconnected from server";
            setStatus(message);
            emit channelError(message);
        }
    });

    connect(clientSocket,
            &QTcpSocket::errorOccurred,
            this,
            [this](QAbstractSocket::SocketError) {
                handshakeTimer->stop();
                stopHeartbeat();
                const QString message = QString("Client connection failed: %1")
                                            .arg(clientSocket ? clientSocket->errorString() : QString("socket closed"));
                controlReady = false;
                closeClientSocket();
                currentMode = NetworkMode::Disabled;
                setStatus(message);
                emit channelError(message);
            });

    clientSocket->connectToHost(serverAddress, port);
    handshakeTimer->start(CONTROL_HANDSHAKE_TIMEOUT_MS);
}

bool NetworkController::sendControlCommand(const QJsonObject &command) {
    if (currentMode == NetworkMode::Client) {
        QTcpSocket *socket = activeSocket();
        if (!socket || socket->state() != QAbstractSocket::ConnectedState || !controlReady) {
            setStatus("Control channel is not ready");
            return false;
        }
        return sendJsonToSocket(socket, command);
    }

    if (currentMode != NetworkMode::Server || readyPeerSockets.isEmpty()) {
        setStatus("Control channel is not ready");
        return false;
    }

    bool sentAny = false;
    const QVector<QTcpSocket*> sockets = peerSockets;
    for (QTcpSocket *socket : sockets) {
        if (!isReadyPeer(socket)) {
            continue;
        }
        const QString type = command.value(QStringLiteral("type")).toString();
        if ((type == QStringLiteral("audio") || type == QStringLiteral("spectrum") ||
             type == QStringLiteral("spectrumbin") || type == QStringLiteral("iq") ||
             type == QStringLiteral("iqbin")) && !acceptsLegacyStream(socket)) {
            continue;
        }
        sentAny = sendJsonToSocket(socket, command) || sentAny;
    }
    return sentAny;
}

bool NetworkController::sendBinaryCommand(const QJsonObject &command, const QByteArray &payload) {
    if (payload.isEmpty()) {
        return sendControlCommand(command);
    }

    if (currentMode == NetworkMode::Client) {
        QTcpSocket *socket = activeSocket();
        if (!socket || socket->state() != QAbstractSocket::ConnectedState || !controlReady) {
            setStatus("Control channel is not ready");
            return false;
        }
        return sendBinaryToSocket(socket, command, payload);
    }

    if (currentMode != NetworkMode::Server || readyPeerSockets.isEmpty()) {
        setStatus("Control channel is not ready");
        return false;
    }

    bool sentAny = false;
    const QVector<QTcpSocket*> sockets = peerSockets;
    for (QTcpSocket *socket : sockets) {
        if (!isReadyPeer(socket)) {
            continue;
        }
        const QString type = command.value(QStringLiteral("type")).toString();
        if ((type == QStringLiteral("audio") || type == QStringLiteral("spectrum") ||
             type == QStringLiteral("spectrumbin") || type == QStringLiteral("iq") ||
             type == QStringLiteral("iqbin")) && !acceptsLegacyStream(socket)) {
            continue;
        }
        sentAny = sendBinaryToSocket(socket, command, payload) || sentAny;
    }
    return sentAny;
}

bool NetworkController::sendControlCommandToPeer(const QString &peerIdValue, const QJsonObject &command) {
    QTcpSocket *socket = peerForId(peerIdValue);
    if (!isReadyPeer(socket)) {
        return false;
    }
    return sendJsonToSocket(socket, command);
}

bool NetworkController::sendBinaryCommandToPeer(const QString &peerIdValue,
                                                const QJsonObject &command,
                                                const QByteArray &payload) {
    QTcpSocket *socket = peerForId(peerIdValue);
    if (!isReadyPeer(socket)) return false;
    return sendBinaryToSocket(socket, command, payload);
}

bool NetworkController::sendControlCommandToController(const QJsonObject &command) {
    if (!isReadyPeer(peerSocket)) {
        return false;
    }
    return sendJsonToSocket(peerSocket, command);
}

bool NetworkController::setPeerStreamProfile(const QString &peerIdValue,
                                             const QString &profile) {
    QTcpSocket *socket = peerForId(peerIdValue);
    if (!isReadyPeer(socket)) return false;
    const QString normalized = profile.trimmed().toLower();
    peerStreamProfiles.insert(socket,
                              normalized == QStringLiteral("remote-vfo")
                                  ? normalized
                                  : QStringLiteral("legacy"));
    return true;
}

QString NetworkController::peerStreamProfile(const QString &peerIdValue) const {
    QTcpSocket *socket = peerForId(peerIdValue);
    return socket ? peerStreamProfiles.value(socket, QStringLiteral("legacy")) : QString();
}

bool NetworkController::setControllerPeer(const QString &peerIdValue) {
    QTcpSocket *socket = peerForId(peerIdValue);
    if (!isReadyPeer(socket)) {
        return false;
    }
    peerSocket = socket;
    broadcastRoleUpdates();
    setStatus(QString("Controller client: %1").arg(peerLabel(peerSocket)));
    return true;
}

bool NetworkController::blockPriorityRequestsFromPeer(const QString &peerIdValue) {
    QTcpSocket *socket = peerForId(peerIdValue);
    if (!socket) {
        return false;
    }
    blockedPriorityRequesterKeys.insert(peerPriorityKey(socket));
    return true;
}

bool NetworkController::isPriorityRequestBlocked(const QString &peerIdValue) const {
    QTcpSocket *socket = peerForId(peerIdValue);
    return socket && blockedPriorityRequesterKeys.contains(peerPriorityKey(socket));
}

void NetworkController::setStatus(const QString &status) {
    if (currentStatus == status) {
        return;
    }
    currentStatus = status;
    emit statusChanged(status);
}

void NetworkController::closeClientSocket() {
    if (!clientSocket) {
        return;
    }
    clientSocket->disconnect(this);
    clientSocket->disconnectFromHost();
    clientSocket->deleteLater();
    clientSocket = nullptr;
    clientReadBuffer.clear();
    clientPendingBinaryCommand = QJsonObject();
    clientPendingBinaryBytes = 0;
}

void NetworkController::closePeerSocket(QTcpSocket *socket) {
    auto closeOne = [this](QTcpSocket *peer) {
        if (!peer) {
            return;
        }
        const bool wasReady = readyPeerSockets.contains(peer);
        const QString disconnectedId = peerId(peer);
        const QString disconnectedLabel = peerLabel(peer);
        readyPeerSockets.remove(peer);
        peerReadBuffers.remove(peer);
        peerPendingBinaryCommands.remove(peer);
        peerPendingBinaryBytes.remove(peer);
        peerIds.remove(peer);
        peerLabels.remove(peer);
        peerPriorityKeys.remove(peer);
        peerStreamProfiles.remove(peer);
        compactSpectrumPeers.remove(peer);
        packedSpectrumPeers.remove(peer);
        binaryAudioPeers.remove(peer);
        uhdRxSpectrumPeers.remove(peer);
        peerLastMessageTimers.remove(peer);
        processingPeerReads.remove(peer);
        peerSockets.removeAll(peer);
        if (peerSocket == peer) {
            peerSocket = nullptr;
        }
        peer->disconnect(this);
        peer->disconnectFromHost();
        peer->deleteLater();
        if (wasReady && !disconnectedId.isEmpty()) {
            emit peerDisconnected(disconnectedId, disconnectedLabel);
        }
    };

    if (socket) {
        closeOne(socket);
        return;
    }

    const QVector<QTcpSocket*> sockets = peerSockets;
    for (QTcpSocket *peer : sockets) {
        closeOne(peer);
    }
    peerSockets.clear();
    readyPeerSockets.clear();
    peerReadBuffers.clear();
    peerPendingBinaryCommands.clear();
    peerPendingBinaryBytes.clear();
    peerIds.clear();
    peerLabels.clear();
    peerPriorityKeys.clear();
    peerStreamProfiles.clear();
    compactSpectrumPeers.clear();
    packedSpectrumPeers.clear();
    binaryAudioPeers.clear();
    uhdRxSpectrumPeers.clear();
    peerLastMessageTimers.clear();
    processingPeerReads.clear();
    peerSocket = nullptr;
}

void NetworkController::sendHello() {
    if (!clientSocket) {
        return;
    }
    sendProtocolLine(clientSocket, HELLO_MESSAGE);
}

void NetworkController::startHeartbeat() {
    if (currentMode == NetworkMode::Client) {
        lastMessageTimer.restart();
    }
    if (!heartbeatTimer->isActive()) {
        heartbeatTimer->start();
    }
}

void NetworkController::stopHeartbeat() {
    if (heartbeatTimer->isActive()) {
        heartbeatTimer->stop();
    }
    lastMessageTimer.invalidate();
    peerLastMessageTimers.clear();
}

void NetworkController::sendProtocolLine(QTcpSocket *socket, const QByteArray &line) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    socket->write(line);
    socket->flush();
}

QTcpSocket *NetworkController::activeSocket() const {
    if (currentMode == NetworkMode::Client) {
        return clientSocket;
    }
    if (currentMode == NetworkMode::Server) {
        return peerSocket;
    }
    return nullptr;
}

bool NetworkController::sendJsonToSocket(QTcpSocket *socket, const QJsonObject &command) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }
    const QByteArray payload = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
    const qint64 written = socket->write(payload);
    if (written != payload.size()) {
        const QString message = QString("Control channel write failed: %1").arg(socket->errorString());
        if (currentMode == NetworkMode::Server && peerSockets.contains(socket)) {
            closePeerSocket(socket);
            promoteControllerIfNeeded();
            setStatus(message);
            emit channelError(message);
        } else {
            handleConnectionLost(message);
        }
        return false;
    }
    return true;
}

bool NetworkController::sendBinaryToSocket(QTcpSocket *socket, const QJsonObject &command, const QByteArray &payload) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    QJsonObject header = command;
    header["payloadBytes"] = QString::number(payload.size());
    const QByteArray headerBytes = QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n';
    const qint64 headerWritten = socket->write(headerBytes);
    const qint64 payloadWritten = socket->write(payload);
    if (headerWritten != headerBytes.size() || payloadWritten != payload.size()) {
        const QString message = QString("Binary channel write failed: %1").arg(socket->errorString());
        if (currentMode == NetworkMode::Server && peerSockets.contains(socket)) {
            closePeerSocket(socket);
            promoteControllerIfNeeded();
            setStatus(message);
            emit channelError(message);
        } else {
            handleConnectionLost(message);
        }
        return false;
    }
    return true;
}

QTcpSocket *NetworkController::peerForId(const QString &peerIdValue) const {
    for (QTcpSocket *socket : peerSockets) {
        if (peerIds.value(socket) == peerIdValue) {
            return socket;
        }
    }
    return nullptr;
}

QString NetworkController::peerId(QTcpSocket *socket) const {
    return peerIds.value(socket);
}

QString NetworkController::peerLabel(QTcpSocket *socket) const {
    return peerLabels.value(socket, QString("unknown client"));
}

QString NetworkController::peerPriorityKey(QTcpSocket *socket) const {
    return peerPriorityKeys.value(socket, peerLabel(socket));
}

bool NetworkController::isReadyPeer(QTcpSocket *socket) const {
    return socket &&
           socket->state() == QAbstractSocket::ConnectedState &&
           readyPeerSockets.contains(socket);
}

bool NetworkController::acceptsLegacyStream(QTcpSocket *socket) const {
    return peerStreamProfiles.value(socket, QStringLiteral("legacy")) !=
           QStringLiteral("remote-vfo");
}

void NetworkController::promoteControllerIfNeeded() {
    if (isReadyPeer(peerSocket)) {
        return;
    }

    peerSocket = nullptr;
    if (!readyPeerSockets.isEmpty()) {
        // Losing the controller must not silently grant hardware control to an
        // arbitrary observer.  Existing peers remain observers until one sends
        // an explicit requestPriority command.
        broadcastRoleUpdates();
    } else if (currentMode == NetworkMode::Server) {
        controlReady = false;
        if (heartbeatTimer->isActive()) {
            heartbeatTimer->stop();
        }
    }
}

void NetworkController::sendRoleUpdate(QTcpSocket *socket) {
    if (!isReadyPeer(socket)) {
        return;
    }

    QJsonObject role;
    role["type"] = "control";
    role["action"] = "role";
    role["peerId"] = peerId(socket);
    role["peerLabel"] = peerLabel(socket);
    role["controllerPeerId"] = controllerPeerId();
    role["canControl"] = socket == peerSocket;
    sendJsonToSocket(socket, role);
}

void NetworkController::broadcastRoleUpdates() {
    const QVector<QTcpSocket*> sockets = peerSockets;
    for (QTcpSocket *socket : sockets) {
        sendRoleUpdate(socket);
    }
}

void NetworkController::handleConnectionLost(const QString &message) {
    handshakeTimer->stop();

    if (currentMode == NetworkMode::Client) {
        stopHeartbeat();
        controlReady = false;
        closeClientSocket();
        setStatus(message);
        emit channelError(message);
        return;
    }

    if (currentMode == NetworkMode::Server) {
        QTcpSocket *senderSocket = qobject_cast<QTcpSocket*>(sender());
        if (senderSocket && peerSockets.contains(senderSocket)) {
            closePeerSocket(senderSocket);
        } else if (peerSocket) {
            closePeerSocket(peerSocket);
        }
        promoteControllerIfNeeded();
        if (server->isListening()) {
            const QString status = QString("%1; server listening on %2:%3")
                                       .arg(message)
                                       .arg(server->serverAddress().toString())
                                       .arg(server->serverPort());
            setStatus(status);
            emit channelError(status);
        } else {
            setStatus(message);
            emit channelError(message);
        }
    }
}

void NetworkController::processSocketData(QTcpSocket *socket, QByteArray &buffer) {
    if (!socket) {
        return;
    }

    const auto socketStillOwned = [this, socket]() {
        return (currentMode == NetworkMode::Client && socket == clientSocket) ||
               (currentMode == NetworkMode::Server && peerSockets.contains(socket));
    };
    if (!socketStillOwned()) return;

    buffer += socket->readAll();
    int newlineIndex = -1;
    while (true) {
        if (deliverPendingBinaryPayload(socket, buffer)) {
            if (!socketStillOwned()) return;
            continue;
        }
        const qint64 pendingBinaryBytes =
            (currentMode == NetworkMode::Client && socket == clientSocket)
                ? clientPendingBinaryBytes
                : peerPendingBinaryBytes.value(socket, 0);
        if (pendingBinaryBytes > 0) {
            break;
        }

        newlineIndex = buffer.indexOf('\n');
        if (newlineIndex < 0) {
            break;
        }
        QByteArray line = buffer.left(newlineIndex).trimmed();
        buffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) {
            if (currentMode == NetworkMode::Client) {
                lastMessageTimer.restart();
            } else if (currentMode == NetworkMode::Server) {
                peerLastMessageTimers[socket].restart();
            }
            processLine(socket, line);
            if (!socketStillOwned()) return;
        }
    }
}

bool NetworkController::deliverPendingBinaryPayload(QTcpSocket *socket, QByteArray &buffer) {
    if (!socket) {
        return false;
    }

    qint64 payloadBytes = 0;
    QJsonObject command;
    if (currentMode == NetworkMode::Client && socket == clientSocket) {
        payloadBytes = clientPendingBinaryBytes;
        command = clientPendingBinaryCommand;
    } else if (currentMode == NetworkMode::Server && peerSockets.contains(socket)) {
        payloadBytes = peerPendingBinaryBytes.value(socket, 0);
        command = peerPendingBinaryCommands.value(socket);
    }

    if (payloadBytes <= 0) {
        return false;
    }
    if (buffer.size() < payloadBytes) {
        return false;
    }

    const QByteArray payload = buffer.left(static_cast<int>(payloadBytes));
    buffer.remove(0, static_cast<int>(payloadBytes));

    if (currentMode == NetworkMode::Client && socket == clientSocket) {
        clientPendingBinaryCommand = QJsonObject();
        clientPendingBinaryBytes = 0;
        lastMessageTimer.restart();
    } else if (currentMode == NetworkMode::Server && peerSockets.contains(socket)) {
        peerPendingBinaryCommands.remove(socket);
        peerPendingBinaryBytes.remove(socket);
        peerLastMessageTimers[socket].restart();
    }

    emit binaryCommandReceived(command, payload);
    return true;
}

void NetworkController::processLine(QTcpSocket *socket, const QByteArray &line) {
    if (line == HELLO_MESSAGE.trimmed()) {
        if (currentMode == NetworkMode::Server && peerSockets.contains(socket)) {
            if (readyPeerSockets.contains(socket)) {
                return;
            }
            const bool hadReadyPeer = !readyPeerSockets.isEmpty();
            readyPeerSockets.insert(socket);
            peerLastMessageTimers[socket].restart();
            if (!peerSocket && !hadReadyPeer) {
                peerSocket = socket;
            }
            sendProtocolLine(socket, OK_MESSAGE);
            const QString message = QString("Control channel ready: %1")
                                        .arg(peerLabel(socket));
            setStatus(message);
            if (socket == peerSocket) {
                broadcastRoleUpdates();
            } else {
                sendRoleUpdate(socket);
            }
            startHeartbeat();
            controlReady = true;
            emit channelReady(message);
            emit peerReady(peerId(socket), peerLabel(socket));
        }
        return;
    }

    if (line == OK_MESSAGE.trimmed()) {
        if (currentMode == NetworkMode::Client && socket == clientSocket) {
            if (controlReady) {
                return;
            }
            handshakeTimer->stop();
            controlReady = true;
            QJsonObject capabilities;
            capabilities[QStringLiteral("type")] = QStringLiteral("capabilities");
            capabilities[QStringLiteral("compactSpectrumS16")] = true;
            capabilities[QStringLiteral("compactSpectrumU8")] = true;
            capabilities[QStringLiteral("binaryAudioPcmS16")] = true;
            if (uhdRxSpectrumEnabled)
                capabilities[QStringLiteral("uhdRxSpectrum")] = true;
            sendJsonToSocket(socket, capabilities);
            const QString message = QString("Control channel ready: %1:%2")
                                        .arg(clientSocket->peerAddress().toString())
                                        .arg(clientSocket->peerPort());
            setStatus(message);
            startHeartbeat();
            emit channelReady(message);
        }
        return;
    }

    if (line == PING_MESSAGE.trimmed()) {
        sendProtocolLine(socket, PONG_MESSAGE);
        return;
    }

    if (line == PONG_MESSAGE.trimmed()) {
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setStatus(QString("Ignoring invalid control message: %1").arg(parseError.errorString()));
        return;
    }

    QJsonObject command = document.object();
    if (currentMode == NetworkMode::Server && peerSockets.contains(socket) &&
        readyPeerSockets.contains(socket) &&
        command.value(QStringLiteral("type")).toString() == QStringLiteral("capabilities")) {
        if (command.value(QStringLiteral("compactSpectrumS16")).toBool(false))
            compactSpectrumPeers.insert(socket);
        else
            compactSpectrumPeers.remove(socket);
        if (command.value(QStringLiteral("compactSpectrumU8")).toBool(false))
            packedSpectrumPeers.insert(socket);
        else
            packedSpectrumPeers.remove(socket);
        if (command.value(QStringLiteral("binaryAudioPcmS16")).toBool(false))
            binaryAudioPeers.insert(socket);
        else
            binaryAudioPeers.remove(socket);
        if (command.value(QStringLiteral("uhdRxSpectrum")).toBool(false))
            uhdRxSpectrumPeers.insert(socket);
        else
            uhdRxSpectrumPeers.remove(socket);
        return;
    }
    if (command.contains("payloadBytes")) {
        bool ok = false;
        const qint64 payloadBytes = command.value("payloadBytes").toVariant().toLongLong(&ok);
        if (!ok || payloadBytes <= 0 || payloadBytes > (512LL * 1024LL * 1024LL)) {
            setStatus(QString("Ignoring invalid binary payload header"));
            return;
        }
        if (currentMode == NetworkMode::Client && socket == clientSocket) {
            clientPendingBinaryCommand = command;
            clientPendingBinaryBytes = payloadBytes;
        } else if (currentMode == NetworkMode::Server && peerSockets.contains(socket)) {
            command["_networkPeerId"] = peerId(socket);
            command["_networkPeerLabel"] = peerLabel(socket);
            command["_networkPeerIsController"] = socket == peerSocket;
            peerPendingBinaryCommands.insert(socket, command);
            peerPendingBinaryBytes.insert(socket, payloadBytes);
        }
        return;
    }

    if (currentMode == NetworkMode::Client &&
        command.value("type").toString() == QStringLiteral("control") &&
        command.value("action").toString() == QStringLiteral("role")) {
        localClientHasControl = command.value("canControl").toBool(localClientHasControl);
    }
    if (currentMode == NetworkMode::Server && peerSockets.contains(socket)) {
        command["_networkPeerId"] = peerId(socket);
        command["_networkPeerLabel"] = peerLabel(socket);
        command["_networkPeerIsController"] = socket == peerSocket;
    }
    emit controlCommandReceived(command);
}
