#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <cmath>
#include <complex>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0;
void require(bool okay, const char *text) { ++checks; if (!okay) throw std::runtime_error(text); }
quint16 freePort() {
    QTcpServer server; require(server.listen(QHostAddress::LocalHost, 0), "reserve port");
    return server.serverPort();
}
QJsonObject rpc(quint16 port, const QJsonObject &request) {
    QTcpSocket socket; socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(200)) return {};
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    QByteArray data; QElapsedTimer timer; timer.start();
    while (!data.contains('\n') && timer.elapsed() < 3000) {
        if (socket.bytesAvailable() || socket.waitForReadyRead(50)) data += socket.readAll();
    }
    return QJsonDocument::fromJson(data).object();
}
QJsonObject state(quint16 port) { return rpc(port, {{"action", "status"}}); }
void ready(quint16 port, QProcess &process) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 5000) {
        if (state(port).value("ok").toBool()) return;
        if (process.state() == QProcess::NotRunning) break;
        QCoreApplication::processEvents(); QThread::msleep(10);
    }
    std::cerr << process.readAllStandardOutput().constData() << process.readAllStandardError().constData();
    require(false, "live TX readiness");
}
void finish(quint16 port, QProcess &process) {
    require(rpc(port, {{"action", "stop"}}).value("ok").toBool(), "stop command");
    require(process.waitForFinished(5000) && process.exitCode() == 0, "graceful live TX exit");
    for (const auto &line : process.readAllStandardOutput().split('\n')) {
        if (!line.trimmed().isEmpty()) require(QJsonDocument::fromJson(line).isObject(), "JSON-only event output");
    }
}
QJsonObject metadata(const QString &path) {
    QFile file(path + ".json"); require(file.open(QIODevice::ReadOnly), "metadata exists");
    return QJsonDocument::fromJson(file.readAll()).object();
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    QTemporaryDir temporary;
    QProcess process;
    QProcess receiver;
    try {
        const QString cli = QString::fromLocal8Bit(argv[1]);
        const auto launch = [&](const QStringList &words, quint16 port) {
            QStringList args{"module", "tx"}; args += words;
            args << "--tx-control-port" << QString::number(port) << "--seconds" << "10";
            process.start(cli, args);
            require(process.waitForStarted(3000), "start live TX CLI"); ready(port, process);
        };
        const QString input = temporary.filePath("input.pcm");
        QFile pcm(input); require(pcm.open(QIODevice::WriteOnly), "create PCM fixture");
        QByteArray bytes(48000 * 4, Qt::Uninitialized);
        for (int i = 0; i < 96000; ++i) {
            const auto value = static_cast<qint16>(std::lround(8192 * std::sin(2 * 3.14159265358979323846 * 1000 * i / 48000)));
            qToLittleEndian(value, bytes.data() + i * 2);
        }
        pcm.write(bytes); pcm.close();
        const auto port = freePort();
        const QString output = temporary.filePath("microphone.cf32");
        launch({"microphone", "--tx-sink", "file", "--output", output, "--pcm-input", input,
            "--freq", "1M", "--rate", "48k", "--tx-watchdog-ms", "400"}, port);
        auto snapshot = state(port);
        require(snapshot.value("armed").toBool() && !snapshot.value("ptt").toBool() &&
            snapshot.value("pcmFrames").toInt() == 0 && !snapshot.value("captureRunning").toBool(), "initial armed state captures nothing");
        require(QFileInfo(output).size() == 0, "no IQ before PTT");
        require(!rpc(port, {{"action", "ptt"}, {"enabled", "true"}}).value("ok").toBool(), "reject non-boolean PTT");
        require(rpc(port, {{"action", "ptt"}, {"enabled", true}}).value("ok").toBool(), "PTT on");
        QThread::msleep(120);
        snapshot = state(port);
        require(snapshot.value("ptt").toBool() && snapshot.value("pcmFrames").toInt() > 0 &&
            snapshot.value("micPeakDbfs").toDouble() > -20, "PCM and level metrics");
        require(rpc(port, {{"action", "ptt"}, {"enabled", false}}).value("ok").toBool(), "PTT off");
        const int firstCount = state(port).value("iqSamples").toInt();
        QThread::msleep(70);
        require(state(port).value("iqSamples").toInt() == firstCount, "released PTT produces no IQ");
        rpc(port, {{"action", "ptt"}, {"enabled", true}}); QThread::msleep(90);
        require(state(port).value("iqSamples").toInt() > firstCount, "restart without relaunch");
        rpc(port, {{"action", "disarm"}});
        require(!rpc(port, {{"action", "ptt"}, {"enabled", true}}).value("ok").toBool(), "disarmed cannot key");
        rpc(port, {{"action", "arm"}}); rpc(port, {{"action", "gain"}, {"value", 2.0}});
        rpc(port, {{"action", "ptt"}, {"enabled", true}}); QThread::msleep(480);
        snapshot = state(port);
        require(!snapshot.value("ptt").toBool() && snapshot.value("lastReleaseReason") == "watchdog", "watchdog releases held PTT");
        finish(port, process);
        const auto meta = metadata(output);
        require(meta.value("complete").toBool() && meta.value("segments").toArray().size() == 3 &&
            meta.value("samples").toDouble() * 8 == QFileInfo(output).size(), "gated capture segments and truthful length");
        QFile iq(output); require(iq.open(QIODevice::ReadOnly), "read modulated IQ");
        const auto raw = iq.readAll();
        double energy1000 = 0, energy2000 = 0;
        std::complex<double> sum1000{}, sum2000{};
        std::complex<float> previous{};
        const int samples = (std::min)(4000, raw.size() / 8);
        for (int i = 0; i < samples; ++i) {
            std::complex<float> current;
            std::memcpy(&current, raw.constData() + i * 8, 8);
            if (i > 240) {
                const double recovered = std::arg(current * std::conj(previous));
                sum1000 += recovered * std::polar(1.0, -2 * 3.14159265358979323846 * 1000 * i / 48000);
                sum2000 += recovered * std::polar(1.0, -2 * 3.14159265358979323846 * 2000 * i / 48000);
            }
            previous = current;
        }
        energy1000 = std::norm(sum1000); energy2000 = std::norm(sum2000);
        require(energy1000 > energy2000 * 100, "voice frequency is preserved by streaming FM");

        const auto cwPort = freePort(); const auto cwPath = temporary.filePath("key.cf32");
        launch({"manual-cw", "--tx-sink", "file", "--output", cwPath, "--freq", "1M"}, cwPort);
        require(rpc(cwPort, {{"action", "key"}, {"enabled", true}}).value("ok").toBool(), "CW key down");
        QThread::msleep(85); rpc(cwPort, {{"action", "key"}, {"enabled", false}});
        finish(cwPort, process);
        QFile cwFile(cwPath); cwFile.open(QIODevice::ReadOnly); const auto cwBytes = cwFile.readAll();
        require(cwBytes.size() > 4800 && cwBytes.size() % 8 == 0, "manual CW IQ emitted");
        std::complex<float> first, last;
        std::memcpy(&first, cwBytes.constData(), 8); std::memcpy(&last, cwBytes.constData() + cwBytes.size() - 8, 8);
        require(std::abs(first) < 0.001 && std::abs(last) < 0.001, "CW edge ramps");
        const auto virtualPort = freePort(), txPort = freePort(), rxPort = freePort();
        launch({"microphone", "--tx-sink", "virtual", "--pcm-input", input, "--freq", "1M", "--rate", "48k",
            "--virtual-host", "127.0.0.1", "--virtual-port", QString::number(virtualPort)}, txPort);
        require(rpc(txPort, {{"action", "ptt"}, {"enabled", true}}).value("ok").toBool(), "virtual PTT before RX starts");
        QThread::msleep(80);
        require(state(txPort).value("pcmFrames").toInt() > 0 && !state(txPort).value("connected").toBool(), "TX source advances without RX");
        const auto startRx = [&] {
            receiver.start(cli, {"run", "--backend", "virtual", "--virtual-rx-transport", "tcp",
                "--virtual-rx-port", QString::number(virtualPort), "--freq", "1M", "--rate", "48k",
                "--control-port", QString::number(rxPort), "--spectrum", "--fft", "1024"});
            require(receiver.waitForStarted(3000), "launch virtual receiver"); ready(rxPort, receiver);
        };
        const auto awaitSamples = [&] {
            QElapsedTimer timer; timer.start();
            while (timer.elapsed() < 3000) {
                if (state(rxPort).value("samples").toInt() > 0) return true;
                QThread::msleep(20);
            }
            return false;
        };
        const auto stopRx = [&] {
            require(rpc(rxPort, {{"action", "quit"}}).value("ok").toBool(), "stop virtual receiver");
            require(receiver.waitForFinished(5000) && receiver.exitCode() == 0, "virtual receiver exits");
        };
        startRx(); require(awaitSamples(), "independent TX connects when RX appears");
        stopRx(); startRx();
        require(awaitSamples(), "TX reconnects after RX restarts without TX restart");
        stopRx(); finish(txPort, process);
        process.start(cli, {"module", "tx", "microphone", "--tx-sink", "hackrf", "--freq", "145M"});
        require(process.waitForFinished(5000) && process.exitCode() != 0 && process.readAllStandardOutput().contains("rf-confirm"), "RF fails closed without confirmation");
        std::cout << checks << " live TX checks passed (PCM/PTT, restart, watchdog, FM, CW, RF gate); no microphone or RF device opened\n";
        return 0;
    } catch (const std::exception &error) {
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(3000); }
        if (receiver.state() != QProcess::NotRunning) { receiver.kill(); receiver.waitForFinished(3000); }
        std::cerr << "Live TX selftest: " << error.what() << '\n'
                  << process.readAllStandardOutput().constData() << process.readAllStandardError().constData();
        return 1;
    }
}
