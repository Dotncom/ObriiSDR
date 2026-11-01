#include "virtualsignaltransport.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstdio>
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
    std::fprintf(stdout, "Two TX to two RX virtual IQ forwarding passed\n");
    return finish(0);
}
