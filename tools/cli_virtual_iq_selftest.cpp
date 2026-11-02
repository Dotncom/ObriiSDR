#include "clivirtualiqsource.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpSocket>
#include <QThread>
#include <complex>
#include <cstdio>

namespace {
template<class Predicate> bool wait(Predicate test, int timeout = 3000) {
    QElapsedTimer timer; timer.start();
    while (!test() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    return test();
}
VirtualSignalFrame signal(quint32 sequence) {
    VirtualSignalFrame frame;
    frame.streamId = QStringLiteral("cli-test"); frame.protocolId = QStringLiteral("raw-iq");
    frame.sequence = sequence; frame.timestampNs = 1000000000LL;
    frame.sampleRate = 48000; frame.centerFrequencyHz = 1000000;
    frame.payloadType = VirtualSignalPayloadType::ComplexFloat32;
    QVector<std::complex<float>> samples(1024, {0.25f, -0.25f});
    frame.payload = virtualSignalComplexFloat32Payload(samples.constData(), samples.size());
    return frame;
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTcpServer reservation; reservation.listen(QHostAddress::LocalHost, 0);
    const quint16 port = reservation.serverPort(); reservation.close();
    VirtualSignalEndpoint endpoint;
    endpoint.mode = VirtualSignalTransportMode::Tcp; endpoint.port = port;
    CliVirtualIqSource source;
    QString error;
    if (!source.start(endpoint, 48000, 1000000, 1024 * 1024, &error)) return 1;
    QTcpSocket tx;
    tx.connectToHost(QHostAddress::LocalHost, port);
    if (!tx.waitForConnected(1000)) return 2;
    auto frame = signal(0);
    tx.write(VirtualSignalStreamParser::wrap(frame)); tx.flush();
    if (!wait([&] { return source.status().value("frames").toInt() == 1; })) return 3;
    float iq[2048];
    if (source.read(iq, 1024) != 1024 || iq[0] != 0.25f || !source.takeDiscontinuity()) return 4;
    for (int i = 1; i <= 150; ++i) {
        frame.sequence = i;
        tx.write(VirtualSignalStreamParser::wrap(frame));
    }
    tx.flush();
    if (!wait([&] { return source.status().value("frames").toInt() == 151; })) return 5;
    if (source.status().value("queuedBytes").toDouble() > 1024 * 1024 ||
        source.status().value("droppedSamples").toInt() == 0) return 6;
    tx.abort(); wait([&] { return !source.status().value("connected").toBool(); });
    source.stop();

    QTcpServer controlPort; controlPort.listen(QHostAddress::LocalHost, 0);
    const auto control = controlPort.serverPort(); controlPort.close();
    QProcess cli;
    const QString executable = QDir(app.applicationDirPath()).absoluteFilePath(
#ifdef _WIN32
        "../../runtime/ObriiCli.exe"
#else
        "ObriiCli"
#endif
    );
    cli.start(executable, {"run", "--backend", "virtual", "--virtual-rx-transport", "tcp",
        "--virtual-rx-port", QString::number(port), "--control-port", QString::number(control),
        "--rate", "48000", "--freq", "1000000", "--seconds", "0.04", "--spectrum", "--fft", "1024"});
    if (!cli.waitForStarted(3000)) return 7;
    const auto finish = [&](int code) {
        if (cli.state() != QProcess::NotRunning) { cli.kill(); cli.waitForFinished(); }
        return code;
    };
    bool connected = wait([&] {
        if (tx.state() == QAbstractSocket::ConnectedState) return true;
        tx.abort(); tx.connectToHost(QHostAddress::LocalHost, port);
        return tx.waitForConnected(30);
    });
    if (!connected) return finish(8);
    tx.write(VirtualSignalStreamParser::wrap(signal(0))); tx.flush();
    tx.waitForBytesWritten(200);
    QThread::msleep(50);
    tx.abort();
    QThread::msleep(50);
    tx.connectToHost(QHostAddress::LocalHost, port);
    if (!tx.waitForConnected(1000)) return finish(9);
    tx.write(VirtualSignalStreamParser::wrap(signal(0))); tx.flush();
    if (!cli.waitForFinished(5000)) return finish(10);
    const QByteArray output = cli.readAllStandardOutput();
    bool correct = false;
    for (const QByteArray &line : output.split('\n')) {
        const QJsonObject event = QJsonDocument::fromJson(line).object();
        if (event.value("event") == "stopped" && event.value("samples").toInt() == 2048) correct = true;
    }
    if (cli.exitCode() || !correct) {
        std::fprintf(stderr, "%s\n%s\n", output.constData(), cli.readAllStandardError().constData());
        return finish(11);
    }
    std::puts("CLI virtual RX bounded queue, independent start and reconnect passed");
    return 0;
}
