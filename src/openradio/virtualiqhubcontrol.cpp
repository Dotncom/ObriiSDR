#include "virtualiqhubcontrol.h"
#include "virtualsignaltransport.h"
#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <limits>
#include <memory>

namespace {
QJsonObject failure(const char *code,const char *message) {
    return {{"ok",false},{"errorCode",QLatin1String(code)},{"error",QLatin1String(message)}};
}
bool sourceId(const QString &id) {
    return virtualSignalValidNodeId(id)||QRegularExpression("^tx([1-9]|1[0-6])$").match(id).hasMatch();
}
}
VirtualIqHubControl::VirtualIqHubControl(VirtualIqHub &h,VirtualIqMedium *m,
    const QJsonArray &initial,bool route,QObject *parent)
    :QObject(parent),hub(h),medium(m),server(this),instance(QUuid::createUuid().toString(QUuid::WithoutBraces)),defaultRoute(route) {
    for(const auto &v:initial) {
        const auto l=v.toObject();
        links.insert(l.value("tx").toString()+"/"+l.value("rx").toString(),{
            {"tx",l.value("tx")},{"rx",l.value("rx")},{"enabled",l.value("enabled").toBool(true)},
            {"gainDb",l.value("gainDb").toDouble(0)},{"delayMs",l.value("delayMs").toInt(0)}});
    }
    rateWindow.start();
    connect(&server,&QTcpServer::newConnection,this,[this]{
        while(auto socket=server.nextPendingConnection()) {
            if(clients.size()>=8){socket->abort();socket->deleteLater();continue;}
            clients.insert(socket);socket->setReadBufferSize(65537);
            auto deadline=new QTimer(socket);deadline->setSingleShot(true);deadline->start(2000);
            connect(deadline,&QTimer::timeout,socket,[socket]{socket->abort();});
            connect(socket,&QTcpSocket::disconnected,this,[this,socket]{clients.remove(socket);socket->deleteLater();});
            auto handled=std::make_shared<bool>(false);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket,handled]{
                if(*handled)return;
                if(socket->bytesAvailable()>65536){*handled=true;socket->abort();return;}
                if(!socket->canReadLine())return;
                *handled=true;
                QJsonParseError parse;const auto doc=QJsonDocument::fromJson(socket->readLine(),&parse);
                auto reply=parse.error==QJsonParseError::NoError&&doc.isObject()?execute(doc.object()):failure("invalid_json","Expected a JSON object");
                const auto wire=QJsonDocument(reply).toJson(QJsonDocument::Compact)+'\n';
                if(wire.size()<=256*1024)socket->write(wire);
                socket->disconnectFromHost();
            });
        }
    });
}
bool VirtualIqHubControl::listen(quint16 p,const QString &secret,QString *error) {
    if(!QRegularExpression("^[a-fA-F0-9]{64}$").match(secret).hasMatch()||links.size()>1024) {
        if(error)*error="Control needs a 64-hex session key and at most 1024 links";return false;
    }
    key=secret;
    if(server.listen(QHostAddress::LocalHost,p))return true;
    if(error)*error=server.errorString();return false;
}
VirtualIqHubControl::~VirtualIqHubControl() {
    server.disconnect(this);server.close();
    for(auto socket:clients){socket->disconnect(this);socket->abort();delete socket;}
    clients.clear();
}
QJsonObject VirtualIqHubControl::status() const {
    QJsonArray applied,receivers;
    for(const auto &l:links)applied.append(l);
    for(const auto &id:hub.subscriberIds()) {
        QJsonObject row{{"id",id}};
        if(medium) {
            const auto c=medium->counters(id);
            row.insert("emittedFrames",QString::number(c.emittedFrames));
            row.insert("bufferedBlocks",c.bufferedBlocks);row.insert("bufferDrops",QString::number(c.bufferDrops));
            row.insert("filteredFrames",QString::number(c.filteredFrames));
        } else {
            const auto c=hub.counters(id);
            row.insert("deliveredFrames",QString::number(c.deliveredFrames));row.insert("queuedFrames",double(c.queuedFrames));
            row.insert("droppedFrames",QString::number(c.droppedFrames));row.insert("filteredFrames",QString::number(c.filteredFrames));
        }
        receivers.append(row);
    }
    return {{"ok",true},{"links",applied},{"receivers",receivers},{"defaultRoute",defaultRoute},
        {"mode",medium?"sample-mix":"packet-fanout"},{"appliesTo","future-published-frames"}};
}
QJsonObject VirtualIqHubControl::execute(const QJsonObject &request) {
    if(rateWindow.elapsed()>=1000){rateWindow.restart();requestsThisWindow=0;}
    if(++requestsThisWindow>64)return failure("rate_limited","Control request rate exceeded");
    const auto supplied=request.value("token").toString();
    unsigned difference=unsigned(supplied.size()^key.size());
    if(supplied.size()==64)for(int i=0;i<64;++i)difference|=supplied[i].unicode()^key[i].unicode();
    if(difference)return failure("unauthorized","Invalid session key");
    auto finish=[&](QJsonObject result){
        result.insert("version",1);result.insert("instanceId",instance);result.insert("revision",QString::number(revision));
        result.insert("id",request.value("id").toString().left(64));return result;
    };
    if(request.value("version")!=1)return finish(failure("version","Unsupported control version"));
    const QString target=request.value("targetInstanceId").toString();
    if(!target.isEmpty()&&target!=instance)return finish(failure("wrong_instance","Hub instance changed"));
    const auto command=request.value("command").toString();
    if(command=="status")return finish(status());
    if(command!="setLink")return finish(failure("unknown_command","Unknown hub command"));
    if(target!=instance)return finish(failure("wrong_instance","setLink requires the current hub instance"));
    if(request.value("expectedRevision").toString()!=QString::number(revision))
        return finish(failure("revision_conflict","Refresh hub state before applying this change"));
    const auto args=request.value("args").toObject();
    const auto tx=args.value("tx").toString(),rx=args.value("rx").toString();
    const auto gain=args.value("gainDb"),delay=args.value("delayMs"),enabled=args.value("enabled");
    const double d=delay.toDouble(-1),g=gain.toDouble();
    if(args.size()!=5||!sourceId(tx)||!hub.subscriberIds().contains(rx)||tx==rx||!enabled.isBool()||
       !delay.isDouble()||!std::isfinite(d)||std::floor(d)!=d||d<0||d>60000||
       !gain.isDouble()||!std::isfinite(g)||g< -120||g>60)
        return finish(failure("invalid_link","Invalid link parameters"));
    const auto pair=tx+"/"+rx;
    if(!links.contains(pair)&&links.size()>=1024)return finish(failure("link_limit","At most 1024 links are supported"));
    if(revision==std::numeric_limits<quint64>::max())return finish(failure("revision_limit","Restart the hub to reset revisions"));
    const VirtualIqLinkSettings value{enabled.toBool(),int(d),float(g)};
    // Validate both paths before mutating either; never copy buffered IQ for control.
    if(!hub.canSetLink(tx,rx,value)||(medium&&!medium->canSetLink(tx,rx,value)))
        return finish(failure("link_capacity","Link exceeds the active medium's delay/buffer limits"));
    hub.setLink(tx,rx,value);if(medium)medium->setLink(tx,rx,value);
    links.insert(pair,args);++revision;
    return finish({{"ok",true},{"link",args},{"appliesTo","future-published-frames"}});
}
