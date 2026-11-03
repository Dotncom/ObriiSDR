#include "virtualiqhubcontrol.h"
#include "virtualsignaltransport.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QThread>
#include <complex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {
bool waitFor(const std::function<bool()> &condition,int ms=3000) {
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<ms){QCoreApplication::processEvents(QEventLoop::AllEvents,10);if(condition())return true;QThread::msleep(1);}
    return condition();
}
void check(bool value,const char *name) {
    std::printf("%s %s\n",value?"PASS":"FAIL",name);if(!value)std::exit(1);
}
QJsonObject request(VirtualIqHubControl &server,const QJsonObject &data) {
    QTcpSocket socket;
    QObject::connect(&socket,&QTcpSocket::connected,&socket,[&]{socket.write(QJsonDocument(data).toJson(QJsonDocument::Compact)+'\n');});
    socket.connectToHost(QHostAddress::LocalHost,server.port());
    if(!waitFor([&]{return socket.canReadLine();}))return {};
    return QJsonDocument::fromJson(socket.readLine()).object();
}
VirtualSignalFrame frame(qint64 timestamp) {
    VirtualSignalFrame f;f.timestampNs=timestamp;f.sampleRate=8000;f.centerFrequencyHz=1000000;
    f.payloadType=VirtualSignalPayloadType::ComplexFloat32;
    QVector<std::complex<float>> iq(128,{0.25f,-0.25f});
    f.payload=virtualSignalComplexFloat32Payload(iq.constData(),iq.size());return f;
}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    VirtualIqHubSettings hs;hs.defaultRoute=false;
    VirtualIqMediumSettings ms;ms.sampleRate=8000;ms.blockSamples=128;ms.maximumBufferedBlocks=4;
    ms.playoutDelayMs=10;ms.defaultRoute=false;
    VirtualIqHub hub(hs);VirtualIqMedium medium(ms);
    for(const auto id:{"B","C"})check(hub.addSubscriber(id)&&medium.addSubscriber(id),"subscriber added");
    check(hub.setLink("A","B",{})&&medium.setLink("A","B",{}),"initial route");
    const QJsonObject initial{{"tx","A"},{"rx","B"},{"enabled",true},{"delayMs",0},{"gainDb",0}};
    VirtualIqHubControl control(hub,&medium,QJsonArray{initial},false);
    QString error;const QString key(64,'a');
    check(!control.listen(0,"short",&error),"invalid session key rejected");
    check(control.listen(0,key,&error),"loopback control starts");
    QJsonObject envelope{{"version",1},{"id","test"},{"command","status"},{"token","wrong"}};
    auto reply=request(control,envelope);
    check(reply.value("errorCode")=="unauthorized"&&!reply.contains("instanceId")&&!reply.contains("token"),"unauthorized requests reveal no identity or key");
    envelope["token"]=key;reply=request(control,envelope);
    check(reply.value("ok").toBool()&&reply.value("revision")=="0"&&reply.value("links").toArray().size()==1,"authenticated initial snapshot");
    const auto instance=reply.value("instanceId");
    envelope["command"]="setLink";envelope["targetInstanceId"]="obsolete";envelope["expectedRevision"]="0";envelope["args"]=initial;
    check(request(control,envelope).value("errorCode")=="wrong_instance","stale session rejected");
    envelope["targetInstanceId"]=instance;envelope["expectedRevision"]="9";
    check(request(control,envelope).value("errorCode")=="revision_conflict","lost-update revision rejected");
    envelope["expectedRevision"]="0";
    auto link=initial;link["gainDb"]=1000;envelope["args"]=link;
    check(request(control,envelope).value("errorCode")=="invalid_link","invalid gain rejected");
    link=initial;link["delayMs"]=500;envelope["args"]=link;
    reply=request(control,envelope);
    check(reply.value("errorCode")=="link_capacity"&&reply.value("revision")=="0","medium capacity rejection preserves revision");
    check(hub.publish("A",frame(1000000000LL),1000)&&hub.takeDue("B",1000).size()==1,"rejected mixed change does not partially change fan-out delay");
    check(hub.publish("A",frame(1000000000LL),1050)&&medium.publish("A",frame(1000000000LL),2000000000LL),"old-condition IQ queued before change");
    link=initial;link["gainDb"]=-6.0206;envelope["args"]=link;
    reply=request(control,envelope);
    check(reply.value("ok").toBool()&&reply.value("revision")=="1"&&reply.value("appliesTo")=="future-published-frames","gain change acknowledged");
    check(hub.publish("A",frame(1000000000LL),1100),"new fan-out data");
    auto output=hub.takeDue("B",1100);QVector<float> decoded;
    check(output.size()==2&&virtualSignalDecodeComplexFloat32(output.first().payload,&decoded)&&std::abs(decoded.first()-0.25f)<0.001f,"queued fan-out IQ retains old gain");
    check(virtualSignalDecodeComplexFloat32(output.last().payload,&decoded)&&std::abs(decoded.first()-0.125f)<0.001f,"updated fan-out gain changes real IQ");
    check(medium.publish("A",frame(1016000000LL),2016000000LL),"new mixed data");
    output=medium.takeReady("B",2045000000LL);
    check(output.size()==2&&virtualSignalDecodeComplexFloat32(output.first().payload,&decoded)&&std::abs(decoded.first()-0.25f)<0.001f,"queued mixed IQ retains old gain");
    check(virtualSignalDecodeComplexFloat32(output.last().payload,&decoded)&&std::abs(decoded.first()-0.125f)<0.001f,"updated sample-mix gain changes real IQ");
    envelope["expectedRevision"]="1";link["enabled"]=false;envelope["args"]=link;
    check(request(control,envelope).value("revision")=="2","disable acknowledged");
    const auto filtered=medium.counters("B").filteredFrames;
    check(hub.publish("A",frame(1032000000LL),1200)&&hub.takeDue("B",1200).isEmpty()&&
        medium.publish("A",frame(1032000000LL),2032000000LL)&&medium.counters("B").filteredFrames>filtered,"disabled link filters subsequent IQ in both modes");
    envelope["expectedRevision"]="2";link["enabled"]=true;link["delayMs"]=8;envelope["args"]=link;
    check(request(control,envelope).value("revision")=="3","restore and delay acknowledged");
    check(hub.publish("A",frame(1064000000LL),1300)&&hub.takeDue("B",1307).isEmpty()&&hub.takeDue("B",1308).size()==1,"new delay affects actual delivery");
    check(medium.takeReady("C",2200000000LL).isEmpty()&&hub.takeDue("C",1400).isEmpty(),"unrelated receiver remains isolated");
    envelope["command"]="status";reply=request(control,envelope);
    check(reply.value("revision")=="3"&&reply.value("links").toArray().first().toObject().value("delayMs")==8&&!reply.contains("token"),"status reports applied link without credentials");
    QTcpSocket idle;idle.connectToHost(QHostAddress::LocalHost,control.port());
    check(waitFor([&]{return idle.state()==QAbstractSocket::ConnectedState;}),"idle client connects");
    check(waitFor([&]{return idle.state()==QAbstractSocket::UnconnectedState;},2600),"idle client timeout bounded");
    QTcpSocket oversized;oversized.connectToHost(QHostAddress::LocalHost,control.port());
    check(waitFor([&]{return oversized.state()==QAbstractSocket::ConnectedState;}),"oversized client connects");
    oversized.write(QByteArray(65537,'x'));
    check(waitFor([&]{return oversized.state()==QAbstractSocket::UnconnectedState;}),"oversized request bounded");
    return 0;
}
