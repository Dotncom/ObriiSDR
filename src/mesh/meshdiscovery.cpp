#include "meshdiscovery.h"
#include "meshnodedirectory.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSet>
#include <QUuid>
#include <QTcpSocket>
#include <QDateTime>
#include <QPointer>
#include <cmath>

namespace Mesh {
namespace {
bool envelope(const QJsonObject &o,const char *op){return o.value("protocol")=="obrii-mesh-discovery"&&o.value("version")==1&&o.value("op")==QLatin1String(op)&&!QUuid(o.value("nonce").toString()).isNull();}
bool port(const QJsonValue &v){return v.isDouble()&&v.toDouble()>=1&&v.toDouble()<=65535&&std::floor(v.toDouble())==v.toDouble();}
QHostAddress group(){return QHostAddress("239.255.79.82");}
bool unicast(const QString &host){QHostAddress address;if(!address.setAddress(host)||address.protocol()!=QAbstractSocket::IPv4Protocol||address.toString()!=host)return false;const auto first=address.toIPv4Address()>>24;return first>0&&first<224;}
class PublicRpc : public QObject {
public:
    using Reply=std::function<void(PublicRpc *,const QJsonObject &,bool)>;
    PublicRpc(QObject *parent):QObject(parent){
        timeout.setSingleShot(true);socket.setReadBufferSize(65537);
        connect(&timeout,&QTimer::timeout,this,[this]{done({},false);});
        connect(&socket,&QTcpSocket::connected,this,[this]{socket.write(wire);wire.clear();});
        connect(&socket,&QTcpSocket::readyRead,this,[this]{read();});
        connect(&socket,&QTcpSocket::disconnected,this,[this]{read();if(!completed)done({},false);});
        connect(&socket,&QTcpSocket::errorOccurred,this,[this](QAbstractSocket::SocketError e){if(e!=QAbstractSocket::RemoteHostClosedError)done({},false);});
    }
    void start(const QString &host,quint16 port,QJsonObject request,Reply callback){
        reply=std::move(callback);id=QUuid::createUuid().toString(QUuid::WithoutBraces);request["requestId"]=id;
        wire=QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n';timeout.start(1200);socket.connectToHost(host,port);
    }
    void cancel(){completed=true;reply={};timeout.stop();socket.disconnect(this);socket.abort();wire.clear();}
private:
    QTcpSocket socket;QTimer timeout;QByteArray wire,buffer;QString id;Reply reply;bool completed=false;
    void read(){
        if(completed)return;buffer+=socket.readAll();if(buffer.size()>65536){done({},false);return;}
        const int end=buffer.indexOf('\n');if(end<0)return;
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(buffer.left(end),&error);
        const auto result=doc.object();done(result,error.error==QJsonParseError::NoError&&doc.isObject()&&buffer.mid(end+1).trimmed().isEmpty()&&result.value("ok")==true&&result.value("requestId")==id);
    }
    void done(const QJsonObject &value,bool okay){
        if(completed)return;completed=true;timeout.stop();socket.disconnect(this);socket.abort();auto callback=std::move(reply);deleteLater();if(callback)callback(this,value,okay);
    }
};
}
struct DiscoveryClient::RoutedState {
    struct Job {QString host;quint16 port=0;bool directory=false;QString expected;};
    DiscoveryClient *owner;QJsonArray configured;QQueue<Job> pending;QHash<PublicRpc *,Job> active;QSet<QString> queued;
    QElapsedTimer elapsed;QTimer deferred;quint64 generation=0;int udp=0,tcp=0,catalogs=0,failed=0;
    explicit RoutedState(DiscoveryClient *o):owner(o){deferred.setSingleShot(true);QObject::connect(&deferred,&QTimer::timeout,o,[this]{pump();});}
    void abort(){++generation;deferred.stop();const auto calls=active.keys();active.clear();pending.clear();queued.clear();for(auto call:calls){call->cancel();call->deleteLater();}}
    void remember(QJsonObject entry,const QString &method){
        entry=publicDiscoveryEntry(entry);if(entry.isEmpty())return;
        const auto id=QUuid(entry.value("instanceId").toString()).toString(QUuid::WithoutBraces);entry["instanceId"]=id;
        if(owner->found.size()>=64&&!owner->found.contains(id))return;
        const auto previous=owner->found.value(id);
        if(!previous.isEmpty()&&!QHostAddress(previous.value("host").toString()).isLoopback()&&QHostAddress(entry.value("host").toString()).isLoopback())return;
        auto methods=previous.value("discoveryMethods").toArray();if(!methods.contains(method))methods.append(method);
        entry["discoveryMethods"]=methods;entry["lastSeenUtcMs"]=double(QDateTime::currentMSecsSinceEpoch());owner->found[id]=entry;emit owner->foundInstance(entry);
    }
    void add(const QString &host,quint16 port,bool directory,const QString &expected={}){
        if(!owner->searching()||!unicast(host)||!port)return;
        const auto key=host+":"+QString::number(port)+(directory?"/directory":"/seed")+expected;
        if(queued.contains(key)||queued.size()>=96)return;queued.insert(key);pending.enqueue({host,port,directory,expected});pump();
    }
    QJsonObject replyEntry(const QJsonObject &reply,const Job &job) const {
        QJsonObject entry;for(const auto key:{"instanceId","name","active","availableBearers","directoryVersion"})if(reply.contains(key))entry[key]=reply.value(key);
        entry["protocol"]="obrii-mesh-node";entry["version"]=1;entry["host"]=job.host;entry["port"]=job.port;
        return publicDiscoveryEntry(entry);
    }
    void pump(){
        if(!owner->searching())return;
        if(elapsed.elapsed()<250){if(!deferred.isActive())deferred.start(int(250-elapsed.elapsed()));return;}
        const auto current=generation;
        while(active.size()<4&&!pending.isEmpty()){
            const auto job=pending.dequeue();bool known=false;
            if(!job.directory&&job.expected.isEmpty())for(const auto &entry:owner->found)known|=entry.value("host")==job.host&&entry.value("port")==job.port;
            if(known)continue;
            auto call=new PublicRpc(owner);active.insert(call,job);
            call->start(job.host,job.port,{{"op",job.directory?"directoryList":"discover"},{"protocol","obrii-mesh-discovery"},{"version",1},{"nonce",owner->nonce}},
                [this,current,job](PublicRpc *rpc,const QJsonObject &reply,bool okay){
                    if(current!=generation||!owner->searching())return;active.remove(rpc);
                    const QPointer<DiscoveryClient> guard(owner);
                    okay=okay&&envelope(reply,job.directory?"directory":"instance")&&reply.value("nonce")==owner->nonce;
                    if(okay&&job.directory){
                        const auto entries=reply.value("entries").toArray();const auto catalog=replyEntry(reply,job);
                        okay=reply.value("directoryVersion")==1&&!catalog.isEmpty()&&reply.value("entries").isArray()&&entries.size()<=64;
                        if(okay){++catalogs;remember(catalog,"tcp");if(!guard||current!=generation||!owner->searching())return;for(const auto &v:entries){const auto entry=publicDiscoveryEntry(v.toObject());if(!entry.isEmpty())add(entry.value("host").toString(),quint16(entry.value("port").toInt()),false,entry.value("instanceId").toString());}}
                    }else if(okay){
                        const auto entry=replyEntry(reply,job);okay=!entry.isEmpty()&&(job.expected.isEmpty()||QUuid(job.expected)==QUuid(entry.value("instanceId").toString()));
                        if(okay){++tcp;remember(entry,job.expected.isEmpty()?"tcp":"directory");if(!guard||current!=generation||!owner->searching())return;}
                    }
                    if(!okay)++failed;pump();
                });
        }
        if(pending.isEmpty()&&active.isEmpty()&&elapsed.elapsed()>=2000)owner->finish();
    }
};
DiscoveryResponder::DiscoveryResponder(std::function<QJsonObject()> f,QObject *parent):QObject(parent),metadata(std::move(f)){
    socket.setParent(this);window.start();connect(&socket,&QUdpSocket::readyRead,this,[this]{receive();});
}
bool DiscoveryResponder::start(){
    if(!socket.bind(QHostAddress::AnyIPv4,DiscoveryPort,QUdpSocket::ShareAddress|QUdpSocket::ReuseAddressHint))return false;
    // Multicast loopback complements broadcast when Windows shares the UDP port.
    for(const auto &iface:QNetworkInterface::allInterfaces())if(iface.flags().testFlag(QNetworkInterface::IsUp)&&iface.flags().testFlag(QNetworkInterface::CanMulticast))socket.joinMulticastGroup(group(),iface);
    return true;
}
void DiscoveryResponder::receive(){
    if(window.elapsed()>1000){window.restart();responses=0;}
    int budget=32;
    while(socket.hasPendingDatagrams()&&budget-->0){
        const auto d=socket.receiveDatagram(1536);if(d.data().size()>512||d.senderAddress().protocol()!=QAbstractSocket::IPv4Protocol)continue;
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(d.data(),&error);if(error.error!=QJsonParseError::NoError||!doc.isObject()||!envelope(doc.object(),"probe")||responses>=16)continue;
        const auto r=doc.object();const auto identity=d.senderAddress().toString()+"/"+QString::number(d.senderPort())+"/"+r.value("nonce").toString();if(recent.contains(identity))continue;
        const auto data=metadata();QHostAddress bind(data.value("bind").toString());
        if(bind.isLoopback()&&!d.senderAddress().isLoopback()&&!QNetworkInterface::allAddresses().contains(d.senderAddress()))continue;
        QJsonObject reply{{"protocol","obrii-mesh-discovery"},{"version",1},{"op","instance"},{"nonce",r.value("nonce")},
            {"instanceId",data.value("instanceId")},{"port",data.value("port")},{"name",data.value("name").toString().left(128)},
            {"availableBearers",data.value("availableBearers").toArray(QJsonArray{"data","iq"})},{"active",data.value("active").toBool()}};
        if(!bind.isNull()&&bind!=QHostAddress::Any&&bind!=QHostAddress::AnyIPv4)reply["host"]=bind.toString();
        if(data.value("directoryVersion")==1)reply["directoryVersion"]=1;
        socket.writeDatagram(QJsonDocument(reply).toJson(QJsonDocument::Compact),d.senderAddress(),quint16(d.senderPort()));
        if(recent.size()>=64)recent.dequeue();recent.enqueue(identity);++responses;
    }
    if(socket.hasPendingDatagrams())QTimer::singleShot(0,this,[this]{receive();});
}
DiscoveryClient::DiscoveryClient(QObject *parent):QObject(parent),routed(new RoutedState(this)){
    socket.setParent(this);timer.setParent(this);timer.setSingleShot(true);timer.setInterval(2000);
    connect(&socket,&QUdpSocket::readyRead,this,[this]{receive();});
    connect(&timer,&QTimer::timeout,this,[this]{
        if((!routed->pending.isEmpty()||!routed->active.isEmpty())&&routed->elapsed.elapsed()<5000){timer.start(100);routed->pump();return;}finish();
    });
}
DiscoveryClient::~DiscoveryClient(){cancel();}
void DiscoveryClient::cancel(){timer.stop();socket.close();request.clear();routed->abort();}
void DiscoveryClient::finish(){cancel();emit finished();}
QJsonObject DiscoveryClient::statistics() const {return {{"udpReplies",routed->udp},{"tcpReplies",routed->tcp},{"directoryReplies",routed->catalogs},{"failedProbes",routed->failed}};}
bool DiscoveryClient::setEndpoints(const QJsonArray &entries,QString *error){
    const auto fail=[&](const char *why){if(error)*error=QLatin1String(why);return false;};
    if(searching())return fail("Wait for the current search");if(entries.size()>48)return fail("At most 48 saved search endpoints");
    QSet<QString> seen;int directories=0;
    for(const auto &v:entries){const auto e=v.toObject();const auto role=e.value("role").toString();
        if(!v.isObject()||!e.value("enabled").isBool()||(role!="seed"&&role!="directory")||!unicast(e.value("host").toString())||!port(e.value("port"))||!e.value("name").isString()||e.value("name").toString().size()>64)return fail("Invalid saved search endpoint");
        for(auto it=e.begin();it!=e.end();++it)if(!QStringList{"enabled","role","name","host","port"}.contains(it.key()))return fail("Unknown saved search setting");
        const auto key=role+"/"+e.value("host").toString()+"/"+QString::number(e.value("port").toInt());if(seen.contains(key))return fail("Duplicate saved search endpoint");seen.insert(key);
        if(role=="directory"&&++directories>8)return fail("At most eight directories");
    }routed->configured=entries;if(error)error->clear();return true;
}
bool DiscoveryClient::start(QString *error){
    if(searching()){if(error)*error="Discovery is already running";return false;}
    socket.close();if(!socket.bind(QHostAddress(QHostAddress::AnyIPv4),quint16(0))){if(error)*error=socket.errorString();return false;}
    routed->abort();routed->elapsed.start();routed->udp=routed->tcp=routed->catalogs=routed->failed=0;
    found.clear();nonce=QUuid::createUuid().toString(QUuid::WithoutBraces);
    request=QJsonDocument(QJsonObject{{"protocol","obrii-mesh-discovery"},{"version",1},{"op","probe"},{"nonce",nonce}}).toJson(QJsonDocument::Compact);
    timer.start();QSet<QString> destinations{"255.255.255.255","127.255.255.255"};
    for(const auto &iface:QNetworkInterface::allInterfaces())if(iface.flags().testFlag(QNetworkInterface::IsUp))
        for(const auto &address:iface.addressEntries())if(address.ip().protocol()==QAbstractSocket::IPv4Protocol){
            if(!address.broadcast().isNull())destinations.insert(address.broadcast().toString());
            destinations.insert(address.ip().toString());
        }
    for(const auto &host:destinations)probe(host);
    socket.setSocketOption(QAbstractSocket::MulticastTtlOption,1);socket.setSocketOption(QAbstractSocket::MulticastLoopbackOption,1);
    for(const auto &iface:QNetworkInterface::allInterfaces())if(iface.flags().testFlag(QNetworkInterface::IsUp)&&iface.flags().testFlag(QNetworkInterface::CanMulticast)){
        socket.setMulticastInterface(iface);socket.writeDatagram(request,group(),DiscoveryPort);
    }
    for(const auto &v:routed->configured){const auto e=v.toObject();if(e.value("enabled").toBool())probeEndpoint(e.value("host").toString(),quint16(e.value("port").toInt()),e.value("role")=="directory");}
    return true;
}
void DiscoveryClient::probe(const QString &host){QHostAddress address;if(searching()&&address.setAddress(host)&&address.protocol()==QAbstractSocket::IPv4Protocol)socket.writeDatagram(request,address,DiscoveryPort);}
void DiscoveryClient::probeEndpoint(const QString &host,quint16 servicePort,bool directory){if(!unicast(host)||!servicePort)return;probe(host);routed->add(host,servicePort,directory);}
void DiscoveryClient::receive(){
    int budget=32;
    while(socket.hasPendingDatagrams()&&budget-->0){
        const auto d=socket.receiveDatagram(1536);if(!searching()||d.data().size()>1024)continue;
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(d.data(),&error);if(error.error!=QJsonParseError::NoError||!doc.isObject())continue;
        const auto r=doc.object();const auto id=r.value("instanceId").toString();
        if(!envelope(r,"instance")||r.value("nonce")!=nonce||QUuid(id).isNull()||!port(r.value("port"))||!r.value("active").isBool()||!r.value("name").isString()||r.value("name").toString().size()>128)continue;
        QHostAddress address(r.value("host").toString(d.senderAddress().toString()));
        if(address.isNull()||address.protocol()!=QAbstractSocket::IPv4Protocol||address==QHostAddress::AnyIPv4||address.isMulticast())continue;
        QJsonObject entry{{"protocol","obrii-mesh-node"},{"version",1},{"instanceId",id},{"host",address.toString()},{"port",r.value("port")},
            {"name",r.value("name")},{"active",r.value("active")},{"availableBearers",r.value("availableBearers")}};
        if(r.contains("directoryVersion"))entry["directoryVersion"]=r.value("directoryVersion");
        if(publicDiscoveryEntry(entry).isEmpty())continue;
        ++routed->udp;routed->remember(entry,"udp");
    }
    if(socket.hasPendingDatagrams())QTimer::singleShot(0,this,[this]{receive();});
}
}
