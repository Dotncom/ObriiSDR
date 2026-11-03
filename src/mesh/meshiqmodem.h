#pragma once
#include "meshpacketengine.h"
#include <QUuid>
#include <QVector>
#include <complex>

namespace Mesh {
struct IqEnvelope {
    QUuid session;
    quint64 flight=0;
    QString transmitter,next;
    PacketEngine::Datagram packet;
};
class IqModem {
public:
    static constexpr int Rate=48000,MaximumPayload=256;
    static QByteArray encode(const IqEnvelope &,QString *error);
    static bool decode(const QByteArray &,IqEnvelope *,QString *error);
    static QVector<std::complex<float>> modulate(const QByteArray &wire,QString *error);
};
class IqReceiver {
public:
    QList<QByteArray> append(const float *iq,int samples,bool discontinuity=false);
    void reset();
    quint64 validFrames=0,badBursts=0,resets=0;
private:
    QList<QByteArray> finish();
    QVector<float> audio;
    float previousI=0,previousQ=0;
    bool havePrevious=false,discard=false;
    int quiet=0;
};
}
