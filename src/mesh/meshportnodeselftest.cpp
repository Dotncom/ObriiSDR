#include "meshnode.h"
#include "meshprotocoladapter.h"
#include "meshwiremodem.h"
#include "../core/dspiqsourceregistry.h"
#include "../meshtastic/meshtastic.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <complex>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr int ReplyLimit=128*1024;
int failures=0;
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
QString runKey(){auto value=uuid()+uuid();value.remove('-');return value;}
void events(){QCoreApplication::processEvents(QEventLoop::AllEvents,10);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
void require(bool okay,const char *name){
    if(!okay)throw std::runtime_error(name);
    std::printf("PASS %s\n",name);std::fflush(stdout);
}
quint64 counter(const QJsonValue &value){return value.isString()?value.toString().toULongLong():quint64(value.toDouble());}
QString scalar(const QJsonValue &value){return value.isString()?value.toString():QString::number(value.toDouble(),'f',0);}
QJsonObject rpc(int port,const QString &token,QJsonObject request,int timeoutMs=3000){
    const auto requestId=uuid();request["requestId"]=requestId;
    if(request.value("op")!="pair")request["token"]=token;else request.remove("token");
    QTcpSocket socket;socket.setReadBufferSize(ReplyLimit+1);bool failed=false;
    QObject::connect(&socket,&QTcpSocket::connected,&socket,[&]{socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');});
    QObject::connect(&socket,&QTcpSocket::errorOccurred,&socket,[&](QAbstractSocket::SocketError error){if(error!=QAbstractSocket::RemoteHostClosedError)failed=true;});
    socket.connectToHost("127.0.0.1",quint16(port));QByteArray bytes;QElapsedTimer elapsed;elapsed.start();
    while(elapsed.elapsed()<timeoutMs){
        events();bytes+=socket.readAll();if(bytes.size()>ReplyLimit)return {};
        if(bytes.contains('\n')||failed)break;QThread::msleep(2);
    }
    const int end=bytes.indexOf('\n');if(end<0)return {};
    QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes.left(end),&parse);
    if(parse.error!=QJsonParseError::NoError||!document.isObject()||document.object().value("requestId")!=requestId)return {};
    return document.object();
}
QJsonObject fastLoRa(int os=1){return {{"type","lora"},{"sf",7},{"bw",125000},{"cr",1},{"os",os},{"preamble",16},{"syncWord",43}};}
QJsonObject port(const QString &id,const QString &direction){return {{"id",id},{"direction",direction},{"backend","network"},{"enabled",true}};}
QJsonArray ports(const QString &rx="in",const QString &tx="out"){return {port(rx,"rx"),port(tx,"tx")};}
QJsonObject link(const QString &from,const QString &to,const QString &bearer,const QString &tx="out",const QString &rx="in"){
    QJsonObject result{{"tx",from},{"rx",to},{"bearer",bearer},{"txPort",tx},{"rxPort",rx},{"enabled",true},{"delayMs",2},{"gainDb",0}};
    if(bearer=="iq")result["modem"]=fastLoRa();return result;
}
QJsonObject node(const QString &id,const QJsonArray &bindings,const QJsonObject &protocol={}){
    QJsonObject result{{"id",id},{"name",id},{"enabled",true},{"ports",bindings}};if(!protocol.isEmpty())result["protocol"]=protocol;return result;
}
QJsonObject nativeConfiguration(quint32 id){
    return {{"type","meshtastic"},{"profileId","integration"},{"nodeNumber",double(id)},{"relayRole","router"},
        {"relayDelayMs",0},{"relayJitterMs",0},{"ackTimeoutMs",8000},{"retryDelayMs",20},{"maxRetries",1},{"historyTtlMs",60000}};
}
QJsonObject localProfile(){
    return {{"channels",QJsonArray{QJsonObject{{"name","PortTest"},{"pskBase64","AQ=="},{"enabled",true}}}}};
}
QJsonObject labResult(const QJsonObject &state,const QString &id,const QString &status){
    for(const auto &value:state.value("results").toArray()){const auto result=value.toObject();if(result.value("id")==id&&result.value("status")==status)return result;}return {};
}
QList<QJsonObject> nativeResults(const QJsonObject &state,const QString &status,const QString &id={}){
    QList<QJsonObject> result;
    for(const auto &value:state.value("results").toArray()){const auto event=value.toObject();
        if(event.value("protocol")=="meshtastic"&&event.value("status")==status&&(id.isEmpty()||scalar(event.value("nativeId"))==id))result.append(event);
    }return result;
}
QJsonObject linkStatus(const QJsonObject &state,const QString &destination){
    for(const auto &value:state.value("links").toArray())if(value.toObject().value("rx")==destination)return value.toObject();return {};
}

class Rig {
public:
    struct Endpoint {
        QString id,admin,token,instance,pairingCode,stateDirectory;
        int port=0;
        std::unique_ptr<Mesh::NodeService> service;
    };
    QTemporaryDir state;
    std::vector<std::unique_ptr<Endpoint>> endpoints;
    QString client=uuid(),session,key;
    QVector<QJsonObject> latest;
    Rig(){if(!state.isValid())throw std::runtime_error("temporary service state root");}
    ~Rig(){release();for(auto &endpoint:endpoints)endpoint->service->command({{"operation","stop"}});events();}
    void add(const QString &id,QJsonObject localOptions={}){
        auto endpoint=std::make_unique<Endpoint>();endpoint->id=id;endpoint->stateDirectory=state.filePath("host-"+id);
        if(!QDir().mkpath(QDir(endpoint->stateDirectory).filePath(id)))throw std::runtime_error("create node-local state directory");
        endpoint->service=std::make_unique<Mesh::NodeService>();
        localOptions["operation"]="start";localOptions["bind"]="127.0.0.1";localOptions["port"]=0;localOptions["protocolStateDirectory"]=endpoint->stateDirectory;
        localOptions["protocolProfiles"]=QJsonObject{{"integration",localProfile()}};
        const auto started=endpoint->service->command(localOptions);
        if(!started.value("ok").toBool())throw std::runtime_error("test-owned NodeService start");
        endpoint->admin=started.value("token").toString();endpoint->pairingCode=started.value("pairingSecret").toString();endpoint->instance=started.value("instanceId").toString();endpoint->port=started.value("port").toInt();
        auto authenticated=rpc(endpoint->port,endpoint->admin,{{"op","status"}});
        if(authenticated.value("protocol")!="obrii-mesh-node"||authenticated.value("version")!=1||authenticated.value("instanceId")!=endpoint->instance||authenticated.value("active").toBool())throw std::runtime_error("authenticate and pin fresh endpoint");
        auto paired=rpc(endpoint->port,{},{{"op","pair"},{"instanceId",endpoint->instance},{"clientId",client},{"clientName","Mesh port integration test"},{"code",endpoint->pairingCode}});
        if(!paired.value("ok").toBool()||paired.value("instanceId")!=endpoint->instance||paired.value("token").toString().isEmpty())throw std::runtime_error("pair isolated test client");
        endpoint->token=paired.value("token").toString();endpoints.push_back(std::move(endpoint));
    }
    QJsonObject call(int index,QJsonObject request){
        const auto &endpoint=*endpoints.at(std::size_t(index));request["instanceId"]=endpoint.instance;
        if(!session.isEmpty()&&!request.contains("session"))request["session"]=session;
        return rpc(endpoint.port,endpoint.token,request);
    }
    bool configure(const QJsonObject &topology,const QJsonObject &peerOverrides={}){
        release();session=uuid();key=runKey();QJsonObject peers;
        for(const auto &endpoint:endpoints)peers[endpoint->id]=QJsonObject{{"host","127.0.0.1"},{"port",endpoint->port},{"instanceId",endpoint->instance}};
        for(auto it=peerOverrides.begin();it!=peerOverrides.end();++it)peers[it.key()]=it.value();
        bool okay=true;
        for(int index=0;index<int(endpoints.size());++index){
            const auto reply=call(index,{{"op","configure"},{"key",key},{"nodeId",endpoints[std::size_t(index)]->id},{"transport","data"},{"topology",topology},{"peers",peers}});
            if(!reply.value("ok").toBool()){
                auto message=reply.value("error").toString("no bounded RPC reply");message.replace(key,"[key]");
                for(const auto &endpoint:endpoints){message.replace(endpoint->token,"[credential]");message.replace(endpoint->admin,"[credential]");message.replace(endpoint->stateDirectory,"[state]");}
                std::fprintf(stderr,"configure %s: %s\n",endpoints[std::size_t(index)]->id.toUtf8().constData(),message.left(240).toUtf8().constData());okay=false;
            }
        }
        latest=poll();return okay;
    }
    QVector<QJsonObject> poll(){
        QVector<QJsonObject> result;for(int index=0;index<int(endpoints.size());++index)result.append(call(index,{{"op","status"}}));return result;
    }
    bool await(const std::function<bool(const QVector<QJsonObject> &)> &predicate,int milliseconds=10000){
        QElapsedTimer elapsed;elapsed.start();
        do {latest=poll();if(predicate(latest))return true;events();QThread::msleep(10);}while(elapsed.elapsed()<milliseconds);
        for(int index=0;index<latest.size();++index){const auto &value=latest[index];
            std::fprintf(stderr,"node %s active=%d queue=%s tx=%s rx=%s dropped=%s iqTx=%s iqRx=%s\n",endpoints[std::size_t(index)]->id.toUtf8().constData(),int(value.value("active").toBool()),scalar(value.value("queue")).toUtf8().constData(),scalar(value.value("tx")).toUtf8().constData(),scalar(value.value("rx")).toUtf8().constData(),scalar(value.value("dropped")).toUtf8().constData(),scalar(value.value("iqTxSamples")).toUtf8().constData(),scalar(value.value("iqRxSamples")).toUtf8().constData());
            QJsonObject summary;const auto protocol=value.value("nativeProtocol").toObject();
            for(const auto name:{"type","protocol","ready","nativeAddress","nodeNumber","queued","pending","error"})if(protocol.contains(name))summary[name]=protocol.value(name);
            QJsonArray outcomes;const auto all=value.value("results").toArray();
            for(int position=qMax(0,all.size()-16);position<all.size();++position){const auto original=all[position].toObject();QJsonObject result;QJsonArray fields;
                for(const auto &name:original.keys())fields.append(name);result["fields"]=fields;
                for(const auto name:{"protocol","node","status","event","serial","localMs","id","nativeId","packetId","from","to","port","security","authenticated","reason","payloadBase64"})if(original.contains(name))result[name]=original.value(name);
                outcomes.append(result);
            }
            QJsonObject diagnostic{{"protocol",value.value("protocol")},{"ownsSession",value.value("session")==session},{"resultCount",all.size()},{"lastResultSerial",value.value("lastResultSerial")},{"delivered",value.value("delivered")},{"nativeProtocol",summary},{"results",outcomes},{"radioIngress",value.value("radioIngress")}};
            auto text=QString::fromUtf8(QJsonDocument(diagnostic).toJson(QJsonDocument::Compact));
            for(const auto &endpoint:endpoints){text.replace(endpoint->admin,"[credential]");text.replace(endpoint->token,"[credential]");text.replace(endpoint->stateDirectory,"[state]");}
            if(!key.isEmpty())text.replace(key,"[run-key]");text.replace(session,"[session]");
            std::fprintf(stderr,"outcomes %s %s\n",endpoints[std::size_t(index)]->id.toUtf8().constData(),text.toUtf8().constData());
        }return false;
    }
    bool release(){
        if(session.isEmpty())return true;bool okay=true;
        for(int index=0;index<int(endpoints.size());++index){const auto reply=call(index,{{"op","release"}});okay&=reply.value("ok").toBool()&&!reply.value("active").toBool();}
        session.clear();key.fill('0');key.clear();return okay;
    }
    void privacy(){
        for(int index=0;index<int(endpoints.size());++index){const auto document=QJsonDocument(call(index,{{"op","status"}})).toJson(QJsonDocument::Compact);
            const auto &endpoint=*endpoints[std::size_t(index)];
            if(document.contains(endpoint.admin.toUtf8())||document.contains(endpoint.token.toUtf8())||document.contains(endpoint.stateDirectory.toUtf8())||document.contains("pskBase64")||document.contains("pairingSecret")||(!key.isEmpty()&&document.contains(key.toUtf8())))throw std::runtime_error("service status leaked a credential, profile key or local state path");
        }
    }
};

class StallPeer : public QObject {
public:
    QTcpServer server;
    int connections=0;
    StallPeer(){
        if(!server.listen(QHostAddress::LocalHost,0))throw std::runtime_error("bind isolated stalled peer");
        connect(&server,&QTcpServer::newConnection,this,[this]{while(auto socket=server.nextPendingConnection()){
            ++connections;socket->setReadBufferSize(128*1024);
            connect(socket,&QTcpSocket::readyRead,socket,[socket]{socket->readAll();});
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        }});
    }
    void failCurrent(){for(auto socket:server.findChildren<QTcpSocket *>())socket->abort();}
};

void mixedLab(){
    Rig rig;for(const auto &id:{"A","B","C"})rig.add(id);
    QJsonArray nodes;for(const auto &id:{"A","B","C"})nodes.append(node(id,ports()));
    const QJsonArray links{link("A","B","data"),link("B","C","iq"),link("C","B","iq"),link("B","A","data")};
    require(rig.configure({{"nodes",nodes},{"links",links}}),"mixed custom-port DATA/LoRa graph configures on actual services");
    const auto forward=QByteArray::fromHex("006d697865642d666f7277617264ff"),reverse=QByteArray::fromHex("ff72657665727365006366333200");
    require(rig.call(0,{{"op","send"},{"id","mixed_forward"},{"destination","C"},{"ttl",2},{"payloadBase64",QString::fromLatin1(forward.toBase64())}}).value("ok").toBool(),"accept DATA then LoRa forward packet");
    require(rig.call(2,{{"op","send"},{"id","mixed_reverse"},{"destination","A"},{"ttl",2},{"payloadBase64",QString::fromLatin1(reverse.toBase64())}}).value("ok").toBool(),"accept LoRa then DATA reverse packet concurrently");
    require(rig.await([](const QVector<QJsonObject> &states){return !labResult(states[2],"mixed_forward","delivered").isEmpty()&&!labResult(states[0],"mixed_reverse","delivered").isEmpty()&&counter(states[1].value("forwarded"))>=2&&counter(states[2].value("iqTxSamples"))>0;}),"independent node RX/TX forwards both mixed-bearer directions");
    const auto delivered=labResult(rig.latest[2],"mixed_forward","delivered"),back=labResult(rig.latest[0],"mixed_reverse","delivered");
    require(QByteArray::fromBase64(delivered.value("payloadBase64").toString().toLatin1())==forward&&delivered.value("path").toArray()==QJsonArray{"A","B","C"},"DATA to LoRa preserves exact binary payload and actual relay path");
    require(QByteArray::fromBase64(back.value("payloadBase64").toString().toLatin1())==reverse&&back.value("path").toArray()==QJsonArray{"C","B","A"},"LoRa to DATA preserves exact reverse payload and path");
    require(counter(rig.latest[0].value("iqTxSamples"))==0&&counter(rig.latest[1].value("iqTxSamples"))>0&&counter(rig.latest[2].value("iqTxSamples"))>0&&counter(rig.latest[1].value("forwarded"))>=2,"telemetry proves link overrides rather than a global IQ substitution");
    const auto out=linkStatus(rig.latest[1],"C"),backLink=linkStatus(rig.latest[1],"A");
    require(out.value("bearer")=="iq"&&out.value("txPort")=="out"&&out.value("rxPort")=="in"&&out.value("modem").toObject().value("type")=="lora"&&counter(out.value("samples"))>0&&backLink.value("bearer")=="data"&&counter(backLink.value("samples"))==0,"custom link bindings and per-link samples are reported independently");
    rig.privacy();require(rig.release(),"mixed graph releases owned sessions without stopping listeners");
}

void isolatedQueues(){
    Rig rig;for(const auto &id:{"A","B","C"})rig.add(id);StallPeer stalled;
    const QJsonArray nodes{node("A",{port("in","rx"),port("bad","tx"),port("good","tx")}),node("B",ports()),node("C",ports())};
    const QJsonArray links{link("A","B","data","bad"),link("A","C","data","good")};
    const QJsonObject peers{{"B",QJsonObject{{"host","127.0.0.1"},{"port",stalled.server.serverPort()},{"instanceId",rig.endpoints[1]->instance}}}};
    require(rig.configure({{"nodes",nodes},{"links",links}},peers),"configure broken and working neighbors on independent TX ports");
    bool submitted=true;for(int index=0;index<20;++index)submitted&=rig.call(0,{{"op","send"},{"id","blocked_"+QString::number(index)},{"destination","B"},{"ttl",1},{"payloadBase64","Qg=="}}).value("ok").toBool();
    require(submitted,"submit bounded bad-neighbor traffic");
    const auto before=rig.call(0,{{"op","status"}});
    bool queueFull=false;for(const auto &value:before.value("results").toArray())queueFull|=value.toObject().value("status")=="queue_full";
    require(stalled.connections>0&&linkStatus(before,"B").value("queued").toInt()==16&&queueFull,"a silent peer fills only its bounded sixteen-frame queue");
    const QByteArray payload("working-neighbor");
    require(rig.call(0,{{"op","send"},{"id","working"},{"destination","C"},{"ttl",1},{"payloadBase64",QString::fromLatin1(payload.toBase64())}}).value("ok").toBool(),"good neighbor accepts work while bad neighbor is saturated");
    require(rig.await([](const QVector<QJsonObject> &states){return !labResult(states[2],"working","delivered").isEmpty()&&linkStatus(states[0],"B").value("queued").toInt()>0;},5000),"working link delivers before the broken link's timeout or release");
    require(QByteArray::fromBase64(labResult(rig.latest[2],"working","delivered").value("payloadBase64").toString().toLatin1())==payload&&labResult(rig.latest[1],"working","delivered").isEmpty(),"good-port payload is exact and not delivered through the stalled neighbor");
    require(rig.release(),"release cancels saturated test queues without killing services");
}

void nativeFanout(){
    Rig rig;for(const auto &id:{"A","B","C"})rig.add(id);StallPeer stalled;
    QJsonArray nodes;for(int index=0;index<3;++index)nodes.append(node(QString(QChar('A'+index)),ports("radio","radio"),nativeConfiguration(0x100u+quint32(index))));
    const QJsonArray links{link("A","B","data","radio","radio"),link("A","C","data","radio","radio"),link("B","A","data","radio","radio")};
    const QJsonObject peers{{"C",QJsonObject{{"host","127.0.0.1"},{"port",stalled.server.serverPort()},{"instanceId",rig.endpoints[2]->instance}}}};
    require(rig.configure({{"nodes",nodes},{"links",links}},peers),"native DATA fan-out uses one radio interface for a working and a blackhole peer");
    const auto submit=[&](const QByteArray &payload){
        const auto reply=rig.call(0,{{"op","protocol"},{"args",QJsonObject{{"op","send"},{"destination",257},{"payloadBase64",QString::fromLatin1(payload.toBase64())},{"reliable",true}}}});
        const auto id=scalar(reply.value("protocolReply").toObject().value("nativeId"));
        require(reply.value("ok").toBool()&&reply.value("instanceId")==rig.endpoints[0]->instance&&!id.isEmpty()&&id!="0","fan-out command returns a native packet ID on the owned service");return id;
    };
    const auto completed=[&](const QVector<QJsonObject> &states,const QString &id){
        const auto native=states[0].value("nativeProtocol").toObject();
        return !nativeResults(states[1],"message",id).isEmpty()&&!nativeResults(states[0],"sent",id).isEmpty()&&!nativeResults(states[0],"acknowledged",id).isEmpty()
            &&native.contains("queued")&&counter(native.value("queued"))==0&&native.contains("pending")&&counter(native.value("pending"))==0&&linkStatus(states[0],"C").value("queued").toInt()>0;
    };
    const auto checkPayload=[&](const QString &id,const QByteArray &payload){
        const auto messages=nativeResults(rig.latest[1],"message",id);
        require(messages.size()==1&&scalar(messages.first().value("from"))=="256"&&scalar(messages.first().value("to"))=="257"&&scalar(messages.first().value("packetId"))==id
            &&QByteArray::fromBase64(messages.first().value("payloadBase64").toString().toLatin1())==payload,"working fan-out peer delivers exact native bytes and original addresses once");
        require(nativeResults(rig.latest[2],"message",id).isEmpty()&&nativeResults(rig.latest[0],"tx_failed",id).isEmpty(),"blackhole does not fabricate delivery or invalidate successful native TX");
    };
    const auto firstPayload=QByteArray::fromHex("0066616e6f75742d31ff"),secondPayload=QByteArray::fromHex("ff66616e6f75742d3200");
    const auto first=submit(firstPayload),second=submit(secondPayload);
    require(first!=second&&rig.await([&](const QVector<QJsonObject> &states){return stalled.connections>0&&completed(states,first)&&completed(states,second);},5000),"native DATA messages and ACKs complete while the blackhole handshake is still pending");
    checkPayload(first,firstPayload);checkPayload(second,secondPayload);
    const auto dropped=counter(rig.latest[0].value("dropped"));stalled.failCurrent();
    require(rig.await([&](const QVector<QJsonObject> &states){return counter(states[0].value("dropped"))>dropped;},3000),"late blackhole failure is accounted for after native TX already completed");
    const QByteArray afterFailure("native-after-late-failure");const auto third=submit(afterFailure);
    require(third!=first&&third!=second&&rig.await([&](const QVector<QJsonObject> &states){return completed(states,third);},5000),"late fan-out failure neither holds the native interface busy nor blocks the next reliable send");
    checkPayload(third,afterFailure);rig.privacy();require(rig.release(),"native fan-out releases only test-owned sessions and pending peer work");
}

void nativeLoRa(){
    Rig rig;for(const auto &id:{"A","B","C"})rig.add(id);
    QJsonArray nodes;for(int index=0;index<3;++index)nodes.append(node(QString(QChar('A'+index)),ports("radio","radio"),nativeConfiguration(0x100u+quint32(index))));
    const QJsonArray links{link("A","B","iq","radio","radio"),link("B","C","iq","radio","radio"),link("C","B","iq","radio","radio"),link("B","A","iq","radio","radio")};
    require(rig.configure({{"nodes",nodes},{"links",links}}),"real Meshtastic adapters configure through local private profiles");
    const auto payload=QByteArray::fromHex("004e41544956452d4c6f5261ff");
    const auto sent=rig.call(0,{{"op","protocol"},{"args",QJsonObject{{"op","send"},{"destination",258},{"payloadBase64",QString::fromLatin1(payload.toBase64())},{"reliable",true}}}});
    const auto nativeId=scalar(sent.value("protocolReply").toObject().value("nativeId"));
    std::fprintf(stderr,"native LoRa expected nativeId=%s payloadBase64=%s\n",nativeId.toUtf8().constData(),payload.toBase64().constData());
    require(sent.value("ok").toBool()&&sent.value("instanceId")==rig.endpoints[0]->instance&&!nativeId.isEmpty()&&nativeId!="0","native protocol RPC returns identity envelope and actual native packet ID");
    require(rig.await([&](const QVector<QJsonObject> &states){return !nativeResults(states[2],"message",nativeId).isEmpty()&&!nativeResults(states[0],"acknowledged",nativeId).isEmpty();},18000),"native payload and ACK traverse a real LoRa relay in both directions");
    const auto messages=nativeResults(rig.latest[2],"message",nativeId);
    require(messages.size()==1&&QByteArray::fromBase64(messages.first().value("payloadBase64").toString().toLatin1())==payload&&scalar(messages.first().value("from"))=="256"&&scalar(messages.first().value("to"))=="258","Meshtastic delivers exact bytes once with native source and destination addresses");
    require(!messages.first().contains("path")&&!messages.first().contains("source")&&!messages.first().contains("destination")&&counter(rig.latest[0].value("iqTxSamples"))>0&&counter(rig.latest[2].value("iqRxSamples"))>0,"native delivery uses CF32 LoRa and does not invent a laboratory route");
    const auto observed=nativeResults(rig.latest[1],"observed",nativeId),forwarded=nativeResults(rig.latest[1],"sent",nativeId);
    bool observedOriginal=false,sentOriginal=false;
    for(const auto &event:observed)observedOriginal|=scalar(event.value("from"))=="256"&&scalar(event.value("to"))=="258"&&QByteArray::fromBase64(event.value("payloadBase64").toString().toLatin1())==payload;
    for(const auto &event:forwarded)sentOriginal|=scalar(event.value("from"))=="256"&&scalar(event.value("to"))=="258"&&scalar(event.value("packetId"))==nativeId;
    require(observedOriginal&&sentOriginal,"middle node observes exact payload and confirms native forwarding with original source, destination and packet ID");
    rig.privacy();require(rig.release(),"native LoRa releases private profile state and session ownership");
}

void syntheticRadioIngress(){
    const auto source=DspSourceIdentity::normalized("mesh-test-"+uuid(),"rx0");auto &registry=DspIqSourceRegistry::instance();
    const auto baseline=registry.stats().subscribedSources;const bool previouslyActive=Mesh::NodeService::radioCaptureActive();
    struct SourceCleanup {DspSourceIdentity source;~SourceCleanup(){DspIqSourceRegistry::instance().clear(source);}} cleanup{source};
    const auto modemConfiguration=fastLoRa(2);const double center=145500000;
    auto radio=port("radio","rx");radio["backend"]="radio";radio["receiverId"]=source.receiverId;radio["channelId"]=source.channelId;radio["centerHz"]=center;radio["modem"]=modemConfiguration;
    {
        Rig denied;denied.add("Denied");const QJsonObject topology{{"nodes",QJsonArray{node("Denied",{radio,port("radio","tx")},nativeConfiguration(0x100))}},{"links",QJsonArray()}};
        require(!denied.configure(topology)&&!denied.call(0,{{"op","status"}}).value("active").toBool()&&Mesh::NodeService::radioCaptureActive()==previouslyActive,"physical RX cannot subscribe without a local host permission");
    }
    {
        Rig rig;rig.add("Ingress",{{"allowRadioRx",true},{"radioSources",QJsonArray{source.toJson()}}});rig.add("Destination");
        const QJsonArray nodes{node("Ingress",{radio,port("radio","tx")},nativeConfiguration(0x100)),node("Destination",ports("radio","radio"),nativeConfiguration(0x101))};
        const QJsonObject topology{{"nodes",nodes},{"links",QJsonArray{link("Ingress","Destination","data","radio","radio")}}};
        require(rig.configure(topology),"synthetic source is explicitly allowlisted for read-only RX ingress");
        require(Mesh::NodeService::radioCaptureActive()&&registry.stats().subscribedSources==baseline+1,"authorized radio port opts into only its own DSP registry source");
        const auto generatorDirectory=rig.state.filePath("synthetic-generator");require(QDir().mkpath(generatorDirectory),"create isolated synthetic sender counter state");
        auto generator=Mesh::makeMeshtasticAdapter();auto config=nativeConfiguration(0x700);config.remove("type");config.remove("profileId");config["channels"]=localProfile().value("channels");QString error;
        require(generator->start(config,{"radio"},generatorDirectory,&error),"independent native sender generates an over-air Meshtastic frame in memory");
        QByteArray wire;quint64 ticket=0;
        QObject::connect(generator.get(),&Mesh::ProtocolAdapter::transmit,generator.get(),[&](quint64 value,const QString &interfaceId,const QByteArray &bytes){if(interfaceId=="radio"){ticket=value;wire=bytes;}});
        const auto payload=QByteArray::fromHex("0053594e5448455449432d5258ff");
        const auto sent=generator->command({{"op","send"},{"destination",257},{"payloadBase64",QString::fromLatin1(payload.toBase64())},{"reliable",false}},0);generator->step(0);
        require(sent.value("ok").toBool()&&!wire.isEmpty()&&ticket!=0,"synthetic sender emits actual native bytes rather than a laboratory envelope");
        generator->complete(ticket,true,1);
        const auto decoded=obrii::meshtastic::decodeRadio(obrii::meshtastic::Bytes(wire.begin(),wire.end()));
        require(decoded.header.from==0x700&&decoded.header.to==0x101,"synthetic frame carries the intended native addresses");
        Mesh::WireModem modem(modemConfiguration);const auto wave=modem.modulate(wire,&error);require(!wave.isEmpty()&&modem.sampleRate()==250000,"LoRa modem produces synthetic 250 kS/s CF32 input");
        // Repeat each source sample at 500 kS/s; RadioIngress must DDC back to 250 kS/s.
        std::vector<float> wideband(4*(std::size_t(wave.size())+1024),0);std::size_t cursor=4*512;
        for(const auto &sample:wave)for(int repeat=0;repeat<2;++repeat){wideband[cursor++]=sample.real();wideband[cursor++]=sample.imag();}
        require(wideband.size()<=524288&&registry.publish(source,wideband.data(),wideband.size(),500000,center,true),"publish bounded synthetic wideband IQ only to the explicitly subscribed source");
        const auto nativeId=scalar(sent.value("nativeId"));
        std::fprintf(stderr,"synthetic RX expected nativeId=%s payloadBase64=%s\n",nativeId.toUtf8().constData(),payload.toBase64().constData());
        require(rig.await([&](const QVector<QJsonObject> &states){return !nativeResults(states[1],"message",nativeId).isEmpty();},12000),"registry IQ passes RadioIngress DDC, LoRa decode and native forwarding to destination");
        const auto messages=nativeResults(rig.latest[1],"message",nativeId);
        require(messages.size()==1&&QByteArray::fromBase64(messages.first().value("payloadBase64").toString().toLatin1())==payload&&scalar(messages.first().value("from"))=="1792"&&scalar(messages.first().value("to"))=="257","synthetic RX delivers exact native bytes once without falsifying source identity");
        const auto ingress=rig.latest[0].value("radioIngress").toArray();
        require(!ingress.isEmpty()&&ingress.first().toObject().value("state")=="receiving"&&counter(ingress.first().toObject().value("samples"))>0&&counter(rig.latest[0].value("iqRxSamples"))>0&&counter(rig.latest[1].value("iqRxSamples"))==0,"RX telemetry proves registry/DDC decoding followed by independent DATA backhaul");
        rig.privacy();require(rig.release()&&Mesh::NodeService::radioCaptureActive()==previouslyActive&&registry.stats().subscribedSources==baseline,"release removes synthetic RX subscription without touching other registry sources");
        generator->stop();
    }
}
}

int main(int argc,char **argv){
    QCoreApplication app(argc,argv);app.setApplicationName("Obrii mesh port integration self-test");
    auto run=[](const char *name,const std::function<void()> &test){
        std::printf("CASE %s\n",name);std::fflush(stdout);
        try{test();}catch(const std::exception &error){++failures;std::fprintf(stderr,"FAIL %s: %s\n",name,error.what());}
    };
    run("mixed per-link lab DATA and LoRa",mixedLab);
    run("independent queues under a broken neighbor",isolatedQueues);
    run("native DATA fan-out with a blackhole neighbor",nativeFanout);
    run("native Meshtastic over LoRa relay",nativeLoRa);
    run("synthetic allowed radio RX and native forwarding",syntheticRadioIngress);
    std::printf("%d failure(s)\n",failures);return failures?1:0;
}
