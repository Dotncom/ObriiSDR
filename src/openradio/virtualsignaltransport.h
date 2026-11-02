#ifndef VIRTUALSIGNALTRANSPORT_H
#define VIRTUALSIGNALTRANSPORT_H

#include "virtualsignalframe.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <complex>

enum class VirtualSignalTransportMode {
    LocalIpc = 0,
    Tcp = 1
};

struct VirtualSignalEndpoint {
    VirtualSignalTransportMode mode = VirtualSignalTransportMode::LocalIpc;
    QString localName = QStringLiteral("obrii-virtual-iq");
    QString host = QStringLiteral("127.0.0.1");
    quint16 port = 17661;
};

class VirtualSignalStreamParser {
public:
    static constexpr quint32 MaximumWireBytes = 1024U * 1024U;

    void append(const QByteArray &bytes);
    bool takeFrame(VirtualSignalFrame *frame, QString *error = nullptr);
    void clear();
    qsizetype bufferedBytes() const;

    static QByteArray wrap(const VirtualSignalFrame &frame, QString *error = nullptr);

private:
    QByteArray buffer;
};

QByteArray virtualSignalComplexFloat32Payload(const std::complex<float> *samples, int count);
bool virtualSignalValidNodeId(const QString &id);
bool virtualSignalDecodeComplexFloat32(const QByteArray &payload, QVector<float> *interleaved,
                                       QString *error = nullptr);

#endif // VIRTUALSIGNALTRANSPORT_H
