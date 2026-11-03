#include "meshnode.h"
#include "meshpacketengine.h"
#include "meshproject.h"
#include "meshdiscovery.h"
#include "meshiqmodem.h"
#include "meshports.h"
#include "meshlinksender.h"
#include "meshwiremodem.h"
#include "meshprotocoladapter.h"
#include "meshradioingress.h"
#include "meshradioegress.h"
#include "../lorawan/lorawanlora.h"
#include "../lorawan/regional.h"
#include "../openradio/virtualsignalframe.h"
#include "../openradio/virtualsignaltransport.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <QHostInfo>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <memory>
#include <cstring>
#include <cmath>

namespace Mesh {
namespace {
QJsonObject failure(const QString &error){return {{"ok",false},{"error",error}};}
QByteArray line(const QJsonObject &o){return QJsonDocument(o).toJson(QJsonDocument::Compact)+'\n';}
QString key(){return QUuid::createUuid().toString(QUuid::WithoutBraces)+QUuid::createUuid().toString(QUuid::WithoutBraces);}
bool integer(const QJsonValue &v,int low,int high){return v.isDouble()&&v.toDouble()>=low&&v.toDouble()<=high&&std::floor(v.toDouble())==v.toDouble();}
bool hostname(const QString &s){return !s.isEmpty()&&s.size()<=253&&!s.contains(QRegularExpression("[^A-Za-z0-9_.:-]"));}
bool containsPrivateConfig(const QJsonValue &value,int depth=0){
    if(depth>16)return true;
    if(value.isArray()){for(const auto &v:value.toArray())if(containsPrivateConfig(v,depth+1))return true;}
    if(value.isObject()){const auto object=value.toObject();for(auto i=object.begin();i!=object.end();++i){
        const auto k=i.key().toLower();if(k.contains("psk")||k.contains("key")||k.contains("password")||k.contains("secret")||containsPrivateConfig(i.value(),depth+1))return true;
    }}
    return false;
}
QJsonArray strings(const QStringList &list){QJsonArray a;for(const auto &s:list)a.append(s);return a;}
bool prepareStateDirectory(const QString &path,QString *error){
    QString cursor=QDir::cleanPath(path);QStringList missing;
    if(!QDir::isAbsolutePath(cursor)){if(error)*error="Protocol state root must be absolute";return false;}
    for(int depth=0;depth<128;++depth){
        const QFileInfo info(cursor);
        if(info.isSymLink()||(info.exists()&&!info.isDir())){if(error)*error="Protocol state path must not traverse a link or regular file";return false;}
        if(!info.exists())missing.prepend(cursor);
        const auto parent=info.absolutePath();if(parent==cursor)break;cursor=parent;
        if(depth==127){if(error)*error="Protocol state path is too deep";return false;}
    }
    for(const auto &directory:missing){
        if(!QDir().mkdir(directory)||!QFile::setPermissions(directory,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner)){if(error)*error="Cannot create private protocol state directory";return false;}
    }
    return true;
}
bool radioFrame(const QJsonObject &input,bool uplink,QJsonObject *canonical){
    const auto hz=input.value("frequencyHz"),rate=input.value("dataRate"),duration=input.value("airtimeUs");bool okay=false;
    const auto us=duration.toString().toULongLong(&okay);
    if(!integer(hz,863000000,870000000)||!integer(rate,0,6)||!obrii::lorawan::Eu868::validFrequency(quint32(hz.toDouble()))||!okay||us<1||us>120000000||QString::number(us)!=duration.toString()||!input.value("modem").isObject())return false;
    const auto expected=obrii::lorawan::phyConfig(unsigned(rate.toInt()),uplink?obrii::lorawan::Direction::Uplink:obrii::lorawan::Direction::Downlink,1);
    QJsonObject modem;QString error;if(!WireModem::normalize(input.value("modem").toObject(),&modem,&error)||modem.value("type")!="lora")return false;
    if(modem.value("sf")!=expected.spreadingFactor||modem.value("bw")!=expected.bandwidthHz||modem.value("cr")!=expected.codingRate||modem.value("os")!=1||modem.value("preamble")!=expected.preambleSymbols||modem.value("syncWord")!=int(expected.syncWord)||modem.value("invertIq")!=expected.invertIq||modem.value("payloadCrc")!=expected.payloadCrc||modem.value("ldro")!="auto")return false;
    if(canonical)*canonical={{"frequencyHz",hz},{"dataRate",rate},{"airtimeUs",duration},{"modem",modem},{"timingSource","simulated-transport"}};return true;
}
struct Packet {PacketEngine::Datagram data;QStringList path;};
QJsonObject packetJson(const Packet &p){return {{"id",p.data.id},{"source",p.data.source},{"destination",p.data.destination},{"ttl",p.data.ttl},{"payloadBase64",QString::fromLatin1(p.data.payload.toBase64())},{"path",strings(p.path)}};}
class NodeWorker : public QObject {
    Q_OBJECT
public:
    QTcpServer server;
    QTimer heartbeat,pump,radioPump,rfWatchdog;
    QString admin,instance=QUuid::createUuid().toString(QUuid::WithoutBraces),session,sessionKey,nodeId,transport,ownerToken;
    QString pairingSecret=QString::number(QRandomGenerator::system()->bounded(1000000)).rightJustified(6,'0');
    struct Registration {QString token,name;};
    QMap<QString,Registration> registered;
    DiscoveryResponder *discovery=nullptr;
    QElapsedTimer pairWindow;
    int pairFailures=0;
    PacketEngine graph;
    PortPlan plan;
    std::unique_ptr<ProtocolAdapter> native;
    QString nativeError,localStateRoot;
    QJsonObject localReticulum;
    QJsonObject localProfiles;
    QJsonObject localTxProfiles;
    qint64 rfArmedUntil=0;
    QMap<QString,RadioEgress *> radioTx;
    QSet<quint64> radioTickets;
    QString approvedStateRoot;
    bool allowRadioRx=false;
    QSet<QString> radioSources;
    RadioIngress radio;
    QMap<QString,std::shared_ptr<WireModem>> radioModems;
    QMap<QString,LinkSender *> senders;
    QMap<quint64,Packet> labFlights;
    struct NativeFlight {quint64 ticket=0;int remaining=0;bool sent=false,reported=false;QJsonObject radioFrame;};
    QMap<quint64,std::shared_ptr<NativeFlight>> nativeFlights;
    QMap<quint64,QJsonObject> txPlans;
    QJsonObject peers,topology;
    QStringList ids;
    QElapsedTimer lease,clock;
    struct Connection {QByteArray buffer;QString from;bool peer=false,frameScheduled=false;int sequence=0;LinkBinding link;QJsonObject radioFrame;qint64 packetBeginMs=-1;std::unique_ptr<WireModem> modem;};
    QHash<QTcpSocket *,std::shared_ptr<Connection>> connections;
    QSet<QString> seen;QQueue<QString> seenOrder;
    QJsonArray outcomes;
    quint64 flight=0,tx=0,rx=0,delivered=0,forwarded=0,dropped=0,iqTx=0,iqRx=0,evicted=0,resultSerial=0;
    explicit NodeWorker(const QString &token):admin(token){server.setParent(this);heartbeat.setParent(this);pump.setParent(this);radioPump.setParent(this);rfWatchdog.setParent(this);clock.start();}
    ~NodeWorker() override {stopSession();for(auto s:connections.keys()){s->disconnect();s->abort();delete s;}connections.clear();server.close();}
    QJsonObject status() const {
        QJsonArray ports,links;const auto ingress=radio.status();
        for(const auto &p:plan.ports(nodeId)){auto value=p.json();bool ready=p.enabled&&!session.isEmpty();QString state=ready?"ready":"disabled";
            if(p.backend=="radio"&&p.direction=="rx"){ready=false;state="waiting";for(const auto &v:ingress)if(v.toObject().value("port")==p.id){state=v.toObject().value("state").toString();ready=state=="receiving";}}
            if(p.backend=="radio"&&p.direction=="tx"){ready=radioTx.contains(p.id)&&rfArmedUntil>clock.elapsed();state=ready?"armed":"disarmed";}
            value["ready"]=ready;value["state"]=state;ports.append(value);
        }for(auto s:senders)links.append(s->status());QJsonArray egress;for(auto tx:radioTx)egress.append(tx->status());
        auto protocol=native?native->status():QJsonObject{{"type","lab"},{"ready",!session.isEmpty()}};if(!nativeError.isEmpty())protocol["error"]=nativeError;
        if(plan.protocol(nodeId).value("type")=="lorawan")protocol["timingSource"]="simulated-transport";
        int pending=0;for(auto sender:senders)pending+=sender->pending();
        return {{"ok",true},{"protocol","obrii-mesh-node"},{"version",1},{"instanceId",instance},{"listening",server.isListening()},
            {"port",server.serverPort()},{"bind",server.serverAddress().toString()},{"active",!session.isEmpty()},{"session",session},{"nodeId",nodeId},
            {"transport",transport},{"ports",ports},{"links",links},{"nativeProtocol",protocol},{"radioIngress",ingress},{"radioEgress",egress},{"radioRxAllowed",allowRadioRx},{"radioTxAllowed",!localTxProfiles.isEmpty()&&rfArmedUntil>clock.elapsed()},{"rfArmedMs",qMax<qint64>(0,rfArmedUntil-clock.elapsed())},{"portBindingsVersion",1},{"perLinkBearers",true},{"availableModems",QJsonArray{"afsk1200","lora"}},{"nativeProtocols",QJsonArray{"lab","meshtastic","reticulum","lorawan"}},
            {"discovery",discovery&&discovery->listening()},{"registrations",registered.size()},{"rfEnabled",!radioTx.isEmpty()&&rfArmedUntil>clock.elapsed()},{"availableBearers",QJsonArray{"data","iq"}},{"queue",pending},{"tx",QString::number(tx)},{"rx",QString::number(rx)},
            {"forwarded",QString::number(forwarded)},{"delivered",QString::number(delivered)},{"dropped",QString::number(dropped)},
            {"iqTxSamples",QString::number(iqTx)},{"iqRxSamples",QString::number(iqRx)},{"lastResultSerial",QString::number(resultSerial)},{"resultsEvicted",QString::number(evicted)},{"results",outcomes}};
    }
    void publish(){emit changed(status());}
    QJsonObject start(const QString &bind,quint16 port) {
        if(!server.listen(QHostAddress(bind),port))return failure(server.errorString());
        localStateRoot=QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("ObriiSDR/mesh-protocols/port-"+QString::number(server.serverPort()));
        if(!approvedStateRoot.isEmpty())localStateRoot=approvedStateRoot;
        pairWindow.start();discovery=new DiscoveryResponder([this]{return QJsonObject{{"instanceId",instance},{"port",server.serverPort()},
            {"name",QCoreApplication::applicationName()+" @ "+QHostInfo::localHostName()},{"bind",server.serverAddress().toString()},{"active",!session.isEmpty()}};},this);
        discovery->start();
        heartbeat.setInterval(1000);connect(&heartbeat,&QTimer::timeout,this,[this]{if(!session.isEmpty()&&lease.elapsed()>15000){stopSession();publish();}});
        pump.setInterval(20);pump.setTimerType(Qt::PreciseTimer);connect(&pump,&QTimer::timeout,this,[this]{if(native)native->step(clock.elapsed());});
        rfWatchdog.setSingleShot(true);connect(&rfWatchdog,&QTimer::timeout,this,[this]{disarmTx();publish();});
        radioPump.setInterval(5);radioPump.setTimerType(Qt::PreciseTimer);connect(&radioPump,&QTimer::timeout,this,[this]{radio.poll([this](const QString &port,const float *iq,int count,bool gap){
            auto decoder=radioModems.value(port);if(!decoder)return;iqRx+=quint64(count);
            for(const auto &wire:decoder->receive(iq,count,gap)){
                ++rx;if(native)native->receive(port,wire,clock.elapsed());
                else appendResult({{"protocol","lab"},{"node",nodeId},{"status","radio_frame"},{"payloadBase64",QString::fromLatin1(wire.toBase64())},{"localMs",double(clock.elapsed())}});
            }
        });});
        connect(&server,&QTcpServer::newConnection,this,[this]{while(auto s=server.nextPendingConnection())accept(s);});
        publish();return status();
    }
    void accept(QTcpSocket *s) {
        if(connections.size()>=32){s->abort();s->deleteLater();return;}
        auto c=std::make_shared<Connection>();connections.insert(s,c);s->setReadBufferSize(128*1024);
        s->setSocketOption(QAbstractSocket::LowDelayOption,1);
        auto deadline=new QTimer(s);deadline->setSingleShot(true);deadline->start(5000);
        connect(deadline,&QTimer::timeout,s,[s]{s->abort();});
        connect(s,&QTcpSocket::disconnected,this,[this,s]{connections.remove(s);s->deleteLater();});
        connect(s,&QTcpSocket::readyRead,this,[this,s,c,deadline]{
            c->buffer+=s->readAll();if(c->buffer.size()>256*1024){s->abort();return;}
            int budget=16;
            while(c->buffer.contains('\n')&&budget-->0){
                const int end=c->buffer.indexOf('\n');const auto bytes=c->buffer.left(end);c->buffer.remove(0,end+1);
                if(bytes.size()>128*1024){s->abort();return;}
                QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);
                if(error.error!=QJsonParseError::NoError||!doc.isObject()){s->abort();return;}
                deadline->start(5000);const auto request=doc.object();QJsonObject reply;
                if(request.value("op")=="peer"&&!c->peer){
                    const auto from=request.value("from").toString();
                    if(session.isEmpty()||request.value("session")!=session||request.value("key")!=sessionKey||request.value("instanceId")!=instance||request.value("next")!=nodeId||!peers.contains(from)||from==nodeId){s->abort();return;}
                    const auto link=plan.link(from,nodeId);
                    if(!link||!link->enabled||(request.contains("bearer")&&request.value("bearer")!=link->bearer)||(request.contains("txPort")&&request.value("txPort")!=link->txPort)||(request.contains("rxPort")&&request.value("rxPort")!=link->rxPort)){s->abort();return;}
                    c->link=*link;
                    const bool lorawan=plan.protocol(nodeId).value("type")=="lorawan";
                    if(lorawan){
                        const bool uplink=native&&native->status().value("role")=="network";
                        if(!request.value("radioFrame").isObject()||!radioFrame(request.value("radioFrame").toObject(),uplink,&c->radioFrame)){s->abort();return;}
                        c->link.modem=c->radioFrame.value("modem").toObject();deadline->start(int(c->radioFrame.value("airtimeUs").toString().toULongLong()/1000)+5000);
                    }else if(request.contains("modem")){QJsonObject offered;QString error;if(!request.value("modem").isObject()||!WireModem::normalize(request.value("modem").toObject(),&offered,&error)||offered!=link->modem){s->abort();return;}}
                    if(link->bearer=="iq"){try{c->modem=std::make_unique<WireModem>(c->link.modem);}catch(...){s->abort();return;}}
                    c->peer=true;c->from=from;reply={{"ok",true},{"ready",true}};
                } else if(c->peer){
                    if(session.isEmpty()){s->abort();return;}
                    reply=receive(request,*c);
                    if(!c->radioFrame.isEmpty())deadline->start(int(c->radioFrame.value("airtimeUs").toString().toULongLong()/1000)+5000);
                } else {
                    if(request.value("op")=="pair"){
                        reply=pair(request);reply["requestId"]=request.value("requestId");s->write(line(reply));s->disconnectFromHost();break;
                    }
                    bool authenticated=request.value("token")==admin;
                    for(const auto &registration:registered)authenticated|=request.value("token")==registration.token;
                    if(!authenticated){s->abort();return;}
                    if(request.value("op")!="status"&&request.value("instanceId")!=instance){reply=failure("Instance changed; reconnect before control");}
                    else reply=control(request);
                    reply["requestId"]=request.value("requestId");
                }
                if(!reply.isEmpty()){
                    if(s->bytesToWrite()>128*1024){s->abort();return;}
                    s->write(line(reply));
                    if(!c->peer){s->disconnectFromHost();break;}
                }
            }
            if(c->buffer.size()>128*1024&&!c->buffer.contains('\n')){s->abort();return;}
            if(c->buffer.contains('\n'))QTimer::singleShot(0,s,[s]{emit s->readyRead();});
        });
    }
    void stopSession() {
        pump.stop();heartbeat.stop();radioPump.stop();radio.clear();radioModems.clear();
        disarmTx();
        for(auto s:senders){s->disconnect(this);s->cancel();delete s;}senders.clear();
        for(const auto &p:labFlights)result(p,"session_stopped");labFlights.clear();nativeFlights.clear();txPlans.clear();
        if(native){native->stop();native.reset();}nativeError.clear();
        for(auto s:connections.keys())if(connections.value(s)->peer)s->abort();
        session.clear();sessionKey.clear();ownerToken.clear();nodeId.clear();peers={};topology={};ids.clear();seen.clear();seenOrder.clear();
    }
    QJsonObject pair(const QJsonObject &r) {
        if(pairWindow.elapsed()>60000){pairWindow.restart();pairFailures=0;}
        const auto client=r.value("clientId").toString(),name=r.value("clientName").toString();
        if(pairFailures>=5)return failure("Pairing rate limit; wait one minute");
        if(r.value("instanceId")!=instance||QUuid(client).isNull()||name.trimmed().isEmpty()||name.size()>128||r.value("code")!=pairingSecret){++pairFailures;return failure("Pairing code or instance identity is invalid");}
        if(registered.size()>=16&&!registered.contains(client))return failure("Registration limit reached");
        if(registered.contains(client)&&registered[client].token==ownerToken&&!session.isEmpty())return failure("Disconnect the registered client's active session before pairing again");
        registered[client]={key(),name};auto reply=QJsonObject{{"ok",true},{"protocol","obrii-mesh-node"},{"version",1},{"instanceId",instance},{"token",registered[client].token},{"registrationId",client}};
        publish();return reply;
    }
    QJsonObject revoke(const QString &client) {
        if(!registered.contains(client))return failure("Unknown registration");
        if(registered[client].token==ownerToken)stopSession();registered.remove(client);publish();return status();
    }
    QJsonObject registrations() const {
        QJsonArray clients;for(auto i=registered.cbegin();i!=registered.cend();++i)clients.append(QJsonObject{{"clientId",i.key()},{"name",i->name},{"ownsSession",!session.isEmpty()&&i->token==ownerToken}});
        return {{"ok",true},{"clients",clients}};
    }
    void disarmTx(){
        rfArmedUntil=0;rfWatchdog.stop();
        for(auto tx:radioTx){tx->disconnect(this);tx->requestStop();}
        for(auto tx:radioTx)delete tx;radioTx.clear();
        const auto tickets=radioTickets;radioTickets.clear();for(const auto ticket:tickets)completed(ticket,false,"RF locally disarmed",0);
    }
    QJsonObject armTx(int seconds){
        if(!session.isEmpty())return failure("Release the mesh session before changing local RF authorization");
        if(localTxProfiles.isEmpty())return failure("No locally approved RF TX profiles are installed");
        if(seconds<1||seconds>3600)return failure("RF arm lifetime must be 1..3600 seconds");
        rfArmedUntil=clock.elapsed()+qint64(seconds)*1000;rfWatchdog.start(seconds*1000);publish();return status();
    }
    QJsonObject configure(const QJsonObject &r) {
        if(!session.isEmpty())return failure("Node already belongs to a session; release it first");
        const auto proposed=r.value("session").toString(),secret=r.value("key").toString(),id=r.value("nodeId").toString();
        const auto mode=r.value("transport").toString();const auto ts=r.value("topology").toObject(),ps=r.value("peers").toObject();
        PacketEngine candidate;PortPlan proposedPlan;QString error;
        if(QUuid(proposed).isNull()||secret.size()<32||secret.size()>128||!validId(id)||(mode!="data"&&mode!="iq"))return failure("Invalid session, bearer or topology");
        if(!proposedPlan.configure(ts,mode,&error)||!candidate.configure(proposedPlan.routingTopology(),&error))return failure(error);
        QStringList names;for(const auto &v:ts.value("nodes").toArray())names<<v.toObject().value("id").toString();
        if(names.size()>8||!names.contains(id)||ps.size()!=names.size())return failure("Distributed mode requires 1..8 configured endpoints");
        for(const auto &name:names){const auto p=ps.value(name).toObject();if(!hostname(p.value("host").toString())||!integer(p.value("port"),1,65535)||QUuid(p.value("instanceId").toString()).isNull())return failure("Invalid peer endpoint/identity");}
        if(ps.value(id).toObject().value("instanceId")!=instance)return failure("Local peer identity mismatch");
        for(const auto &v:ts.value("links").toArray()){const auto l=v.toObject();const double gain=l.value("gainDb").toDouble(-6);if(!std::isfinite(gain)||gain< -120||gain>60||l.value("delayMs").toInt()>500)return failure("Link gain/delay out of range");}
        auto config=proposedPlan.protocol(id);const auto type=config.value("type").toString();std::unique_ptr<ProtocolAdapter> adapter;
        // Qt 5.15.2 take() can null a value in a shared QJsonObject before detaching.
        config.remove("type");const auto profile=config.value("profileId").toString();config.remove("profileId");
        if(containsPrivateConfig(config))return failure("Secret protocol material must come from a locally approved profile, not the exported topology");
        if(!profile.isEmpty()){
            const auto values=localProfiles.value(profile);if(!values.isObject())return failure("Requested local protocol profile is not installed");
            const auto object=values.toObject();for(auto i=object.begin();i!=object.end();++i)config[i.key()]=i.value();
        }
        for(const auto &port:proposedPlan.ports(id))if(port.enabled&&port.backend=="radio"){
            if(type=="lorawan")return failure("Physical LoRaWAN requires qualified timestamped TX/RX; only modeled network transports are enabled");
            if(port.direction=="tx"){
                if(rfArmedUntil<=clock.elapsed()||!localTxProfiles.value(port.id).isObject())return failure("Physical TX requires a locally armed, bounded RF profile");
                if(!RadioEgress::validate(port,localTxProfiles.value(port.id).toObject(),&error))return failure(error);
                if(type=="lab")return failure("Physical TX requires a native protocol adapter");
            }else if(!allowRadioRx||!radioSources.contains(port.receiverId+"/"+port.channelId))return failure("Physical RX source is not locally authorized for mesh capture");
        }
        if(type=="meshtastic")adapter=makeMeshtasticAdapter();else if(type=="reticulum")adapter=makeReticulumAdapter();else if(type=="lorawan"){
            if(proposedPlan.outgoing(id).size()!=1)return failure("LoRaWAN lab currently requires one outgoing peer per native interface");
            if(!config.contains("clockToleranceMs"))config["clockToleranceMs"]=250;
            adapter=makeLoRaWanAdapter();
        }else if(type!="lab")return failure("Protocol adapter is not installed yet");
        if(adapter){
            QStringList interfaces;for(const auto &port:proposedPlan.ports(id))if(port.enabled&&!interfaces.contains(port.id))interfaces.append(port.id);
            if(type=="reticulum"){
                config.remove("python");config.remove("moduleDirectory");config.remove("stateDirectory");
                for(auto i=localReticulum.begin();i!=localReticulum.end();++i)config[i.key()]=i.value();
            }
            const auto stateDirectory=QDir(localStateRoot).filePath(id);
            if(!prepareStateDirectory(stateDirectory,&error)||!adapter->start(config,interfaces,stateDirectory,&error))return failure(error);
        }
        if(!radio.configure(proposedPlan.ports(id),&error))return failure(error);
        for(const auto &port:proposedPlan.ports(id))if(port.enabled&&port.backend=="radio"&&port.direction=="rx")radioModems[port.id]=std::make_shared<WireModem>(port.modem);
        graph=std::move(candidate);plan=std::move(proposedPlan);topology=plan.topology();peers=ps;ids=names;session=proposed;sessionKey=secret;nodeId=id;transport=mode;ownerToken=r.value("token").toString();
        native=std::move(adapter);
        for(const auto &port:plan.ports(id))if(port.enabled&&port.backend=="radio"&&port.direction=="tx"){
            RadioEgress *tx=nullptr;
            try{tx=new RadioEgress(port,localTxProfiles.value(port.id).toObject(),this,{},rfArmedUntil-clock.elapsed());}
            catch(const std::exception &){stopSession();return failure("Cannot create the optional RF TX workers");}
            radioTx[port.id]=tx;
            connect(tx,&RadioEgress::completed,this,[this](quint64 ticket,bool okay,const QString &reason,quint64 count){radioTickets.remove(ticket);completed(ticket,okay,reason,count);});
        }
        if(native){
            connect(native.get(),&ProtocolAdapter::transmit,this,[this](quint64 ticket,const QString &port,const QByteArray &wire){nativeTransmit(ticket,port,wire);});
            connect(native.get(),&ProtocolAdapter::event,this,[this](QJsonObject event){
                if(plan.protocol(nodeId).value("type")=="lorawan"&&event.value("event")=="tx_plan")txPlans[event.value("ticket").toString().toULongLong()]=event;
                if(plan.protocol(nodeId).value("type")=="lorawan")event["timingSource"]="simulated-transport";
                event["protocol"]=plan.protocol(nodeId).value("type");event["node"]=nodeId;event["localMs"]=double(clock.elapsed());
                if(!event.contains("status"))event["status"]=event.value("kind").toString(event.value("event").toString("observed"));
                if(event.value("status")=="message")++delivered;
                appendResult(event);
            });
            connect(native.get(),&ProtocolAdapter::failed,this,[this](const QString &s){nativeError=s.left(300);pump.stop();publish();});
            pump.start();
        }
        if(radio.active())radioPump.start();
        outcomes={};tx=rx=delivered=forwarded=dropped=iqTx=iqRx=evicted=resultSerial=0;lease.start();heartbeat.start();publish();return status();
    }
    QJsonObject control(const QJsonObject &r) {
        const auto op=r.value("op").toString();
        if(op=="configure")return configure(r);
        if(op=="status") {
            if(!session.isEmpty()&&r.value("token")!=ownerToken){auto reply=status();reply.remove("results");reply.remove("session");return reply;}
            bool valid=false;quint64 after=0;
            if(r.contains("after")){after=r.value("after").toString().toULongLong(&valid);if(!valid)return failure("Invalid result cursor");}
            if(!session.isEmpty()&&r.value("session")==session)lease.restart();
            auto reply=status();
            if(r.contains("after")){QJsonArray delta;for(const auto &v:outcomes)if(v.toObject().value("serial").toString().toULongLong()>after)delta.append(v);reply["results"]=delta;}
            return reply;
        }
        if(session.isEmpty()||r.value("session")!=session||r.value("token")!=ownerToken)return failure("Session ownership mismatch");
        lease.restart();
        if(op=="release"){stopSession();publish();return status();}
        if(op=="protocol"){
            if(!native)return failure("This node uses the laboratory router, not a native protocol adapter");
            if(QStringList{"tx_started","rx_context"}.contains(r.value("args").toObject().value("op").toString()))return failure("Radio timing callbacks are host-only, not remote application commands");
            auto reply=native->command(r.value("args").toObject(),clock.elapsed());auto response=status();response["ok"]=reply.value("ok").toBool();response["protocolReply"]=reply;if(reply.contains("error"))response["error"]=reply.value("error");return response;
        }
        if(op=="send"){
            if(native){
                const auto type=plan.protocol(nodeId).value("type").toString();
                auto destination=r.value("nativeDestination");
                if(destination.isUndefined()&&type=="meshtastic")destination=plan.protocol(r.value("destination").toString()).value("nodeNumber");
                if(destination.isUndefined())destination=r.value("destination");
                QJsonObject args{{"op","send"},{"payloadBase64",r.value("payloadBase64")}};
                if(type!="lorawan")args["destination"]=destination;
                if(type=="meshtastic")args["reliable"]=r.value("reliable").toBool(true);
                auto reply=native->command(args,clock.elapsed());auto response=status();response["ok"]=reply.value("ok").toBool();response["protocolReply"]=reply;if(reply.contains("error"))response["error"]=reply.value("error");return response;
            }
            const auto payload=QByteArray::fromBase64(r.value("payloadBase64").toString().toLatin1());const auto dest=r.value("destination").toString(),id=r.value("id").toString();
            const int maximum=4096;
            if(!ids.contains(dest)||!validId(id)||!integer(r.value("ttl"),0,64)||payload.isEmpty()||payload.size()>maximum||QString::fromLatin1(payload.toBase64())!=r.value("payloadBase64").toString())return failure("Invalid destination, packet ID, TTL or payload (data <=4096 B; IQ <=240 B)");
            int pending=0;for(auto sender:senders)pending+=sender->pending();if(pending>=64)return failure("Node TX queue is full");
            Packet p{{id,nodeId,dest,payload,r.value("ttl").toInt()}, {}};route(std::move(p),{});publish();return status();
        }
        return failure("Unknown mesh node operation");
    }
    void result(const Packet &p,const QString &reason) {
        QJsonObject event{{"id",p.data.id},{"source",p.data.source},{"destination",p.data.destination},{"node",nodeId},{"status",reason},{"path",strings(p.path)},
            {"payloadBase64",QString::fromLatin1(p.data.payload.toBase64())},{"localMs",double(clock.elapsed())}};
        if(reason=="delivered")++delivered;else if(reason!="forwarded")++dropped;appendResult(event);
    }
    void appendResult(QJsonObject event){
        if(outcomes.size()>=128){outcomes.removeFirst();++evicted;}event["serial"]=QString::number(++resultSerial);outcomes.append(event);
        while(QJsonDocument(outcomes).toJson(QJsonDocument::Compact).size()>48*1024){outcomes.removeFirst();++evicted;}publish();
    }
    LinkSender *sender(const LinkBinding &link){
        const auto id=link.tx+"/"+link.rx;auto value=senders.value(id);if(value)return value;
        value=new LinkSender(link,peers.value(link.rx).toObject(),session,sessionKey,this);senders[id]=value;
        value->setStartAuthorization([this](quint64 ticket){
            const auto group=nativeFlights.value(ticket);if(!native||!group||group->radioFrame.isEmpty())return failure("Missing native radio plan");
            const auto now=clock.elapsed();return native->command({{"op","tx_started"},{"ticket",QString::number(group->ticket)},{"startMs",double(now)}},now);
        });
        connect(value,&LinkSender::completed,this,[this](quint64 ticket,bool sent,const QString &reason,quint64 samples){completed(ticket,sent,reason,samples);});return value;
    }
    void completed(quint64 ticket,bool sent,const QString &reason,quint64 samples){
        iqTx+=samples;if(sent)++tx;
        auto group=nativeFlights.take(ticket);
        if(group){
            group->sent|=sent;--group->remaining;
            if(!sent){++dropped;appendResult({{"protocol",plan.protocol(nodeId).value("type")},{"node",nodeId},{"status","tx_failed"},{"reason",reason.left(256)},{"ticket",QString::number(ticket)}});}
            // One emitted broadcast is enough to finish local TX. Peer ACKs and
            // a stalled recipient must not hold the native radio interface busy.
            if(!group->reported&&(sent||group->remaining==0)&&native){group->reported=true;native->complete(group->ticket,group->sent,clock.elapsed());}
            publish();return;
        }
        if(labFlights.contains(ticket)){const auto p=labFlights.take(ticket);if(sent&&p.data.source!=nodeId)++forwarded;result(p,sent?"forwarded":reason);}
    }
    void nativeTransmit(quint64 ticket,const QString &port,const QByteArray &wire){
        const auto links=plan.outgoing(nodeId,port);auto rf=radioTx.value(port);auto group=std::make_shared<NativeFlight>();group->ticket=ticket;group->remaining=links.size()+(rf?1:0);
        if(plan.protocol(nodeId).value("type")=="lorawan"){
            const auto proposed=txPlans.take(ticket);if(!radioFrame(proposed,native->status().value("role")=="device",&group->radioFrame)){native->complete(ticket,false,clock.elapsed());return;}
        }
        if(group->remaining==0){native->complete(ticket,false,clock.elapsed());return;}
        if(rf){const auto serial=++flight;nativeFlights[serial]=group;radioTickets.insert(serial);QString error;if(!rf->submit(serial,wire,&error)){radioTickets.remove(serial);completed(serial,false,error,0);}}
        for(const auto &link:links){const auto serial=++flight;nativeFlights[serial]=group;QString error;
            if(!sender(link)->submit({serial,wire,{},true,group->radioFrame},&error))completed(serial,false,error,0);
        }
    }
    void receiveNative(const QString &port,const QByteArray &wire,const QJsonObject &frame={},qint64 beginMs=-1){
        if(!native)return;
        if(plan.protocol(nodeId).value("type")!="lorawan"){native->receive(port,wire,clock.elapsed());return;}
        const bool uplink=native->status().value("role")=="network";QJsonObject validated;
        if(!radioFrame(frame,uplink,&validated)||beginMs<0||wire.size()<12||wire.size()>255){++dropped;return;}
        const auto cfg=obrii::lorawan::phyConfig(unsigned(validated.value("dataRate").toInt()),uplink?obrii::lorawan::Direction::Uplink:obrii::lorawan::Direction::Downlink,1);
        const auto expected=quint64(std::ceil(obrii::lora::PacketCodec(cfg).timeOnAirSeconds(wire.size())*1000000));
        if(expected!=validated.value("airtimeUs").toString().toULongLong()){++dropped;return;}
        deliverModeledNative(port,wire,validated,beginMs,beginMs+qint64((expected+999)/1000),session);
    }
    void deliverModeledNative(const QString &port,const QByteArray &wire,const QJsonObject &frame,qint64 beginMs,qint64 endMs,const QString &expectedSession){
        if(session!=expectedSession||!native)return;const auto now=clock.elapsed();
        // Timer delivery is not a capture timestamp. Never present a future packet end to the MAC.
        if(now<endMs){
            QTimer::singleShot(int(endMs-now),Qt::PreciseTimer,this,[this,port,wire,frame,beginMs,endMs,expectedSession]{
                deliverModeledNative(port,wire,frame,beginMs,endMs,expectedSession);
            });return;
        }
        const auto context=native->command({{"op","rx_context"},{"frequencyHz",frame.value("frequencyHz")},{"dataRate",frame.value("dataRate")},{"packetStartMs",double(beginMs)},{"packetEndMs",double(endMs)}},now);
        if(!context.value("ok").toBool()){
            ++dropped;appendResult({{"protocol","lorawan"},{"node",nodeId},{"port",port},{"status","rx_context_rejected"},{"reason",context.value("error").toString().left(256)},{"beginMs",double(beginMs)},{"endMs",double(endMs)},{"nowMs",double(now)},{"timingSource","simulated-transport"}});return;
        }
        native->receive(port,wire,now);
    }
    bool parsePacket(const QJsonObject &o,Packet *p) {
        const auto path=o.value("path").toArray();p->data={o.value("id").toString(),o.value("source").toString(),o.value("destination").toString(),QByteArray::fromBase64(o.value("payloadBase64").toString().toLatin1()),o.value("ttl").toInt(-1)};
        if(!validId(p->data.id)||!ids.contains(p->data.source)||!ids.contains(p->data.destination)||!integer(o.value("ttl"),0,64)||p->data.payload.isEmpty()||p->data.payload.size()>4096||QString::fromLatin1(p->data.payload.toBase64())!=o.value("payloadBase64").toString()||path.isEmpty()||path.size()>8)return false;
        QSet<QString> visited;for(const auto &v:path){const auto id=v.toString();if(!ids.contains(id)||visited.contains(id))return false;visited.insert(id);p->path<<id;}
        return p->path.first()==p->data.source;
    }
    QByteArray compact(const Packet &p) const {
        QByteArray bytes("MP1",3);bytes.append(char(p.path.size()));for(const auto &s:p.path)bytes.append(char(ids.indexOf(s)));bytes+=p.data.payload;return bytes;
    }
    bool expand(const IqEnvelope &e,Packet *p) const {
        const auto bytes=e.packet.payload;if(bytes.size()<5||bytes.left(3)!="MP1")return false;
        const int count=quint8(bytes[3]);if(count<1||count>8||bytes.size()<=4+count)return false;
        *p={e.packet,{}};QSet<int> visited;
        for(int i=0;i<count;++i){const int n=quint8(bytes[4+i]);if(n>=ids.size()||visited.contains(n))return false;visited.insert(n);p->path<<ids[n];}
        p->data.payload=bytes.mid(4+count);return p->path.first()==p->data.source;
    }
    QJsonObject receive(const QJsonObject &r,Connection &c) {
        Packet p;
        if(c.link.bearer=="data"&&r.value("op")=="wire"&&native){
            const auto encoded=r.value("payloadBase64").toString();const auto bytes=QByteArray::fromBase64(encoded.toLatin1());
            if(bytes.isEmpty()||bytes.size()>4096||QString::fromLatin1(bytes.toBase64())!=encoded)return failure("Invalid native frame");
            if(!c.radioFrame.isEmpty()&&c.frameScheduled)return failure("A modeled radio burst carries only one native frame");
            c.frameScheduled=true;++rx;receiveNative(c.link.rxPort,bytes,c.radioFrame,clock.elapsed());return {{"ok",true},{"accepted",true}};
        }
        if(c.link.bearer=="data"&&r.value("op")=="packet"&&!native){
            if(!parsePacket(r.value("packet").toObject(),&p))return failure("Invalid packet");
        } else if(c.link.bearer=="iq"&&r.value("op")=="iq"&&c.modem){
            const auto bytes=QByteArray::fromBase64(r.value("samples").toString().toLatin1());
            if(!integer(r.value("sequence"),0,1000000)||r.value("sequence").toInt()!=c.sequence++||bytes.isEmpty()||bytes.size()>65536||bytes.size()%8||QString::fromLatin1(bytes.toBase64())!=r.value("samples").toString())return failure("Invalid IQ block");
            QVector<float> values(bytes.size()/4);for(int i=0;i<values.size();++i){quint32 v=qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()+i*4));std::memcpy(&values[i],&v,4);}
            iqRx+=quint64(values.size()/2);
            if(c.packetBeginMs<0)c.packetBeginMs=clock.elapsed();
            const auto frames=c.modem->receive(values.constData(),values.size()/2);
            for(const auto &body:frames){
                if(native){if(!c.radioFrame.isEmpty()&&c.frameScheduled)return failure("Duplicate native radio frame in one burst");c.frameScheduled=true;++rx;receiveNative(c.link.rxPort,body,c.radioFrame,c.packetBeginMs);continue;}
                IqEnvelope e;QString error;if(!IqModem::decode(body,&e,&error)||e.session!=QUuid(session)||e.transmitter!=c.from||e.next!=nodeId||!expand(e,&p))return failure("IQ envelope identity mismatch");
                route(std::move(p),c.from);
            }
            return frames.isEmpty()?QJsonObject():QJsonObject{{"ok",true},{"accepted",true}};
        } else return failure("Bearer/operation mismatch");
        if(p.path.last()!=c.from)return failure("Packet previous-hop mismatch");
        route(std::move(p),c.from);return {{"ok",true},{"accepted",true}};
    }
    void route(Packet p,const QString &from) {
        for(const auto &v:topology.value("nodes").toArray()){const auto n=v.toObject();if(n.value("id")==nodeId&&!n.value("enabled").toBool(true)){result(p,"node_down");return;}}
        if(!from.isEmpty()){
            bool linked=false;for(const auto &v:topology.value("links").toArray()){const auto l=v.toObject();if(l.value("tx")==from&&l.value("rx")==nodeId&&l.value("enabled").toBool())linked=true;}
            if(!linked||p.path.isEmpty()||p.path.last()!=from||p.path.contains(nodeId)||p.path.first()!=p.data.source){result(p,"invalid_hop");return;}++rx;
        }
        const QString identity=p.data.source+"/"+p.data.id;
        if(seen.contains(identity)){result(p,"duplicate");return;}
        if(seenOrder.size()>=4096)seen.remove(seenOrder.dequeue());seen.insert(identity);seenOrder.enqueue(identity);p.path.append(nodeId);
        if(nodeId==p.data.destination){result(p,"delivered");return;}
        if(p.data.ttl==0){result(p,"ttl_expired");return;}
        const auto path=graph.route(nodeId,p.data.destination);if(path.size()<2){result(p,"no_route");return;}
        const auto link=plan.link(nodeId,path[1]);if(!link||!link->enabled){result(p,"link_down");return;}
        auto next=p;--next.data.ttl;const auto serial=++flight;LinkJob job;job.ticket=serial;job.legacyPacket=packetJson(next);
        if(link->bearer=="iq"){
            auto envelope=IqEnvelope{QUuid(session),serial,nodeId,path[1],next.data};envelope.packet.payload=compact(next);QString error;job.wire=IqModem::encode(envelope,&error);
            if(job.wire.isEmpty()){result(p,"modem_mtu_exceeded");return;}
        }
        labFlights[serial]=p;QString error;if(!sender(*link)->submit(job,&error)){labFlights.remove(serial);result(p,"queue_full");}publish();
    }
signals:void changed(const QJsonObject &);
};
}
NodeService::NodeService(QObject *parent):QObject(parent){}
bool NodeService::radioCaptureActive(){return RadioIngress::anyActive();}
NodeService::~NodeService(){stop();}
void NodeService::stop(){++generation;if(worker)worker->disconnect(this);thread.quit();thread.wait();worker=nullptr;secret.clear();snapshot={{"ok",true},{"listening",false},{"rfEnabled",false}};}
QJsonObject NodeService::command(const QJsonObject &input) {
    auto r=input;
    const auto op=r.value("operation").toString();
    if(op=="status")return snapshot.isEmpty()?QJsonObject{{"ok",true},{"listening",false},{"rfEnabled",false}}:snapshot;
    if(op=="stop"){stop();return snapshot;}
    if((op=="arm-tx"||op=="disarm-tx")&&worker){QJsonObject reply;auto w=static_cast<NodeWorker *>(worker);QMetaObject::invokeMethod(w,[&]{if(op=="arm-tx")reply=w->armTx(r.value("seconds").toInt());else {w->disarmTx();w->publish();reply=w->status();}},Qt::BlockingQueuedConnection);if(reply.value("ok").toBool())snapshot=reply;return reply;}
    if(op=="revoke"&&worker){QJsonObject reply;auto w=static_cast<NodeWorker *>(worker);QMetaObject::invokeMethod(w,[&]{reply=w->revoke(r.value("clientId").toString());},Qt::BlockingQueuedConnection);return reply;}
    if(op=="registrations"&&worker){QJsonObject reply;auto w=static_cast<NodeWorker *>(worker);QMetaObject::invokeMethod(w,[&]{reply=w->registrations();},Qt::BlockingQueuedConnection);return reply;}
    if(op!="start")return failure("Use mesh.service start|status|stop");
    if(worker)return failure("Mesh service already started");
    if(r.contains("optionsFile")){
        QFile file(r.value("optionsFile").toString());QJsonParseError parse;
        if(!file.open(QIODevice::ReadOnly)||file.size()>65536)return failure("Cannot read private node options (limit 64 KiB)");
        const auto doc=QJsonDocument::fromJson(file.readAll(),&parse);if(parse.error!=QJsonParseError::NoError||!doc.isObject())return failure("Private node options must be a JSON object");
        const auto values=doc.object();for(auto i=values.begin();i!=values.end();++i){if(!QStringList{"allowRadioRx","radioSources","radioTxProfiles","reticulum","protocolProfiles","protocolStateDirectory"}.contains(i.key()))return failure("Unknown private node option");r[i.key()]=i.value();}
    }
    const auto bind=r.value("bind").toString("127.0.0.1");QHostAddress address;
    if(!address.setAddress(bind)||!integer(r.value("port"),0,65535))return failure("Specify a numeric bind address and port 0..65535");
    if(r.contains("protocolStateDirectory")&&(!r.value("protocolStateDirectory").isString()||!QDir::isAbsolutePath(r.value("protocolStateDirectory").toString())))return failure("Local protocol state directory must be absolute");
    secret=key();auto w=new NodeWorker(secret);worker=w;const auto current=++generation;w->moveToThread(&thread);
    w->allowRadioRx=r.value("allowRadioRx").toBool(false);
    if(w->allowRadioRx){
        const auto sources=r.value("radioSources").toArray();
        if(sources.isEmpty())w->radioSources.insert("main/rx0");
        for(const auto &v:sources){const auto s=v.toObject();w->radioSources.insert(s.value("receiverId").toString()+"/"+s.value("channelId").toString());}
    }
    w->localReticulum=r.value("reticulum").toObject();
    w->localProfiles=r.value("protocolProfiles").toObject();
    w->localTxProfiles=r.value("radioTxProfiles").toObject();
    w->approvedStateRoot=r.value("protocolStateDirectory").toString();
    if(w->localReticulum.isEmpty()){
        const QDir app(QCoreApplication::applicationDirPath());
        QStringList pythons{app.filePath("backends/reticulum/python.exe"),app.filePath("backends/reticulum/bin/python3"),app.filePath("../reticulum-venv/Scripts/python.exe"),app.filePath("../../reticulum-venv/Scripts/python.exe"),app.filePath("../reticulum-venv/bin/python3")};
        QStringList modules{app.filePath("backends/reticulum"),app.filePath("../../src/reticulum"),app.filePath("../../../src/reticulum")};
        for(const auto &path:pythons)if(QFileInfo(path).isFile()){w->localReticulum["python"]=QFileInfo(path).absoluteFilePath();break;}
        for(const auto &path:modules)if(QFileInfo(QDir(path).filePath("obrii_reticulum/service.py")).isFile()){w->localReticulum["moduleDirectory"]=QFileInfo(path).absoluteFilePath();break;}
    }
    connect(&thread,&QThread::finished,w,&QObject::deleteLater);connect(w,&NodeWorker::changed,this,[this,current](const QJsonObject &s){if(generation==current)snapshot=s;});thread.start();
    QJsonObject result;QMetaObject::invokeMethod(w,[&]{result=w->start(bind,quint16(r.value("port").toInt()));},Qt::BlockingQueuedConnection);
    if(!result.value("ok").toBool()){stop();return result;}snapshot=result;result["token"]=secret;result["pairingSecret"]=w->pairingSecret;return result;
}
}
#include "meshnode.moc"
