#include "virtualsignaltransport.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryFile>
#include <QThread>

#include <cstdio>
#include <cmath>
#include <complex>
#include <memory>

namespace {
bool receive(QTcpSocket *socket, VirtualSignalStreamParser *parser, VirtualSignalFrame *frame) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        parser->append(socket->readAll());
        QString error;
        if (parser->takeFrame(frame, &error)) return true;
        if (!error.isEmpty()) return false;
        socket->waitForReadyRead(50);
    }
    return false;
}

bool connectPublisher(QTcpSocket *socket, quint16 port) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        socket->connectToHost(QHostAddress::LocalHost, port);
        if (socket->waitForConnected(200)) return true;
        socket->abort();
    }
    return false;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTcpServer firstRx, secondRx, reservation;
    if (!firstRx.listen(QHostAddress::LocalHost, 0) ||
        !secondRx.listen(QHostAddress::LocalHost, 0) ||
        !reservation.listen(QHostAddress::LocalHost, 0)) return 1;
    const quint16 hubPort = reservation.serverPort();
    reservation.close();
    QProcess hub;
#ifdef _WIN32
    const QString program = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("../../runtime/ObriiIqHub.exe"));
#else
    const QString program = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("ObriiIqHub"));
#endif
    hub.start(program, {QStringLiteral("--listen=127.0.0.1:%1").arg(hubPort),
                        QStringLiteral("--rx=127.0.0.1:%1").arg(firstRx.serverPort()),
                        QStringLiteral("--rx=127.0.0.1:%1").arg(secondRx.serverPort())});
    if (!hub.waitForStarted(3000)) return 2;
    const auto finish = [&hub](int code) {
        hub.kill();
        hub.waitForFinished(3000);
        return code;
    };
    if (!firstRx.waitForNewConnection(3000) || !secondRx.waitForNewConnection(3000))
        return finish(3);
    std::unique_ptr<QTcpSocket> first(firstRx.nextPendingConnection());
    std::unique_ptr<QTcpSocket> second(secondRx.nextPendingConnection());
    if (!first || !second) return finish(4);
    QTcpSocket tx1, tx2;
    if (!connectPublisher(&tx1, hubPort) || !connectPublisher(&tx2, hubPort))
        return finish(5);
    VirtualSignalFrame sent;
    sent.streamId = QStringLiteral("hub-network-selftest");
    sent.sampleRate = 48000;
    sent.centerFrequencyHz = 1000000;
    sent.payloadType = VirtualSignalPayloadType::ComplexFloat32;
    sent.payload = QByteArray(16, '\0');
    sent.sequence = 7;
    const QByteArray firstWire = VirtualSignalStreamParser::wrap(sent);
    sent.sequence = 9;
    const QByteArray secondWire = VirtualSignalStreamParser::wrap(sent);
    if (tx1.write(firstWire) != firstWire.size() ||
        tx2.write(secondWire) != secondWire.size())
        return finish(6);
    tx1.flush();
    tx2.flush();
    VirtualSignalStreamParser firstParser, secondParser;
    VirtualSignalFrame a1, a2, b1, b2;
    if (!receive(first.get(), &firstParser, &a1) || !receive(first.get(), &firstParser, &a2) ||
        !receive(second.get(), &secondParser, &b1) || !receive(second.get(), &secondParser, &b2))
        return finish(7);
    if (a1.sequence != 0 || a2.sequence != 1 || b1.sequence != 0 || b2.sequence != 1 ||
        a1.metadata.value(QStringLiteral("hubSource")).toString() ==
            a2.metadata.value(QStringLiteral("hubSource")).toString() ||
        a1.payload != sent.payload || b2.payload != sent.payload)
        return finish(8);
    finish(0);
    first.reset();
    second.reset();
    QTemporaryFile scenario;
    if (!scenario.open() || scenario.write(
            "{\"version\":1,\"defaultRoute\":false,\"links\":["
            "{\"tx\":\"speaker\",\"rx\":\"front\",\"delayMs\":20,\"gainDb\":-6}]}"
        ) < 0 || !scenario.flush()) return 9;
    hub.start(program, {QStringLiteral("--listen=127.0.0.1:%1").arg(hubPort),
                        QStringLiteral("--rx=front@127.0.0.1:%1").arg(firstRx.serverPort()),
                        QStringLiteral("--rx=back@127.0.0.1:%1").arg(secondRx.serverPort()),
                        QStringLiteral("--scenario=%1").arg(scenario.fileName())});
    if (!hub.waitForStarted(3000) ||
        !firstRx.waitForNewConnection(3000) || !secondRx.waitForNewConnection(3000))
        return finish(10);
    first.reset(firstRx.nextPendingConnection());
    second.reset(secondRx.nextPendingConnection());
    QTcpSocket selectedTx;
    sent.metadata.insert(QStringLiteral("hubNodeId"), QStringLiteral("speaker"));
    const QByteArray namedWire = VirtualSignalStreamParser::wrap(sent);
    if (!connectPublisher(&selectedTx, hubPort) ||
        selectedTx.write(namedWire) != namedWire.size()) return finish(11);
    selectedTx.flush();
    VirtualSignalStreamParser selectedParser;
    VirtualSignalFrame selectedFrame;
    if (!receive(first.get(), &selectedParser, &selectedFrame) ||
        second->waitForReadyRead(150) || selectedFrame.metadata.value(QStringLiteral("hubSource")).toString() !=
            QStringLiteral("speaker")) return finish(12);
    const QString firstSession = selectedFrame.metadata.value(QStringLiteral("hubSessionId")).toString();
    if (firstSession.isEmpty()) return finish(13);
    QTcpSocket duplicate;
    if (!connectPublisher(&duplicate, hubPort) || duplicate.write(namedWire) != namedWire.size())
        return finish(14);
    duplicate.flush();
    if (!duplicate.waitForDisconnected(1000)) return finish(15);
    if (selectedTx.write(namedWire) != namedWire.size()) return finish(16);
    selectedTx.flush();
    if (!receive(first.get(), &selectedParser, &selectedFrame) ||
        selectedFrame.metadata.value(QStringLiteral("hubSessionId")).toString() != firstSession)
        return finish(17);
    selectedTx.disconnectFromHost();
    if (selectedTx.state() != QAbstractSocket::UnconnectedState &&
        !selectedTx.waitForDisconnected(1000)) return finish(18);
    QThread::msleep(100);
    QTcpSocket replacement;
    if (!connectPublisher(&replacement, hubPort) ||
        replacement.write(namedWire) != namedWire.size()) return finish(19);
    replacement.flush();
    if (!receive(first.get(), &selectedParser, &selectedFrame) ||
        selectedFrame.metadata.value(QStringLiteral("hubSource")).toString() != QStringLiteral("speaker") ||
        selectedFrame.metadata.value(QStringLiteral("hubSessionId")).toString() == firstSession ||
        !selectedFrame.discontinuity) return finish(20);
    finish(0);
    QTcpServer mixedRx;
    if (!mixedRx.listen(QHostAddress::LocalHost, 0)) return 21;
    hub.start(program, {QStringLiteral("--listen=127.0.0.1:%1").arg(hubPort),
                        QStringLiteral("--rx=mixed@127.0.0.1:%1").arg(mixedRx.serverPort()),
                        QStringLiteral("--mix-rate=8000"), QStringLiteral("--mix-center=1000000"),
                        QStringLiteral("--mix-block-samples=128"),
                        QStringLiteral("--mix-latency-ms=100")});
    if (!hub.waitForStarted(3000) || !mixedRx.waitForNewConnection(3000)) return finish(22);
    std::unique_ptr<QTcpSocket> mixed(mixedRx.nextPendingConnection());
    QTcpSocket mixTx1, mixTx2;
    if (!connectPublisher(&mixTx1, hubPort) || !connectPublisher(&mixTx2, hubPort)) return finish(23);
    const QVector<std::complex<float>> firstIq(128, {0.2f, 0.0f});
    const QVector<std::complex<float>> secondIq(128, {0.3f, 0.0f});
    VirtualSignalFrame mixFrame = sent;
    mixFrame.sampleRate = 8000;
    mixFrame.timestampNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
    mixFrame.metadata.remove(QStringLiteral("hubNodeId"));
    mixFrame.payload = virtualSignalComplexFloat32Payload(firstIq.constData(), firstIq.size());
    const QByteArray mixWire1 = VirtualSignalStreamParser::wrap(mixFrame);
    mixFrame.payload = virtualSignalComplexFloat32Payload(secondIq.constData(), secondIq.size());
    const QByteArray mixWire2 = VirtualSignalStreamParser::wrap(mixFrame);
    if (mixTx1.write(mixWire1) != mixWire1.size() ||
        mixTx2.write(mixWire2) != mixWire2.size()) return finish(24);
    mixTx1.flush();
    mixTx2.flush();
    VirtualSignalStreamParser mixParser;
    VirtualSignalFrame mixedFrame;
    QVector<float> mixedIq;
    if (!receive(mixed.get(), &mixParser, &mixedFrame) ||
        !virtualSignalDecodeComplexFloat32(mixedFrame.payload, &mixedIq) ||
        std::fabs(mixedIq.first() - 0.5f) > 0.001f ||
        mixedFrame.metadata.value(QStringLiteral("hubOverlapSamples")).toInt() != 128)
        return finish(25);
    std::fprintf(stdout, "Virtual IQ fan-out, routing and sample mix passed\n");
    return finish(0);
}
