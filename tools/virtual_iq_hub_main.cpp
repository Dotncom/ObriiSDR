#include "virtualiqhub.h"
#include "virtualsignaltransport.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>

namespace {
struct Receiver {
    QString id;
    QString host;
    quint16 port = 0;
    QTcpSocket *socket = nullptr;
    qint64 reconnectAtMs = 0;
    quint64 transportDrops = 0;
    bool discontinuity = true;
};

bool endpoint(const QString &text, QString *host, quint16 *port) {
    const int separator = text.lastIndexOf(':');
    if (separator <= 0) return false;
    QHostAddress address;
    bool ok = false;
    const int parsedPort = text.mid(separator + 1).toInt(&ok);
    const QString parsedHost = text.left(separator);
    if (!ok || parsedPort < 1 || parsedPort > 65535 || !address.setAddress(parsedHost))
        return false;
    *host = parsedHost;
    *port = static_cast<quint16>(parsedPort);
    return true;
}

bool nonnegative(const QString &text, int limit, int *result) {
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value < 0 || value > limit) return false;
    *result = value;
    return true;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ObriiIqHub"));
    QCommandLineParser arguments;
    arguments.setApplicationDescription(QStringLiteral("Bounded virtual-IQ fan-out and packet collision laboratory"));
    arguments.addHelpOption();
    arguments.addOption({QStringLiteral("listen"), QStringLiteral("Publisher bind address:port (default 127.0.0.1:17662)"),
                         QStringLiteral("endpoint"), QStringLiteral("127.0.0.1:17662")});
    arguments.addOption({QStringLiteral("rx"), QStringLiteral("Receiver listen address:port; repeat for each receiver"),
                         QStringLiteral("endpoint")});
    arguments.addOption({QStringLiteral("delay-ms"), QStringLiteral("Fixed frame delay, 0..60000 ms"),
                         QStringLiteral("value"), QStringLiteral("0")});
    arguments.addOption({QStringLiteral("collision-ms"), QStringLiteral("Drop frames from overlapping TX sources, 0..1000 ms"),
                         QStringLiteral("value"), QStringLiteral("0")});
    arguments.addOption({QStringLiteral("noise-sigma"), QStringLiteral("Reproducible CF32 noise standard deviation, 0..10"),
                         QStringLiteral("value"), QStringLiteral("0")});
    arguments.addOption({QStringLiteral("seed"), QStringLiteral("Noise seed"),
                         QStringLiteral("value"), QStringLiteral("1")});
    arguments.addOption({QStringLiteral("queue-frames"), QStringLiteral("Per-receiver queue capacity, 1..256"),
                         QStringLiteral("value"), QStringLiteral("32")});
    arguments.addOption({QStringLiteral("queue-mib"), QStringLiteral("Per-receiver queue bytes, 1..64 MiB"),
                         QStringLiteral("value"), QStringLiteral("8")});
    arguments.process(app);

    QString listenHost;
    quint16 listenPort = 0;
    if (!endpoint(arguments.value("listen"), &listenHost, &listenPort) ||
        arguments.values("rx").isEmpty()) {
        std::fprintf(stderr, "Specify --listen IP:port and at least one --rx IP:port\n");
        return 2;
    }
    VirtualIqHubSettings settings;
    if (!nonnegative(arguments.value("delay-ms"), 60000, &settings.fixedDelayMs) ||
        !nonnegative(arguments.value("collision-ms"), 1000, &settings.collisionWindowMs) ||
        !nonnegative(arguments.value("queue-frames"), 256, &settings.maximumQueuedFrames) ||
        settings.maximumQueuedFrames == 0) return 2;
    int queueMiB = 0;
    if (!nonnegative(arguments.value("queue-mib"), 64, &queueMiB) || queueMiB == 0) return 2;
    settings.maximumQueuedBytes = static_cast<qint64>(queueMiB) * 1024 * 1024;
    bool ok = false;
    settings.noiseSigma = arguments.value("noise-sigma").toFloat(&ok);
    if (!ok || settings.noiseSigma < 0 || settings.noiseSigma > 10) return 2;
    settings.randomSeed = arguments.value("seed").toUInt(&ok);
    if (!ok) return 2;

    VirtualIqHub hub(settings);
    QList<Receiver> receivers;
    for (const QString &value : arguments.values("rx")) {
        Receiver receiver;
        receiver.id = QStringLiteral("rx%1").arg(receivers.size() + 1);
        if (!endpoint(value, &receiver.host, &receiver.port) || !hub.addSubscriber(receiver.id)) {
            std::fprintf(stderr, "Invalid or excess RX endpoint\n");
            return 2;
        }
        receivers.append(receiver);
    }

    QTcpServer server;
    if (!server.listen(QHostAddress(listenHost), listenPort)) {
        std::fprintf(stderr, "IQ hub listen failed: %s\n", qPrintable(server.errorString()));
        return 1;
    }
    QHash<QTcpSocket *, VirtualSignalStreamParser> publishers;
    QHash<QTcpSocket *, QString> sourceIds;
    quint64 nextSourceId = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            if (publishers.size() >= 16) {
                socket->deleteLater();
                continue;
            }
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            publishers.insert(socket, VirtualSignalStreamParser{});
            sourceIds.insert(socket, QStringLiteral("tx%1").arg(++nextSourceId));
            auto *pump = new QTimer(socket);
            pump->setInterval(0);
            QObject::connect(socket, &QTcpSocket::readyRead, pump, [pump] { pump->start(); });
            QObject::connect(pump, &QTimer::timeout, &app, [&, socket, pump] {
                auto parser = publishers.find(socket);
                if (parser == publishers.end()) { pump->stop(); return; }
                qint64 bytesThisTurn = 0;
                while (socket->bytesAvailable() > 0 && bytesThisTurn < 512 * 1024) {
                    const QByteArray chunk = socket->read(64 * 1024);
                    if (chunk.isEmpty()) break;
                    bytesThisTurn += chunk.size();
                    parser->append(chunk);
                    VirtualSignalFrame frame;
                    QString error;
                    while (parser->takeFrame(&frame, &error)) {
                        if (frame.protocolId == QStringLiteral("keepalive")) {
                            VirtualSignalFrame ack;
                            ack.sequence = frame.sequence;
                            ack.timestampNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
                            ack.streamId = frame.streamId;
                            ack.protocolId = QStringLiteral("keepalive-ack");
                            ack.payloadType = VirtualSignalPayloadType::ProtocolFrame;
                            const QByteArray wire = VirtualSignalStreamParser::wrap(ack);
                            if (socket->bytesToWrite() < 65536) socket->write(wire);
                        } else if (frame.payloadType == VirtualSignalPayloadType::ComplexFloat32 &&
                                   !frame.endOfStream &&
                                   !hub.publish(sourceIds.value(socket), frame,
                                                QDateTime::currentMSecsSinceEpoch(), &error)) {
                            break;
                        }
                    }
                    if (!error.isEmpty() || parser->bufferedBytes() > 2 * 1024 * 1024) {
                        pump->stop();
                        socket->disconnectFromHost();
                        return;
                    }
                }
                if (socket->bytesAvailable() == 0) pump->stop();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, &app, [&, socket] {
                publishers.remove(socket);
                sourceIds.remove(socket);
                socket->deleteLater();
            });
        }
    });

    QTimer drain;
    drain.setInterval(5);
    drain.setTimerType(Qt::PreciseTimer);
    QObject::connect(&drain, &QTimer::timeout, &app, [&] {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (Receiver &receiver : receivers) {
            Receiver *target = &receiver;
            if (!receiver.socket) {
                if (nowMs < receiver.reconnectAtMs) continue;
                receiver.socket = new QTcpSocket(&app);
                receiver.socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                QTcpSocket *socket = receiver.socket;
                QObject::connect(socket, &QTcpSocket::stateChanged,
                                 &app, [target, socket](QAbstractSocket::SocketState state) {
                    if (state != QAbstractSocket::UnconnectedState || target->socket != socket) return;
                    socket->deleteLater();
                    target->socket = nullptr;
                    target->reconnectAtMs = QDateTime::currentMSecsSinceEpoch() + 500;
                    target->discontinuity = true;
                });
                socket->connectToHost(receiver.host, receiver.port);
                continue;
            }
            if (receiver.socket->state() != QAbstractSocket::ConnectedState) continue;
            if (receiver.socket->bytesToWrite() > 2 * 1024 * 1024) continue;
            for (VirtualSignalFrame frame : hub.takeDue(receiver.id, nowMs, 16)) {
                frame.discontinuity |= receiver.discontinuity;
                receiver.discontinuity = false;
                QString error;
                const QByteArray wire = VirtualSignalStreamParser::wrap(frame, &error);
                if (wire.isEmpty() || receiver.socket->write(wire) != wire.size()) {
                    ++receiver.transportDrops;
                    receiver.discontinuity = true;
                    receiver.socket->disconnectFromHost();
                    break;
                }
            }
        }
    });
    drain.start();

    QTimer statistics;
    statistics.setInterval(5000);
    QObject::connect(&statistics, &QTimer::timeout, &app, [&] {
        const auto total = hub.totalCounters();
        std::fprintf(stdout, "hub tx=%d in=%llu out=%llu drop=%llu collision=%llu queuedBytes=%lld queuedFrames=%lld avgDelayMs=%.1f\n",
                     publishers.size(), static_cast<unsigned long long>(total.receivedFrames),
                     static_cast<unsigned long long>(total.deliveredFrames),
                     static_cast<unsigned long long>(total.droppedFrames),
                     static_cast<unsigned long long>(total.collisionFrames),
                     static_cast<long long>(total.queuedBytes),
                     static_cast<long long>(total.queuedFrames),
                     total.deliveredFrames ? static_cast<double>(total.accumulatedDelayMs) / total.deliveredFrames : 0.0);
        for (const Receiver &receiver : receivers) {
            const auto count = hub.counters(receiver.id);
            std::fprintf(stdout, "  %s %s:%u connected=%d delivered=%llu drop=%llu collision=%llu transportDrop=%llu queued=%lld avgDelayMs=%.1f maxDelayMs=%lld\n",
                         qPrintable(receiver.id), qPrintable(receiver.host), receiver.port,
                         receiver.socket && receiver.socket->state() == QAbstractSocket::ConnectedState,
                         static_cast<unsigned long long>(count.deliveredFrames),
                         static_cast<unsigned long long>(count.droppedFrames),
                         static_cast<unsigned long long>(count.collisionFrames),
                         static_cast<unsigned long long>(receiver.transportDrops),
                         static_cast<long long>(count.queuedFrames),
                         count.deliveredFrames ? static_cast<double>(count.accumulatedDelayMs) / count.deliveredFrames : 0.0,
                         static_cast<long long>(count.maximumDelayMs));
        }
        std::fflush(stdout);
    });
    statistics.start();
    std::fprintf(stdout, "Obrii IQ hub listening on %s:%u\n", qPrintable(listenHost), listenPort);
    std::fflush(stdout);
    return app.exec();
}
