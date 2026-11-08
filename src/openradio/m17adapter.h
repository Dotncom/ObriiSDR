#ifndef M17ADAPTER_H
#define M17ADAPTER_H

#include <QByteArray>
#include <QString>
#include <QVector>

struct M17PacketSymbols {
    QByteArray linkSetupFrame;
    QVector<qint8> symbols;
    int packetFrameCount = 0;
    int symbolRate = 4800;
};

struct M17DecodedPacket {
    QString source;
    QString destination;
    int channelAccessNumber = -1;
    QByteArray payload;
    int packetFrameCount = 0;
};

class M17Adapter {
public:
    static bool available();
    static QString libraryVersion();
    static bool encodeCallsign(const QString &callsign, QByteArray *encoded,
                               QString *error = nullptr);
    static QString decodeCallsign(const QByteArray &encoded, QString *error = nullptr);
    static M17PacketSymbols buildPacket(const QString &source,
                                        const QString &destination,
                                        const QByteArray &payload,
                                        int channelAccessNumber,
                                        QString *error = nullptr);
    static bool decodePacketSymbols(const QByteArray &symbols,
                                    M17DecodedPacket *packet,
                                    QString *error = nullptr);
};

#endif // M17ADAPTER_H
