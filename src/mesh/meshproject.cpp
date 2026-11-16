#include "meshproject.h"
#include "../openradio/virtualiqhub.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <cmath>

namespace Mesh {
namespace {
bool fail(QString *error, const QString &message) { if(error) *error=message; return false; }
bool number(const QJsonObject &o, const char *key, double minimum, double maximum) {
    const auto v=o.value(QLatin1String(key));
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble()>=minimum && v.toDouble()<=maximum;
}
bool whole(const QJsonObject &o,const char *key,double minimum,double maximum){
    return number(o,key,minimum,maximum)&&std::floor(o.value(key).toDouble())==o.value(key).toDouble();
}
bool portId(const QString &id){return id.size()<=64&&QRegularExpression("^[A-Za-z][A-Za-z0-9_.-]*$").match(id).hasMatch();}
QJsonObject modemJson(const QJsonObject &object);
QJsonObject protocolJson(const QJsonObject &object){
    QJsonObject result;
    if(QStringList{"lab","meshtastic","reticulum","lorawan"}.contains(object.value("type").toString()))result["type"]=object.value("type").toString();
    if(whole(object,"nodeNumber",1,4294967295.0))result["nodeNumber"]=object.value("nodeNumber").toDouble();
    for(const auto key:{"role","relayRole"})if(object.value(key).isString()&&QRegularExpression("^[A-Za-z][A-Za-z0-9_]{0,63}$").match(object.value(key).toString()).hasMatch())result[key]=object.value(key).toString();
    for(const auto key:{"hopLimit","relayDelayMs","relayJitterMs","ackTimeoutMs","retryDelayMs","maxRetries","historyTtlMs","bitrate","mtu"})if(object.value(key).isDouble()&&std::isfinite(object.value(key).toDouble()))result[key]=object.value(key).toDouble();
    for(const auto key:{"relayUnknownChannels","relayPki","transport"})if(object.value(key).isBool())result[key]=object.value(key).toBool();
    for(const auto key:{"interfaceId","framing","profileId"})if(object.value(key).isString()&&object.value(key).toString().size()<=64)result[key]=object.value(key).toString();
    return result;
}
QJsonArray portsJson(const QJsonArray &ports){
    QJsonArray result;
    for(const auto &value:ports){QJsonObject port;const auto input=value.toObject();
        for(const auto key:{"id","direction","backend","receiverId","channelId"})if(input.value(key).isString()&&input.value(key).toString().size()<=128)port[key]=input.value(key).toString();
        if(input.value("enabled").isBool())port["enabled"]=input.value("enabled").toBool();
        if(number(input,"centerHz",0,1e12))port["centerHz"]=input.value("centerHz").toDouble();
        if(input.value("modem").isObject())port["modem"]=modemJson(input.value("modem").toObject());
        result.append(port);
    }
    return result;
}
QJsonObject modemJson(const QJsonObject &object){
    QJsonObject result;
    if(object.value("type")=="afsk1200"||object.value("type")=="lora")result["type"]=object.value("type").toString();
    for(const auto key:{"sf","bw","cr","os","preamble","syncWord","amplitude"})if(object.value(key).isDouble()&&std::isfinite(object.value(key).toDouble()))result[key]=object.value(key).toDouble();
    for(const auto key:{"explicitHeader","payloadCrc","invertIq"})if(object.value(key).isBool())result[key]=object.value(key).toBool();
    if(QStringList{"auto","on","off"}.contains(object.value("ldro").toString()))result["ldro"]=object.value("ldro").toString();
    if(object.value("type")=="lora"){
        const QJsonObject defaults{{"sf",7},{"bw",125000},{"cr",1},{"os",1},{"preamble",16},{"syncWord",43}};
        for(auto it=defaults.begin();it!=defaults.end();++it)if(!result.contains(it.key()))result[it.key()]=it.value();
    }
    return result;
}
}
bool validId(const QString &id) { return QRegularExpression("^[A-Za-z][A-Za-z0-9_.-]{0,63}$").match(id).hasMatch() &&
    !QRegularExpression("^tx[0-9]+$").match(id).hasMatch(); }
QJsonObject HostPreset::json() const { return {{"id",id},{"name",name},{"kind",kind},{"host",host},{"user",user},{"directory",directory},{"sshPort",sshPort}}; }
QJsonObject Node::json() const {
    QJsonObject result{{"id",id},{"name",name},{"preset",presetId},{"x",position.x()},{"y",position.y()},
        {"listeningHz",listeningHz},{"bandwidthHz",bandwidthHz},{"mode",mode},{"txLevel",txLevel},{"enabled",enabled}};
    if(!ports.isEmpty())result["ports"]=portsJson(ports);
    if(!protocol.isEmpty())result["protocol"]=protocolJson(protocol);
    return result;
}
QJsonObject Link::json() const {
    QJsonObject result{{"tx",tx},{"rx",rx},{"gainDb",gainDb},{"delayMs",delayMs},{"enabled",enabled}};
    if(bearer!="inherit")result["bearer"]=bearer;
    if(txPort!="radio")result["txPort"]=txPort;
    if(rxPort!="radio")result["rxPort"]=rxPort;
    if(!modem.isEmpty())result["modem"]=modemJson(modem);
    if(frequencyOffsetHz!=0)result["frequencyOffsetHz"]=frequencyOffsetHz;
    if(clockPpm!=0)result["clockPpm"]=clockPpm;
    return result;
}
QJsonArray defaultNodePorts(){
    return {QJsonObject{{"id","radio"},{"direction","rx"},{"backend","network"},{"enabled",true},{"receiverId","main"},{"channelId","rx0"}},
        QJsonObject{{"id","radio"},{"direction","tx"},{"backend","network"},{"enabled",true},{"channelId","rx0"}}};
}
QString validateNodeOptions(const Node &node){
    if(node.ports.size()>8||QJsonDocument(node.protocol).toJson(QJsonDocument::Compact).size()>4096)return "Node options exceed limits: "+node.id;
    QSet<QString> identities;
    for(const auto &value:node.ports){
        const auto port=value.toObject();const auto direction=port.value("direction").toString(),id=port.value("id").toString();
        if(!value.isObject()||!portId(id)||(direction!="rx"&&direction!="tx")||identities.contains(direction+"/"+id)||
            (port.contains("backend")&&port.value("backend")!="network"&&port.value("backend")!="radio")||
            (port.contains("enabled")&&!port.value("enabled").isBool()))return "Invalid/duplicate directional port: "+node.id;
        for(const auto key:{"receiverId","channelId"})if(port.contains(key)&&(!port.value(key).isString()||port.value(key).toString().size()>64||port.value(key).toString().contains(QRegularExpression("[\\x00-\\x1f]"))))return "Invalid port binding: "+node.id;
        if(port.contains("centerHz")&&!number(port,"centerHz",0,1e12))return "Invalid port center frequency: "+node.id;
        if(port.contains("modem")){
            if(!port.value("modem").isObject())return "Invalid port modem schema: "+node.id;
            Link option;option.modem=port.value("modem").toObject();const auto error=validateLinkOptions(option);if(!error.isEmpty())return error;
        }
        identities.insert(direction+"/"+id);
    }
    const auto protocol=node.protocol;
    if(protocol.contains("type")&&!QStringList{"lab","meshtastic","reticulum","lorawan"}.contains(protocol.value("type").toString()))return "Unknown node protocol: "+node.id;
    if(protocol.contains("nodeNumber")&&!whole(protocol,"nodeNumber",1,4294967295.0))return "Invalid Meshtastic node number: "+node.id;
    for(const auto key:{"role","relayRole"})if(protocol.contains(key)&&(!protocol.value(key).isString()||!QRegularExpression("^[A-Za-z][A-Za-z0-9_]{0,63}$").match(protocol.value(key).toString()).hasMatch()))return "Invalid protocol role: "+node.id;
    for(const auto key:{"hopLimit","relayDelayMs","relayJitterMs","ackTimeoutMs","retryDelayMs","maxRetries","historyTtlMs","bitrate","mtu"})if(protocol.contains(key)&&!whole(protocol,key,0,86400000))return "Invalid numeric protocol option: "+QString(key);
    for(const auto key:{"relayUnknownChannels","relayPki","transport"})if(protocol.contains(key)&&!protocol.value(key).isBool())return "Invalid protocol flag: "+QString(key);
    for(const auto key:{"interfaceId","framing","profileId"})if(protocol.contains(key)&&(!protocol.value(key).isString()||protocol.value(key).toString().size()>64))return "Invalid protocol selector: "+QString(key);
    if(protocol.contains("profileId")&&!protocol.value("profileId").toString().isEmpty()&&!portId(protocol.value("profileId").toString()))return "Invalid local profile ID";
    return {};
}
QString validateLinkOptions(const Link &link){
    if(!QStringList{"inherit","data","iq"}.contains(link.bearer)||!portId(link.txPort)||!portId(link.rxPort))return "Invalid bearer or port selector: "+link.tx+"/"+link.rx;
    const auto modem=link.modem;
    if(QJsonDocument(modem).toJson(QJsonDocument::Compact).size()>2048)return "Modem options exceed limits";
    if(modem.contains("type")&&modem.value("type")!="afsk1200"&&modem.value("type")!="lora")return "Unknown mesh modem";
    for(const auto key:{"sf","bw","cr","os","preamble","syncWord"})if(modem.contains(key)){
        double low=0,high=255;
        if(QString(key)=="sf"){low=7;high=12;}else if(QString(key)=="bw"){low=31250;high=500000;}
        else if(QString(key)=="cr"){low=1;high=4;}else if(QString(key)=="os"){low=1;high=8;}else if(QString(key)=="preamble"){low=6;high=1024;}
        if(!whole(modem,key,low,high))return "Invalid mesh modem parameter: "+QString(key);
    }
    if(modem.contains("os")&&!QList<int>{1,2,4,8}.contains(modem.value("os").toInt()))return "LoRa oversampling must be 1, 2, 4 or 8";
    if(modem.contains("amplitude")&&(!number(modem,"amplitude",0,1)||modem.value("amplitude").toDouble()<=0))return "Invalid modem amplitude";
    for(const auto key:{"explicitHeader","payloadCrc","invertIq"})if(modem.contains(key)&&!modem.value(key).isBool())return "Invalid mesh modem flag";
    if(modem.contains("ldro")&&!QStringList{"auto","on","off"}.contains(modem.value("ldro").toString()))return "Invalid LoRa LDRO mode";
    return {};
}
bool validLinkFrequencyOffset(double offsetHz,int sampleRate,bool mixIq){
    return offsetHz==0||(mixIq&&std::isfinite(offsetHz)&&std::abs(offsetHz)<sampleRate/2.0);
}
bool validLinkClockPpm(double ppm,bool mixIq){
    return ppm==0||(mixIq&&std::isfinite(ppm)&&std::abs(ppm)<=VirtualIqLinkSettings::MaximumClockPpm);
}
QJsonObject Project::json() const {
    QJsonArray n,l; for(const auto &v:nodes)n.append(v.json()); for(const auto &v:links)l.append(v.json());
    return {{"format","obrii-mesh-project"},{"version",1},{"centerHz",centerHz},{"sampleRate",sampleRate},{"mixIq",mixIq},{"nodes",n},{"links",l}};
}
QString validatePresets(const QVector<HostPreset> &presets) {
    QSet<QString> ids;
    for(const auto &p:presets) {
        if(!validId(p.id)||ids.contains(p.id)||p.name.trimmed().isEmpty()||p.name.size()>200||p.directory.trimmed().isEmpty()||p.directory.size()>4096)
            return QString::fromUtf8("Неповний профіль або повторний ID: ")+p.id;
        ids.insert(p.id);
        if(p.kind!="local"&&p.kind!="ssh-posix")return "Unknown host type: "+p.kind;
        if(p.kind=="ssh-posix" && (p.host.isEmpty()||p.host.size()>253||p.host.startsWith('-')||p.host.contains(QRegularExpression("[\\s\\x00-\\x1f]"))||
            !QRegularExpression("^[A-Za-z0-9_.-]+$").match(p.user).hasMatch()||p.user.startsWith('-')||p.sshPort<1||p.sshPort>65535||!p.directory.startsWith('/')))
            return QString::fromUtf8("Перевір адресу, SSH-користувача, порт і абсолютний POSIX-шлях: ")+p.name;
    }
    return {};
}
QString Project::validate(const QVector<HostPreset> &presets) const {
    if(!std::isfinite(centerHz)||centerHz<=0||centerHz>1e12||std::floor(centerHz)!=centerHz||sampleRate<48000||sampleRate>2000000)return "Invalid medium center/rate (whole Hz required)";
    const auto pe=validatePresets(presets); if(!pe.isEmpty())return pe;
    QSet<QString> hosts,ids,pairs; for(const auto &p:presets)hosts.insert(p.id);
    for(const auto &n:nodes) {
        if(!validId(n.id)||ids.contains(n.id)||!hosts.contains(n.presetId)||n.name.trimmed().isEmpty()||n.name.size()>200)return "Invalid/duplicate node or missing device preset: "+n.id;
        ids.insert(n.id);
        const auto options=validateNodeOptions(n);if(!options.isEmpty())return options;
        if(n.mode=="wfm"&&sampleRate<=200000)return "WFM requires a session sample rate above 200000 S/s";
        if(!std::isfinite(n.position.x())||!std::isfinite(n.position.y())||std::abs(n.position.x())>1e7||std::abs(n.position.y())>1e7||
           !std::isfinite(n.bandwidthHz)||n.bandwidthHz<100||n.bandwidthHz>=sampleRate||!std::isfinite(n.listeningHz)||
           std::abs(n.listeningHz-centerHz)+n.bandwidthHz/2>=sampleRate/2.0||!std::isfinite(n.txLevel)||n.txLevel<0||n.txLevel>0.95||
           !QStringList{"am","nfm","wfm","usb","lsb","cw"}.contains(n.mode))return "Invalid radio parameters: "+n.id;
    }
    for(const auto &l:links) {
        const QString key=l.tx+"/"+l.rx;
        if(!ids.contains(l.tx)||!ids.contains(l.rx)||l.tx==l.rx||pairs.contains(key)||!std::isfinite(l.gainDb)||l.gainDb< -120||l.gainDb>60||l.delayMs<0||l.delayMs>500)
            return "Invalid/duplicate link: "+key;
        if(!validLinkFrequencyOffset(l.frequencyOffsetHz,sampleRate,mixIq))
            return "Link frequency offset requires sample-mix mode and must be below half the sample rate: "+key;
        if(!validLinkClockPpm(l.clockPpm,mixIq))
            return "Link clock error requires sample-mix mode and must be within ±1000 ppm: "+key;
        pairs.insert(key);
        const auto options=validateLinkOptions(l);if(!options.isEmpty())return options;
    }
    return {};
}
bool Project::parse(const QJsonObject &o, Project *out, QString *error) {
    if(o.value("format")!="obrii-mesh-project"||o.value("version")!=1||!o.value("nodes").isArray()||!o.value("links").isArray()||!o.value("mixIq").isBool()||
       !number(o,"centerHz",1,1e12)||!number(o,"sampleRate",48000,2000000)||std::floor(o.value("sampleRate").toDouble())!=o.value("sampleRate").toDouble())return fail(error,"Invalid mesh project schema/version");
    Project p; p.centerHz=o.value("centerHz").toDouble();p.sampleRate=o.value("sampleRate").toInt();p.mixIq=o.value("mixIq").toBool();
    for(const auto &v:o.value("nodes").toArray()) {
        if(!v.isObject())return fail(error,"Invalid node"); const auto n=v.toObject();
        if(!number(n,"x",-1e7,1e7)||!number(n,"y",-1e7,1e7)||!number(n,"listeningHz",0,1e12)||!number(n,"bandwidthHz",100,2e6)||!number(n,"txLevel",0,0.95))return fail(error,"Invalid node parameters");
        if((n.contains("ports")&&!n.value("ports").isArray())||(n.contains("protocol")&&!n.value("protocol").isObject()))return fail(error,"Invalid node ports/protocol schema");
        Node node{n.value("id").toString(),n.value("name").toString(),n.value("preset").toString(),{n.value("x").toDouble(),n.value("y").toDouble()},n.value("listeningHz").toDouble(),n.value("bandwidthHz").toDouble(),n.value("txLevel").toDouble(),n.value("mode").toString()};
        node.ports=n.value("ports").toArray();node.protocol=n.value("protocol").toObject();
        if(n.contains("enabled")&&!n.value("enabled").isBool())return fail(error,"Node enabled must be boolean");
        node.enabled=n.value("enabled").toBool(true);
        const auto options=validateNodeOptions(node);if(!options.isEmpty())return fail(error,options);p.nodes.push_back(node);
    }
    for(const auto &v:o.value("links").toArray()) {
        if(!v.isObject())return fail(error,"Invalid link");const auto l=v.toObject();
        if(!number(l,"gainDb",-120,60)||!number(l,"delayMs",0,500)||std::floor(l.value("delayMs").toDouble())!=l.value("delayMs").toDouble()||!l.value("enabled").isBool())return fail(error,"Invalid link parameters");
        if((l.contains("bearer")&&!l.value("bearer").isString())||(l.contains("txPort")&&!l.value("txPort").isString())||
            (l.contains("rxPort")&&!l.value("rxPort").isString())||(l.contains("modem")&&!l.value("modem").isObject()))return fail(error,"Invalid link selectors/modem schema");
        Link link{l.value("tx").toString(),l.value("rx").toString(),l.value("gainDb").toDouble(),l.value("delayMs").toInt(),l.value("enabled").toBool()};
        const auto offset=l.value("frequencyOffsetHz");
        if(!offset.isUndefined()&&(!offset.isDouble()||!validLinkFrequencyOffset(offset.toDouble(),p.sampleRate,p.mixIq)))
            return fail(error,"Invalid link frequency offset for this medium");
        link.frequencyOffsetHz=offset.toDouble();
        const auto clock=l.value("clockPpm");
        if(!clock.isUndefined()&&(!clock.isDouble()||!validLinkClockPpm(clock.toDouble(),p.mixIq)))
            return fail(error,"Invalid link clock error for this medium");
        link.clockPpm=clock.toDouble();
        link.bearer=l.value("bearer").toString("inherit");link.txPort=l.value("txPort").toString("radio");link.rxPort=l.value("rxPort").toString("radio");link.modem=l.value("modem").toObject();
        const auto options=validateLinkOptions(link);if(!options.isEmpty())return fail(error,options);p.links.push_back(link);
    }
    *out=p;return true;
}
bool saveJson(const QString &path,const QJsonObject &o,QString *error) {
    QSaveFile f(path); if(!f.open(QIODevice::WriteOnly))return fail(error,f.errorString());const auto bytes=QJsonDocument(o).toJson();
    if(f.write(bytes)!=bytes.size()||!f.commit())return fail(error,f.errorString());return true;
}
bool loadJson(const QString &path,QJsonObject *o,QString *error) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly))return fail(error,f.errorString());if(f.size()>16*1024*1024)return fail(error,"JSON exceeds 16 MiB");
    QJsonParseError pe;const auto d=QJsonDocument::fromJson(f.readAll(),&pe);if(pe.error!=QJsonParseError::NoError||!d.isObject())return fail(error,"Invalid JSON object");*o=d.object();return true;
}
bool saveLibrary(const QString &path,const QVector<HostPreset> &p,QString *error) {
    const auto e=validatePresets(p);if(!e.isEmpty())return fail(error,e);QJsonArray a;for(const auto &v:p)a.append(v.json());
    return saveJson(path,{{"format","obrii-device-library"},{"version",1},{"devices",a}},error);
}
bool loadLibrary(const QString &path,QVector<HostPreset> *out,QString *error) {
    QJsonObject o;if(!loadJson(path,&o,error))return false;if(o.value("format")!="obrii-device-library"||o.value("version")!=1||!o.value("devices").isArray())return fail(error,"Invalid device library schema/version");
    QVector<HostPreset> presets;
    for(const auto &v:o.value("devices").toArray()) {
        if(!v.isObject())return fail(error,"Invalid device profile"); const auto p=v.toObject();
        if(!number(p,"sshPort",1,65535)||std::floor(p.value("sshPort").toDouble())!=p.value("sshPort").toDouble())return fail(error,"Invalid SSH port");
        presets.push_back({p.value("id").toString(),p.value("name").toString(),p.value("kind").toString(),p.value("host").toString(),p.value("user").toString(),p.value("directory").toString(),p.value("sshPort").toInt()});
    }
    const auto e=validatePresets(presets);if(!e.isEmpty())return fail(error,e);*out=presets;return true;
}
QString executable(const HostPreset &p,const QString &name) {
    return QDir(p.directory).filePath(name+
#ifdef Q_OS_WIN
        (p.kind=="local"?".exe":"")
#else
        ""
#endif
    );
}
Project example(const QString &preset) {
    Project p;
    for(int i=0;i<3;++i){Node n;n.id=QString("node%1").arg(i+1);n.name=QString("SDR %1").arg(QChar('A'+i));n.presetId=preset;n.position={double(i*250),double(i%2*110)};p.nodes<<n;}
    p.links={{"node1","node2",-6,10,true},{"node2","node3",-12,20,true}};return p;
}
}
