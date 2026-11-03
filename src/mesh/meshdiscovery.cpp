#include "meshdiscovery.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSet>
#include <QUuid>
#include <cmath>

namespace Mesh {
namespace {
bool envelope(const QJsonObject &o,const char *op){return o.value("protocol")=="obrii-mesh-discovery"&&o.value("version")==1&&o.value("op")==QLatin1String(op)&&!QUuid(o.value("nonce").toString()).isNull();}
bool port(const QJsonValue &v){return v.isDouble()&&v.toDouble()>=1&&v.toDouble()<=65535&&std::floor(v.toDouble())==v.toDouble();}
QHostAddress group(){return QHostAddress("239.255.79.82");}
}
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
            {"availableBearers",QJsonArray{"data","iq"}},{"active",data.value("active").toBool()}};
        if(!bind.isNull()&&bind!=QHostAddress::Any&&bind!=QHostAddress::AnyIPv4)reply["host"]=bind.toString();
        socket.writeDatagram(QJsonDocument(reply).toJson(QJsonDocument::Compact),d.senderAddress(),quint16(d.senderPort()));
        if(recent.size()>=64)recent.dequeue();recent.enqueue(identity);++responses;
    }
    if(socket.hasPendingDatagrams())QTimer::singleShot(0,this,[this]{receive();});
}
DiscoveryClient::DiscoveryClient(QObject *parent):QObject(parent){
    socket.setParent(this);timer.setParent(this);timer.setSingleShot(true);timer.setInterval(2000);
    connect(&socket,&QUdpSocket::readyRead,this,[this]{receive();});
    connect(&timer,&QTimer::timeout,this,[this]{socket.close();request.clear();emit finished();});
}
bool DiscoveryClient::start(QString *error){
    if(searching()){if(error)*error="Discovery is already running";return false;}
    socket.close();if(!socket.bind(QHostAddress(QHostAddress::AnyIPv4),quint16(0))){if(error)*error=socket.errorString();return false;}
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
    return true;
}
void DiscoveryClient::probe(const QString &host){QHostAddress address;if(searching()&&address.setAddress(host)&&address.protocol()==QAbstractSocket::IPv4Protocol)socket.writeDatagram(request,address,DiscoveryPort);}
void DiscoveryClient::receive(){
    int budget=32;
    while(socket.hasPendingDatagrams()&&budget-->0){
        const auto d=socket.receiveDatagram(1536);if(!searching()||d.data().size()>1024)continue;
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(d.data(),&error);if(error.error!=QJsonParseError::NoError||!doc.isObject())continue;
        const auto r=doc.object();const auto id=r.value("instanceId").toString();
        if(!envelope(r,"instance")||r.value("nonce")!=nonce||QUuid(id).isNull()||!port(r.value("port"))||!r.value("active").isBool()||!r.value("name").isString()||r.value("name").toString().size()>128)continue;
        QHostAddress address(r.value("host").toString(d.senderAddress().toString()));
        if(address.isNull()||address.protocol()!=QAbstractSocket::IPv4Protocol||address==QHostAddress::AnyIPv4||address.isMulticast())continue;
        if(found.contains(id)&&!QHostAddress(found[id].value("host").toString()).isLoopback()&&address.isLoopback())continue;
        if(found.size()>=64&&!found.contains(id))continue;
        QJsonObject entry{{"protocol","obrii-mesh-node"},{"version",1},{"instanceId",id},{"host",address.toString()},{"port",r.value("port")},
            {"name",r.value("name")},{"active",r.value("active")},{"availableBearers",QJsonArray{"data","iq"}}};
        found[id]=entry;emit foundInstance(entry);
    }
    if(socket.hasPendingDatagrams())QTimer::singleShot(0,this,[this]{receive();});
}
}
