#include "meshiqmodem.h"
#include "../openradio/ax25codec.h"
#include "../openradio/ax25afsk1200.h"
#include "../audio/fmphasediscriminator.h"
#include <QDataStream>
#include <QRegularExpression>
#include <cmath>

namespace Mesh {
namespace {
constexpr double Pi=3.14159265358979323846,Deviation=3000.0;
bool fail(QString *error,const char *text){if(error)*error=QLatin1String(text);return false;}
bool id(const QString &value){return QRegularExpression("^[A-Za-z0-9_.:-]{1,64}$").match(value).hasMatch();}
}
QByteArray IqModem::encode(const IqEnvelope &e,QString *error) {
    if(e.session.isNull()||!e.flight||!id(e.transmitter)||!id(e.next)||!id(e.packet.source)||!id(e.packet.destination)||!id(e.packet.id)||
       e.packet.ttl<0||e.packet.ttl>64||e.packet.payload.isEmpty()||e.packet.payload.size()>MaximumPayload){fail(error,"Invalid IQ packet envelope (payload limit 256 bytes)");return {};}
    QByteArray wire;QDataStream stream(&wire,QIODevice::WriteOnly);stream.setByteOrder(QDataStream::BigEndian);
    stream.writeRawData("OBRIIMQ1",8);const auto epoch=e.session.toRfc4122();stream.writeRawData(epoch.constData(),16);
    stream<<e.flight<<quint8(e.packet.ttl);
    for(const auto &s:{e.transmitter,e.next,e.packet.source,e.packet.destination,e.packet.id}){
        const auto bytes=s.toLatin1();stream<<quint8(bytes.size());stream.writeRawData(bytes.constData(),bytes.size());
    }
    stream<<quint16(e.packet.payload.size());stream.writeRawData(e.packet.payload.constData(),e.packet.payload.size());return wire;
}
bool IqModem::decode(const QByteArray &wire,IqEnvelope *result,QString *error) {
    if(!result||wire.size()<40||wire.size()>768||wire.left(8)!="OBRIIMQ1")return fail(error,"Invalid IQ packet magic/length");
    QDataStream stream(wire);stream.setByteOrder(QDataStream::BigEndian);char magic[8],epoch[16];
    stream.readRawData(magic,8);stream.readRawData(epoch,16);IqEnvelope e;e.session=QUuid::fromRfc4122(QByteArray(epoch,16));
    quint8 ttl=0;stream>>e.flight>>ttl;e.packet.ttl=ttl;
    for(auto s:{&e.transmitter,&e.next,&e.packet.source,&e.packet.destination,&e.packet.id}){
        quint8 n=0;stream>>n;if(n<1||n>64)return fail(error,"Invalid IQ packet address length");
        QByteArray bytes(n,Qt::Uninitialized);if(stream.readRawData(bytes.data(),n)!=n)return fail(error,"Truncated IQ address");*s=QString::fromLatin1(bytes);
    }
    quint16 n=0;stream>>n;if(n<1||n>MaximumPayload)return fail(error,"Invalid IQ payload length");
    e.packet.payload.resize(n);if(stream.readRawData(e.packet.payload.data(),n)!=n||stream.status()!=QDataStream::Ok||!stream.atEnd())return fail(error,"Truncated/trailing IQ packet bytes");
    if(encode(e,error)!=wire)return fail(error,"Noncanonical IQ packet envelope");*result=std::move(e);return true;
}
QVector<std::complex<float>> IqModem::modulate(const QByteArray &wire,QString *error) {
    IqEnvelope check;if(!decode(wire,&check,error))return {};
    Ax25UiFrame frame;frame.source.callsign="OBRII";frame.destination.callsign="OBRII";frame.information=wire;
    const auto bytes=Ax25Codec::encodeUiFrame(frame,false,error);if(bytes.isEmpty())return {};
    const auto audio=Ax25Afsk1200::modulate(bytes,Rate,24,error);if(audio.isEmpty())return {};
    QVector<std::complex<float>> iq;iq.reserve(audio.size()+Rate/3+1024);double phase=0;
    for(float sample:audio){phase=std::remainder(phase+2*Pi*Deviation*sample/Rate,2*Pi);iq.append({float(0.35*std::cos(phase)),float(0.35*std::sin(phase))});}
    // A zero-IQ quiet tail lets RX squelch terminate its bounded burst.
    const int count=((iq.size()+Rate/3+1023)/1024)*1024;iq.resize(count);return iq;
}
void IqReceiver::reset(){audio.clear();quiet=0;havePrevious=false;discard=false;++resets;}
QList<QByteArray> IqReceiver::finish() {
    QList<QByteArray> result;
    if(audio.size()>=IqModem::Rate/10&&!discard){
        const auto frames=Ax25Afsk1200::demodulate(audio,IqModem::Rate);
        for(const auto &bytes:frames){Ax25UiFrame frame;if(Ax25Codec::decodeUiFrame(bytes,&frame,true)&&frame.destination.callsign=="OBRII"){result.append(frame.information);++validFrames;}}
        if(result.isEmpty())++badBursts;
    }
    audio.clear();quiet=0;havePrevious=false;discard=false;return result;
}
QList<QByteArray> IqReceiver::append(const float *iq,int count,bool gap) {
    QList<QByteArray> decoded;if(gap)reset();
    for(int i=0;i<count;++i){
        const float real=iq[i*2],imag=iq[i*2+1],power=real*real+imag*imag;
        if(!std::isfinite(power)){reset();continue;}
        if(power>1.0e-6f){
            quiet=0;
            const float audioSample=havePrevious?fmPhaseDifference(real,imag,previousI,previousQ)*float(IqModem::Rate/(2*Pi*Deviation)):0;
            previousI=real;previousQ=imag;havePrevious=true;
            if(!discard)audio.append(audioSample);
        } else {
            havePrevious=false;
            if(!audio.isEmpty()||discard){if(!discard)audio.append(0);if(++quiet>=IqModem::Rate/8)decoded.append(finish());}
        }
        if(audio.size()>IqModem::Rate*10){audio.clear();discard=true;++badBursts;}
    }
    return decoded;
}
}
