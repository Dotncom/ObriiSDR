#include "networkcontroller.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QThread>

#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

bool spinUntil(const std::function<bool()> &predicate, int timeoutMs = 2000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

void connectClient(QTcpSocket &socket, quint16 port) {
    socket.connectToHost(QStringLiteral("127.0.0.1"), port);
    require(spinUntil([&socket]() {
                return socket.state() == QAbstractSocket::ConnectedState;
            }),
            "Network test client did not connect");
    socket.write("FOBOSAPP_HELLO 1\n");
    socket.flush();
}

QList<QJsonObject> takeJsonLines(QTcpSocket &socket, QByteArray &buffer) {
    buffer.append(socket.readAll());
    QList<QJsonObject> result;
    int newline = -1;
    while ((newline = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (line.isEmpty() || line.startsWith("FOBOSAPP_")) continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject()) result.append(document.object());
    }
    return result;
}

bool hasRole(QTcpSocket &socket, QByteArray &buffer, bool canControl) {
    const QList<QJsonObject> frames = takeJsonLines(socket, buffer);
    for (const QJsonObject &frame : frames) {
        if (frame.value(QStringLiteral("action")).toString() == QStringLiteral("role") &&
            frame.value(QStringLiteral("canControl")).toBool() == canControl) {
            return true;
        }
    }
    return false;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        NetworkController server;
        server.startServer(QStringLiteral("127.0.0.1"), 0);
        require(server.listeningPort() != 0, "Network controller did not bind an ephemeral port");

        QTcpSocket first;
        QTcpSocket second;
        QByteArray firstInput;
        QByteArray secondInput;
        connectClient(first, server.listeningPort());
        require(spinUntil([&server]() { return server.readyPeerIds().size() == 1; }),
                "First client handshake did not complete");
        const QString firstId = server.controllerPeerId();
        require(!firstId.isEmpty(), "First client did not become initial controller");
        require(spinUntil([&]() { return hasRole(first, firstInput, true); }),
                "First client did not receive controller role");

        connectClient(second, server.listeningPort());
        require(spinUntil([&server]() { return server.readyPeerIds().size() == 2; }),
                "Second client handshake did not complete");
        require(server.controllerPeerId() == firstId,
                "Observer connection unexpectedly replaced the current controller");
        require(spinUntil([&]() { return hasRole(second, secondInput, false); }),
                "Second client did not receive observer role");

        QString secondId;
        for (const QString &id : server.readyPeerIds()) {
            if (id != firstId) secondId = id;
        }
        require(!secondId.isEmpty(), "Second peer ID was not registered");

        first.disconnectFromHost();
        require(spinUntil([&server]() { return server.readyPeerIds().size() == 1; }),
                "Controller disconnect was not processed");
        require(server.controllerPeerId().isEmpty(),
                "Observer was silently promoted after controller disconnect");
        require(spinUntil([&]() { return hasRole(second, secondInput, false); }),
                "Remaining observer did not receive the vacant-controller role update");
        require(server.setControllerPeer(secondId),
                "Explicit controller assignment failed after disconnect");
        require(spinUntil([&]() { return hasRole(second, secondInput, true); }),
                "Explicitly promoted client did not receive controller role");

        // A partial line must remain buffered without spinning. Completing it
        // exercises the same re-entrant path as real fragmented TCP delivery.
        second.write("{\"type\":\"control\",\"action\":\"partial");
        second.flush();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        bool stoppedReentrantly = false;
        QObject::connect(&server,
                         &NetworkController::controlCommandReceived,
                         [&server, &stoppedReentrantly](const QJsonObject &command) {
                             if (command.value(QStringLiteral("action")).toString() ==
                                 QStringLiteral("partialStop")) {
                                 stoppedReentrantly = true;
                                 server.stop();
                             }
                         });
        second.write("Stop\"}\n");
        second.flush();
        require(spinUntil([&stoppedReentrantly]() { return stoppedReentrantly; }),
                "Fragmented control command was not delivered");
        require(server.mode() == NetworkMode::Disabled,
                "Re-entrant server shutdown did not complete safely");

        std::cout << "Network controller self-test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Network controller self-test failed: " << error.what() << '\n';
        return 1;
    }
}
