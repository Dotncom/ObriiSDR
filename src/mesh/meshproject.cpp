#include "meshproject.h"
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
}
bool validId(const QString &id) { return QRegularExpression("^[A-Za-z][A-Za-z0-9_.-]{0,63}$").match(id).hasMatch() &&
    !QRegularExpression("^tx[0-9]+$").match(id).hasMatch(); }
QJsonObject HostPreset::json() const { return {{"id",id},{"name",name},{"kind",kind},{"host",host},{"user",user},{"directory",directory},{"sshPort",sshPort}}; }
QJsonObject Node::json() const { return {{"id",id},{"name",name},{"preset",presetId},{"x",position.x()},{"y",position.y()},
    {"listeningHz",listeningHz},{"bandwidthHz",bandwidthHz},{"mode",mode},{"txLevel",txLevel}}; }
QJsonObject Link::json() const { return {{"tx",tx},{"rx",rx},{"gainDb",gainDb},{"delayMs",delayMs},{"enabled",enabled}}; }
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
        pairs.insert(key);
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
        p.nodes.push_back({n.value("id").toString(),n.value("name").toString(),n.value("preset").toString(),{n.value("x").toDouble(),n.value("y").toDouble()},n.value("listeningHz").toDouble(),n.value("bandwidthHz").toDouble(),n.value("txLevel").toDouble(),n.value("mode").toString()});
    }
    for(const auto &v:o.value("links").toArray()) {
        if(!v.isObject())return fail(error,"Invalid link");const auto l=v.toObject();
        if(!number(l,"gainDb",-120,60)||!number(l,"delayMs",0,500)||std::floor(l.value("delayMs").toDouble())!=l.value("delayMs").toDouble()||!l.value("enabled").isBool())return fail(error,"Invalid link parameters");
        p.links.push_back({l.value("tx").toString(),l.value("rx").toString(),l.value("gainDb").toDouble(),l.value("delayMs").toInt(),l.value("enabled").toBool()});
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
