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
void receiverRegistrationTest(bool mixing) {
    VirtualIqHubSettings hs;hs.defaultRoute=false;
    VirtualIqMediumSettings ms;ms.sampleRate=8000;ms.blockSamples=128;ms.maximumBufferedBlocks=4;
    ms.playoutDelayMs=10;ms.defaultRoute=false;
    VirtualIqHub hub(hs);VirtualIqMedium medium(ms);
    check(hub.addSubscriber("Existing")&&(!mixing||medium.addSubscriber("Existing")),"registration initial receiver");
    check(hub.setLink("Source","Existing",{})&&(!mixing||medium.setLink("Source","Existing",{})),"registration initial route");
    const QJsonObject route{{"tx","Source"},{"rx","Existing"},{"enabled",true},{"delayMs",0},{"gainDb",0}};
    VirtualIqHubControl control(hub,mixing?&medium:nullptr,QJsonArray{route},false);
    const QString key(64,'b');QString error;
    check(control.listen(0,key,&error),"registration control starts");
    QJsonObject envelope{{"version",1},{"id","register"},{"command","status"},{"token",key}};
    const auto instance=request(control,envelope).value("instanceId");
    envelope["command"]="addReceiver";envelope["expectedRevision"]="0";
    QJsonObject endpoint{{"id","New"},{"host","127.0.0.1"},{"port",12001}};
    auto add=[&](const QJsonObject &args){envelope["args"]=args;return request(control,envelope);};
    check(add(endpoint).value("errorCode")=="wrong_instance","registration needs explicit current instance");
    envelope["targetInstanceId"]=instance;
    check(add(endpoint).value("errorCode")=="receiver_unavailable","registration handler is optional");
    QMap<QString,QJsonObject> endpoints;endpoints.insert("Existing",{{"host","127.0.0.1"},{"port",12000}});
    bool reject=true;int calls=0;
    control.setAddReceiverHandler([&](const QString &id,const QString &host,quint16 port){
        ++calls;if(reject)return false;
        for(const auto &value:endpoints)
            if(value.value("port").toInt()==port&&QHostAddress(value.value("host").toString())==QHostAddress(host))return false;
        endpoints.insert(id,{{"host",host},{"port",port}});return true;
    });
    envelope["token"]="wrong";
    check(add(endpoint).value("errorCode")=="unauthorized"&&calls==0,"registration stays authenticated");
    envelope["token"]=key;envelope["expectedRevision"]="9";
    check(add(endpoint).value("errorCode")=="revision_conflict"&&calls==0,"registration stale revision rejected");
    envelope["expectedRevision"]="0";
    for(const auto host:{"localhost","0.0.0.0","192.0.2.1"}) {
        auto invalid=endpoint;invalid["host"]=host;
        check(add(invalid).value("errorCode")=="invalid_receiver"&&calls==0,"registration rejects names and non-loopback hosts");
    }
    for(const double port:{0.0,65536.0,1.5}) {
        auto invalid=endpoint;invalid["port"]=port;
        check(add(invalid).value("errorCode")=="invalid_receiver"&&calls==0,"registration rejects invalid ports");
    }
    auto invalid=endpoint;invalid["id"]="tx1";
    check(add(invalid).value("errorCode")=="invalid_receiver"&&calls==0,"registration rejects reserved source ID");
    invalid=endpoint;invalid["extra"]=true;
    check(add(invalid).value("errorCode")=="invalid_receiver"&&calls==0,"registration rejects unknown endpoint fields");
    auto reply=add(endpoint);
    check(reply.value("errorCode")=="receiver_rejected"&&reply.value("revision")=="0"&&
        !hub.subscriberIds().contains("New")&&!medium.canSetLink("Source","New",{}),"host rejection leaves both subscriber models unchanged");
    if(mixing) {
        check(medium.addSubscriber("Hidden"),"prepare independent medium rejection");
        auto hidden=endpoint;hidden["id"]="Hidden";const int before=calls;
        reply=add(hidden);
        check(reply.value("errorCode")=="receiver_limit"&&!hub.subscriberIds().contains("Hidden")&&calls==before&&reply.value("revision")=="0","medium rejection cannot partially register fan-out or endpoint");
    }
    reject=false;
    auto duplicateEndpoint=endpoint;duplicateEndpoint["port"]=12000;
    check(add(duplicateEndpoint).value("errorCode")=="receiver_rejected"&&!hub.subscriberIds().contains("New"),"duplicate endpoint rejected without partial registration");
    const auto queuedFrame=frame(1000000000LL);const auto payload=queuedFrame.payload.constData();
    check(hub.publish("Source",queuedFrame,1000)&&(!mixing||medium.publish("Source",queuedFrame,2000000000LL)),"queue existing IQ before receiver registration");
    reply=add(endpoint);
    check(reply.value("ok").toBool()&&reply.value("revision")=="1"&&reply.value("instanceId")==instance&&reply.value("receiver").toObject()==endpoint&&endpoints.contains("New"),"receiver registration acknowledged once");
    const auto queued=hub.takeDue("Existing",1000);
    check(queued.size()==1&&queued.first().payload.constData()==payload&&(!mixing||medium.takeReady("Existing",2045000000LL).size()==1),"receiver registration preserves queued IQ without copying its payload");
    envelope["expectedRevision"]="1";const int beforeDuplicate=calls;
    reply=add(endpoint);
    check(reply.value("errorCode")=="duplicate_receiver"&&reply.value("revision")=="1"&&calls==beforeDuplicate,"duplicate ID preserves revision and endpoint");
    envelope["command"]="setLink";auto newRoute=route;newRoute["rx"]="New";envelope["args"]=newRoute;
    reply=request(control,envelope);
    check(reply.value("ok").toBool()&&reply.value("revision")=="2","new receiver accepts a new route");
    check(hub.publish("Source",frame(1016000000LL),1016)&&hub.takeDue("New",1016).size()==1&&
        (!mixing||(medium.publish("Source",frame(1016000000LL),2016000000LL)&&!medium.takeReady("New",2061000000LL).isEmpty())),"new receiver receives future IQ in active model");
    envelope["command"]="addReceiver";envelope["expectedRevision"]="2";
    const int limit=mixing?8:32;
    for(int i=2;i<limit;++i) {
        const QJsonObject next{{"id",QString("Live%1").arg(i)},{"host","::1"},{"port",12000+i}};
        reply=add(next);check(reply.value("ok").toBool(),"numeric IPv6 loopback receiver registered within limit");
        envelope["expectedRevision"]=reply.value("revision");
    }
    auto excess=endpoint;excess["id"]="Excess";excess["port"]=13000;const int beforeLimit=calls;
    reply=add(excess);
    check(reply.value("errorCode")=="receiver_limit"&&calls==beforeLimit&&hub.subscriberIds().size()==limit&&reply.value("revision")==envelope.value("expectedRevision"),"mixer/fan-out receiver limit leaves state unchanged");
}
void receiverRetryTest() {
    VirtualIqHub hub;VirtualIqHubControl control(hub,nullptr,{},true);
    const QString key(64,'c');QString error;
    check(control.listen(0,key,&error),"retry control starts");
    QJsonObject envelope{{"version",1},{"id","retry"},{"command","status"},{"token",key}};
    const auto instance=request(control,envelope).value("instanceId");
    envelope["targetInstanceId"]=instance;envelope["command"]="getReceiver";envelope["args"]=QJsonObject{{"id","Retry"}};
    auto reply=request(control,envelope);
    check(reply.value("ok").toBool()&&!reply.value("registered").toBool()&&reply.value("revision")=="0","retry query proves receiver absent without changing revision");
    bool reject=true;int calls=0;
    control.setAddReceiverHandler([&](const QString &,const QString &,quint16){++calls;return !reject;});
    const QJsonObject endpoint{{"id","Retry"},{"host","127.0.0.1"},{"port",12345}};
    envelope["command"]="addReceiver";envelope["args"]=endpoint;envelope["expectedRevision"]="0";
    check(request(control,envelope).value("errorCode")=="receiver_rejected","retry fixture rejects first registration");
    envelope["command"]="getReceiver";envelope["args"]=QJsonObject{{"id","Retry"}};
    reply=request(control,envelope);
    check(reply.value("ok").toBool()&&!reply.value("registered").toBool()&&reply.value("revision")=="0","failed registration can be queried safely before explicit retry");
    reject=false;envelope["command"]="addReceiver";envelope["args"]=endpoint;
    // Deliberately discard the acknowledgement after the host accepted the endpoint.
    {
        QTcpSocket lost;
        QObject::connect(&lost,&QTcpSocket::connected,&lost,[&]{lost.write(QJsonDocument(envelope).toJson(QJsonDocument::Compact)+'\n');});
        lost.connectToHost(QHostAddress::LocalHost,control.port());
        check(waitFor([&]{return calls==2;}),"explicit retry accepted while its acknowledgement is discarded");lost.abort();
    }
    envelope["command"]="getReceiver";envelope["args"]=QJsonObject{{"id","Retry"}};
    reply=request(control,envelope);
    check(reply.value("ok").toBool()&&reply.value("registered").toBool()&&reply.value("receiver").toObject()==endpoint&&reply.value("revision")=="1"&&calls==2,"lost registration acknowledgement reconciles exact endpoint without another add");
    auto other=endpoint;other["port"]=12346;
    check(reply.value("receiver").toObject()!=other,"reconciliation distinguishes a different endpoint for the same ID");
    envelope["targetInstanceId"]="obsolete";
    check(request(control,envelope).value("errorCode")=="wrong_instance","receiver reconciliation rejects stale hub identity");
    envelope["targetInstanceId"]=instance;envelope["token"]="wrong";reply=request(control,envelope);
    check(reply.value("errorCode")=="unauthorized"&&!reply.contains("receiver"),"receiver reconciliation cannot expose endpoint without authentication");
    envelope["token"]=key;envelope["args"]=QJsonObject{{"id","tx1"}};
    check(request(control,envelope).value("errorCode")=="invalid_receiver","receiver query validates ID");
    check(hub.addSubscriber("Untracked"),"prepare receiver without verified host endpoint");
    envelope["args"]=QJsonObject{{"id","Untracked"}};reply=request(control,envelope);
    check(reply.value("registered").toBool()&&!reply.value("receiver").toObject().contains("host"),"untracked receiver never supplies fabricated endpoint proof");
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
    receiverRegistrationTest(true);receiverRegistrationTest(false);
    receiverRetryTest();
    return 0;
}
