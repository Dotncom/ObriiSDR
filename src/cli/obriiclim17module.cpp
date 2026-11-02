#include "m17adapter.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

namespace {
void report(const QJsonObject &object) {
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
    std::fflush(stdout);
}

int fail(const QString &error) {
    report({{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}});
    return 1;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (args.size() >= 1 && args[0] == QStringLiteral("decode")) {
        if (args.size() != 2) return fail(QStringLiteral("Usage: m17 decode FILE.m17sym"));
        QFile input(args[1]);
        if (!input.open(QIODevice::ReadOnly) || input.size() > 36 * 192)
            return fail(QStringLiteral("Cannot read M17 symbol file"));
        M17DecodedPacket packet;
        QString error;
        if (!M17Adapter::decodePacketSymbols(input.readAll(), &packet, &error)) return fail(error);
        QByteArray textPayload = packet.payload;
        if (textPayload.startsWith(static_cast<char>(0x05)) && textPayload.endsWith('\0')) {
            textPayload.remove(0, 1);
            textPayload.chop(1);
        }
        report({{QStringLiteral("ok"), true}, {QStringLiteral("event"), QStringLiteral("m17Packet")},
                {QStringLiteral("source"), packet.source},
                {QStringLiteral("destination"), packet.destination},
                {QStringLiteral("can"), packet.channelAccessNumber},
                {QStringLiteral("packetFrames"), packet.packetFrameCount},
                {QStringLiteral("payloadHex"), QString::fromLatin1(packet.payload.toHex())},
                {QStringLiteral("payloadText"), QString::fromUtf8(textPayload)}});
        return 0;
    }
    if (args.size() >= 1 && args[0] == QStringLiteral("encode")) {
        if (args.size() != 5 && args.size() != 6)
            return fail(QStringLiteral("Usage: m17 encode SOURCE DESTINATION MESSAGE FILE.m17sym [CAN]"));
        bool okay = true;
        const int can = args.size() == 6 ? args[5].toInt(&okay) : 0;
        if (!okay || can < 0 || can > 15) return fail(QStringLiteral("M17 CAN must be 0..15"));
        QByteArray payload = args[3].toUtf8();
        payload.prepend(static_cast<char>(0x05));
        payload.append('\0');
        QString error;
        const M17PacketSymbols packet = M17Adapter::buildPacket(args[1], args[2], payload, can, &error);
        if (packet.symbols.isEmpty()) return fail(error);
        QFile output(args[4]);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail(output.errorString());
        const QByteArray symbols(reinterpret_cast<const char *>(packet.symbols.constData()),
                                 packet.symbols.size());
        if (output.write(symbols) != symbols.size()) {
            output.close();
            output.remove();
            return fail(QStringLiteral("Cannot write complete M17 symbol file"));
        }
        report({{QStringLiteral("ok"), true}, {QStringLiteral("event"), QStringLiteral("m17Encoded")},
                {QStringLiteral("path"), QFileInfo(output.fileName()).absoluteFilePath()},
                {QStringLiteral("symbolRate"), packet.symbolRate},
                {QStringLiteral("symbols"), packet.symbols.size()},
                {QStringLiteral("packetFrames"), packet.packetFrameCount},
                {QStringLiteral("durationSeconds"),
                    static_cast<double>(packet.symbols.size()) / packet.symbolRate}});
        return 0;
    }
    return fail(QStringLiteral("Usage: m17 encode SOURCE DESTINATION MESSAGE FILE.m17sym [CAN] | m17 decode FILE.m17sym"));
}
