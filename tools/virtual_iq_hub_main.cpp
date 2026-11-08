#include "virtualiqhub.h"
#include "virtualiqmedium.h"
#include "virtualiqhubcontrol.h"
#include "virtualsignaltransport.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

#include <cstdio>
#include <memory>
#include <list>

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

bool legacySourceId(const QString &id) {
    if (!id.startsWith(QStringLiteral("tx"))) return false;
    bool ok = false;
    const int number = id.mid(2).toInt(&ok);
    return ok && number >= 1 && number <= 16 &&
           id == QStringLiteral("tx%1").arg(number);
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ObriiIqHub"));
    QCommandLineParser arguments;
    arguments.setApplicationDescription(QStringLiteral("Bounded virtual-IQ fan-out or sample-aligned RF medium"));
    arguments.addHelpOption();
    arguments.addOption({QStringLiteral("listen"), QStringLiteral("Publisher bind address:port (default 127.0.0.1:17662)"),
                         QStringLiteral("endpoint"), QStringLiteral("127.0.0.1:17662")});
    arguments.addOption({QStringLiteral("rx"), QStringLiteral("Receiver [node-id@]listen-address:port; repeat for each receiver"),
                         QStringLiteral("endpoint")});
    arguments.addOption({QStringLiteral("scenario"), QStringLiteral("Initial TX-to-RX link scenario JSON"),
                         QStringLiteral("file")});
    arguments.addOption({QStringLiteral("control-port"), QStringLiteral("Optional authenticated loopback control port"), QStringLiteral("port")});
    arguments.addOption({QStringLiteral("control-key-file"), QStringLiteral("Private JSON file containing the 64-hex control token"), QStringLiteral("file")});
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
    arguments.addOption({QStringLiteral("mix-rate"), QStringLiteral("Enable sample-aligned IQ mixing at this rate, 8000..2000000 S/s"),
                         QStringLiteral("sps")});
    arguments.addOption({QStringLiteral("mix-center"), QStringLiteral("Required exact center frequency for IQ mixing, Hz"),
                         QStringLiteral("hz")});
    arguments.addOption({QStringLiteral("mix-block-samples"), QStringLiteral("Mixed output block size, 128..4096 samples"),
                         QStringLiteral("count"), QStringLiteral("1024")});
    arguments.addOption({QStringLiteral("mix-latency-ms"), QStringLiteral("Playout wait for other TX streams, 10..5000 ms"),
                         QStringLiteral("ms"), QStringLiteral("100")});
    arguments.addOption({QStringLiteral("mix-max-blocks"), QStringLiteral("Per-RX IQ block capacity, 4..256"),
                         QStringLiteral("count"), QStringLiteral("256")});
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
    const bool mixing = arguments.isSet(QStringLiteral("mix-rate"));
    VirtualIqMediumSettings mediumSettings;
    if (mixing) {
        int rate = 0;
        if (!nonnegative(arguments.value("mix-rate"), 2000000, &rate) || rate < 8000 ||
            !arguments.isSet(QStringLiteral("mix-center")) ||
            settings.collisionWindowMs != 0) {
            std::fprintf(stderr, "Mix mode needs --mix-rate 8000..2000000 and --mix-center Hz; --collision-ms is packet-only\n");
            return 2;
        }
        mediumSettings.sampleRate = static_cast<quint32>(rate);
        mediumSettings.centerFrequencyHz = arguments.value("mix-center").toLongLong(&ok);
        if (!ok || mediumSettings.centerFrequencyHz < 0 ||
            !nonnegative(arguments.value("mix-block-samples"), 4096, &mediumSettings.blockSamples) ||
            mediumSettings.blockSamples < 128 ||
            !nonnegative(arguments.value("mix-latency-ms"), 5000, &mediumSettings.playoutDelayMs) ||
            mediumSettings.playoutDelayMs < 10 ||
            !nonnegative(arguments.value("mix-max-blocks"), 256, &mediumSettings.maximumBufferedBlocks) ||
            mediumSettings.maximumBufferedBlocks < 4) return 2;
        mediumSettings.fixedDelayMs = settings.fixedDelayMs;
        mediumSettings.noiseSigma = settings.noiseSigma;
        mediumSettings.randomSeed = settings.randomSeed;
        const qint64 capacity = static_cast<qint64>(mediumSettings.maximumBufferedBlocks) *
            mediumSettings.blockSamples;
        const qint64 required =
            (static_cast<qint64>(mediumSettings.playoutDelayMs + mediumSettings.fixedDelayMs) *
             mediumSettings.sampleRate + 999) / 1000 + mediumSettings.blockSamples;
        if (capacity < required) {
            std::fprintf(stderr, "Mix block capacity is shorter than playout + fixed delay; raise --mix-max-blocks or --mix-block-samples, or lower latency/rate\n");
            return 2;
        }
        if (static_cast<qint64>(mediumSettings.blockSamples) * 2000 <
            mediumSettings.sampleRate) {
            std::fprintf(stderr, "Mix block is too small for the 5-ms output pump; increase --mix-block-samples\n");
            return 2;
        }
    } else if (arguments.isSet(QStringLiteral("mix-center"))) {
        std::fprintf(stderr, "--mix-center requires --mix-rate\n");
        return 2;
    }

    QJsonObject scenario;
    if (arguments.isSet(QStringLiteral("scenario"))) {
        QFile file(arguments.value(QStringLiteral("scenario")));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
            std::fprintf(stderr, "Cannot read IQ hub scenario (maximum 1 MiB)\n");
            return 2;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            std::fprintf(stderr, "Invalid IQ hub scenario JSON\n");
            return 2;
        }
        scenario = document.object();
        if (scenario.value(QStringLiteral("version")).toInt(-1) != 1 ||
            !scenario.value(QStringLiteral("links")).isArray() ||
            (scenario.contains(QStringLiteral("defaultRoute")) &&
             !scenario.value(QStringLiteral("defaultRoute")).isBool())) {
            std::fprintf(stderr, "IQ hub scenario needs version 1, links array and boolean defaultRoute\n");
            return 2;
        }
        settings.defaultRoute = scenario.value(QStringLiteral("defaultRoute")).toBool(true);
    }

    VirtualIqHub hub(settings);
    mediumSettings.defaultRoute = settings.defaultRoute;
    VirtualIqMedium medium(mediumSettings);
    std::list<Receiver> receivers;
    for (const QString &value : arguments.values("rx")) {
        Receiver receiver;
        const int separator = value.indexOf(QLatin1Char('@'));
        const bool named = separator >= 0;
        receiver.id = named ? value.left(separator)
                            : QStringLiteral("rx%1").arg(receivers.size() + 1);
        const QString address = named ? value.mid(separator + 1) : value;
        if ((named && !virtualSignalValidNodeId(receiver.id)) ||
            !endpoint(address, &receiver.host, &receiver.port) ||
            !hub.addSubscriber(receiver.id) || (mixing && !medium.addSubscriber(receiver.id))) {
            std::fprintf(stderr, "Invalid or excess RX endpoint\n");
            return 2;
        }
        receivers.push_back(receiver);
    }
    if (mixing && receivers.size() > 8) {
        std::fprintf(stderr, "Mix mode supports up to 8 RX nodes\n");
        return 2;
    }
    QSet<QString> seenLinks;
    for (const QJsonValue &entry : scenario.value(QStringLiteral("links")).toArray()) {
        if (!entry.isObject()) {
            std::fprintf(stderr, "IQ hub link must be an object\n");
            return 2;
        }
        const QJsonObject link = entry.toObject();
        const QString source = link.value(QStringLiteral("tx")).toString();
        const QString target = link.value(QStringLiteral("rx")).toString();
        const QString pair = source + QLatin1Char('\n') + target;
        const QJsonValue enabled = link.value(QStringLiteral("enabled"));
        const QJsonValue delay = link.value(QStringLiteral("delayMs"));
        const QJsonValue gain = link.value(QStringLiteral("gainDb"));
        const QJsonValue frequencyOffset = link.value(QStringLiteral("frequencyOffsetHz"));
        if ((!legacySourceId(source) && !virtualSignalValidNodeId(source)) ||
            !hub.subscriberIds().contains(target) || seenLinks.contains(pair) ||
            (!enabled.isUndefined() && !enabled.isBool()) ||
            (!delay.isUndefined() && (!delay.isDouble() || delay.toDouble() != delay.toInt(-1))) ||
            (!gain.isUndefined() && !gain.isDouble()) ||
            (!frequencyOffset.isUndefined() && !frequencyOffset.isDouble()) ||
            (!mixing && frequencyOffset.toDouble() != 0.0)) {
            std::fprintf(stderr, "Invalid or duplicate IQ hub link (frequencyOffsetHz requires mix mode)\n");
            return 2;
        }
        VirtualIqLinkSettings linkSettings;
        linkSettings.enabled = enabled.toBool(true);
        linkSettings.delayMs = delay.toInt(0);
        linkSettings.gainDb = static_cast<float>(gain.toDouble(0.0));
        auto mixedLink = linkSettings;
        mixedLink.frequencyOffsetHz = frequencyOffset.toDouble();
        if (!hub.canSetLink(source, target, linkSettings) ||
            (mixing && !medium.canSetLink(source, target, mixedLink))) {
            std::fprintf(stderr, "IQ hub link exceeds delay, gain or mix frequency limits\n");
            return 2;
        }
        hub.setLink(source, target, linkSettings);
        if (mixing) medium.setLink(source, target, mixedLink);
        seenLinks.insert(pair);
    }

    std::unique_ptr<VirtualIqHubControl> control;
    if(arguments.isSet("control-port")!=arguments.isSet("control-key-file")) {
        std::fprintf(stderr,"Hub control needs both --control-port and --control-key-file\n");return 2;
    }
    if(arguments.isSet("control-port")) {
        int port=0;
        QFile file(arguments.value("control-key-file"));
        if(!nonnegative(arguments.value("control-port"),65535,&port)||port==0||
           !file.open(QIODevice::ReadOnly)||file.size()>4096) {
            std::fprintf(stderr,"Invalid control port or private key file\n");return 2;
        }
        QJsonParseError parse;
        const auto key=QJsonDocument::fromJson(file.readAll(),&parse).object().value("token").toString();
        QString error;
        control=std::make_unique<VirtualIqHubControl>(hub,mixing?&medium:nullptr,scenario.value("links").toArray(),settings.defaultRoute);
        control->setAddReceiverHandler([&](const QString &id,const QString &host,quint16 port){
            if(receivers.size()>=size_t(mixing?8:32))return false;
            for(const auto &receiver:receivers)
                if(receiver.id==id||(receiver.port==port&&QHostAddress(receiver.host)==QHostAddress(host)))return false;
            Receiver receiver;receiver.id=id;receiver.host=host;receiver.port=port;
            // List insertion leaves every existing socket callback's Receiver* stable.
            receivers.push_back(std::move(receiver));return true;
        });
        if(parse.error!=QJsonParseError::NoError||!control->listen(quint16(port),key,&error)) {
            std::fprintf(stderr,"Cannot start authenticated loopback hub control\n");return 2;
        }
    }
    QTcpServer server;
    QElapsedTimer mediumClock;
    mediumClock.start();
    if (!server.listen(QHostAddress(listenHost), listenPort)) {
        std::fprintf(stderr, "IQ hub listen failed: %s\n", qPrintable(server.errorString()));
        return 1;
    }
    QHash<QTcpSocket *, VirtualSignalStreamParser> publishers;
    QHash<QTcpSocket *, QString> fallbackIds;
    QHash<QTcpSocket *, QString> sourceIds;
    QHash<QTcpSocket *, QString> sessionIds;
    QHash<QString, QTcpSocket *> activeNodeIds;
    QSet<QTcpSocket *> firstData;
    quint64 nextSourceId = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            if (publishers.size() >= 16) {
                socket->deleteLater();
                continue;
            }
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            publishers.insert(socket, VirtualSignalStreamParser{});
            fallbackIds.insert(socket, QStringLiteral("tx%1").arg(++nextSourceId));
            sessionIds.insert(socket, QUuid::createUuid().toString(QUuid::WithoutBraces));
            firstData.insert(socket);
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
                        const QJsonValue declaredId = frame.metadata.value(QStringLiteral("hubNodeId"));
                        if (!declaredId.isUndefined() &&
                            (!declaredId.isString() ||
                             !virtualSignalValidNodeId(declaredId.toString()))) {
                            error = QStringLiteral("Invalid virtual IQ node ID");
                            break;
                        }
                        const QString declaredName = declaredId.toString();
                        if (!sourceIds.contains(socket)) {
                            const QString name = declaredName.isEmpty()
                                ? fallbackIds.value(socket) : declaredName;
                            if (activeNodeIds.contains(name)) {
                                error = QStringLiteral("Virtual IQ node ID is already connected");
                                break;
                            }
                            sourceIds.insert(socket, name);
                            activeNodeIds.insert(name, socket);
                            std::fprintf(stdout, "hub node=%s session=%s connected\n",
                                         qPrintable(name), qPrintable(sessionIds.value(socket)));
                            std::fflush(stdout);
                        } else if (!declaredName.isEmpty() && declaredName != sourceIds.value(socket)) {
                            error = QStringLiteral("Virtual IQ node ID changed during session");
                            break;
                        }
                        if (frame.protocolId == QStringLiteral("keepalive")) {
                            VirtualSignalFrame ack;
                            ack.sequence = frame.sequence;
                            ack.timestampNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
                            ack.streamId = frame.streamId;
                            ack.protocolId = QStringLiteral("keepalive-ack");
                            ack.payloadType = VirtualSignalPayloadType::ProtocolFrame;
                            ack.metadata.insert(QStringLiteral("hubNodeId"), sourceIds.value(socket));
                            ack.metadata.insert(QStringLiteral("hubSessionId"), sessionIds.value(socket));
                            const QByteArray wire = VirtualSignalStreamParser::wrap(ack);
                            if (socket->bytesToWrite() < 65536) socket->write(wire);
                        } else if (frame.payloadType == VirtualSignalPayloadType::ComplexFloat32 &&
                                   !frame.endOfStream) {
                            frame.metadata.insert(QStringLiteral("hubSessionId"), sessionIds.value(socket));
                            frame.discontinuity |= firstData.remove(socket);
                            const qint64 nowNs = mixing ? mediumClock.nsecsElapsed()
                                                       : QDateTime::currentMSecsSinceEpoch() * 1000000LL;
                            if (!(mixing ? medium.publish(sourceIds.value(socket), frame, nowNs, &error)
                                         : hub.publish(sourceIds.value(socket), frame, nowNs / 1000000LL, &error)))
                                break;
                        }
                    }
                    if (!error.isEmpty() || parser->bufferedBytes() > 2 * 1024 * 1024) {
                        if (!error.isEmpty())
                            std::fprintf(stderr, "IQ hub publisher rejected: %s\n", qPrintable(error));
                        pump->stop();
                        socket->disconnectFromHost();
                        return;
                    }
                }
                if (socket->bytesAvailable() == 0) pump->stop();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, &app, [&, socket] {
                publishers.remove(socket);
                const QString name = sourceIds.value(socket);
                if (activeNodeIds.value(name) == socket) activeNodeIds.remove(name);
                fallbackIds.remove(socket);
                sourceIds.remove(socket);
                sessionIds.remove(socket);
                firstData.remove(socket);
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
            const auto frames = mixing
                ? medium.takeReady(receiver.id, mediumClock.nsecsElapsed(), 16)
                : hub.takeDue(receiver.id, nowMs, 16);
            for (VirtualSignalFrame frame : frames) {
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
        if (mixing) {
            const auto total = medium.totalCounters();
            std::fprintf(stdout, "medium tx=%d in=%llu out=%llu overlapSamples=%llu gaps=%llu late=%llu bufferDrops=%llu clipped=%llu bufferedBlocks=%d\n",
                         publishers.size(), static_cast<unsigned long long>(total.receivedFrames),
                         static_cast<unsigned long long>(total.emittedFrames),
                         static_cast<unsigned long long>(total.overlapSamples),
                         static_cast<unsigned long long>(total.gapSamples),
                         static_cast<unsigned long long>(total.lateSamples),
                         static_cast<unsigned long long>(total.bufferDrops),
                         static_cast<unsigned long long>(total.clippedComponents), total.bufferedBlocks);
            for (const Receiver &receiver : receivers) {
                const auto count = medium.counters(receiver.id);
                std::fprintf(stdout, "  %s connected=%d out=%llu overlap=%llu gaps=%llu late=%llu drop=%llu transportDrop=%llu buffered=%d\n",
                             qPrintable(receiver.id),
                             receiver.socket && receiver.socket->state() == QAbstractSocket::ConnectedState,
                             static_cast<unsigned long long>(count.emittedFrames),
                             static_cast<unsigned long long>(count.overlapSamples),
                             static_cast<unsigned long long>(count.gapSamples),
                             static_cast<unsigned long long>(count.lateSamples),
                             static_cast<unsigned long long>(count.bufferDrops),
                             static_cast<unsigned long long>(receiver.transportDrops), count.bufferedBlocks);
            }
            std::fflush(stdout);
            return;
        }
        const auto total = hub.totalCounters();
        std::fprintf(stdout, "hub tx=%d in=%llu out=%llu drop=%llu collision=%llu filtered=%llu queuedBytes=%lld queuedFrames=%lld avgDelayMs=%.1f\n",
                     publishers.size(), static_cast<unsigned long long>(total.receivedFrames),
                     static_cast<unsigned long long>(total.deliveredFrames),
                     static_cast<unsigned long long>(total.droppedFrames),
                     static_cast<unsigned long long>(total.collisionFrames),
                     static_cast<unsigned long long>(total.filteredFrames),
                     static_cast<long long>(total.queuedBytes),
                     static_cast<long long>(total.queuedFrames),
                     total.deliveredFrames ? static_cast<double>(total.accumulatedDelayMs) / total.deliveredFrames : 0.0);
        for (const Receiver &receiver : receivers) {
            const auto count = hub.counters(receiver.id);
            std::fprintf(stdout, "  %s %s:%u connected=%d delivered=%llu drop=%llu collision=%llu filtered=%llu transportDrop=%llu queued=%lld avgDelayMs=%.1f maxDelayMs=%lld\n",
                         qPrintable(receiver.id), qPrintable(receiver.host), receiver.port,
                         receiver.socket && receiver.socket->state() == QAbstractSocket::ConnectedState,
                         static_cast<unsigned long long>(count.deliveredFrames),
                         static_cast<unsigned long long>(count.droppedFrames),
                         static_cast<unsigned long long>(count.collisionFrames),
                         static_cast<unsigned long long>(count.filteredFrames),
                         static_cast<unsigned long long>(receiver.transportDrops),
                         static_cast<long long>(count.queuedFrames),
                         count.deliveredFrames ? static_cast<double>(count.accumulatedDelayMs) / count.deliveredFrames : 0.0,
                         static_cast<long long>(count.maximumDelayMs));
        }
        std::fflush(stdout);
    });
    statistics.start();
    std::fprintf(stdout, "Obrii IQ hub listening on %s:%u mode=%s\n", qPrintable(listenHost), listenPort,
                 mixing ? "sample-mix" : "packet-fanout");
    std::fflush(stdout);
    const int result=app.exec();
    // Receiver sockets belong to app, which outlives this stable endpoint list.
    for(auto &receiver:receivers)if(receiver.socket) {
        receiver.socket->disconnect(&app);receiver.socket->abort();delete receiver.socket;receiver.socket=nullptr;
    }
    return result;
}
