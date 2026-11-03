#include "meshnode.h"
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
#include <QVector>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr int ReplyLimit=128*1024;
int failures=0;
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
void events(){QCoreApplication::processEvents(QEventLoop::AllEvents,10);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
void require(bool okay,const char *name){
    if(!okay)throw std::runtime_error(name);
    std::printf("PASS %s\n",name);std::fflush(stdout);
}
quint64 count(const QJsonValue &value){return value.isString()?value.toString().toULongLong():quint64(value.toDouble());}
QJsonObject rpc(int port,const QString &token,QJsonObject request){
    const auto requestId=uuid();request["requestId"]=requestId;
    if(request.value("op")!="pair")request["token"]=token;
    QTcpSocket socket;socket.setReadBufferSize(ReplyLimit+1);bool failed=false;
    QObject::connect(&socket,&QTcpSocket::connected,&socket,[&]{socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');});
    QObject::connect(&socket,&QTcpSocket::errorOccurred,&socket,[&](QAbstractSocket::SocketError error){if(error!=QAbstractSocket::RemoteHostClosedError)failed=true;});
    socket.connectToHost(QHostAddress::LocalHost,quint16(port));QByteArray bytes;QElapsedTimer timer;timer.start();
    while(timer.elapsed()<3000){
        events();bytes+=socket.readAll();if(bytes.size()>ReplyLimit)return {};
        if(bytes.contains('\n')||failed||socket.state()==QAbstractSocket::UnconnectedState)break;
        QThread::msleep(2);
    }
    const auto end=bytes.indexOf('\n');if(end<0)return {};
    QJsonParseError error;const auto document=QJsonDocument::fromJson(bytes.left(end),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject()||document.object().value("requestId")!=requestId)return {};
    return document.object();
}
QJsonObject port(const QString &id,const QString &direction){return {{"id",id},{"direction",direction},{"backend","network"},{"enabled",true}};}
QJsonObject node(const QString &id,const QJsonObject &protocol={{"type","lab"}}){
    return {{"id",id},{"name",id},{"enabled",true},{"ports",QJsonArray{port("radio","rx"),port("radio","tx")}},{"protocol",protocol}};
}
QJsonObject link(const QString &tx,const QString &rx,bool enabled=true){
    return {{"tx",tx},{"rx",rx},{"txPort","radio"},{"rxPort","radio"},{"bearer","data"},{"enabled",enabled},{"delayMs",2},{"gainDb",0}};
}
QJsonObject result(const QJsonObject &state,const QString &id,const QString &status){
    for(const auto &value:state.value("results").toArray()){
        const auto event=value.toObject();if(event.value("id")==id&&event.value("status")==status)return event;
    }return {};
}
QJsonObject linkState(const QJsonObject &state,const QString &rx){
    for(const auto &value:state.value("links").toArray())if(value.toObject().value("rx")==rx)return value.toObject();return {};
}
QJsonObject editNode(QJsonObject topology,const QString &id,const std::function<void(QJsonObject &)> &edit){
    auto nodes=topology.value("nodes").toArray();
    for(int i=0;i<nodes.size();++i){auto value=nodes[i].toObject();if(value.value("id")==id){edit(value);nodes[i]=value;}}
    topology["nodes"]=nodes;return topology;
}
QJsonObject stableState(const QJsonObject &state){
    QJsonObject result;
    for(const auto name:{"instanceId","active","listening","session","nodeId","transport","topologyRevision","liveTopologyVersion",
            "ports","links","nativeProtocol","queue","tx","rx","delivered","forwarded","dropped","iqTxSamples","iqRxSamples",
            "lastResultSerial","resultsEvicted","results"})result[name]=state.value(name);
    return result;
}
void retained(const QJsonObject &before,const QJsonObject &after,const QString &revision){
    require(after.value("ok").toBool()&&after.value("active").toBool()&&after.value("listening").toBool()
        &&after.value("session")==before.value("session")&&after.value("instanceId")==before.value("instanceId")
        &&after.value("nodeId")==before.value("nodeId")&&after.value("transport")==before.value("transport")
        &&after.value("ports")==before.value("ports")&&after.value("nativeProtocol")==before.value("nativeProtocol")
        &&after.value("topologyRevision")==revision&&after.value("liveTopologyVersion")==1,
        "live update retains local bindings, identity and session and advances the decimal revision");
    bool countersRetained=true,historyRetained=true;
    for(const auto name:{"tx","rx","delivered","forwarded","dropped","iqTxSamples","iqRxSamples","lastResultSerial","resultsEvicted"})
        countersRetained&=after.value(name).isString()&&count(after.value(name))>=count(before.value(name));
    for(const auto &event:before.value("results").toArray())historyRetained&=after.value("results").toArray().contains(event);
    require(countersRetained,"live update does not reset telemetry counters");
    require(historyRetained,"live update retains existing result history and serials");
}

class Rig {
public:
    struct Endpoint {
        QString id,admin,token,instance,code;
        int port=0;
        bool configured=false;
        std::unique_ptr<Mesh::NodeService> service;
    };
    QTemporaryDir state;
    std::vector<std::unique_ptr<Endpoint>> endpoints;
    QString client=uuid(),session=uuid(),key=uuid()+uuid();
    QVector<QJsonObject> latest;
    Rig(){if(!state.isValid())throw std::runtime_error("temporary protocol state directory");}
    ~Rig(){
        for(int i=0;i<int(endpoints.size());++i)if(endpoints[std::size_t(i)]->configured)call(i,{{"op","release"}});
        for(auto &endpoint:endpoints)endpoint->service->command({{"operation","stop"}});
        events();
    }
    QString pair(Endpoint &endpoint,const QString &clientId){
        const auto reply=rpc(endpoint.port,{},{{"op","pair"},{"instanceId",endpoint.instance},{"clientId",clientId},
            {"clientName","Live topology regression"},{"code",endpoint.code}});
        require(reply.value("ok").toBool()&&reply.value("instanceId")==endpoint.instance&&!reply.value("token").toString().isEmpty(),"pair a test-owned client over loopback");
        return reply.value("token").toString();
    }
    void add(const QString &id){
        auto endpoint=std::make_unique<Endpoint>();endpoint->id=id;endpoint->service=std::make_unique<Mesh::NodeService>();
        const auto root=state.filePath("host-"+id);require(QDir().mkpath(QDir(root).filePath(id)),"create private temporary node state");
        const QJsonObject profile{{"channels",QJsonArray{QJsonObject{{"name","LiveTopology"},{"pskBase64","AQ=="},{"enabled",true}}}}};
        const auto started=endpoint->service->command({{"operation","start"},{"bind","127.0.0.1"},{"port",0},
            {"protocolStateDirectory",root},{"protocolProfiles",QJsonObject{{"integration",profile}}}});
        require(started.value("ok").toBool()&&!started.value("active").toBool(),"start an isolated NodeService without SDR or RF");
        endpoint->admin=started.value("token").toString();endpoint->code=started.value("pairingSecret").toString();
        endpoint->instance=started.value("instanceId").toString();endpoint->port=started.value("port").toInt();
        require(!endpoint->admin.isEmpty()&&!endpoint->instance.isEmpty()&&endpoint->port>0,"pin actual loopback endpoint identity");
        endpoint->token=pair(*endpoint,client);endpoints.push_back(std::move(endpoint));
    }
    QJsonObject peers() const {
        QJsonObject result;for(const auto &endpoint:endpoints)result[endpoint->id]=QJsonObject{{"host","127.0.0.1"},{"port",endpoint->port},{"instanceId",endpoint->instance}};return result;
    }
    QJsonObject call(int index,QJsonObject request){
        const auto &endpoint=*endpoints.at(std::size_t(index));
        if(!request.contains("instanceId"))request["instanceId"]=endpoint.instance;
        if(!request.contains("session"))request["session"]=session;
        return rpc(endpoint.port,endpoint.token,request);
    }
    QJsonObject status(int index){
        const auto reply=call(index,{{"op","status"}});require(reply.value("ok").toBool(),"owned status returns a bounded correlated RPC reply");return reply;
    }
    void configure(int index,const QJsonObject &topology,const QJsonObject &peers){
        const auto reply=call(index,{{"op","configure"},{"key",key},{"nodeId",endpoints[std::size_t(index)]->id},
            {"transport","data"},{"topology",topology},{"peers",peers}});
        if(reply.value("ok").toBool())endpoints[std::size_t(index)]->configured=true;
        require(reply.value("ok").toBool()&&reply.value("active").toBool()&&reply.value("session")==session
            &&reply.value("topologyRevision")=="1"&&reply.value("liveTopologyVersion")==1,
            "unchanged configure request starts an owned session at topology revision string 1");
    }
    QJsonObject updateRequest(const QJsonObject &topology,const QJsonObject &peers,const QJsonValue &revision){
        return {{"op","updateTopology"},{"expectedRevision",revision},{"topology",topology},{"peers",peers}};
    }
    void update(int index,const QJsonObject &topology,const QJsonObject &peers,const QString &revision){
        const auto before=status(index);const auto reply=call(index,updateRequest(topology,peers,revision));
        require(reply.value("ok").toBool(),"owner updateTopology accepts a full topology and peers without reconfiguration");
        retained(before,status(index),QString::number(revision.toULongLong()+1));
    }
    void reject(int index,QJsonObject request,const char *label,const QString &token={}){
        const auto before=status(index);QJsonObject reply;
        if(token.isEmpty())reply=call(index,request);
        else {
            auto &endpoint=*endpoints[std::size_t(index)];request["instanceId"]=endpoint.instance;request["session"]=session;
            reply=rpc(endpoint.port,token,request);
        }
        require(reply.value("ok").isBool()&&!reply.value("ok").toBool()&&!reply.value("error").toString().isEmpty(),label);
        require(stableState(status(index))==stableState(before),"rejected update leaves revision, ownership, bindings, history and counters unchanged");
    }
    void send(int source,const QString &id,const QString &destination,const QByteArray &payload){
        require(call(source,{{"op","send"},{"id",id},{"destination",destination},{"ttl",4},
            {"payloadBase64",QString::fromLatin1(payload.toBase64())}}).value("ok").toBool(),"owned service accepts a binary DATA packet");
    }
    bool await(const std::function<bool(const QVector<QJsonObject> &)> &predicate,int milliseconds=5000){
        QElapsedTimer timer;timer.start();
        do {
            latest.clear();for(int i=0;i<int(endpoints.size());++i)latest.append(call(i,{{"op","status"}}));
            if(predicate(latest))return true;
            events();QThread::msleep(5);
        }while(timer.elapsed()<milliseconds);
        for(int i=0;i<latest.size();++i){const auto &s=latest[i];
            std::fprintf(stderr,"node %s active=%d revision=%s queue=%llu tx=%llu rx=%llu delivered=%llu dropped=%llu\n",
                endpoints[std::size_t(i)]->id.toUtf8().constData(),int(s.value("active").toBool()),s.value("topologyRevision").toString().toUtf8().constData(),
                static_cast<unsigned long long>(count(s.value("queue"))),static_cast<unsigned long long>(count(s.value("tx"))),
                static_cast<unsigned long long>(count(s.value("rx"))),static_cast<unsigned long long>(count(s.value("delivered"))),static_cast<unsigned long long>(count(s.value("dropped"))));
        }return false;
    }
    void delivered(int destination,const QString &id,const QByteArray &payload,const QJsonArray &path){
        require(await([&](const QVector<QJsonObject> &states){
            if(result(states[destination],id,"delivered").isEmpty())return false;
            for(const auto &state:states)if(count(state.value("queue"))!=0)return false;
            return true;
        }),"packet delivers and retained services drain their DATA queues");
        const auto outcome=result(latest[destination],id,"delivered");
        require(outcome.value("path").toArray()==path&&QByteArray::fromBase64(outcome.value("payloadBase64").toString().toLatin1())==payload,
            "delivery reports the actual recomputed route and exact binary bytes");
    }
    void release(int index){
        const auto reply=call(index,{{"op","release"}});
        require(reply.value("ok").toBool()&&!reply.value("active").toBool()&&reply.value("listening").toBool(),"release only the removed node session, keeping its listener alive");
        endpoints[std::size_t(index)]->configured=false;
    }
};

void routesAndRejections(){
    Rig rig;for(const auto &id:{"A","B","C"})rig.add(id);
    QJsonObject topology{{"nodes",QJsonArray{node("A"),node("B"),node("C")}},
        {"links",QJsonArray{link("A","B"),link("B","C"),link("C","B"),link("B","A"),link("A","C",false),link("C","A",false)}}};
    auto peers=rig.peers();for(int i=0;i<3;++i)rig.configure(i,topology,peers);
    const auto payload=QByteArray::fromHex("006c6976652d746f706f6c6f6779ff");
    rig.send(0,"via_b","C",payload);rig.delivered(2,"via_b",payload,{"A","B","C"});
    require(count(rig.latest[1].value("forwarded"))>0&&count(rig.latest[2].value("iqRxSamples"))==0,"B actually forwards DATA with no IQ or RF");

    // All rejection probes are issued after drain so exact snapshots are meaningful.
    auto request=rig.updateRequest(topology,peers,"1");
    for(const auto &bad:QJsonArray{QJsonValue(),QJsonValue(1),QJsonValue(true),"","0","-1","1.0","1e0","garbage","18446744073709551616"}){
        auto invalid=request;invalid["expectedRevision"]=bad;rig.reject(0,invalid,"reject an invalid or non-string expectedRevision");
    }
    auto missing=request;missing.remove("expectedRevision");rig.reject(0,missing,"reject missing expectedRevision");
    auto foreign=request;foreign["session"]=uuid();rig.reject(0,foreign,"reject another session's topology update");
    auto staleInstance=request;staleInstance["instanceId"]=uuid();rig.reject(0,staleInstance,"reject an update pinned to another service instance");
    auto &a=*rig.endpoints[0];const auto otherToken=rig.pair(a,uuid());
    rig.reject(0,request,"reject a paired non-owner using the correct session",otherToken);
    rig.reject(0,request,"admin authentication does not replace session ownership",a.admin);
    auto duplicate=peers;auto b=duplicate.value("B").toObject();b["instanceId"]=a.instance;duplicate["B"]=b;
    rig.reject(0,rig.updateRequest(topology,duplicate,"1"),"reject duplicate peer instance identities");
    auto localPeer=peers;auto local=localPeer.value("A").toObject();local["port"]=local.value("port").toInt()==65535?65534:local.value("port").toInt()+1;localPeer["A"]=local;
    rig.reject(0,rig.updateRequest(topology,localPeer,"1"),"reject a changed local listener binding");
    const auto changedPorts=editNode(topology,"A",[](QJsonObject &n){auto ports=n.value("ports").toArray();ports.append(port("extra","tx"));n["ports"]=ports;});
    rig.reject(0,rig.updateRequest(changedPorts,peers,"1"),"reject changed local port bindings");
    auto changedProtocol=topology;
    for(const auto &id:{"A","B","C"})changedProtocol=editNode(changedProtocol,id,[](QJsonObject &n){
        n["protocol"]=QJsonObject{{"type","meshtastic"},{"profileId","integration"},{"nodeNumber",256+n.value("id").toString().at(0).unicode()-'A'}};
    });
    rig.reject(0,rig.updateRequest(changedProtocol,peers,"1"),"reject changed local protocol");
    const auto unavailable=editNode(topology,"A",[](QJsonObject &n){n["enabled"]=false;});
    rig.reject(0,rig.updateRequest(unavailable,peers,"1"),"reject disabled local node");
    const QJsonObject removed{{"nodes",QJsonArray{node("B"),node("C")}},{"links",QJsonArray{link("B","C"),link("C","B")}}};
    auto noA=peers;noA.remove("A");rig.reject(0,rig.updateRequest(removed,noA,"1"),"reject removal of the configured local node");
    auto badTopology=topology;auto badLinks=topology.value("links").toArray();badLinks.append(link("A","Missing"));badTopology["links"]=badLinks;
    rig.reject(0,rig.updateRequest(badTopology,peers,"1"),"reject invalid full topology");
    rig.send(0,"after_rejections","C",payload);rig.delivered(2,"after_rejections",payload,{"A","B","C"});

    topology["links"]=QJsonArray{link("A","B",false),link("B","C",false),link("C","B",false),link("B","A",false),link("A","C"),link("C","A")};
    // Each retained service is updated explicitly; no cross-service atomicity is assumed.
    for(int i=0;i<3;++i)rig.update(i,topology,peers,"1");
    rig.reject(0,request,"reject stale revision after a successful update");
    const auto relayCount=count(rig.status(1).value("forwarded"));
    rig.send(0,"direct","C",payload);rig.delivered(2,"direct",payload,{"A","C"});
    rig.send(2,"direct_reverse","A",payload);rig.delivered(0,"direct_reverse",payload,{"C","A"});
    require(count(rig.status(1).value("forwarded"))==relayCount,"disabled B links no longer carry packets");

    rig.release(1);topology=editNode(topology,"B",[](QJsonObject &n){n["enabled"]=false;});
    for(const int i:{0,2})rig.update(i,topology,peers,"2");
    rig.reject(1,rig.updateRequest(topology,peers,"2"),"inactive service cannot accept a live topology update");
    rig.send(0,"b_disabled","C",payload);rig.delivered(2,"b_disabled",payload,{"A","C"});

    rig.add("D");peers=rig.peers();auto nodes=topology.value("nodes").toArray();nodes.append(node("D"));topology["nodes"]=nodes;
    auto links=topology.value("links").toArray();links.append(link("C","D"));links.append(link("D","C"));topology["links"]=links;
    rig.configure(3,topology,peers);
    for(const int i:{0,2})rig.update(i,topology,peers,"3");
    rig.send(0,"new_d","D",payload);rig.delivered(3,"new_d",payload,{"A","C","D"});
    rig.send(3,"from_d","A",payload);rig.delivered(0,"from_d",payload,{"D","C","A"});
    require(rig.status(0).value("session")==rig.session&&rig.status(2).value("session")==rig.session
        &&rig.status(3).value("session")==rig.session&&!rig.status(1).value("active").toBool(),"A and C keep their original sessions as D joins and B stays released");
}

// Hold a real LinkSender handshake, then forward its original bytes to NodeService.
class GatePeer : public QObject {
public:
    QTcpServer server;
    QVector<QPointer<QTcpSocket>> held;
    int connections=0;
    GatePeer(){
        if(!server.listen(QHostAddress::LocalHost,0))throw std::runtime_error("isolated loopback gate listen");
        connect(&server,&QTcpServer::newConnection,this,[this]{while(auto socket=server.nextPendingConnection()){
            ++connections;socket->setReadBufferSize(ReplyLimit);held.append(socket);
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        }});
    }
    int live() const {int result=0;for(const auto &socket:held)if(socket&&socket->state()==QAbstractSocket::ConnectedState)++result;return result;}
    void open(int port){
        for(const auto &guard:held)if(guard&&guard->state()==QAbstractSocket::ConnectedState){
            auto incoming=guard.data();
            auto upstream=new QTcpSocket(incoming);upstream->setReadBufferSize(ReplyLimit);
            connect(upstream,&QTcpSocket::connected,incoming,[incoming,upstream]{upstream->write(incoming->readAll());});
            connect(incoming,&QTcpSocket::readyRead,upstream,[incoming,upstream]{if(upstream->state()==QAbstractSocket::ConnectedState)upstream->write(incoming->readAll());});
            connect(upstream,&QTcpSocket::readyRead,incoming,[incoming,upstream]{incoming->write(upstream->readAll());});
            connect(upstream,&QTcpSocket::disconnected,incoming,&QTcpSocket::disconnectFromHost);
            upstream->connectToHost(QHostAddress::LocalHost,quint16(port));
        }
    }
};

void affectedAndUnrelatedWork(){
    Rig rig;for(const auto &id:{"A","B","C"})rig.add(id);GatePeer affected,unrelated;
    const QJsonObject topology{{"nodes",QJsonArray{node("A"),node("B"),node("C")}},{"links",QJsonArray{link("A","B"),link("A","C")}}};
    auto peers=rig.peers();auto b=peers.value("B").toObject(),c=peers.value("C").toObject();
    b["port"]=affected.server.serverPort();c["port"]=unrelated.server.serverPort();peers["B"]=b;peers["C"]=c;
    for(int i=0;i<3;++i)rig.configure(i,topology,i==0?peers:rig.peers());
    const auto payload=QByteArray::fromHex("0071756575652d6c697665ff");
    rig.send(0,"cancel_old_b","B",payload);rig.send(0,"preserve_c","C",payload);
    require(rig.await([&](const QVector<QJsonObject> &states){return affected.live()==1&&unrelated.live()==1
        &&count(linkState(states[0],"B").value("queued"))==1&&count(linkState(states[0],"C").value("queued"))==1;}),"affected and unrelated senders each have genuine in-flight handshakes");
    const auto before=rig.status(0);peers["B"]=rig.peers().value("B");
    for(int i=0;i<3;++i)rig.update(i,topology,i==0?peers:rig.peers(),"1");
    require(rig.await([&](const QVector<QJsonObject> &states){return affected.live()==0&&unrelated.live()==1
        &&count(linkState(states[0],"B").value("queued"))==0&&count(linkState(states[0],"C").value("queued"))==1;}),"changed remote peer cancels affected work and leaves unrelated work in flight");
    require(affected.connections==1&&unrelated.connections==1&&count(rig.latest[0].value("tx"))==count(before.value("tx")),"live update neither restarts the unrelated sender nor fabricates a successful transmit");
    rig.send(0,"new_peer_b","B",payload);
    require(rig.await([](const QVector<QJsonObject> &states){return !result(states[1],"new_peer_b","delivered").isEmpty();}),"updated remote peer endpoint is used for subsequent delivery");
    unrelated.open(rig.endpoints[2]->port);rig.delivered(2,"preserve_c",payload,{"A","C"});
    const auto bState=rig.status(1);
    require(result(bState,"cancel_old_b","delivered").isEmpty()&&result(bState,"new_peer_b","delivered").value("path").toArray()==QJsonArray{"A","B"}
        &&QByteArray::fromBase64(result(bState,"new_peer_b","delivered").value("payloadBase64").toString().toLatin1())==payload,
        "canceled work never delivers while fresh work reaches the real replacement endpoint");

    b["port"]=affected.server.serverPort();peers["B"]=b;
    for(int i=0;i<3;++i)rig.update(i,topology,i==0?peers:rig.peers(),"2");
    rig.send(0,"disable_inflight_b","B",payload);rig.send(0,"preserve_c_link","C",payload);
    require(rig.await([&](const QVector<QJsonObject> &states){return affected.live()==1&&unrelated.live()==1
        &&count(linkState(states[0],"B").value("queued"))==1&&count(linkState(states[0],"C").value("queued"))==1;}),"queue affected and unrelated work before disabling a link");
    auto disabled=topology;disabled["links"]=QJsonArray{link("A","B",false),link("A","C")};
    for(int i=0;i<3;++i)rig.update(i,disabled,i==0?peers:rig.peers(),"3");
    require(rig.await([&](const QVector<QJsonObject> &states){return affected.live()==0&&unrelated.live()==1
        &&count(linkState(states[0],"B").value("queued"))==0&&count(linkState(states[0],"C").value("queued"))==1;}),"disabling a link cancels its in-flight socket without canceling the unchanged link");
    require(affected.connections==2&&unrelated.connections==2,"neither cancellation path silently reconnects unaffected queued work");
    unrelated.open(rig.endpoints[2]->port);rig.delivered(2,"preserve_c_link",payload,{"A","C"});
    require(result(rig.status(1),"disable_inflight_b","delivered").isEmpty(),"work canceled by link disable is not delivered later");
}

QJsonObject nativeEvent(const QJsonObject &state,const QString &status,quint64 id){
    for(const auto &value:state.value("results").toArray()){
        const auto event=value.toObject();if(event.value("protocol")=="meshtastic"&&event.value("status")==status&&count(event.value("nativeId"))==id)return event;
    }return {};
}
QJsonObject nativeConfiguration(int id){
    return {{"type","meshtastic"},{"profileId","integration"},{"nodeNumber",id},
        {"relayRole","router"},{"relayDelayMs",0},{"relayJitterMs",0},{"ackTimeoutMs",8000},{"retryDelayMs",20},{"maxRetries",1},{"historyTtlMs",60000}};
}
void nativeCancellation(){
    Rig rig;rig.add("A");rig.add("C");GatePeer gate;
    QJsonObject topology{{"nodes",QJsonArray{node("A",nativeConfiguration(256)),node("C",nativeConfiguration(258))}},
        {"links",QJsonArray{link("A","C"),link("C","A")}}};
    const auto peers=rig.peers();auto heldPeers=peers;auto remote=heldPeers.value("C").toObject();remote["port"]=gate.server.serverPort();heldPeers["C"]=remote;
    rig.configure(0,topology,heldPeers);rig.configure(1,topology,peers);
    const auto send=[&](const QByteArray &payload){
        const auto reply=rig.call(0,{{"op","protocol"},{"args",QJsonObject{{"op","send"},{"destination",258},
            {"payloadBase64",QString::fromLatin1(payload.toBase64())},{"reliable",true}}}});
        const auto id=count(reply.value("protocolReply").toObject().value("nativeId"));
        require(reply.value("ok").toBool()&&id>0,"native cancellation fixture accepts a reliable DATA packet");return id;
    };
    const auto active=send(QByteArray::fromHex("0063616e63656c2d616374697665ff"));
    require(rig.await([&](const QVector<QJsonObject> &states){return gate.live()==1&&count(linkState(states[0],"C").value("queued"))==1
        &&nativeEvent(states[0],"sent",active).isEmpty();}),"native transmission is genuinely in flight on a held peer handshake");
    const auto queued=send(QByteArray::fromHex("0063616e63656c2d717565756564ff"));
    require(queued>active&&rig.await([](const QVector<QJsonObject> &states){return count(states[0].value("nativeProtocol").toObject().value("queued"))>=1
        &&count(linkState(states[0],"C").value("queued"))==1;}),"second native packet queues behind the unfinished native transmission");

    topology["links"]=QJsonArray{link("A","C",false),link("C","A")};
    // Completing cancellation changes native pending/queued state, so do not use the quiescent update helper.
    for(int i=0;i<2;++i)require(rig.call(i,rig.updateRequest(topology,i==0?heldPeers:peers,"1")).value("ok").toBool(),
        "explicitly update both retained native endpoints while disabling the in-flight link");
    require(rig.await([&](const QVector<QJsonObject> &states){const auto native=states[0].value("nativeProtocol").toObject();
        return gate.live()==0&&count(states[0].value("queue"))==0&&count(native.value("queued"))==0&&count(native.value("pending"))==0
            &&!nativeEvent(states[0],"tx_failed",active).isEmpty()&&!nativeEvent(states[0],"tx_failed",queued).isEmpty();
    }),"link cancellation finishes active native TX and drains queued native work instead of leaving the adapter busy");
    require(nativeEvent(rig.latest[0],"sent",active).isEmpty()&&nativeEvent(rig.latest[1],"message",active).isEmpty()
        &&nativeEvent(rig.latest[1],"message",queued).isEmpty(),"canceled native packets are not reported sent or delivered");

    topology["links"]=QJsonArray{link("A","C"),link("C","A")};
    for(int i=0;i<2;++i)rig.update(i,topology,peers,"2");
    const auto payload=QByteArray::fromHex("006e61746976652d61667465722d63616e63656cff");const auto next=send(payload);
    require(next>queued&&rig.await([&](const QVector<QJsonObject> &states){const auto native=states[0].value("nativeProtocol").toObject();
        return !nativeEvent(states[1],"message",next).isEmpty()&&!nativeEvent(states[0],"sent",next).isEmpty()
            &&!nativeEvent(states[0],"acknowledged",next).isEmpty()&&count(native.value("queued"))==0&&count(native.value("pending"))==0
            &&count(states[0].value("queue"))==0&&count(states[1].value("queue"))==0;
    }),"subsequent native DATA send and ACK complete after link restoration without adapter restart");
    const auto delivered=nativeEvent(rig.latest[1],"message",next);
    require(count(delivered.value("from"))==256&&count(delivered.value("to"))==258&&count(delivered.value("packetId"))==next
        &&QByteArray::fromBase64(delivered.value("payloadBase64").toString().toLatin1())==payload,"post-cancellation delivery retains native identity and exact binary payload");
    require(nativeEvent(rig.latest[1],"message",active).isEmpty()&&nativeEvent(rig.latest[1],"message",queued).isEmpty(),"restoring the link does not resurrect canceled packets");
    for(int i=0;i<2;++i)require(rig.latest[i].value("session")==rig.session&&rig.latest[i].value("instanceId")==rig.endpoints[std::size_t(i)]->instance
        &&rig.latest[i].value("active").toBool()&&rig.latest[i].value("topologyRevision")=="3"
        &&rig.latest[i].value("nativeProtocol").toObject().value("ready").toBool()&&count(rig.latest[i].value("iqTxSamples"))==0,
        "native cancellation recovery preserves sessions, revisions and adapter readiness using only DATA");
}
void nativeContinuity(){
    Rig rig;rig.add("A");rig.add("C");
    QJsonObject topology{{"nodes",QJsonArray{node("A",nativeConfiguration(256)),node("C",nativeConfiguration(258))}},
        {"links",QJsonArray{link("A","C"),link("C","A")}}};
    const auto peers=rig.peers();for(int i=0;i<2;++i)rig.configure(i,topology,peers);
    const auto payload=QByteArray::fromHex("006e61746976652d6c697665ff");
    const auto send=[&]{
        const auto reply=rig.call(0,{{"op","protocol"},{"args",QJsonObject{{"op","send"},{"destination",258},
            {"payloadBase64",QString::fromLatin1(payload.toBase64())},{"reliable",true}}}});
        const auto id=count(reply.value("protocolReply").toObject().value("nativeId"));
        require(reply.value("ok").toBool()&&id>0,"native DATA send returns an actual native packet ID");return id;
    };
    const auto delivered=[&](quint64 id){
        require(rig.await([&](const QVector<QJsonObject> &states){
            const auto native=states[0].value("nativeProtocol").toObject();
            return !nativeEvent(states[1],"message",id).isEmpty()&&!nativeEvent(states[0],"acknowledged",id).isEmpty()
                &&count(native.value("queued"))==0&&count(native.value("pending"))==0&&count(states[0].value("queue"))==0&&count(states[1].value("queue"))==0;
        }),"native message and ACK complete using only loopback DATA");
        const auto message=nativeEvent(rig.latest[1],"message",id);
        require(count(message.value("from"))==256&&count(message.value("to"))==258&&count(message.value("packetId"))==id
            &&QByteArray::fromBase64(message.value("payloadBase64").toString().toLatin1())==payload,"native delivery preserves original identity, packet ID and binary payload");
    };
    const auto first=send();delivered(first);
    const auto changedIdentity=editNode(topology,"A",[](QJsonObject &n){auto protocol=n.value("protocol").toObject();protocol["nodeNumber"]=999;n["protocol"]=protocol;});
    rig.reject(0,rig.updateRequest(changedIdentity,peers,"1"),"reject changes to an existing native identity without resetting its adapter");
    const auto beforeA=rig.status(0),beforeC=rig.status(1);
    auto links=topology.value("links").toArray();auto changed=links[0].toObject();changed["delayMs"]=3;links[0]=changed;topology["links"]=links;
    for(int i=0;i<2;++i)rig.update(i,topology,peers,"1");
    const auto afterA=rig.status(0),afterC=rig.status(1);
    require(beforeA.value("nativeProtocol")==afterA.value("nativeProtocol")&&beforeC.value("nativeProtocol")==afterC.value("nativeProtocol")
        &&count(afterA.value("nativeProtocol").toObject().value("nativeAddress"))==256
        &&count(afterC.value("nativeProtocol").toObject().value("nativeAddress"))==258
        &&count(afterA.value("tx"))==count(beforeA.value("tx"))&&count(afterC.value("delivered"))==count(beforeC.value("delivered")),
        "native identity, readiness and nonzero service counters survive an affected-link update");
    const auto second=send();require(second>first,"native packet counter continues instead of reusing an earlier ID");delivered(second);
    require(!nativeEvent(rig.latest[1],"message",first).isEmpty()&&count(rig.latest[1].value("delivered"))>count(beforeC.value("delivered"))
        &&count(rig.latest[0].value("iqTxSamples"))==0&&count(rig.latest[1].value("iqRxSamples"))==0,
        "native history and delivery counters accumulate across live updates without IQ or RF");
}
}

int main(int argc,char **argv){
    QCoreApplication app(argc,argv);app.setApplicationName("Obrii live topology service self-test");
    const auto run=[](const char *name,const std::function<void()> &test){
        std::printf("CASE %s\n",name);std::fflush(stdout);
        try{test();}catch(const std::exception &error){++failures;std::fprintf(stderr,"FAIL %s: %s\n",name,error.what());}
    };
    run("live routes, endpoint append and rejection rollback",routesAndRejections);
    run("changed peer cancels only affected in-flight work",affectedAndUnrelatedWork);
    run("native in-flight cancellation releases adapter busy state",nativeCancellation);
    run("native identity and counters survive live updates",nativeContinuity);
    std::printf("%d failure(s)\n",failures);return failures?1:0;
}
