#ifndef AX25CODEC_H
#define AX25CODEC_H

#include <QByteArray>
#include <QString>
#include <QVector>

struct Ax25Address {
    QString callsign;
    quint8 ssid = 0;
    bool repeated = false;
};

struct Ax25UiFrame {
    Ax25Address destination;
    Ax25Address source;
    QVector<Ax25Address> digipeaters;
    quint8 control = 0x03;
    quint8 pid = 0xF0;
    QByteArray information;
};

class Ax25Codec {
public:
    static QByteArray encodeUiFrame(const Ax25UiFrame &frame,
                                    bool appendFcs = false,
                                    QString *error = nullptr);
    static bool decodeUiFrame(const QByteArray &bytes,
                              Ax25UiFrame *frame,
                              bool containsFcs = false,
                              QString *error = nullptr);
    static quint16 fcs(const QByteArray &bytes);
    static QString addressText(const Ax25Address &address);
    static bool parseAddress(const QString &text, Ax25Address *address,
                             QString *error = nullptr);

private:
    static QByteArray encodeAddress(const Ax25Address &address, bool last,
                                    QString *error);
    static bool decodeAddress(const QByteArray &bytes, int offset,
                              Ax25Address *address, bool *last,
                              QString *error);
};

#endif // AX25CODEC_H
