#include "dmrdecoder.h"
#include "gophertrunkbridge.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTcpServer>
#include <QThread>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <functional>

namespace {
QString flipped(QString dibits) {
    for (int i = 0; i < dibits.size(); ++i) {
        dibits[i] = QLatin1Char(static_cast<char>('0' + ((dibits.at(i).unicode() - '0' + 2) & 3)));
    }
    return dibits;
}
bool waitFor(const std::function<bool()> &ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}
bool testPolarity() {
    const QString voice = QString(54, '0') + "133313311131311113313331" + QString(54, '1');
    const QString data = flipped(voice);
    for (bool oldVoiceFlip : {false, true}) {
        if (GopherTrunkBridge::normalizedBurstDibits(data, true, oldVoiceFlip) != data ||
            GopherTrunkBridge::normalizedBurstDibits(flipped(data), true, oldVoiceFlip) != data) {
            qCritical() << "PI normalization inherited unrelated voice polarity";
            return false;
        }
    }
    return GopherTrunkBridge::normalizedBurstDibits(flipped(voice), false, true) == voice &&
           GopherTrunkBridge::normalizedBurstDibits(QString(132, '0'), true, true).isEmpty();
}
int exportPcm(const QString &input, int rate, const QString &output) {
    if (rate != 24000 && rate != 48000) return 2;
    QFile pcm(input);
    QFile target(output);
    if (!pcm.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    QTcpServer server;
    QUdpSocket probe;
    if (!server.listen(QHostAddress::LocalHost, 0) || !probe.bind(QHostAddress(QHostAddress::LocalHost), quint16(0))) return 2;
    const quint16 audioPort = probe.localPort();
    probe.close();
    GopherTrunkBridge bridge;
    bridge.configure(false, QString(), server.serverPort(), audioPort);
    bridge.setEnabled(true);
    if (!waitFor([&]() { return server.hasPendingConnections(); })) return 2;
    QTcpSocket *socket = server.nextPendingConnection();
    QByteArray captured;
    auto drain = [&]() {
        QCoreApplication::processEvents();
        captured.append(socket->readAll());
    };
    DmrDecoder decoder;
    decoder.configure(rate);
    decoder.setLabHints(true, -1, -1, -1, -1);
    int bursts = 0;
    int data = 0;
    int metadataFrames = 0;
    while (!pcm.atEnd()) {
        const auto result = decoder.processPcmFrame(pcm.read(rate * 2 * 60 / 1000), rate);
        for(const auto &payload:result.ambePayloads){
            if(payload.burstIndex<0||payload.burstIndex>5||payload.frameIndex<0||payload.frameIndex>2||
               payload.timeslot<0||payload.timeslot>2){qCritical()<<"Invalid native privacy RF metadata";return 3;}
            ++metadataFrames;
        }
        for (const auto &b : result.dibitBursts) {
            bridge.sendDibitBurst(b.dibits, b.burstKind, b.dataType, b.dataTypeName, b.slotTypeErrors,
                b.sample, b.baseDibit, b.burstIndex, b.cadenceSymbols, b.colorCode,
                b.timeslot, b.timeslotSource, b.cachDecoded, b.cachChannel, b.cachLcss,
                b.cachMapName, b.cachPayloadBits, b.ambe49Payloads);
            ++bursts;
            if (b.burstKind == "data") ++data;
        }
        drain();
    }
    waitFor([&]() { drain(); return bridge.findChildren<QTcpSocket *>().first()->bytesToWrite() == 0; });
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 100) { drain(); QThread::msleep(1); }
    bridge.setEnabled(false);
    drain();
    if (target.write(captured) != captured.size()) return 2;
    qInfo() << "Exported burst packets" << bursts << "PI candidates" << data << "bytes" << captured.size()
            << "native RF metadata frames" << metadataFrames;
    return 0;
}

bool testStreamPolarity() {
    QTcpServer server;
    QUdpSocket probe;
    if (!server.listen(QHostAddress::LocalHost, 0) ||
        !probe.bind(QHostAddress(QHostAddress::LocalHost), quint16(0))) return false;
    const auto audioPort = probe.localPort();
    probe.close();
    GopherTrunkBridge bridge;
    bridge.configure(false, QString(), server.serverPort(), audioPort);
    bridge.configurePrivacy("ARC4", 1, "0102030405", true);
    bridge.setEnabled(true);
    if (!waitFor([&]() { return server.hasPendingConnections(); })) return false;
    auto *socket = server.nextPendingConnection();
    QByteArray captured;
    auto drain = [&]() { captured += socket->readAll(); return captured.endsWith('\n'); };
    const QString voice = QString(54, '0') + "133313311131311113313331" + QString(54, '1');
    auto send = [&](const QString &dibits, int burst) {
        bridge.sendDibitBurst(dibits, "voice", -1, QString(), -1, 0, 0, burst,
            288, 1, 1, "test", false, -1, -1, QString(), QString(), {});
    };
    send(voice, 2); // A coincidental B-F sync must not lock the stream.
    send(flipped(voice), 0);
    send(voice, 0); // A subsequent valid A may change polarity with a key selected.
    if (!waitFor(drain)) return false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 50) { QCoreApplication::processEvents(); drain(); QThread::msleep(1); }
    int count = 0;
    for (const auto &line : captured.split('\n')) {
        const auto packet = QJsonDocument::fromJson(line).object();
        if (!packet.contains("dibits")) continue;
        if (packet.value("burst").toInt() != 0 || packet.value("dibits").toString() != voice) return false;
        ++count;
    }
    bridge.setEnabled(false);
    return count == 2;
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (!testPolarity() || !testStreamPolarity()) return 1;
    const auto args = app.arguments();
    if (args.size() == 5 && args.at(1) == "--export-pcm") {
        // Export at the usual 24 kHz discriminator rate through the real TCP bridge.
        return exportPcm(args.at(2), args.at(3).toInt(), args.at(4));
    }
    qInfo() << "Gopher bridge polarity tests: 0 failures";
    return 0;
}
