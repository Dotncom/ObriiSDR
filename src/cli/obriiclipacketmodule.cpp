#include "climoduleprotocol.h"

#include "ax25codec.h"
#include "ax25livedecoder.h"
#include "kisscodec.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>

#include <cstdio>
#include <memory>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
bool readExact(char *target, std::size_t bytes) {
    std::size_t offset = 0;
    while (offset < bytes) {
        const std::size_t count = std::fread(target + offset, 1, bytes - offset, stdin);
        if (!count) return false;
        offset += count;
    }
    return true;
}

void send(const QJsonObject &object) {
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
    std::fflush(stdout);
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3 || QByteArray(argv[1]) != "rx" || QByteArray(argv[2]).toUInt() > 65535) {
        std::fprintf(stderr, "Usage: ObriiCliPacketModule rx KISS_PORT\n");
        return 2;
    }
    bool okay = false;
    const int port = QByteArray(argv[2]).toInt(&okay);
    if (!okay || port < 1 || port > 65535) return 2;
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) < 0 ||
        _setmode(_fileno(stdout), _O_BINARY) < 0) return 2;
#endif
    QTcpServer kissServer;
    if (!kissServer.listen(QHostAddress::LocalHost, static_cast<quint16>(port))) {
        std::fprintf(stderr, "KISS listen failed: %s\n", kissServer.errorString().toLocal8Bit().constData());
        return 2;
    }
    QList<QPointer<QTcpSocket>> clients;
    QObject::connect(&kissServer, &QTcpServer::newConnection, &app, [&] {
        while (QTcpSocket *socket = kissServer.nextPendingConnection()) {
            clients.append(socket);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] { socket->readAll(); });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    std::unique_ptr<Ax25LiveDecoder> decoder;
    const auto startDecoder = [&] {
        if (decoder) decoder->stop();
        decoder = std::make_unique<Ax25LiveDecoder>();
        QObject::connect(decoder.get(), &Ax25LiveDecoder::frameDecoded, &app,
                         [&](const QByteArray &frame) {
            Ax25UiFrame ui;
            QString error;
            QJsonObject event{{QStringLiteral("event"), QStringLiteral("ax25")},
                              {QStringLiteral("module"), QStringLiteral("ax25")},
                              {QStringLiteral("utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                              {QStringLiteral("rawBase64"), QString::fromLatin1(frame.toBase64())}};
            if (Ax25Codec::decodeUiFrame(frame, &ui, false, &error)) {
                event.insert(QStringLiteral("source"), Ax25Codec::addressText(ui.source));
                event.insert(QStringLiteral("destination"), Ax25Codec::addressText(ui.destination));
                event.insert(QStringLiteral("information"), QString::fromUtf8(ui.information));
                event.insert(QStringLiteral("informationBase64"), QString::fromLatin1(ui.information.toBase64()));
                event.insert(QStringLiteral("aprsCandidate"), ui.destination.callsign.startsWith(QStringLiteral("APRS")) ||
                    ui.information.startsWith('!') || ui.information.startsWith('=') ||
                    ui.information.startsWith('/') || ui.information.startsWith('@'));
            }
            send(event);
            const QByteArray wire = KissCodec::encode({0, 0, frame});
            for (auto it = clients.begin(); it != clients.end(); ) {
                if (it->isNull() || (*it)->state() != QAbstractSocket::ConnectedState) {
                    it = clients.erase(it);
                } else {
                    if ((*it)->bytesToWrite() > 1024 * 1024) (*it)->disconnectFromHost();
                    else (*it)->write(wire);
                    ++it;
                }
            }
        });
        decoder->start();
    };
    startDecoder();
    send({{QStringLiteral("event"), QStringLiteral("status")},
          {QStringLiteral("module"), QStringLiteral("ax25")},
          {QStringLiteral("text"), QStringLiteral("RX-only KISS on 127.0.0.1:%1").arg(port)}});

    for (;;) {
        uchar header[12];
        if (!readExact(reinterpret_cast<char *>(header), sizeof(header))) break;
        const quint32 kind = qFromLittleEndian<quint32>(header);
        const quint32 rate = qFromLittleEndian<quint32>(header + 4);
        const quint32 size = qFromLittleEndian<quint32>(header + 8);
        if (size > CliModuleProtocol::MaximumPayloadBytes) return 2;
        QByteArray payload(static_cast<int>(size), Qt::Uninitialized);
        if (size && !readExact(payload.data(), size)) return 1;
        if (kind == CliModuleProtocol::Reset) startDecoder();
        else if (kind == CliModuleProtocol::Pcm16 && rate == 48000 && size % 2 == 0)
            decoder->submitPcm16(payload, static_cast<int>(rate));
        else if (kind != CliModuleProtocol::Configure) return 2;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    decoder->stop();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::feof(stdin) ? 0 : 1;
}
