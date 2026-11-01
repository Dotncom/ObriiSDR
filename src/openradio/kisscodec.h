#ifndef KISSCODEC_H
#define KISSCODEC_H

#include <QByteArray>
#include <QList>
#include <QString>

struct KissFrame {
    quint8 port = 0;
    quint8 command = 0;
    QByteArray payload;
};

class KissCodec {
public:
    static QByteArray encode(const KissFrame &frame, QString *error = nullptr);
    static bool decode(const QByteArray &wireFrame, KissFrame *frame,
                       QString *error = nullptr);
};

class KissStreamDecoder {
public:
    QList<KissFrame> push(const QByteArray &bytes, QStringList *errors = nullptr);
    void reset();

private:
    QByteArray buffer;
};

#endif // KISSCODEC_H
