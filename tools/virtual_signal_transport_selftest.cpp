#include "virtualtransmitterbackend.h"
#include "virtualsignaltransport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
bool receiveFrame(QIODevice *socket, VirtualSignalStreamParser *parser,
                  VirtualSignalFrame *frame, QString *error);

class ThreadedLocalReceiver final : public QThread {
public:
    QString name;
    QString error;
    QSemaphore ready;
    int acceptedState = -1;
    int receivedSamples = 0;

    void run() override {
        QLocalServer server;
        if (!server.listen(name)) {
            error = server.errorString();
            ready.release();
            return;
        }
        ready.release();
        if (!server.waitForNewConnection(3000)) {
            error = QStringLiteral("Threaded local server did not accept a connection");
            return;
        }
        std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
        if (!socket) {
            error = QStringLiteral("Threaded local server returned no socket");
            return;
        }
        acceptedState = static_cast<int>(socket->state());
        VirtualSignalStreamParser parser;
        VirtualSignalFrame frame;
        if (!receiveFrame(socket.get(), &parser, &frame, &error)) return;
        QVector<float> iq;
        if (!virtualSignalDecodeComplexFloat32(frame.payload, &iq, &error)) return;
        receivedSamples = iq.size() / 2;
    }
};

class ReconnectingLocalReceiver final : public QThread {
public:
    QString name;
    QString error;
    QSemaphore ready;
    QSemaphore frameReceived;
    int receivedFrames = 0;

    void run() override {
        QLocalServer server;
        if (!server.listen(name)) {
            error = server.errorString();
            ready.release();
            return;
        }
        ready.release();
        for (int connection = 0; connection < 2; ++connection) {
            if (!server.waitForNewConnection(3000)) {
                error = QStringLiteral("Reconnecting receiver missed connection %1")
                            .arg(connection + 1);
                return;
            }
            std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
            if (!socket) {
                error = QStringLiteral("Reconnecting receiver returned no socket");
                return;
            }
            VirtualSignalStreamParser parser;
            VirtualSignalFrame frame;
            if (!receiveFrame(socket.get(), &parser, &frame, &error)) return;
            ++receivedFrames;
            frameReceived.release();
            QElapsedTimer timer;
            timer.start();
            while (socket->state() == QLocalSocket::ConnectedState && timer.elapsed() < 2000) {
                socket->waitForReadyRead(100);
                socket->readAll();
            }
            if (socket->state() == QLocalSocket::ConnectedState) {
                error = QStringLiteral("Receiver did not notice local TX disconnect");
                return;
            }
        }
    }
};

bool receiveFrame(QIODevice *socket, VirtualSignalStreamParser *parser,
                  VirtualSignalFrame *frame, QString *error) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (parser->takeFrame(frame, error)) return true;
        if (!error->isEmpty()) return false;
        if (auto *local = qobject_cast<QLocalSocket *>(socket)) {
            if (!local->waitForReadyRead(100)) continue;
        } else if (auto *tcp = qobject_cast<QTcpSocket *>(socket)) {
            if (!tcp->waitForReadyRead(100)) continue;
        }
        parser->append(socket->readAll());
    }
    *error = QStringLiteral("Timed out waiting for virtual IQ frame");
    return false;
}

bool exercise(VirtualSignalTransportMode mode, QString *error) {
    const QString localName = QStringLiteral("obrii-transport-test-%1")
                                  .arg(QCoreApplication::applicationPid());
    QLocalServer localServer;
    QTcpServer tcpServer;
    VirtualSignalEndpoint endpoint;
    endpoint.mode = mode;
    endpoint.localName = localName;
    endpoint.host = QStringLiteral("127.0.0.1");
    if (mode == VirtualSignalTransportMode::LocalIpc) {
        if (!localServer.listen(localName)) {
            *error = localServer.errorString();
            return false;
        }
    } else {
        if (!tcpServer.listen(QHostAddress::LocalHost, 0)) {
            *error = tcpServer.errorString();
            return false;
        }
        endpoint.port = tcpServer.serverPort();
    }
    VirtualTransmitterBackend transmitter;
    transmitter.setEndpoint(endpoint);
    transmitter.setNodeId(QStringLiteral("lab-tx-a"));
    TxConfiguration config;
    config.frequencyHz = 145500000.0;
    config.sampleRate = 48000;
    if (!transmitter.start(config, error)) return false;
    const bool accepted = mode == VirtualSignalTransportMode::LocalIpc
        ? localServer.waitForNewConnection(1000) : tcpServer.waitForNewConnection(1000);
    if (!accepted) {
        *error = QStringLiteral("Virtual transmitter did not connect");
        return false;
    }
    QIODevice *socket = mode == VirtualSignalTransportMode::LocalIpc
        ? static_cast<QIODevice *>(localServer.nextPendingConnection())
        : static_cast<QIODevice *>(tcpServer.nextPendingConnection());
    const std::complex<float> samples[] = {{0.25f, -0.5f}, {-0.125f, 0.75f}};
    if (!transmitter.writeIq(samples, 2, error)) return false;
    if (!transmitter.drain(1000, error)) return false;
    VirtualSignalStreamParser parser;
    VirtualSignalFrame frame;
    if (!receiveFrame(socket, &parser, &frame, error)) return false;
    QVector<float> floats;
    if (!virtualSignalDecodeComplexFloat32(frame.payload, &floats, error) ||
        frame.sampleRate != 48000 || frame.centerFrequencyHz != 145500000 ||
        frame.sequence != 0 || floats.size() != 4 ||
        frame.metadata.value(QStringLiteral("hubNodeId")).toString() != QStringLiteral("lab-tx-a") ||
        std::abs(floats[0] - 0.25f) > 1e-6f ||
        std::abs(floats[1] + 0.5f) > 1e-6f ||
        std::abs(floats[2] + 0.125f) > 1e-6f ||
        std::abs(floats[3] - 0.75f) > 1e-6f) {
        *error = QStringLiteral("Virtual IQ payload or metadata mismatch");
        return false;
    }
    const qint64 firstTimestampNs = frame.timestampNs;
    if (!transmitter.writeIq(samples, 2, error) ||
        !transmitter.drain(1000, error) ||
        !receiveFrame(socket, &parser, &frame, error) ||
        std::llabs(frame.timestampNs - firstTimestampNs - 41667LL) > 1) {
        *error = QStringLiteral("Virtual IQ timestamps must advance by sample count");
        return false;
    }
    transmitter.setOutputEnabled(false, error);
    transmitter.setOutputEnabled(true, error);
    if (!transmitter.writeIq(samples, 2, error)) return false;
    if (!transmitter.drain(1000, error)) return false;
    if (!receiveFrame(socket, &parser, &frame, error) || !frame.discontinuity ||
        frame.sequence != 2) {
        *error = QStringLiteral("Virtual IQ discontinuity or sequence mismatch");
        return false;
    }
    std::vector<std::complex<float>> largeSamples(16384);
    for (size_t index = 0; index < largeSamples.size(); ++index) {
        const float phase = static_cast<float>(index) * 0.01f;
        largeSamples[index] = {std::cos(phase), std::sin(phase)};
    }
    if (!transmitter.writeIq(largeSamples.data(), static_cast<int>(largeSamples.size()), error) ||
        !transmitter.drain(1000, error) || !receiveFrame(socket, &parser, &frame, error) ||
        frame.sequence != 3 || frame.payload.size() != 16384 * 2 * static_cast<int>(sizeof(float))) {
        *error = QStringLiteral("Large virtual IQ frame did not arrive intact: %1").arg(*error);
        return false;
    }
    transmitter.stop();
    if (!receiveFrame(socket, &parser, &frame, error) || !frame.endOfStream) {
        *error = QStringLiteral("Virtual IQ end-of-stream marker missing");
        return false;
    }
    delete socket;
    return true;
}

bool exerciseThreadedLocal(QString *error) {
    ThreadedLocalReceiver receiver;
    receiver.name = QStringLiteral("obrii-threaded-transport-test-%1")
                        .arg(QCoreApplication::applicationPid());
    receiver.start();
    if (!receiver.ready.tryAcquire(1, 3000)) {
        *error = QStringLiteral("Threaded local receiver did not start");
        receiver.wait();
        return false;
    }
    VirtualTransmitterBackend transmitter;
    VirtualSignalEndpoint endpoint;
    endpoint.localName = receiver.name;
    transmitter.setEndpoint(endpoint);
    TxConfiguration config;
    config.frequencyHz = 145500000.0;
    config.sampleRate = 48000;
    std::vector<std::complex<float>> samples(16384, {0.25f, -0.5f});
    const bool sent = transmitter.start(config, error) &&
                      transmitter.writeIq(samples.data(), static_cast<int>(samples.size()), error);
    receiver.wait();
    transmitter.stop();
    if (!sent || !receiver.error.isEmpty() || receiver.receivedSamples != 16384 ||
        receiver.acceptedState != static_cast<int>(QLocalSocket::ConnectedState)) {
        *error = QStringLiteral("Threaded local receive failed: %1; state=%2 samples=%3")
                     .arg(receiver.error).arg(receiver.acceptedState).arg(receiver.receivedSamples);
        return false;
    }
    return true;
}

bool exerciseLiveStop(QString *error) {
    const QString localName = QStringLiteral("obrii-live-stop-test-%1")
                                  .arg(QCoreApplication::applicationPid());
    QLocalServer server;
    if (!server.listen(localName)) {
        *error = server.errorString();
        return false;
    }
    VirtualTransmitterBackend transmitter;
    VirtualSignalEndpoint endpoint;
    endpoint.localName = localName;
    transmitter.setEndpoint(endpoint);
    TxConfiguration config;
    config.sampleRate = 48000;
    config.liveSource = true;
    if (!transmitter.start(config, error) || !server.waitForNewConnection(1000)) {
        *error = QStringLiteral("Live-stop receiver did not connect: %1").arg(*error);
        return false;
    }
    std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
    std::vector<std::complex<float>> samples(1024 * 1024, {0.1f, 0.2f});
    if (!transmitter.writeIq(samples.data(), static_cast<int>(samples.size()), error)) return false;
    QElapsedTimer timer;
    timer.start();
    transmitter.stop();
    if (timer.elapsed() > 1000 || transmitter.isRunning()) {
        *error = QStringLiteral("Live virtual transmitter stop blocked for %1 ms")
                     .arg(timer.elapsed());
        return false;
    }
    return true;
}

bool exerciseReconnect(QString *error) {
    ReconnectingLocalReceiver receiver;
    receiver.name = QStringLiteral("obrii-reconnect-test-%1")
                        .arg(QCoreApplication::applicationPid());
    receiver.start();
    if (!receiver.ready.tryAcquire(1, 3000)) {
        *error = QStringLiteral("Reconnecting receiver did not start");
        receiver.wait();
        return false;
    }
    VirtualTransmitterBackend transmitter;
    VirtualSignalEndpoint endpoint;
    endpoint.localName = receiver.name;
    transmitter.setEndpoint(endpoint);
    TxConfiguration config;
    config.sampleRate = 48000;
    config.liveSource = true;
    std::vector<std::complex<float>> samples(1024, {0.25f, -0.5f});
    for (int connection = 0; connection < 2; ++connection) {
        if (!transmitter.start(config, error) ||
            !transmitter.writeIq(samples.data(), static_cast<int>(samples.size()), error)) {
            receiver.wait();
            return false;
        }
        if (!receiver.frameReceived.tryAcquire(1, 3000)) {
            *error = QStringLiteral("Receiver did not get live frame %1")
                         .arg(connection + 1);
            transmitter.stop();
            receiver.wait();
            return false;
        }
        transmitter.stop();
    }
    receiver.wait();
    if (!receiver.error.isEmpty() || receiver.receivedFrames != 2) {
        *error = QStringLiteral("Live reconnect failed: %1; received=%2")
                     .arg(receiver.error).arg(receiver.receivedFrames);
        return false;
    }
    return true;
}

bool exerciseLateReceiver(QString *error) {
    const QString localName = QStringLiteral("obrii-late-receiver-test-%1")
                                  .arg(QCoreApplication::applicationPid());
    VirtualTransmitterBackend transmitter;
    VirtualSignalEndpoint endpoint;
    endpoint.localName = localName;
    transmitter.setEndpoint(endpoint);
    TxConfiguration config;
    config.sampleRate = 48000;
    config.liveSource = true;
    const std::complex<float> sample(0.25f, -0.5f);
    if (!transmitter.start(config, error) ||
        !transmitter.writeIq(&sample, 1, error) || !transmitter.isRunning()) {
        *error = QStringLiteral("Live TX failed without a receiver: %1").arg(*error);
        return false;
    }
    QLocalServer server;
    if (!server.listen(localName)) {
        *error = server.errorString();
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    while (!transmitter.pollConnection() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    if (!transmitter.isConnected() || !server.waitForNewConnection(1000)) {
        *error = QStringLiteral("Live TX did not reconnect to a late receiver");
        return false;
    }
    std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
    if (!transmitter.writeIq(&sample, 1, error) || !transmitter.drain(1000, error)) {
        return false;
    }
    VirtualSignalStreamParser parser;
    VirtualSignalFrame frame;
    if (!receiveFrame(socket.get(), &parser, &frame, error)) return false;
    socket->abort();
    socket.reset();
    timer.restart();
    while (transmitter.isConnected() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    timer.restart();
    while (!transmitter.pollConnection() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    const bool reconnected = transmitter.isConnected();
    const bool accepted = server.waitForNewConnection(1000);
    if (!reconnected || !accepted) {
        *error = QStringLiteral("Live TX did not reconnect after RX restart (client=%1, server=%2)")
                     .arg(reconnected).arg(accepted);
        return false;
    }
    socket.reset(server.nextPendingConnection());
    if (!transmitter.writeIq(&sample, 1, error) || !transmitter.drain(1000, error) ||
        !receiveFrame(socket.get(), &parser, &frame, error)) {
        return false;
    }
    transmitter.stop();
    return true;
}

bool exerciseHeartbeatAcknowledgement(QString *error) {
    const QString localName = QStringLiteral("obrii-heartbeat-test-%1")
                                  .arg(QCoreApplication::applicationPid());
    QLocalServer server;
    if (!server.listen(localName)) {
        *error = server.errorString();
        return false;
    }
    VirtualTransmitterBackend transmitter;
    VirtualSignalEndpoint endpoint;
    endpoint.localName = localName;
    transmitter.setEndpoint(endpoint);
    TxConfiguration config;
    config.liveSource = true;
    if (!transmitter.start(config, error) || !server.waitForNewConnection(1000)) {
        *error = QStringLiteral("Heartbeat receiver did not connect: %1").arg(*error);
        return false;
    }
    std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
    VirtualSignalStreamParser parser;
    QElapsedTimer timer;
    timer.start();
    int heartbeatCount = 0;
    while (timer.elapsed() < 6200) {
        transmitter.pollConnection();
        if (socket->bytesAvailable() > 0) parser.append(socket->readAll());
        VirtualSignalFrame frame;
        QString frameError;
        while (parser.takeFrame(&frame, &frameError)) {
            if (frame.protocolId != QStringLiteral("keepalive")) continue;
            ++heartbeatCount;
            VirtualSignalFrame reply;
            reply.sequence = frame.sequence;
            reply.streamId = frame.streamId;
            reply.protocolId = QStringLiteral("keepalive-ack");
            reply.payloadType = VirtualSignalPayloadType::ProtocolFrame;
            socket->write(VirtualSignalStreamParser::wrap(reply, error));
            socket->flush();
        }
        if (!frameError.isEmpty()) {
            *error = frameError;
            return false;
        }
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    const bool connected = transmitter.isConnected();
    transmitter.stop();
    if (!connected || heartbeatCount < 4) {
        *error = QStringLiteral("Heartbeat acknowledgement failed (connected=%1, beats=%2)")
                     .arg(connected).arg(heartbeatCount);
        return false;
    }
    return true;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (!virtualSignalValidNodeId(QStringLiteral("lab-tx-a")) ||
        virtualSignalValidNodeId(QStringLiteral("tx1")) ||
        virtualSignalValidNodeId(QStringLiteral("bad space")) ||
        virtualSignalValidNodeId(QString())) return 3;
    QString error;
    VirtualSignalFrame fixture;
    fixture.streamId = QStringLiteral("fragmentation-test");
    fixture.payloadType = VirtualSignalPayloadType::ComplexFloat32;
    const std::complex<float> fixtureSample(0.5f, -0.25f);
    fixture.payload = virtualSignalComplexFloat32Payload(&fixtureSample, 1);
    const QByteArray wrapped = VirtualSignalStreamParser::wrap(fixture, &error);
    VirtualSignalStreamParser splitParser;
    VirtualSignalFrame splitFrame;
    splitParser.append(wrapped.left(3));
    if (splitParser.takeFrame(&splitFrame, &error)) return 2;
    splitParser.append(wrapped.mid(3, 13));
    if (splitParser.takeFrame(&splitFrame, &error)) return 2;
    splitParser.append(wrapped.mid(16));
    if (!splitParser.takeFrame(&splitFrame, &error) || splitFrame.payload != fixture.payload) {
        qCritical() << "Fragmented virtual IQ frame FAIL" << error;
        return 2;
    }
    if (!exercise(VirtualSignalTransportMode::LocalIpc, &error) ||
        !exercise(VirtualSignalTransportMode::Tcp, &error) ||
        !exerciseThreadedLocal(&error) || !exerciseLiveStop(&error) ||
        !exerciseReconnect(&error) || !exerciseLateReceiver(&error) ||
        !exerciseHeartbeatAcknowledgement(&error)) {
        std::fprintf(stderr, "Virtual signal transport FAIL: %s\n",
                     error.toLocal8Bit().constData());
        std::fflush(stderr);
        qCritical().noquote() << "Virtual signal transport FAIL:" << error;
        return 1;
    }
    qInfo() << "Virtual signal local IPC + TCP self-test PASS";
    return 0;
}
