#include "meshports.h"
#include "meshwiremodem.h"
#include "meshproject.h"
#include <QSet>
#include <cmath>

namespace Mesh {
namespace {
bool fail(QString *error,const QString &s){if(error)*error=s;return false;}
bool field(const QJsonObject &o,const char *key,QJsonValue::Type type){return !o.contains(key)||o.value(key).type()==type;}
bool whole(const QJsonValue &v){return v.isDouble()&&std::isfinite(v.toDouble())&&std::floor(v.toDouble())==v.toDouble();}
}
QJsonObject PortBinding::json() const {return {{"id",id},{"direction",direction},{"backend",backend},{"enabled",enabled},{"receiverId",receiverId},{"channelId",channelId},{"centerHz",centerHz},{"modem",modem}};}
QJsonObject LinkBinding::json() const {return {{"tx",tx},{"rx",rx},{"txPort",txPort},{"rxPort",rxPort},{"bearer",bearer},{"enabled",enabled},{"delayMs",delayMs},{"gainDb",gainDb},{"modem",modem}};}
bool PortPlan::configure(const QJsonObject &t,const QString &fallback,QString *error){
    if(fallback!="data"&&fallback!="iq")return fail(error,"Invalid default bearer");
    PortPlan candidate;QSet<QString> ids;
    if(!t.value("nodes").isArray()||!field(t,"links",QJsonValue::Array))return fail(error,"Nodes and links must be arrays");
    const auto ns=t.value("nodes").toArray();if(ns.isEmpty()||ns.size()>8)return fail(error,"Port plan requires 1..8 nodes");
    for(int index=0;index<ns.size();++index){const auto n=ns[index].toObject();const auto id=n.value("id").toString();
        if(!ns[index].isObject()||!validId(id)||ids.contains(id)||!field(n,"enabled",QJsonValue::Bool)||!field(n,"ports",QJsonValue::Array)||!field(n,"protocol",QJsonValue::Object))return fail(error,"Invalid node identity or port/protocol fields");ids.insert(id);candidate.names.append(QJsonObject{{"id",id},{"name",n.value("name").toString(id)},{"enabled",n.value("enabled").toBool(true)}});
        auto ps=n.value("ports").toArray();if(ps.isEmpty())ps={QJsonObject{{"id","radio"},{"direction","rx"}},QJsonObject{{"id","radio"},{"direction","tx"}}};
        if(ps.size()>8)return fail(error,"At most eight independent ports per node");
        QSet<QString> ports;
        for(const auto &v:ps){const auto p=v.toObject();PortBinding b;b.id=p.value("id").toString();b.direction=p.value("direction").toString();b.backend=p.value("backend").toString("network");b.enabled=p.value("enabled").toBool(true);
            b.receiverId=p.value("receiverId").toString("main");b.channelId=p.value("channelId").toString("rx0");b.centerHz=p.value("centerHz").toDouble(145500000);
            if(!v.isObject()||!validId(b.id)||(b.direction!="rx"&&b.direction!="tx")||(b.backend!="network"&&b.backend!="radio")||ports.contains(b.direction+"/"+b.id)||!std::isfinite(b.centerHz)||b.centerHz<0||b.centerHz>1e12||!field(p,"enabled",QJsonValue::Bool)||!field(p,"centerHz",QJsonValue::Double)||!field(p,"modem",QJsonValue::Object)||!validId(b.receiverId)||!validId(b.channelId))
                return fail(error,"Invalid or duplicate RX/TX port");
            if(!WireModem::normalize(p.value("modem").toObject(),&b.modem,error))return false;
            ports.insert(b.direction+"/"+b.id);candidate.bindings[id].append(b);
        }
        auto protocol=n.value("protocol").toObject();const auto type=protocol.value("type").toString("lab");
        if(!field(protocol,"type",QJsonValue::String)||!QStringList{"lab","meshtastic","reticulum","lorawan"}.contains(type))return fail(error,"Unknown native protocol");
        protocol["type"]=type;if(type=="meshtastic"&&!protocol.contains("nodeNumber"))protocol["nodeNumber"]=0x100+index;
        candidate.protocols[id]=protocol;
    }
    QSet<QString> pairs;
    for(const auto &v:t.value("links").toArray()){const auto o=v.toObject();LinkBinding l;l.tx=o.value("tx").toString();l.rx=o.value("rx").toString();l.txPort=o.value("txPort").toString("radio");l.rxPort=o.value("rxPort").toString("radio");l.bearer=o.value("bearer").toString(fallback);if(l.bearer=="inherit")l.bearer=fallback;
        l.enabled=o.value("enabled").toBool(true);l.delayMs=o.value("delayMs").toInt(0);l.gainDb=o.value("gainDb").toDouble(-6);
        const auto tx=candidate.port(l.tx,l.txPort,"tx"),rx=candidate.port(l.rx,l.rxPort,"rx");
        if(!v.isObject()||!tx||!rx||l.tx==l.rx||pairs.contains(l.tx+"/"+l.rx)||(l.bearer!="data"&&l.bearer!="iq")||!field(o,"enabled",QJsonValue::Bool)||!field(o,"modem",QJsonValue::Object)||!field(o,"gainDb",QJsonValue::Double)||(o.contains("delayMs")&&!whole(o.value("delayMs")))||!std::isfinite(l.gainDb)||l.gainDb< -120||l.gainDb>60||l.delayMs<0||l.delayMs>500)return fail(error,"Invalid per-link bearer, ports or channel conditions");
        if(l.enabled&&(!tx->enabled||!rx->enabled))l.enabled=false;
        if(l.enabled&&(tx->backend!="network"||rx->backend!="network"))return fail(error,"TCP links require network RX/TX ports; physical ingress/egress is bound locally");
        if(!WireModem::normalize(o.value("modem").toObject(),&l.modem,error))return false;
        const auto txType=candidate.protocol(l.tx).value("type"),rxType=candidate.protocol(l.rx).value("type");
        if(l.enabled&&txType!=rxType)return fail(error,"A radio link must connect matching protocols; explicit protocol gateways are required for translation");
        pairs.insert(l.tx+"/"+l.rx);candidate.links.append(l);
    }
    *this=std::move(candidate);return true;
}
const LinkBinding *PortPlan::link(const QString &tx,const QString &rx) const {for(const auto &l:links)if(l.tx==tx&&l.rx==rx)return &l;return nullptr;}
const PortBinding *PortPlan::port(const QString &node,const QString &id,const QString &direction) const {const auto i=bindings.constFind(node);if(i==bindings.cend())return nullptr;for(const auto &p:*i)if(p.id==id&&p.direction==direction)return &p;return nullptr;}
QVector<LinkBinding> PortPlan::outgoing(const QString &node,const QString &p) const {QVector<LinkBinding> result;for(const auto &l:links)if(l.tx==node&&l.enabled&&(p.isEmpty()||l.txPort==p))result.append(l);return result;}
QJsonObject PortPlan::routingTopology() const {QJsonArray ls;for(const auto &l:links)ls.append(l.json());return {{"nodes",names},{"links",ls}};}
QJsonObject PortPlan::topology() const {auto result=routingTopology();QJsonArray ns;for(const auto &v:names){auto n=v.toObject();QJsonArray ps;for(const auto &p:bindings.value(n.value("id").toString()))ps.append(p.json());n["ports"]=ps;n["protocol"]=protocols.value(n.value("id").toString());ns.append(n);}result["nodes"]=ns;return result;}
}
