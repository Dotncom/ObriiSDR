#include "meshiqbearer.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonArray>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {
void check(bool ok,const char *name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);std::fflush(stdout);if(!ok)std::exit(1);}
bool waitFor(const std::function<bool()> &fn,int ms=20000){QElapsedTimer t;t.start();while(t.elapsed()<ms){QCoreApplication::processEvents(QEventLoop::AllEvents,10);if(fn())return true;QThread::msleep(2);}return fn();}
QJsonObject topology(double gain=-6) {
    return {{"nodes",QJsonArray{QJsonObject{{"id","A"},{"name","A"}},QJsonObject{{"id","B"},{"name","B"}},QJsonObject{{"id","C"},{"name","C"}}}},
        {"links",QJsonArray{QJsonObject{{"tx","A"},{"rx","B"},{"enabled",true},{"delayMs",10},{"gainDb",gain}},
                            QJsonObject{{"tx","B"},{"rx","C"},{"enabled",true},{"delayMs",20},{"gainDb",gain}}}}};
}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);QString error;
    Mesh::IqEnvelope original{QUuid::createUuid(),1,"A","B",{"packet","A","C",QByteArray("IQ\0DATA",7),1}};
    const auto wire=Mesh::IqModem::encode(original,&error);Mesh::IqEnvelope decoded;
    check(!wire.isEmpty()&&Mesh::IqModem::decode(wire,&decoded,&error)&&decoded.packet.payload==original.packet.payload,"compact IQ envelope preserves binary payload");
    const auto iq=Mesh::IqModem::modulate(wire,&error);check(!iq.isEmpty(),"existing AX25/AFSK generates FM CF32");
    Mesh::IqReceiver receiver;QList<QByteArray> frames;
    for(int offset=0;offset<iq.size();offset+=317){QVector<float> chunk;const int count=qMin(317,iq.size()-offset);for(int i=0;i<count;++i){chunk<<iq[offset+i].real()*0.5f<<iq[offset+i].imag()*0.5f;}frames.append(receiver.append(chunk.constData(),count));}
    check(frames.contains(wire)&&receiver.validFrames>0,"FM discriminator and AFSK FCS recover envelope across arbitrary IQ chunks");
    auto maximum=original;maximum.transmitter=QString(64,'A');maximum.next=QString(64,'B');maximum.packet.source=QString(64,'C');maximum.packet.destination=QString(64,'D');maximum.packet.id=QString(64,'E');maximum.packet.payload=QByteArray(256,'X');
    const auto maximumWire=Mesh::IqModem::encode(maximum,&error);const auto maximumIq=Mesh::IqModem::modulate(maximumWire,&error);receiver.reset();frames.clear();
    for(int offset=0;offset<maximumIq.size();offset+=1024){QVector<float> chunk;const int count=qMin(1024,maximumIq.size()-offset);for(int i=0;i<count;++i)chunk<<maximumIq[offset+i].real()<<maximumIq[offset+i].imag();frames.append(receiver.append(chunk.constData(),count));}
    check(frames.contains(maximumWire),"maximum envelope addresses and payload fit bounded modem burst");
    receiver.reset();auto broken=iq;for(int i=Mesh::IqModem::Rate/3;i<qMin(broken.size(),Mesh::IqModem::Rate/3+3840);++i)broken[i]={0,0};frames.clear();
    for(int offset=0;offset<broken.size();offset+=1024){QVector<float> chunk;const int count=qMin(1024,broken.size()-offset);for(int i=0;i<count;++i)chunk<<broken[offset+i].real()<<broken[offset+i].imag();frames.append(receiver.append(chunk.constData(),count));}
    check(!frames.contains(wire),"damaged IQ does not bypass AX25 FCS");
    Mesh::PacketEngine engine;check(engine.configure(topology(),&error),"configure IQ route");
    Mesh::IqBearer bearer;QString backendError;
    QObject::connect(&bearer,&Mesh::IqBearer::failed,&app,[&](const QString &s){backendError=s;});
    QObject::connect(&bearer,&Mesh::IqBearer::decoded,&app,[&](const QString &at,const QByteArray &body){Mesh::IqEnvelope e;QString problem;if(Mesh::IqModem::decode(body,&e,&problem))engine.receiveIq(e.flight,e.transmitter,at,e.packet,&problem);});
    check(bearer.start(topology(),&error)&&waitFor([&]{return bearer.ready()||!backendError.isEmpty();})&&bearer.ready(),"real loopback CF32 TX/RX endpoints ready");
    check(engine.attachIqBearer([&](quint64 f,const QString &from,const QString &to,const Mesh::PacketEngine::Datagram &p,QString *problem){return bearer.transmit(f,from,to,p,problem);},&error),"attach asynchronous IQ bearer to existing packet router");
    check(!engine.send("A","C",original.packet.payload,2,&error).isEmpty()&&engine.advanceTo(0)&&engine.snapshot(false).value("delivered")=="0","no model-only delivery while waiting for actual demodulation");
    QElapsedTimer pace;pace.start();
    check(waitFor([&]{engine.advanceTo(pace.elapsed(),512);return engine.pending()==0||!backendError.isEmpty();},30000)&&backendError.isEmpty()&&engine.snapshot(false).value("delivered")=="1","A-B-C delivered only after two actual IQ modem hops");
    const auto result=engine.results().last().toObject();
    check(result.value("path").toArray()==QJsonArray{"A","B","C"}&&QByteArray::fromBase64(result.value("payloadBase64").toString().toLatin1())==original.packet.payload,"relay delivery preserves path and exact demodulated bytes");
    check(waitFor([&]{return bearer.statistics().value("decoded").toString().toULongLong()>=2;},1500)&&bearer.statistics().value("txBursts").toString().toULongLong()>=2,"CF32 transport counters prove two transmitted and decoded RF-sample bursts");
    check(waitFor([&]{return bearer.statistics().contains("active")&&!bearer.statistics().value("active").toBool();},2000),"IQ pump sleeps after buffers drain");
    bearer.stop();check(!bearer.ready(),"IQ worker stops cleanly");
    check(engine.configure(topology(),&error)&&bearer.start(topology(-120),&error)&&waitFor([&]{return bearer.ready();}),"restart with an attenuated real IQ channel");
    check(engine.attachIqBearer([&](quint64 f,const QString &from,const QString &to,const Mesh::PacketEngine::Datagram &p,QString *problem){return bearer.transmit(f,from,to,p,problem);},&error),"attach muted channel");
    engine.send("A","C","below squelch",2,&error);engine.advanceTo(0);pace.restart();
    check(waitFor([&]{engine.advanceTo(pace.elapsed(),512);return engine.pending()==0;},18000)&&engine.results().last().toObject().value("status")=="modem_timeout"&&bearer.statistics().value("decoded").toString().toInt()==0,"actual IQ below carrier gate cannot deliver a packet");
    bearer.stop();
    check(engine.configure(topology(),&error)&&engine.attachIqBearer([](quint64,const QString &,const QString &,const Mesh::PacketEngine::Datagram &,QString *){return true;},&error),"attach a deliberately silent bearer");
    check(!engine.send("A","C","lost",2,&error).isEmpty()&&engine.advanceTo(15000)&&engine.snapshot(false).value("delivered")=="0"&&engine.results().last().toObject().value("status")=="modem_timeout","missing decode produces timeout, never simulated delivery");
    engine.send("A","C","cancel",2,&error);engine.advanceTo(engine.now());engine.abortIq();
    check(engine.pending()==0&&engine.results().last().toObject().value("status")=="modem_stopped","cancellation releases pending IQ queues with an explicit outcome");
    std::printf("IQ packet bridge self-test passed\n");return 0;
}
