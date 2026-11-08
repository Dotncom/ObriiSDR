#include "meshremotedialog.h"
#include "meshdiscovery.h"
#include "meshrouteddiscoverydialog.h"
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

namespace Mesh {
namespace {
QString u(const char *text){return QString::fromUtf8(text);}
constexpr int MaximumReply=128*1024;
constexpr int MaximumResults=1024;
struct Endpoint {
    QString id,host,token,instance;
    int port=0;
};
using Reply=std::function<void(const QJsonObject &,const QString &)>;
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
QString randomKey(){
    QString result;
    for(int i=0;i<8;++i)result+=QString::number(QRandomGenerator::system()->generate(),16).rightJustified(8,'0');
    return result;
}
bool hostValid(const QString &host){
    return !host.isEmpty()&&host.size()<=253&&!host.contains(QRegularExpression("[^A-Za-z0-9_.:-]"));
}
bool integer(const QJsonValue &value,int minimum,int maximum){
    return value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=minimum&&
        value.toDouble()<=maximum&&std::floor(value.toDouble())==value.toDouble();
}
QJsonObject discoveryEntry(const QJsonObject &object){
    QHostAddress address;const auto name=object.value("name");
    if(object.value("protocol")!="obrii-mesh-node"||!integer(object.value("version"),1,1)||
        QUuid(object.value("instanceId").toString()).isNull()||!integer(object.value("port"),1,65535)||
        !name.isString()||name.toString().size()>128||!object.value("active").isBool()||
        !address.setAddress(object.value("host").toString())||address.protocol()!=QAbstractSocket::IPv4Protocol||
        address==QHostAddress::AnyIPv4||address==QHostAddress::Broadcast||address.isMulticast())return {};
    const auto bearers=object.value("availableBearers").toArray();
    if(bearers.isEmpty()||bearers.size()>2)return {};
    for(const auto &bearer:bearers)if(bearer!="data"&&bearer!="iq")return {};
    QJsonObject result{{"protocol","obrii-mesh-node"},{"version",1},{"instanceId",object.value("instanceId").toString()},
        {"host",address.toString()},{"port",object.value("port").toInt()},{"name",name.toString()},
        {"active",object.value("active").toBool()},{"availableBearers",bearers}};
    if(object.value("directoryVersion")==1)result["directoryVersion"]=1;
    QJsonArray methods;for(const auto &v:object.value("discoveryMethods").toArray())if((v=="udp"||v=="tcp"||v=="directory")&&!methods.contains(v))methods.append(v);
    if(!methods.isEmpty())result["discoveryMethods"]=methods;
    const auto seen=object.value("lastSeenUtcMs").toDouble();if(std::isfinite(seen)&&seen>0&&seen<1e16)result["lastSeenUtcMs"]=seen;
    return result;
}
QString discoveryKey(const QJsonObject &object){
    return QUuid(object.value("instanceId").toString()).toString(QUuid::WithoutBraces)+"/"+
        object.value("host").toString()+"/"+QString::number(object.value("port").toInt());
}
struct InstanceRegistry {
    QString clientId;
    QMap<QString,QJsonObject> known;
    QMap<QString,QString> credentials;
    QSet<QString> pairing;
    QMap<RemoteDialog *,std::function<void()>> listeners;
    QJsonObject searchConfiguration=defaultRoutedDiscoveryConfiguration();
    InstanceRegistry(){
        QSettings settings(QSettings::defaultFormat(),QSettings::UserScope,"Obrii","ObriiMeshDesigner");clientId=settings.value("meshRemote/clientId").toString();
        if(QUuid(clientId).isNull()){clientId=uuid();settings.setValue("meshRemote/clientId",clientId);}
        const auto bytes=settings.value("meshRemote/routedDiscovery").toByteArray();
        if(bytes.size()<=32768){const auto parsed=QJsonDocument::fromJson(bytes).object();if(routedDiscoveryConfigurationValid(parsed))searchConfiguration=parsed;}
    }
};
InstanceRegistry &instanceRegistry(){static InstanceRegistry registry;return registry;}
void registryChanged(){const auto listeners=instanceRegistry().listeners.values();for(const auto &listener:listeners)listener();}
QTableWidgetItem *cell(const QString &text){
    auto item=new QTableWidgetItem(text);item->setFlags(item->flags()&~Qt::ItemIsEditable);item->setToolTip(text);return item;
}
QString pathText(const QJsonArray &path){QStringList ids;for(const auto &id:path)ids<<id.toString();return ids.join(" -> ");}
QString counter(const QJsonValue &value){return value.isString()?value.toString():QString::number(value.toDouble(), 'f',0);}
QString scalarText(const QJsonValue &value){return value.isString()?value.toString():value.isDouble()?counter(value):QString();}
QString resultName(const QString &status){
    static const QMap<QString,QString> names{{"delivered",u("Доставлено")},{"forwarded",u("Переслано")},
        {"no_route",u("Немає маршруту")},{"ttl_expired",u("Вичерпано TTL")},{"duplicate",u("Дублікат")},
        {"queue_full",u("Черга заповнена")},{"link_down",u("Розрив зв'язку")},{"node_down",u("Вузол недоступний")},
        {"modem_timeout",u("Модем: немає декодування")},{"modem_tx_rejected",u("Модем: TX відхилено")},
        {"send_unconfirmed",u("Надсилання не підтверджене")}};
    return names.value(status,status);
}

// Requests survive the dialog so an in-flight configure can still be rolled back.
class JsonRpc final : public QObject {
public:
    static void call(const Endpoint &endpoint,QJsonObject request,Reply reply){
        new JsonRpc(endpoint,std::move(request),std::move(reply));
    }
private:
    QTcpSocket socket;
    QTimer timeout;
    QByteArray buffer,wire;
    QString requestId;
    Reply reply;
    bool finished=false;
    JsonRpc(const Endpoint &endpoint,QJsonObject request,Reply callback):QObject(QCoreApplication::instance()),reply(std::move(callback)){
        requestId=uuid();request["requestId"]=requestId;
        if(request.value("op")=="pair")request.remove("token");else request["token"]=endpoint.token;
        wire=QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n';
        socket.setReadBufferSize(MaximumReply+1);timeout.setSingleShot(true);
        connect(&socket,&QTcpSocket::connected,this,[this]{socket.write(wire);wire.fill('\0');wire.clear();});
        connect(&socket,&QTcpSocket::readyRead,this,[this]{read();});
        connect(&socket,&QTcpSocket::disconnected,this,[this]{read();if(!finished)finish({},u("З'єднання закрито без відповіді: перевірте токен і службу."));});
        connect(&socket,&QTcpSocket::errorOccurred,this,[this](QAbstractSocket::SocketError error){
            if(error==QAbstractSocket::RemoteHostClosedError)return;
            finish({},u("Помилка TCP: ")+socket.errorString());
        });
        connect(&timeout,&QTimer::timeout,this,[this]{finish({},u("Час очікування RPC вичерпано (3 с)."));});
        if(wire.size()>MaximumReply){QTimer::singleShot(0,this,[this]{finish({},u("Запит RPC перевищує 128 КіБ."));});return;}
        timeout.start(3000);socket.connectToHost(endpoint.host,quint16(endpoint.port));
    }
    void read(){
        if(finished)return;
        const auto bytes=socket.readAll();
        if(bytes.size()>MaximumReply-buffer.size()){finish({},u("Відповідь RPC перевищує 128 КіБ."));return;}
        buffer+=bytes;const int end=buffer.indexOf('\n');if(end<0)return;
        QJsonParseError parse;const auto document=QJsonDocument::fromJson(buffer.left(end),&parse);
        if(parse.error!=QJsonParseError::NoError||!document.isObject()||!buffer.mid(end+1).trimmed().isEmpty()){
            finish({},u("Некоректна відповідь JSON-lines."));return;
        }
        const auto object=document.object();
        if(object.value("requestId")!=requestId||!object.value("ok").isBool()){
            finish({},u("Відповідь RPC має інший requestId або некоректний формат."));return;
        }
        QString error;
        if(!object.value("ok").toBool())error=object.value("error").toString().trimmed();
        if(!object.value("ok").toBool()&&error.isEmpty())error=u("Служба відхилила запит.");
        finish(object,error);
    }
    void finish(const QJsonObject &object,const QString &error){
        if(finished)return;finished=true;timeout.stop();socket.disconnect(this);socket.abort();wire.fill('\0');wire.clear();
        auto callback=std::move(reply);deleteLater();if(callback)callback(object,error);
    }
};

struct Run {
    QVector<Endpoint> endpoints;
    QString session=uuid(),key=randomKey(),transport;
    int probes=0,configures=0,releases=0;
    QVector<int> releaseRevision;
    QVector<int> pendingReleases;
    QVector<bool> releaseFailed;
    QVector<bool> available,enabled,updating,joining;
    QVector<quint64> epochs;
    QVector<QByteArray> appliedTopology;
    QJsonObject topology;
    bool stopped=false,cleanupFinished=false,configureStarted=false,fatalIdentity=false;
    std::function<void(int)> cleaned;
    std::function<void(int,const QJsonObject &,const QString &)> released;
};
void cleanupComplete(const std::shared_ptr<Run> &run){
    if(!run->stopped||run->cleanupFinished||run->configures||run->releases)return;
    run->cleanupFinished=true;run->key.fill('0');run->key.clear();
    for(auto &endpoint:run->endpoints){endpoint.token.fill('0');endpoint.token.clear();}
    int errors=0;for(bool failed:run->releaseFailed)if(failed)++errors;
    run->released={};auto callback=std::move(run->cleaned);if(callback)callback(errors);
}
QString protocolError(const QJsonObject &reply);
void releaseEndpoint(const std::shared_ptr<Run> &run,int index){
    const auto endpoint=run->endpoints[index];if(endpoint.instance.isEmpty())return;
    ++run->releases;++run->pendingReleases[index];const int revision=++run->releaseRevision[index];
    JsonRpc::call(endpoint,{{"op","release"},{"instanceId",endpoint.instance},{"session",run->session}},
        [run,index,revision](const QJsonObject &reply,QString error){
            --run->pendingReleases[index];--run->releases;
            if(error.isEmpty())error=protocolError(reply);
            if(error.isEmpty()&&(reply.value("instanceId")!=run->endpoints[index].instance||reply.value("active").toBool()))
                error=u("Звільнення сеансу не підтверджено.");
            if(revision==run->releaseRevision[index]){
                run->releaseFailed[index]=!error.isEmpty();if(run->released)run->released(index,reply,error);
            }
            cleanupComplete(run);
        });
}
QString protocolError(const QJsonObject &reply){
    if(reply.value("protocol")!="obrii-mesh-node"||!integer(reply.value("version"),1,1))return u("Несумісний протокол або версія служби.");
    if(QUuid(reply.value("instanceId").toString()).isNull()||!reply.value("active").isBool()||!reply.value("listening").toBool())
        return u("Служба не слухає порт або повернула некоректний стан.");
    return {};
}
QJsonValue publicScalar(const QJsonValue &value,int maximum=256){
    if(value.isString()&&value.toString().size()<=maximum&&!value.toString().contains(QRegularExpression("[\\x00-\\x08\\x0b\\x0c\\x0e-\\x1f]")))return value;
    if(value.isBool())return value;
    if(value.isDouble()&&std::isfinite(value.toDouble()))return value;
    return QJsonValue(QJsonValue::Undefined);
}
QJsonObject nativeStatus(const QJsonObject &object){
    QJsonObject result;
    for(const auto key:{"type","protocol","ready","available","nativeAddress","nodeNumber","role","relayRole","interfaceId","channels","hopLimit","queued","pending","droppedEvents","running","framing","transport","bitrate","mtu","queuedRpcs","txPending","rns_version","single_mdu","receipts_pending","requests_pending","dropped_events","backend","backendAvailable","errorCode","state","maxPayload","tx","rx","forwarded","delivered","dropped"}){
        const auto value=publicScalar(object.value(key));if(!value.isUndefined())result[key]=value;
    }
    for(const auto key:{"pki","pkiPeerCount","nativeFingerprint","publicKeyHex","admin","pkiAdmin","deferredRx","startupTimeoutMs","rpcTimeoutMs","activation","activated","region","rxContextRequired","hardwareTimingVerified","timingSource","received","transmitted","rejected","nextTransmitCounter","dataRate","frequencyHz","nextUplinkUs","ackPending","nbTrans"}){
        const auto value=publicScalar(object.value(key));if(!value.isUndefined())result[key]=value;
    }
    QJsonArray windows;for(const auto &v:object.value("windows").toArray()){
        if(windows.size()>=2)break;QJsonObject window;const auto input=v.toObject();
        for(const auto key:{"opensUs","nominalUs","closesUs","frequencyHz","dataRate"}){const auto value=publicScalar(input.value(key));if(!value.isUndefined())window[key]=value;}
        windows.append(window);
    }if(!windows.isEmpty())result["windows"]=windows;
    if(!result.contains("type")&&QStringList{"lab","meshtastic","reticulum","lorawan"}.contains(result.value("protocol").toString()))result["type"]=result.value("protocol");
    if(!object.value("error").toString().isEmpty())result["backendError"]=true;
    return result;
}
QJsonObject nativeReplyBody(const QJsonObject &object){
    auto body=nativeStatus(object);
    for(const auto field:{"ok","pending","accepted","queued","completed","id","status","op","operation","command","nativeId","packet","request","link","destination","identity","peer_identity","from","to","payloadBytes","known","active","hops","encrypted","sender_authenticated","signature_valid","mdu"}){
        const auto value=publicScalar(object.value(field));if(!value.isUndefined())body[field]=value;
    }
    if(object.value("status").isObject())body["status"]=nativeStatus(object.value("status").toObject());
    return body;
}
QJsonArray publicPorts(const QJsonArray &ports){
    QJsonArray result;
    for(const auto &value:ports){if(result.size()>=16)break;const auto input=value.toObject();QJsonObject port;
        for(const auto key:{"id","direction","backend","enabled","receiverId","channelId","ready","state","tx","rx","queue","sampleRate"}){
            const auto scalar=publicScalar(input.value(key),128);if(!scalar.isUndefined())port[key]=scalar;
        }
        if(!port.isEmpty())result.append(port);
    }
    return result;
}
QJsonObject publicResult(const QJsonObject &reply);
QJsonObject publicProtocolReply(const QJsonObject &reply){
    auto result=nativeStatus(reply);result["ok"]=reply.value("ok").toBool();
    if(reply.value("nativeProtocol").isObject())result["nativeProtocol"]=nativeStatus(reply.value("nativeProtocol").toObject());
    if(reply.value("ports").isArray())result["ports"]=publicPorts(reply.value("ports").toArray());
    for(const auto key:{"result","protocolReply"})if(reply.value(key).isObject()){
        const auto object=reply.value(key).toObject();auto body=nativeReplyBody(object);const auto event=publicResult(object);for(auto it=event.begin();it!=event.end();++it)body[it.key()]=it.value();
        result[key]=body;
    }
    QJsonArray events;for(const auto &value:reply.value("results").toArray()){const auto event=publicResult(value.toObject());if(!event.isEmpty())events.append(event);}if(!events.isEmpty())result["results"]=events;
    return result;
}
QJsonObject publicTelemetry(const QJsonObject &reply,const QString &id){
    QJsonObject result{{"nodeId",id}};
    if(reply.value("instanceId").isString())result["instanceId"]=reply.value("instanceId").toString();
    for(const auto key:{"listening","active"})if(reply.value(key).isBool())result[key]=reply.value(key).toBool();
    if(integer(reply.value("port"),1,65535))result["port"]=reply.value("port").toInt();
    const auto transport=reply.value("transport").toString();
    if(transport.isEmpty()||transport=="data"||transport=="iq")result["transport"]=transport;
    for(const auto key:{"queue","tx","rx","forwarded","delivered","dropped","iqTxSamples","iqRxSamples","resultsEvicted","lastResultSerial","topologyRevision"}){
        const auto value=reply.value(key);
        if(value.isString()){
            bool valid=false;value.toString().toULongLong(&valid);
            if(valid&&QRegularExpression("^[0-9]{1,20}$").match(value.toString()).hasMatch())result[key]=value.toString();
        }else if(value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0&&std::floor(value.toDouble())==value.toDouble())result[key]=value.toDouble();
    }
    if(reply.value("nativeProtocol").isObject())result["nativeProtocol"]=nativeStatus(reply.value("nativeProtocol").toObject());
    if(reply.value("ports").isArray())result["ports"]=publicPorts(reply.value("ports").toArray());
    for(const auto key:{"availableBearers","availableModems","nativeProtocols"}){
        QJsonArray values;for(const auto &value:reply.value(key).toArray())if(value.isString()&&value.toString().size()<=32&&values.size()<16)values.append(value.toString());
        if(!values.isEmpty())result[key]=values;
    }
    if(integer(reply.value("portBindingsVersion"),1,1))result["portBindingsVersion"]=1;
    if(integer(reply.value("liveTopologyVersion"),1,1))result["liveTopologyVersion"]=1;
    if(reply.value("perLinkBearers").isBool())result["perLinkBearers"]=reply.value("perLinkBearers").toBool();
    for(const auto key:{"radioRxAllowed","radioTxAllowed"})if(reply.value(key).isBool())result[key]=reply.value(key).toBool();
    return result;
}
QJsonObject publicResult(const QJsonObject &reply){
    const bool native=reply.contains("event")||reply.contains("nativeProtocol")||QStringList{"meshtastic","reticulum","lorawan"}.contains(reply.value("protocol").toString());
    if(native){
        QJsonObject result;
        for(const auto key:{"timingSource","hardwareTimingVerified","frequencyHz","dataRate","airtimeUs","startUs","endUs","startDeadlineUs","counter","confirmed","ack","senderPublicKeyHex","senderFingerprint"}){
            const auto value=publicScalar(reply.value(key));if(!value.isUndefined())result[key]=value;
        }
        for(const auto key:{"id","packetId","nativeId","source","destination","from","to","nativeSource","nativeDestination","node","protocol","nativeProtocol","status","event","type","kind","interfaceId","interface","reason","serial","ticket","localMs","atMs","nodeNumber","port","portnum","channel","hops","payloadBytes","snrDb","rssiDbm","rxSnr","rxRssi","authenticated","security","routingError","backendEvent","packet","request","link","identity","peer_identity","encrypted","sender_authenticated","signature_valid","initiator","online","mdu","op","ok","nativeAddress"}){
            const auto scalar=publicScalar(reply.value(key));if(!scalar.isUndefined())result[key]=scalar;
        }
        for(const auto key:{"text","payloadUtf8"})if(reply.value(key).isString()&&reply.value(key).toString().size()<=4096)result[key]=reply.value(key).toString();
        for(const auto key:{"payloadBase64","wireBase64","appDataBase64"})if(reply.value(key).isString()){
            const auto encoded=reply.value(key).toString().toLatin1();if(encoded.size()<=10924&&QByteArray::fromBase64(encoded).toBase64()==encoded)result[key]=QString::fromLatin1(encoded);
        }
        if(reply.value("path").isArray()&&reply.value("path").toArray().size()<=8){QJsonArray path;for(const auto &value:reply.value("path").toArray()){const auto scalar=publicScalar(value);if(!scalar.isUndefined())path.append(scalar);}result["path"]=path;}
        if(reply.value("result").isObject())result["result"]=nativeReplyBody(reply.value("result").toObject());
        if(result.isEmpty())return {};
        if(!result.contains("status"))result["status"]=result.value("event").toString("native_event");
        return result;
    }
    const QRegularExpression identifier("^[A-Za-z][A-Za-z0-9_.-]{0,63}$");
    for(const auto key:{"id","source","destination","node","status"})
        if(!reply.value(key).isString()||!identifier.match(reply.value(key).toString()).hasMatch())return {};
    if(!reply.value("path").isArray()||reply.value("path").toArray().size()>8)return {};
    for(const auto &id:reply.value("path").toArray())if(!id.isString()||!identifier.match(id.toString()).hasMatch())return {};
    if(!reply.value("localMs").isDouble()||!std::isfinite(reply.value("localMs").toDouble())||reply.value("localMs").toDouble()<0)return {};
    if(!reply.value("payloadBase64").isString())return {};
    const auto encoded=reply.value("payloadBase64").toString().toLatin1();if(encoded.size()>5464)return {};
    const auto payload=QByteArray::fromBase64(encoded);if(payload.isEmpty()||payload.size()>4096||payload.toBase64()!=encoded)return {};
    QJsonObject result;
    for(const auto key:{"id","source","destination","node","status","path","payloadBase64","localMs"})
        if(reply.contains(key))result[key]=reply.value(key);
    return result;
}
class Controller final : public QObject {
public:
    enum State {Idle,Checking,Configuring,Attached,Stopping};
    State state=Idle;
    QString message=u("Не приєднано"),lastError;
    QVector<QJsonObject> telemetry;
    QVector<QString> nodeStates;
    QJsonArray results,events;
    std::function<void()> changed;
    std::function<void(int,bool,const QJsonObject &,const QString &)> protocolReply;
    explicit Controller(QObject *parent):QObject(parent){
        timer.setInterval(1000);connect(&timer,&QTimer::timeout,this,[this]{poll();});
    }
    ~Controller() override {changed={};stop();}
    void begin(const QVector<Endpoint> &endpoints,const QJsonObject &topology,const QString &transport){
        if(state!=Idle)return;
        ++generation;const auto serial=generation;
        run=std::make_shared<Run>();const auto owner=run;
        owner->endpoints=endpoints;owner->transport=transport;owner->probes=endpoints.size();
        owner->topology=topology;owner->available=QVector<bool>(endpoints.size(),false);owner->enabled=QVector<bool>(endpoints.size(),true);
        owner->updating=owner->joining=QVector<bool>(endpoints.size(),false);owner->epochs=QVector<quint64>(endpoints.size(),0);owner->appliedTopology.resize(endpoints.size());
        const auto initialNodes=topology.value("nodes").toArray();owner->probes=0;
        for(int i=0;i<endpoints.size();++i){owner->enabled[i]=initialNodes.at(i).toObject().value("enabled").toBool(true);if(owner->enabled[i])++owner->probes;}
        owner->releaseRevision=QVector<int>(endpoints.size(),0);owner->releaseFailed=QVector<bool>(endpoints.size(),false);
        owner->pendingReleases=QVector<int>(endpoints.size(),0);
        state=Checking;message=u("Перевірка служб...");lastError.clear();results={};events={};resultKeys.clear();
        telemetry=QVector<QJsonObject>(endpoints.size());nodeStates=QVector<QString>(endpoints.size(),u("Перевірка"));
        polling=QVector<bool>(endpoints.size(),false);sending=polling;clock.start();
        const QPointer<Controller> guard(this);
        owner->released=[guard,serial](int index,const QJsonObject &reply,const QString &error){
            if(!guard||guard->generation!=serial||!guard->run)return;
            const auto owner=guard->run;
            if(error.isEmpty()){
                guard->telemetry[index]=publicTelemetry(reply,owner->endpoints[index].id);
                for(const auto &value:reply.value("results").toArray())if(value.isObject())guard->addResult(publicResult(value.toObject()));
                if(!guard->nodeStates[index].startsWith(u("Помилка:")))guard->nodeStates[index]=u("Від'єднано");
            }else {
                const auto text=guard->redact(owner,error);guard->event(owner->endpoints[index].id,text);
                guard->nodeStates[index]=u("Помилка звільнення: ")+text;
            }
            guard->notify();
        };
        owner->cleaned=[guard,serial](int errors){
            if(!guard||guard->generation!=serial)return;
            guard->run.reset();guard->state=Idle;
            guard->message=errors?u("Від'єднано; частину release не підтверджено. Резерв: оренда 15 с."):u("Від'єднано");
            for(auto &status:guard->nodeStates)if(!status.startsWith(u("Помилка")))status=u("Від'єднано");
            guard->notify();
        };
        notify();
        if(owner->probes==0){configure(owner,serial,topology);return;}
        for(int index=0;index<endpoints.size();++index){
            if(!owner->enabled[index]){nodeStates[index]=u("Вимкнено");continue;}
            JsonRpc::call(endpoints[index],{{"op","status"}},[guard,owner,serial,index,topology](const QJsonObject &reply,QString error){
                if(!guard||owner->stopped||guard->generation!=serial)return;
                if(error.isEmpty())error=protocolError(reply);
                if(error.isEmpty()&&reply.contains("availableBearers")&&!reply.value("availableBearers").toArray().contains(owner->transport))
                    error=u("Служба не підтримує вибраний транспорт.");
                if(error.isEmpty()&&reply.value("active").toBool())error=u("Вузол уже має активний сеанс. Перехоплення заборонено.");
                if(error.isEmpty()){
                    bool portFeatures=false,linkFeatures=false,protocolFeatures=false;
                    for(const auto &value:topology.value("nodes").toArray()){const auto node=value.toObject();portFeatures|=node.contains("ports");protocolFeatures|=node.contains("protocol");}
                    for(const auto &value:topology.value("links").toArray()){const auto link=value.toObject();linkFeatures|=link.contains("bearer")||link.contains("txPort")||link.contains("rxPort")||link.contains("modem");portFeatures|=link.contains("txPort")||link.contains("rxPort");}
                    if(portFeatures&&!integer(reply.value("portBindingsVersion"),1,1))error=u("Служба не підтримує прив'язки портів v1.");
                    if(linkFeatures&&reply.value("perLinkBearers")!=true)error=u("Служба не підтримує окремі носії зв'язків.");
                    if(protocolFeatures&&!reply.value("nativeProtocols").isArray())error=u("Служба не підтвердила підтримку протокольних параметрів вузлів.");
                    const auto node=topology.value("nodes").toArray().at(index).toObject();const auto protocol=node.value("protocol").toObject().value("type").toString("lab");
                    if((protocol!="lab"||protocolFeatures)&&!reply.value("nativeProtocols").toArray().contains(protocol))error=u("Служба не підтвердила підтримку протоколу: ")+protocol;
                    for(const auto &value:node.value("ports").toArray()){
                        const auto port=value.toObject();if(!port.value("enabled").toBool(true)||port.value("backend")!="radio")continue;
                        if(port.value("direction")=="rx"&&reply.value("radioRxAllowed")!=true)error=u("Фізичний RX потребує локального дозволу host-служби (radioRxAllowed).");
                        if(port.value("direction")=="tx"&&reply.value("radioTxAllowed")!=true)error=u("Фізичний TX потребує локально озброєного backend і дозволу host-служби (radioTxAllowed).");
                    }
                    for(const auto &value:topology.value("links").toArray()){
                        const auto link=value.toObject();if(!link.value("enabled").toBool()||(link.value("tx")!=owner->endpoints[index].id&&link.value("rx")!=owner->endpoints[index].id))continue;
                        const auto bearer=link.value("bearer").toString("inherit");const auto effective=bearer=="inherit"?owner->transport:bearer;
                        if(reply.contains("availableBearers")&&!reply.value("availableBearers").toArray().contains(effective))error=u("Носій зв'язку не підтримується службою: ")+effective;
                        const auto modem=link.value("modem").toObject().value("type").toString("afsk1200");
                        if(effective=="iq"&&modem!="afsk1200"&&!reply.value("availableModems").toArray().contains(modem))error=u("Служба не підтвердила підтримку модема: ")+modem;
                    }
                }
                if(error.isEmpty()){
                    const auto instance=reply.value("instanceId").toString();
                    if(!owner->endpoints[index].instance.isEmpty()&&QUuid(owner->endpoints[index].instance)!=QUuid(instance))
                        error=u("Знайдений екземпляр змінився. Виберіть і зареєструйте його повторно.");
                    for(int other=0;other<owner->endpoints.size();++other)if(other!=index&&owner->enabled[other]&&
                        !owner->endpoints[other].instance.isEmpty()&&QUuid(owner->endpoints[other].instance)==QUuid(instance)){
                        error=u("Кілька адрес вказують на той самий instanceId.");owner->fatalIdentity=true;break;
                    }
                    if(error.isEmpty())owner->endpoints[index].instance=instance;
                }
                if(!error.isEmpty())guard->fail(owner,index,error);
                else {owner->available[index]=true;guard->telemetry[index]=publicTelemetry(reply,owner->endpoints[index].id);guard->nodeStates[index]=u("Перевірено");}
                if(--owner->probes==0){if(owner->fatalIdentity)guard->stop();else guard->configure(owner,serial,topology);}else guard->notify();
            });
        }
    }
    void stop(){
        timer.stop();if(!run||run->stopped)return;
        const auto owner=run;owner->stopped=true;state=Stopping;message=u("Звільнення власного сеансу...");
        // A second release follows every late configure reply, even after a timeout.
        if(owner->configureStarted)for(int index=0;index<owner->endpoints.size();++index)releaseEndpoint(owner,index);
        cleanupComplete(owner);notify();
    }
    bool canSend(int index) const {return state==Attached&&run&&index>=0&&index<sending.size()&&run->available[index]&&run->enabled[index]&&!sending[index]&&!run->joining[index]&&!run->updating[index]&&
        run->appliedTopology[index]==QJsonDocument(QJsonObject{{"topology",effectiveTopology(run)},{"peers",peerMap(run)}}).toJson(QJsonDocument::Compact);}
    bool nodeEnabled(int index) const {return run&&index>=0&&index<run->enabled.size()&&run->enabled[index];}
    bool nodeAvailable(int index) const {return run&&index>=0&&index<run->available.size()&&run->enabled[index]&&run->available[index]&&!run->joining[index];}
    bool canEditEndpoint(int index) const {return state==Idle||(state==Attached&&run&&index>=0&&index<run->available.size()&&!run->enabled[index]&&!run->available[index]&&!run->joining[index]&&!run->updating[index]&&!run->pendingReleases[index]);}
    bool canSetEnabled(int index,bool enabled) const {
        return state==Attached&&run&&index>=0&&index<run->endpoints.size()&&(!enabled||(!run->pendingReleases[index]&&!run->joining[index]));
    }
    bool setEnabled(int index,bool enabled){
        if(!canSetEnabled(index,enabled))return false;
        const auto owner=run;owner->enabled[index]=enabled;++owner->epochs[index];
        if(!enabled){owner->available[index]=false;nodeStates[index]=u("Вимкнено");releaseEndpoint(owner,index);syncTopology();notify();}
        else join(index);
        return true;
    }
    bool updateTopology(const QJsonObject &topology){
        if(state!=Attached||!run||topology.value("nodes").toArray().size()!=run->endpoints.size())return false;
        run->topology=topology;syncTopology();notify();return true;
    }
    bool append(const Endpoint &endpoint,const QJsonObject &topology){
        if(state!=Attached||!run||run->endpoints.size()>=8)return false;
        run->endpoints.append(endpoint);run->releaseRevision.append(0);run->releaseFailed.append(false);run->pendingReleases.append(0);
        run->available.append(false);run->enabled.append(false);run->updating.append(false);run->joining.append(false);run->epochs.append(0);run->appliedTopology.append(QByteArray{});
        telemetry.append(QJsonObject{});nodeStates.append(u("Вимкнено"));polling.append(false);sending.append(false);run->topology=topology;return true;
    }
    bool setEndpoint(int index,const Endpoint &endpoint){
        if(!run||index<0||index>=run->endpoints.size()||run->available[index]||run->joining[index]||run->pendingReleases[index])return false;
        auto replacement=endpoint;const auto old=run->endpoints[index];
        if(replacement.instance.isEmpty()&&old.host==replacement.host&&old.port==replacement.port&&old.token==replacement.token)replacement.instance=old.instance;
        run->endpoints[index]=replacement;return true;
    }
    void send(int index,const QString &destination,const QString &id,int ttl,const QByteArray &payload,const QString &nativeDestination={}){
        if(!canSend(index)||!run)return;
        const auto owner=run;const auto serial=generation,epoch=owner->epochs[index];const auto endpoint=owner->endpoints[index];sending[index]=true;
        const QPointer<Controller> guard(this);
        QJsonObject request{{"op","send"},{"instanceId",endpoint.instance},{"session",owner->session},{"destination",destination},
            {"id",id},{"ttl",ttl},{"payloadBase64",QString::fromLatin1(payload.toBase64())}};if(!nativeDestination.isEmpty())request["nativeDestination"]=nativeDestination;
        JsonRpc::call(endpoint,request,
            [guard,owner,serial,epoch,index,destination,id,payload,nativeDestination](const QJsonObject &reply,QString error){
                if(!guard||owner->stopped||guard->generation!=serial)return;
                guard->sending[index]=false;
                if(owner->epochs[index]!=epoch){guard->notify();return;}
                if(error.isEmpty())error=guard->ownershipError(owner,index,reply);
                if(!error.isEmpty()){
                    error=guard->redact(owner,error);guard->lastError=error;
                    QJsonObject result{{"id",id},{"node",owner->endpoints[index].id},{"status","send_unconfirmed"},{"payloadBase64",QString::fromLatin1(payload.toBase64())},{"localMs",double(guard->clock.elapsed())},{"error",error}};
                    if(nativeDestination.isEmpty()){result["source"]=owner->endpoints[index].id;result["destination"]=destination;result["path"]=QJsonArray();}
                    else {const auto native=guard->telemetry[index].value("nativeProtocol").toObject();result["protocol"]=native.value("type");result["event"]="send_unconfirmed";result["to"]=nativeDestination;if(native.contains("nativeAddress"))result["from"]=native.value("nativeAddress");}
                    guard->addResult(result);
                    guard->event(owner->endpoints[index].id,error);
                }else {guard->lastError.clear();guard->ingest(owner,index,reply);}
                guard->notify();
            });
        notify();
    }
    void protocol(int index,const QJsonObject &args){
        if(!canSend(index)||!run)return;const auto owner=run;const auto serial=generation,epoch=owner->epochs[index];const auto endpoint=owner->endpoints[index];sending[index]=true;const QPointer<Controller> guard(this);
        JsonRpc::call(endpoint,{{"op","protocol"},{"instanceId",endpoint.instance},{"session",owner->session},{"args",args}},
            [guard,owner,serial,index,epoch](const QJsonObject &reply,const QString &error){
                if(!guard||owner->stopped||guard->generation!=serial)return;guard->sending[index]=false;
                if(owner->epochs[index]!=epoch){guard->notify();return;}
                QString problem;if(!error.isEmpty())problem=u("Native-команду не підтверджено. Перевірте стан backend і параметри команди.");
                else if(reply.contains("instanceId")&&reply.value("instanceId")!=owner->endpoints[index].instance)problem=u("Екземпляр служби змінився.");
                if(problem.isEmpty()&&reply.contains("session"))problem=guard->ownershipError(owner,index,reply);
                if(problem.isEmpty()&&reply.value("protocolReply").toObject().value("ok")==false)problem=u("Native-backend відхилив команду.");
                if(problem.isEmpty()&&reply.contains("session"))guard->ingest(owner,index,reply);
                guard->lastError=problem;if(!problem.isEmpty())guard->event(owner->endpoints[index].id,problem);const auto sanitized=publicProtocolReply(reply);guard->notify();if(guard->protocolReply)guard->protocolReply(index,problem.isEmpty(),sanitized,problem);
            });notify();
    }
private:
    QTimer timer;
    QElapsedTimer clock;
    std::shared_ptr<Run> run;
    quint64 generation=0;
    QVector<bool> polling,sending;
    QSet<QByteArray> resultKeys;
    void notify(){if(changed)changed();}
    QJsonObject peerMap(const std::shared_ptr<Run> &owner) const {
        QJsonObject peers;int index=0;for(const auto &endpoint:owner->endpoints){
            const auto instance=(endpoint.instance.isEmpty()||!owner->enabled[index])?QUuid::createUuidV5(QUuid(owner->session),endpoint.id.toUtf8()).toString(QUuid::WithoutBraces):endpoint.instance;++index;
            peers[endpoint.id]=QJsonObject{{"host",endpoint.host},{"port",endpoint.port},{"instanceId",instance}};
        }return peers;
    }
    QJsonObject effectiveTopology(const std::shared_ptr<Run> &owner,int joining=-1) const {
        auto result=owner->topology;QJsonArray nodes;QSet<QString> active;int index=0;
        for(const auto &value:result.value("nodes").toArray()){auto node=value.toObject();
            node["enabled"]=index<owner->enabled.size()&&owner->enabled[index]&&(owner->available[index]||index==joining);if(node.value("enabled").toBool())active.insert(node.value("id").toString());nodes.append(node);++index;
        }result["nodes"]=nodes;QJsonArray links;for(const auto &v:result.value("links").toArray()){auto l=v.toObject();l["enabled"]=l.value("enabled").toBool(true)&&active.contains(l.value("tx").toString())&&active.contains(l.value("rx").toString());links.append(l);}result["links"]=links;return result;
    }
    void syncTopology(){
        if(state!=Attached||!run)return;const auto owner=run;const auto serial=generation;
        const auto topology=effectiveTopology(owner),peers=peerMap(owner);
        int active=0;for(int i=0;i<owner->available.size();++i)if(owner->enabled[i]&&owner->available[i])++active;
        message=u("Мережа працює · доступні %1 / %2 вузлів").arg(active).arg(owner->enabled.count(true));if(active==owner->enabled.count(true))lastError.clear();
        const auto hash=QJsonDocument(QJsonObject{{"topology",topology},{"peers",peers}}).toJson(QJsonDocument::Compact);
        const QPointer<Controller> guard(this);
        for(int index=0;index<owner->endpoints.size();++index){
            if(!owner->available[index]||!owner->enabled[index]||owner->updating[index]||owner->joining[index]||polling[index]||owner->appliedTopology[index]==hash)continue;
            if(telemetry[index].value("liveTopologyVersion")!=1){fail(owner,index,u("Оновіть host-службу: потрібна підтримка живої топології v1."));continue;}
            const auto epoch=owner->epochs[index];owner->updating[index]=true;const auto endpoint=owner->endpoints[index];
            JsonRpc::call(endpoint,{{"op","updateTopology"},{"instanceId",endpoint.instance},{"session",owner->session},
                {"expectedRevision",telemetry[index].value("topologyRevision")},{"topology",topology},{"peers",peers}},
                [guard,owner,serial,index,epoch,hash](const QJsonObject &reply,QString error){
                    if(!guard||owner->stopped||guard->generation!=serial)return;owner->updating[index]=false;
                    if(owner->epochs[index]!=epoch){guard->notify();return;}
                    if(error.isEmpty())error=guard->ownershipError(owner,index,reply);
                    if(!error.isEmpty())guard->fail(owner,index,error);
                    else {guard->ingest(owner,index,reply);owner->appliedTopology[index]=hash;}
                    guard->notify();
                });
        }
    }
    void join(int index){
        if(!run||state!=Attached||run->joining[index])return;const auto owner=run;const auto serial=generation,epoch=owner->epochs[index];
        owner->joining[index]=true;nodeStates[index]=u("Приєднання...");const auto endpoint=owner->endpoints[index];const QPointer<Controller> guard(this);
        JsonRpc::call(endpoint,{{"op","status"}},[guard,owner,serial,index,epoch](const QJsonObject &reply,QString error){
            if(!guard||owner->stopped||guard->generation!=serial)return;
            if(epoch!=owner->epochs[index]){owner->joining[index]=false;guard->notify();return;}
            if(error.isEmpty())error=protocolError(reply);
            if(error.isEmpty()&&reply.value("liveTopologyVersion")!=1)error=u("Host-служба не підтримує живу топологію v1.");
            const auto instance=reply.value("instanceId").toString();const auto pinned=owner->endpoints[index].instance;
            if(error.isEmpty()&&!pinned.isEmpty()&&QUuid(instance)!=QUuid(pinned))error=u("Екземпляр змінився: виберіть і зареєструйте його повторно.");
            if(error.isEmpty())for(int other=0;other<owner->endpoints.size();++other)if(other!=index&&owner->enabled[other]&&!owner->endpoints[other].instance.isEmpty()&&QUuid(owner->endpoints[other].instance)==QUuid(instance))error=u("Екземпляр уже призначений іншому вузлу.");
            if(error.isEmpty()&&reply.value("active").toBool()){
                error=guard->ownershipError(owner,index,reply);
                if(error.isEmpty()){owner->joining[index]=false;owner->available[index]=true;guard->ingest(owner,index,reply);guard->syncTopology();guard->notify();return;}
            }
            if(!error.isEmpty()){owner->joining[index]=false;guard->fail(owner,index,error);guard->notify();return;}
            owner->endpoints[index].instance=instance;const auto endpoint=owner->endpoints[index];++owner->configures;owner->configureStarted=true;
            JsonRpc::call(endpoint,{{"op","configure"},{"instanceId",instance},{"session",owner->session},{"key",owner->key},
                {"nodeId",endpoint.id},{"transport",owner->transport},{"topology",guard->effectiveTopology(owner,index)},{"peers",guard->peerMap(owner)}},
                [guard,owner,serial,index,epoch](const QJsonObject &configured,QString problem){
                    --owner->configures;
                    if(owner->stopped){releaseEndpoint(owner,index);cleanupComplete(owner);return;}
                    if(!guard||guard->generation!=serial)return;owner->joining[index]=false;
                    if(epoch!=owner->epochs[index]){releaseEndpoint(owner,index);guard->notify();return;}
                    if(problem.isEmpty())problem=guard->ownershipError(owner,index,configured);
                    if(!problem.isEmpty())guard->fail(owner,index,problem);
                    else {owner->available[index]=true;owner->appliedTopology[index].clear();guard->telemetry[index]={};guard->ingest(owner,index,configured);}
                    guard->syncTopology();guard->notify();
                });
        });notify();
    }
    QString redact(const std::shared_ptr<Run> &owner,QString error) const {
        for(const auto &endpoint:owner->endpoints)if(!endpoint.token.isEmpty())error.replace(endpoint.token,"[redacted]");
        if(!owner->key.isEmpty())error.replace(owner->key,"[redacted]");
        error.replace(QRegularExpression("(?:[A-Za-z]:[\\\\/]|/(?:home|Users|opt|usr|tmp|var|etc)/)[^\\s\\\"<>]+"),"[host path]");
        error.replace(owner->session,"[session]");return error.left(512);
    }
    void event(const QString &node,const QString &error){
        events.append(QJsonObject{{"node",node},{"error",error},{"controllerMs",double(clock.elapsed())}});
        if(events.size()>256)events.removeFirst();
    }
    void fail(const std::shared_ptr<Run> &owner,int index,const QString &error){
        lastError=redact(owner,error);const auto previous=nodeStates[index];owner->available[index]=false;
        nodeStates[index]=u("Недоступний: ")+lastError;
        if(previous!=nodeStates[index])event(owner->endpoints[index].id,lastError);
        if(state==Attached)message=u("Мережа працює · є недоступні вузли");
    }
    QString ownershipError(const std::shared_ptr<Run> &owner,int index,const QJsonObject &reply) const {
        const auto error=protocolError(reply);if(!error.isEmpty())return error;
        if(reply.value("instanceId")!=owner->endpoints[index].instance)return u("Екземпляр служби змінився. Потрібне нове приєднання.");
        if(!reply.value("active").toBool()||reply.value("session")!=owner->session||reply.value("nodeId")!=owner->endpoints[index].id||reply.value("transport")!=owner->transport)
            return u("Служба більше не належить цьому сеансу.");
        return {};
    }
    void configure(const std::shared_ptr<Run> &owner,quint64 serial,const QJsonObject &topology){
        state=Configuring;message=u("Налаштування власного сеансу...");owner->configureStarted=true;owner->configures=owner->available.count(true);
        Q_UNUSED(topology);const auto current=effectiveTopology(owner);const auto peers=peerMap(owner);
        const QPointer<Controller> guard(this);
        for(int index=0;index<owner->endpoints.size();++index){
            if(!owner->available[index])continue;
            const auto endpoint=owner->endpoints[index];nodeStates[index]=u("Налаштування");
            JsonRpc::call(endpoint,{{"op","configure"},{"instanceId",endpoint.instance},{"session",owner->session},{"key",owner->key},
                {"nodeId",endpoint.id},{"transport",owner->transport},{"topology",current},{"peers",peers}},
                [guard,owner,serial,index](const QJsonObject &reply,QString error){
                    --owner->configures;
                    if(owner->stopped){releaseEndpoint(owner,index);cleanupComplete(owner);return;}
                    if(!guard||guard->generation!=serial)return;
                    if(error.isEmpty())error=guard->ownershipError(owner,index,reply);
                    if(!error.isEmpty())guard->fail(owner,index,error);else {guard->telemetry[index]={};guard->ingest(owner,index,reply);}
                    if(owner->configures==0){guard->state=Attached;guard->message=u("Мережа працює");guard->timer.start();guard->syncTopology();}
                    guard->notify();
                });
        }
        if(owner->configures==0){state=Attached;message=u("Мережа працює · вузли недоступні");timer.start();}
        notify();
    }
    void addResult(const QJsonObject &result){
        if(result.isEmpty())return;
        const auto key=QJsonDocument(result).toJson(QJsonDocument::Compact);if(resultKeys.contains(key))return;
        resultKeys.insert(key);results.append(result);
        if(results.size()>MaximumResults){resultKeys.remove(QJsonDocument(results.first().toObject()).toJson(QJsonDocument::Compact));results.removeFirst();}
    }
    void ingest(const std::shared_ptr<Run> &owner,int index,const QJsonObject &reply){
        // Probe telemetry may retain the previous session's history cursor.
        const auto previous=telemetry[index].value("active").toBool()?telemetry[index].value("lastResultSerial").toString().toULongLong():0;
        const auto previousRevision=telemetry[index].value("topologyRevision").toString().toULongLong();
        telemetry[index]=publicTelemetry(reply,owner->endpoints[index].id);
        if(telemetry[index].value("topologyRevision").toString().toULongLong()<previousRevision)telemetry[index]["topologyRevision"]=QString::number(previousRevision);
        if(telemetry[index].value("lastResultSerial").toString().toULongLong()<previous)telemetry[index]["lastResultSerial"]=QString::number(previous);
        nodeStates[index]=u("Приєднано");
        for(const auto &value:reply.value("results").toArray())if(value.isObject())addResult(publicResult(value.toObject()));
    }
    void poll(){
        if(state!=Attached||!run)return;
        const auto owner=run;const auto serial=generation;const QPointer<Controller> guard(this);
        for(int index=0;index<owner->endpoints.size();++index){
            if(polling[index]||owner->updating[index]||owner->joining[index]||!owner->enabled[index]||owner->endpoints[index].instance.isEmpty())continue;polling[index]=true;const auto endpoint=owner->endpoints[index];const auto epoch=owner->epochs[index];
            JsonRpc::call(endpoint,{{"op","status"},{"instanceId",endpoint.instance},{"session",owner->session},{"after",telemetry[index].value("lastResultSerial").toString("0")}},
                [guard,owner,serial,index,epoch](const QJsonObject &reply,QString error){
                    if(!guard||owner->stopped||guard->generation!=serial)return;
                    guard->polling[index]=false;if(owner->epochs[index]!=epoch)return;if(error.isEmpty())error=guard->ownershipError(owner,index,reply);
                    if(!error.isEmpty())guard->fail(owner,index,error);
                    else {owner->available[index]=true;guard->ingest(owner,index,reply);}
                    guard->syncTopology();guard->notify();
                });
        }
        syncTopology();
    }
};
QString topologyError(const Project &project){
    if(project.nodes.isEmpty()||project.nodes.size()>8)return u("Для віддаленого сеансу потрібні 1–8 вузлів у знімку проєкту.");
    QSet<QString> ids,edges;
    for(const auto &node:project.nodes){
        if(!validId(node.id)||ids.contains(node.id)||node.name.trimmed().isEmpty()||node.name.size()>200)return u("Некоректний ID, назва або повтор вузла у знімку проєкту.");
        ids.insert(node.id);
        const auto options=validateNodeOptions(node);if(!options.isEmpty())return options;
    }
    for(const auto &link:project.links){
        const auto edge=link.tx+"/"+link.rx;
        if(!ids.contains(link.tx)||!ids.contains(link.rx)||link.tx==link.rx||edges.contains(edge)||link.delayMs<0||link.delayMs>500||
            !std::isfinite(link.gainDb)||link.gainDb< -120||link.gainDb>60)return u("Некоректний зв'язок: потрібні різні вузли, затримка 0–500 мс і підсилення −120…60 dB.");
        edges.insert(edge);
        const auto options=validateLinkOptions(link);if(!options.isEmpty())return options;
    }
    return {};
}
QJsonObject topologySnapshot(const Project &project){
    QJsonArray nodes,links;
    for(int index=0;index<project.nodes.size();++index){const auto &node=project.nodes[index];auto value=node.json();
        QJsonObject entry{{"id",node.id},{"name",node.name},{"enabled",node.enabled}};for(const auto key:{"ports","protocol"})if(value.contains(key))entry[key]=value.value(key);
        if(node.protocol.value("type")=="meshtastic"){auto protocol=entry.value("protocol").toObject();if(!protocol.contains("nodeNumber"))protocol["nodeNumber"]=0x100+index;if(!protocol.contains("relayRole"))protocol["relayRole"]=protocol.value("role").toString("router");protocol.remove("role");entry["protocol"]=protocol;}
        nodes.append(entry);
    }
    for(const auto &link:project.links)links.append(link.json());
    return {{"nodes",nodes},{"links",links}};
}
}

class RemoteDialog::Private {
public:
    RemoteDialog *dialog;
    Project project;
    QJsonObject topology;
    QTableWidget *graph=nullptr;
    std::function<void(int)> createEndpointRow;
    Controller controller;
    QTableWidget *endpoints=nullptr,*telemetry=nullptr,*results=nullptr,*portsTable=nullptr;
    QComboBox *bearer=nullptr,*source=nullptr,*destination=nullptr,*inputEncoding=nullptr,*outputEncoding=nullptr;
    QLineEdit *packetId=nullptr,*input=nullptr;
    QLineEdit *nativeDestination=nullptr;
    QComboBox *protocolNode=nullptr;
    QPlainTextEdit *protocolCommand=nullptr,*protocolResponse=nullptr;
    QLabel *nativeState=nullptr;
    QPushButton *protocolSend=nullptr;
    QSpinBox *ttl=nullptr;
    QPlainTextEdit *payload=nullptr;
    QLabel *state=nullptr,*error=nullptr,*limit=nullptr;
    QLabel *discoveryState=nullptr;
    QJsonObject lastDiscoveryStats;
    QPushButton *start=nullptr,*stop=nullptr,*send=nullptr,*importButton=nullptr,*searchButton=nullptr;
    QJsonArray displayed;
    QJsonObject experimentEndpoints;
    QPointer<DiscoveryClient> discovery;
    QMap<QString,QJsonObject> &known;
    QMap<QString,QString> &credentials;
    QSet<QString> &pairingKeys;
    QVector<QString> assigned;
    QVector<quint64> rowGeneration;
    QVector<bool> pairing;
    QVector<QPointer<QInputDialog>> prompts;
    const QString clientId=instanceRegistry().clientId;
    quint64 generation=0,scanGeneration=0;
    bool closed=false;
    QString interactionError;
    explicit Private(RemoteDialog *owner,const Project &snapshot):dialog(owner),project(snapshot),topology(topologySnapshot(snapshot)),controller(owner),
        known(instanceRegistry().known),credentials(instanceRegistry().credentials),pairingKeys(instanceRegistry().pairing),
        assigned(snapshot.nodes.size()),rowGeneration(snapshot.nodes.size(),0),pairing(snapshot.nodes.size(),false),prompts(snapshot.nodes.size()){}
    void setup(){
        auto layout=new QVBoxLayout(dialog);auto commands=new QHBoxLayout;
        bearer=new QComboBox;bearer->setObjectName("remoteBearer");
        bearer->addItem(u("Дані · TCP · до 4096 байтів"),"data");bearer->addItem(u("IQ · CF32 · параметри зв'язків"),"iq");
        bearer->setToolTip(u("Типовий носій для зв'язків inherit. AFSK1200/FM: 48 кГц; LoRa: BW × OS."));
        bearer->addItem(u("Апаратний носій сеансу · недоступно"),"hardware");
        if(auto model=qobject_cast<QStandardItemModel *>(bearer->model())){auto item=model->item(2);item->setEnabled(false);item->setToolTip(u("Радіо прив'язується до окремих RX/TX-портів із локальними дозволами, не до глобального носія сеансу."));}
        bearer->setMinimumContentsLength(20);bearer->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        commands->addWidget(bearer,1);
        start=new QPushButton(dialog->style()->standardIcon(QStyle::SP_MediaPlay),u("Приєднати"));start->setObjectName("remoteStart");
        stop=new QPushButton(dialog->style()->standardIcon(QStyle::SP_MediaStop),u("Від'єднати"));stop->setObjectName("remoteStop");
        commands->addWidget(start);commands->addWidget(stop);layout->addLayout(commands);
        auto tools=new QHBoxLayout;
        auto tool=[&](QStyle::StandardPixmap icon,const QString &tip,const QString &name){
            auto button=new QPushButton(dialog->style()->standardIcon(icon),QString(),dialog);button->setToolTip(tip);button->setAccessibleName(tip);button->setObjectName(name);button->setFixedWidth(36);tools->addWidget(button);return button;
        };
        searchButton=tool(QStyle::SP_BrowserReload,u("Знайти mesh-вузли в LAN (2 с)"),"remoteDiscover");
        discoveryState=new QLabel(u("Знайдено: 0"));discoveryState->setObjectName("remoteDiscoveryState");tools->addWidget(discoveryState);
        auto routedButton=tool(QStyle::SP_DriveNetIcon,u("Збережені адреси та каталоги вузлів для VPN"),"remoteDiscoverySettings");
        QObject::connect(routedButton,&QPushButton::clicked,dialog,[this]{
            RoutedDiscoveryDialog editor(instanceRegistry().searchConfiguration,dialog);editor.setAttribute(Qt::WA_DontShowOnScreen,dialog->testAttribute(Qt::WA_DontShowOnScreen));
            if(editor.exec()==QDialog::Accepted){QString problem;if(!dialog->setRoutedDiscoveryConfiguration(editor.configuration(),&problem)){interactionError=problem;refresh();}}
        });
        importButton=tool(QStyle::SP_DialogOpenButton,u("Імпорт адрес без токенів"),"remoteImportEndpoints");
        auto exportButton=tool(QStyle::SP_DialogSaveButton,u("Експорт адрес без токенів"),"remoteExportEndpoints");
        auto exportResults=tool(QStyle::SP_FileDialogDetailedView,u("Експорт результатів експерименту"),"remoteExportResults");
        state=new QLabel;state->setObjectName("remoteStateLabel");state->setWordWrap(true);state->setTextFormat(Qt::PlainText);state->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);tools->addWidget(state,1);layout->addLayout(tools);
        error=new QLabel;error->setObjectName("remoteErrorLabel");error->setWordWrap(true);error->setTextFormat(Qt::PlainText);error->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);error->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(error);
        auto splitter=new QSplitter(Qt::Vertical);layout->addWidget(splitter,1);auto tabs=new QTabWidget;splitter->addWidget(tabs);
        endpoints=new QTableWidget(project.nodes.size(),7);endpoints->setObjectName("remoteEndpoints");
        endpoints->setHorizontalHeaderLabels({"ID",u("Адреса"),u("Порт"),u("Токен"),u("Екземпляр"),u("Код"),u("Увімкнено")});endpoints->verticalHeader()->hide();
        endpoints->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
        endpoints->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
        endpoints->horizontalHeader()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
        endpoints->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch);
        endpoints->horizontalHeader()->setSectionResizeMode(4,QHeaderView::Stretch);
        endpoints->horizontalHeader()->setSectionResizeMode(5,QHeaderView::ResizeToContents);
        endpoints->horizontalHeader()->setSectionResizeMode(6,QHeaderView::ResizeToContents);
        endpoints->horizontalHeader()->moveSection(4,1);
        endpoints->horizontalHeader()->moveSection(6,1);
        createEndpointRow=[this](int row){
            const auto &node=project.nodes[row];auto id=cell(node.id);id->setToolTip(node.name);endpoints->setItem(row,0,id);
            auto host=new QLineEdit("127.0.0.1");host->setMaxLength(253);host->setObjectName("remoteHost_"+node.id);host->setMinimumWidth(100);endpoints->setCellWidget(row,1,host);
            host->setToolTip(u("Адреса має бути доступною і конструктору, і решті вузлів. 127.0.0.1 — лише коли всі вузли на одному ПК."));
            auto port=new QSpinBox;port->setRange(1,65535);port->setValue(17690+row);port->setMinimumWidth(80);port->setObjectName("remotePort_"+node.id);endpoints->setCellWidget(row,2,port);
            auto token=new QLineEdit;token->setEchoMode(QLineEdit::Password);token->setMaxLength(4096);token->setClearButtonEnabled(true);token->setObjectName("remoteToken_"+node.id);
            token->setInputMethodHints(Qt::ImhHiddenText|Qt::ImhSensitiveData|Qt::ImhNoPredictiveText);token->setMinimumWidth(100);endpoints->setCellWidget(row,3,token);
            auto instances=new QComboBox;instances->setObjectName("remoteInstance_"+node.id);instances->setMinimumWidth(120);instances->setMinimumContentsLength(10);
            instances->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);instances->addItem(u("Вручну"),QString());endpoints->setCellWidget(row,4,instances);
            auto pairButton=new QPushButton(dialog->style()->standardIcon(QStyle::SP_DialogApplyButton),QString());pairButton->setObjectName("remotePair_"+node.id);pairButton->setFixedWidth(36);
            pairButton->setAccessibleName(u("Зареєструвати екземпляр за локальним кодом"));pairButton->setToolTip(u("Реєстрація за локальним кодом. Токен лише в пам'яті Designer; після перезапуску служби потрібен новий код."));endpoints->setCellWidget(row,5,pairButton);
            QObject::connect(host,&QLineEdit::textChanged,dialog,[this,row]{endpointEdited(row);});
            QObject::connect(port,QOverload<int>::of(&QSpinBox::valueChanged),dialog,[this,row]{endpointEdited(row);});
            QObject::connect(token,&QLineEdit::textEdited,dialog,[this,row]{invalidateRow(row);refresh();});
            QObject::connect(instances,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[this,row,instances]{applyAssignment(row,instances->currentData().toString());});
            QObject::connect(pairButton,&QPushButton::clicked,dialog,[this,row]{promptCode(row);});
            endpoints->setRowHeight(row,38);
            auto enabled=new QCheckBox;enabled->setObjectName("remoteEnabled_"+node.id);enabled->setChecked(node.enabled);endpoints->setCellWidget(row,6,enabled);
            QObject::connect(enabled,&QCheckBox::toggled,dialog,[this,row](bool value){QString problem;if(!dialog->setNodeEnabled(project.nodes[row].id,value,&problem)){interactionError=problem;refresh();}});
        };
        for(int row=0;row<project.nodes.size();++row)createEndpointRow(row);
        tabs->addTab(endpoints,u("З'єднання"));
        telemetry=new QTableWidget(project.nodes.size(),13);telemetry->setObjectName("remoteTelemetry");
        telemetry->setHorizontalHeaderLabels({"ID",u("Стан"),u("Черга"),"TX","RX",u("Переслано"),u("Доставлено"),u("Втрати"),"IQ TX","IQ RX",u("Витіснено записів"),u("Екземпляр"),u("Служба активна")});
        telemetry->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);telemetry->horizontalHeader()->setStretchLastSection(true);
        for(int row=0;row<project.nodes.size();++row)for(int column=0;column<13;++column)telemetry->setItem(row,column,cell(column==0?project.nodes[row].id:QString()));
        tabs->addTab(telemetry,u("Телеметрія"));
        portsTable=new QTableWidget(0,8);portsTable->setObjectName("remotePorts");portsTable->setHorizontalHeaderLabels({u("Вузол"),"ID",u("Напрямок"),"Backend",u("Увімкнено"),u("Готовий"),u("Приймач"),u("Канал")});
        portsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);portsTable->horizontalHeader()->setStretchLastSection(true);tabs->addTab(portsTable,u("Порти"));
        auto protocolPage=new QWidget;auto protocolLayout=new QVBoxLayout(protocolPage);auto protocolTools=new QHBoxLayout;
        protocolNode=new QComboBox;protocolNode->setObjectName("remoteProtocolNode");for(const auto &node:project.nodes)protocolNode->addItem(node.id,node.id);protocolTools->addWidget(protocolNode);
        nativeState=new QLabel;nativeState->setObjectName("remoteNativeState");nativeState->setTextFormat(Qt::PlainText);nativeState->setWordWrap(true);nativeState->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);protocolTools->addWidget(nativeState,1);
        protocolSend=new QPushButton(dialog->style()->standardIcon(QStyle::SP_ArrowRight),u("Виконати"));protocolSend->setObjectName("remoteProtocolSend");protocolTools->addWidget(protocolSend);protocolLayout->addLayout(protocolTools);
        protocolCommand=new QPlainTextEdit;protocolCommand->setObjectName("remoteProtocolCommand");protocolCommand->setPlainText("{\n  \"op\": \"status\"\n}");protocolCommand->setMaximumBlockCount(256);protocolLayout->addWidget(protocolCommand,1);
        protocolCommand->setToolTip(u("Native-команда передається у полі args; не зберігається в експорті. Секрети налаштовує host-служба."));
        protocolResponse=new QPlainTextEdit;protocolResponse->setObjectName("remoteProtocolResponse");protocolResponse->setReadOnly(true);protocolResponse->setMaximumBlockCount(256);protocolLayout->addWidget(protocolResponse,1);tabs->addTab(protocolPage,u("Протокол"));
        graph=new QTableWidget(project.links.size(),9);graph->setObjectName("remoteTopology");
        graph->setHorizontalHeaderLabels({"TX","RX",u("Увімкнено"),u("Затримка, мс"),u("Підсилення, dB"),u("Носій"),"TX port","RX port",u("Модем")});
        graph->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);graph->horizontalHeader()->setStretchLastSection(true);
        rebuildGraph();
        tabs->addTab(graph,u("Зв'язки"));
        auto packets=new QWidget;auto packetLayout=new QVBoxLayout(packets);packetLayout->setContentsMargins(0,0,0,0);
        packetLayout->setSizeConstraint(QLayout::SetMinimumSize);
        auto packetScroll=new QScrollArea;packetScroll->setWidgetResizable(true);packetScroll->setFrameShape(QFrame::NoFrame);packetScroll->setWidget(packets);splitter->addWidget(packetScroll);
        auto form=new QGridLayout;source=new QComboBox;destination=new QComboBox;ttl=new QSpinBox;ttl->setRange(0,64);ttl->setValue(16);
        source->setObjectName("remoteSource");destination->setObjectName("remoteDestination");ttl->setObjectName("remoteTtl");
        for(const auto &node:project.nodes){source->addItem(node.id,node.id);destination->addItem(node.id,node.id);}
        destination->setCurrentIndex(qMax(0,destination->count()-1));
        for(auto combo:{source,destination}){combo->setMinimumContentsLength(8);combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);}
        form->addWidget(new QLabel(u("Від")),0,0);form->addWidget(source,0,1);form->addWidget(new QLabel(u("До")),0,2);form->addWidget(destination,0,3);form->addWidget(new QLabel("TTL"),0,4);form->addWidget(ttl,0,5);
        packetId=new QLineEdit("packet_"+uuid());packetId->setMaxLength(64);packetId->setObjectName("remotePacketId");
        form->addWidget(new QLabel("ID"),1,0);form->addWidget(packetId,1,1,1,3);
        send=new QPushButton(dialog->style()->standardIcon(QStyle::SP_ArrowRight),u("Надіслати"));send->setObjectName("remoteSend");form->addWidget(send,1,4,1,2);
        inputEncoding=new QComboBox;inputEncoding->addItems({"UTF-8","HEX"});inputEncoding->setObjectName("remoteInputEncoding");
        input=new QLineEdit("TEST");input->setMaxLength(16384);input->setObjectName("remotePayloadInput");
        form->addWidget(inputEncoding,2,0);form->addWidget(input,2,1,1,5);
        nativeDestination=new QLineEdit;nativeDestination->setObjectName("remoteNativeDestination");nativeDestination->setMaxLength(256);nativeDestination->setToolTip(u("Meshtastic: !xxxxxxxx або 0x… у hex. Reticulum: 32 hex-цифри. Не ID вузла лабораторного графа."));
        form->addWidget(new QLabel(u("Native-адреса")),3,0);form->addWidget(nativeDestination,3,1,1,5);form->setColumnStretch(1,1);form->setColumnStretch(3,1);packetLayout->addLayout(form);
        limit=new QLabel;limit->setObjectName("remotePayloadLimit");limit->setWordWrap(true);packetLayout->addWidget(limit);
        results=new QTableWidget(0,7);results->setObjectName("remoteResults");
        results->setHorizontalHeaderLabels({"ID",u("Від"),u("До"),u("Вузол"),u("Результат"),u("Локально, мс"),u("Фактичний шлях")});
        results->setSelectionBehavior(QAbstractItemView::SelectRows);results->setSelectionMode(QAbstractItemView::SingleSelection);
        results->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);results->horizontalHeader()->setStretchLastSection(true);packetLayout->addWidget(results,1);
        outputEncoding=new QComboBox;outputEncoding->addItems({"UTF-8","HEX"});outputEncoding->setObjectName("remoteOutputEncoding");packetLayout->addWidget(outputEncoding);
        payload=new QPlainTextEdit;payload->setReadOnly(true);payload->setObjectName("remotePayloadResult");payload->setMaximumHeight(85);packetLayout->addWidget(payload);
        splitter->setStretchFactor(0,1);splitter->setStretchFactor(1,2);splitter->setSizes({260,420});
        auto note=new QLabel(u("IQ між вузлами: AFSK1200/FM або LoRa/CSS. Фізичний RX — лише з локальним дозволом; фізичний TX потребує локального озброєння. Спільного RF-середовища немає."));
        note->setWordWrap(true);layout->addWidget(note);
        QObject::connect(start,&QPushButton::clicked,dialog,[this]{attach();});QObject::connect(stop,&QPushButton::clicked,dialog,[this]{controller.stop();});
        QObject::connect(send,&QPushButton::clicked,dialog,[this]{sendPacket();});QObject::connect(input,&QLineEdit::returnPressed,dialog,[this]{sendPacket();});
        QObject::connect(input,&QLineEdit::textChanged,dialog,[this]{refreshLimit();});
        QObject::connect(inputEncoding,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[this]{refreshLimit();});
        QObject::connect(bearer,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[this]{refreshLimit();});
        QObject::connect(source,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[this]{refresh();});
        QObject::connect(results,&QTableWidget::itemSelectionChanged,dialog,[this]{showPayload();});
        QObject::connect(outputEncoding,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[this]{showPayload();});
        QObject::connect(protocolNode,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[this]{protocolResponse->clear();refresh();});
        QObject::connect(protocolSend,&QPushButton::clicked,dialog,[this]{
            const auto bytes=protocolCommand->toPlainText().toUtf8();QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes,&parse);QString problem;
            if(bytes.size()>16384||parse.error!=QJsonParseError::NoError||!document.isObject())problem=u("Потрібен JSON-об'єкт команди, не більше 16 КіБ.");
            else executeProtocolCommand(protocolNode->currentIndex(),document.object(),&problem);
            interactionError=problem;refresh();
        });
        QObject::connect(importButton,&QPushButton::clicked,dialog,[this]{importEndpoints();});
        QObject::connect(searchButton,&QPushButton::clicked,dialog,[this]{QString problem;if(!searchInstances(&problem)){interactionError=problem;refresh();}});
        QObject::connect(exportButton,&QPushButton::clicked,dialog,[this]{save(false);});
        QObject::connect(exportResults,&QPushButton::clicked,dialog,[this]{save(true);});
        for(auto button:dialog->findChildren<QPushButton *>())button->setAutoDefault(false);
        const QPointer<RemoteDialog> guard(dialog);
        instanceRegistry().listeners[dialog]=[guard]{
            if(!guard||guard->d->closed)return;
            guard->d->rebuildInstances();guard->d->refresh();
        };
        controller.protocolReply=[guard](int row,bool ok,const QJsonObject &reply,const QString &error){
            if(!guard||guard->d->closed)return;
            if(row==guard->d->protocolNode->currentIndex())guard->d->protocolResponse->setPlainText(QString::fromUtf8(QJsonDocument(reply).toJson()));
            emit guard->protocolCommandFinished(row,ok,reply,error);
        };
        rebuildInstances();controller.changed=[this]{refresh();emit dialog->sessionStateChanged();};refresh();
        if(!topologyError(project).isEmpty())error->setText(topologyError(project));
    }
    void rebuildGraph(){
        graph->setRowCount(project.links.size());
        for(int row=0;row<project.links.size();++row){const auto &link=project.links[row];
            const QStringList values{link.tx,link.rx,QString(),QString::number(link.delayMs),QString::number(link.gainDb,'f',1),link.bearer,link.txPort,link.rxPort,QString::fromUtf8(QJsonDocument(link.json().value("modem").toObject()).toJson(QJsonDocument::Compact))};
            for(int column=0;column<values.size();++column)graph->setItem(row,column,cell(values[column]));
            auto enabled=new QCheckBox;enabled->setObjectName("remoteLinkEnabled_"+link.tx+"_"+link.rx);enabled->setChecked(link.enabled);graph->setCellWidget(row,2,enabled);
            const auto tx=link.tx,rx=link.rx;QObject::connect(enabled,&QCheckBox::toggled,dialog,[this,tx,rx](bool value){QString problem;if(!dialog->setLinkEnabled(tx,rx,value,&problem)){interactionError=problem;refresh();}});
        }
    }
    Endpoint endpoint(int row) const {
        Endpoint value;value.id=project.nodes[row].id;value.host=static_cast<QLineEdit *>(endpoints->cellWidget(row,1))->text().trimmed();
        value.port=static_cast<QSpinBox *>(endpoints->cellWidget(row,2))->value();value.token=static_cast<QLineEdit *>(endpoints->cellWidget(row,3))->text();
        if(known.contains(assigned[row]))value.instance=known.value(assigned[row]).value("instanceId").toString();return value;
    }
    bool replaceProject(const Project &next,QString *problem){
        const auto fail=[&](const QString &message){if(problem)*problem=message;return false;};
        if(closed||controller.state==Controller::Checking||controller.state==Controller::Configuring||controller.state==Controller::Stopping)return fail(u("Дочекайтеся завершення поточної операції."));
        const auto validation=topologyError(next);if(!validation.isEmpty())return fail(validation);
        if(next.nodes.size()<project.nodes.size())return fail(u("Під час роботи вимикайте вузли замість видалення."));
        for(int i=0;i<project.nodes.size();++i){
            if(project.nodes[i].id!=next.nodes[i].id)return fail(u("Збережіть порядок та ID існуючих вузлів."));
            if(controller.state==Controller::Attached&&(project.nodes[i].ports!=next.nodes[i].ports||project.nodes[i].protocol!=next.nodes[i].protocol))return fail(u("Порти й протокол вузла змінюються після його від'єднання."));
            if(controller.state==Controller::Attached&&project.nodes[i].enabled!=next.nodes[i].enabled&&!controller.canSetEnabled(i,next.nodes[i].enabled))return fail(u("Дочекайтеся завершення від'єднання або приєднання вузла."));
            if(controller.state==Controller::Attached&&!project.nodes[i].enabled&&next.nodes[i].enabled){
                const auto target=endpoint(i);
                if(!controller.canEditEndpoint(i)||!hostValid(target.host)||target.token.isEmpty())return fail(u("Вкажіть адресу й токен вузла та дочекайтеся завершення його запитів."));
            }
        }
        if(topologySnapshot(next)==topology){project=next;if(problem)problem->clear();return true;}
        const auto old=project;project=next;const int previous=old.nodes.size();
        for(int i=previous;i<project.nodes.size();++i)project.nodes[i].enabled=false;
        topology=topologySnapshot(project);
        assigned.resize(project.nodes.size());rowGeneration.resize(project.nodes.size());pairing.resize(project.nodes.size());prompts.resize(project.nodes.size());
        endpoints->setRowCount(project.nodes.size());telemetry->setRowCount(project.nodes.size());
        for(int row=previous;row<project.nodes.size();++row){
            createEndpointRow(row);for(int col=0;col<13;++col)telemetry->setItem(row,col,cell(col==0?project.nodes[row].id:QString()));
            source->addItem(project.nodes[row].id,project.nodes[row].id);destination->addItem(project.nodes[row].id,project.nodes[row].id);protocolNode->addItem(project.nodes[row].id,project.nodes[row].id);
            if(controller.state==Controller::Attached)controller.append(endpoint(row),topology);
        }
        if(controller.state==Controller::Attached){
            controller.updateTopology(topology);
            for(int i=0;i<previous;++i)if(old.nodes[i].enabled!=project.nodes[i].enabled){
                if(project.nodes[i].enabled)controller.setEndpoint(i,endpoint(i));
                controller.setEnabled(i,project.nodes[i].enabled);
            }
        }
        experimentEndpoints=endpointConfiguration();rebuildGraph();rebuildInstances();refresh();if(problem)problem->clear();return true;
    }
    void invalidateRow(int row){
        ++rowGeneration[row];pairing[row]=false;
        const auto prompt=prompts[row];prompts[row]=nullptr;if(prompt)prompt->reject();
    }
    void endpointEdited(int row){
        invalidateRow(row);assigned[row].clear();static_cast<QLineEdit *>(endpoints->cellWidget(row,3))->clear();
        auto combo=static_cast<QComboBox *>(endpoints->cellWidget(row,4));const QSignalBlocker block(combo);combo->setCurrentIndex(0);
        interactionError.clear();refresh();
    }
    bool rowMatches(int row,const QJsonObject &entry) const {
        return static_cast<QLineEdit *>(endpoints->cellWidget(row,1))->text().trimmed()==entry.value("host").toString()&&
            static_cast<QSpinBox *>(endpoints->cellWidget(row,2))->value()==entry.value("port").toInt();
    }
    void rebuildInstances(){
        for(int row=0;row<project.nodes.size();++row){
            if(!assigned[row].isEmpty()&&!known.contains(assigned[row])){
                invalidateRow(row);assigned[row].clear();static_cast<QLineEdit *>(endpoints->cellWidget(row,3))->clear();
            }
            auto combo=static_cast<QComboBox *>(endpoints->cellWidget(row,4));const QSignalBlocker block(combo);
            combo->clear();combo->addItem(u("Вручну"),QString());
            for(auto it=known.cbegin();it!=known.cend();++it){
                const auto entry=it.value();QString name=entry.value("name").toString();if(name.isEmpty())name=entry.value("host").toString();
                const auto label=name+" · "+entry.value("host").toString()+":"+QString::number(entry.value("port").toInt())+
                    (entry.value("active").toBool()?u(" · зайнятий"):u(" · вільний"))+(credentials.contains(it.key())?u(" · зареєстровано"):QString());
                combo->addItem(label,it.key());combo->setItemData(combo->count()-1,label+"\ninstanceId: "+entry.value("instanceId").toString(),Qt::ToolTipRole);
                const auto seen=qint64(entry.value("lastSeenUtcMs").toDouble());QStringList methods;for(const auto &value:entry.value("discoveryMethods").toArray())methods<<value.toString();
                combo->setItemData(combo->count()-1,label+"\ninstanceId: "+entry.value("instanceId").toString()+"\n"+
                    (seen?u("Остання відповідь: ")+QDateTime::fromMSecsSinceEpoch(seen).toString("yyyy-MM-dd HH:mm:ss"):u("Ще не підтверджено пошуком"))+"\n"+methods.join(" / "),Qt::ToolTipRole);
            }
            combo->setCurrentIndex(qMax(0,combo->findData(assigned[row])));
            combo->setToolTip(combo->currentIndex()>0?combo->itemData(combo->currentIndex(),Qt::ToolTipRole).toString():u("Ручна адреса та токен"));
        }
    }
    bool rememberInstance(const QJsonObject &object){
        const auto entry=discoveryEntry(object);if(entry.isEmpty())return false;
        const auto key=discoveryKey(entry);QStringList replaced;
        for(auto it=known.cbegin();it!=known.cend();++it)if(it.key()!=key&&it.value().value("host")==entry.value("host")&&it.value().value("port")==entry.value("port"))replaced<<it.key();
        for(const auto &old:replaced){known.remove(old);credentials.remove(old);}
        if(known.size()>=128&&!known.contains(key))return false;
        known[key]=entry;registryChanged();return true;
    }
    void applyAssignment(int row,const QString &key){
        if(closed||!controller.canEditEndpoint(row))return;
        invalidateRow(row);assigned[row].clear();auto token=static_cast<QLineEdit *>(endpoints->cellWidget(row,3));token->clear();
        if(known.contains(key)){
            const auto entry=known.value(key);auto host=static_cast<QLineEdit *>(endpoints->cellWidget(row,1));auto port=static_cast<QSpinBox *>(endpoints->cellWidget(row,2));
            const QSignalBlocker hostBlock(host),portBlock(port);host->setText(entry.value("host").toString());port->setValue(entry.value("port").toInt());
            assigned[row]=key;token->setText(credentials.value(key));
        }
        interactionError.clear();rebuildInstances();refresh();
    }
    bool assignDiscoveredInstance(int row,const QJsonObject &object,QString *outError){
        auto fail=[&](const QString &text){if(outError)*outError=text;return false;};
        if(closed||!controller.canEditEndpoint(row))return fail(u("Спочатку вимкніть цей вузол і дочекайтеся завершення його запитів."));
        if(row<0||row>=project.nodes.size())return fail(u("Невідомий рядок вузла."));
        const auto entry=discoveryEntry(object);if(entry.isEmpty())return fail(u("Некоректний знайдений екземпляр."));
        if(!rememberInstance(entry))return fail(u("Досягнуто межу відомих екземплярів (128)."));
        applyAssignment(row,discoveryKey(entry));if(outError)outError->clear();return true;
    }
    bool registerDiscoveredInstance(int row,const QJsonObject &object,const QString &code,QString *outError){
        auto fail=[&](const QString &text){if(outError)*outError=text;return false;};
        const auto entry=discoveryEntry(object);if(entry.isEmpty())return fail(u("Некоректний знайдений екземпляр."));
        if(code.size()!=6||!QRegularExpression("^[0-9]{6}$").match(code).hasMatch())return fail(u("Потрібен локальний код із шести цифр."));
        const auto key=discoveryKey(entry);if(pairingKeys.contains(key))return fail(u("Реєстрація цього екземпляра вже очікує відповіді."));
        if(!assignDiscoveredInstance(row,entry,outError))return false;
        // Re-registering rotates the service credential; a cancelled reply must not leave a stale cache.
        const auto previousCredential=credentials.take(key);
        for(int other=0;other<assigned.size();++other)if(assigned[other]==key){
            auto field=static_cast<QLineEdit *>(endpoints->cellWidget(other,3));if(field->text()==previousCredential)field->clear();
        }
        pairing[row]=true;pairingKeys.insert(key);interactionError.clear();
        const auto serial=generation,revision=rowGeneration[row];const QPointer<RemoteDialog> guard(dialog);
        Endpoint endpoint;endpoint.host=entry.value("host").toString();endpoint.port=entry.value("port").toInt();
        JsonRpc::call(endpoint,{{"op","pair"},{"instanceId",entry.value("instanceId")},{"clientId",clientId},{"clientName","Obrii Mesh Designer"},{"code",code}},
            [guard,entry,key,serial,revision,row](const QJsonObject &reply,const QString &rpcError){
                instanceRegistry().pairing.remove(key);
                if(!guard||guard->d->closed||guard->d->generation!=serial){registryChanged();return;}
                auto &self=*guard->d;
                if(self.rowGeneration[row]!=revision||self.assigned[row]!=key||!self.rowMatches(row,entry)){
                    registryChanged();if(guard)emit guard->instanceRegistrationFinished(row,false,u("Реєстрацію скасовано після зміни адреси або вибору."));return;
                }
                self.pairing[row]=false;
                const auto token=reply.value("token").toString();
                const bool valid=rpcError.isEmpty()&&reply.value("protocol")=="obrii-mesh-node"&&integer(reply.value("version"),1,1)&&
                    reply.value("instanceId")==entry.value("instanceId")&&!QUuid(reply.value("registrationId").toString()).isNull()&&
                    reply.value("token").isString()&&!token.isEmpty()&&token.size()<=4096&&!token.contains(QRegularExpression("[\\x00-\\x20\\x7f]"));
                if(valid){
                    const auto previous=self.credentials.value(key);self.credentials[key]=token;
                    for(int other=0;other<self.assigned.size();++other)if(self.assigned[other]==key&&self.rowMatches(other,entry)){
                        auto field=static_cast<QLineEdit *>(self.endpoints->cellWidget(other,3));
                        if(other==row||field->text().isEmpty()||field->text()==previous)field->setText(token);
                    }
                    self.interactionError.clear();
                }else self.interactionError=u("Реєстрацію не підтверджено. Перевірте локальний код, екземпляр і доступність служби.");
                const auto error=self.interactionError;registryChanged();if(guard)emit guard->instanceRegistrationFinished(row,valid,error);
            });
        registryChanged();if(outError)outError->clear();return true;
    }
    void promptCode(int row){
        if(closed||!controller.canEditEndpoint(row)||!known.contains(assigned[row])||prompts[row]||pairingKeys.contains(assigned[row]))return;
        const auto entry=known.value(assigned[row]);const auto revision=rowGeneration[row],serial=generation;
        auto prompt=new QInputDialog(dialog);prompts[row]=prompt;prompt->setObjectName("remotePairingCode");prompt->setAttribute(Qt::WA_DeleteOnClose);
        prompt->setAttribute(Qt::WA_DontShowOnScreen,dialog->testAttribute(Qt::WA_DontShowOnScreen));prompt->setWindowTitle(u("Реєстрація mesh-екземпляра"));
        prompt->setInputMode(QInputDialog::TextInput);prompt->setTextEchoMode(QLineEdit::Password);
        prompt->setLabelText(u("Локальний код із шести цифр для %1:%2").arg(entry.value("host").toString()).arg(entry.value("port").toInt()));
        prompt->setOkButtonText(u("Зареєструвати"));prompt->setCancelButtonText(u("Скасувати"));
        if(auto field=prompt->findChild<QLineEdit *>()){
            field->setMaxLength(6);field->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,6}"),field));
            field->setInputMethodHints(Qt::ImhHiddenText|Qt::ImhSensitiveData|Qt::ImhNoPredictiveText);
        }
        const QPointer<RemoteDialog> guard(dialog);
        QObject::connect(prompt,&QDialog::accepted,dialog,[guard,prompt,row,entry,revision,serial]{
            if(!guard||guard->d->closed||guard->d->generation!=serial||guard->d->rowGeneration[row]!=revision)return;
            auto &self=*guard->d;self.prompts[row]=nullptr;const auto code=prompt->textValue();prompt->setTextValue(QString());QString problem;
            if(!self.registerDiscoveredInstance(row,entry,code,&problem)){self.interactionError=problem;self.refresh();}
        });
        QObject::connect(prompt,&QDialog::rejected,dialog,[guard,prompt,row]{
            if(!guard||guard->d->closed)return;if(guard->d->prompts[row]==prompt)guard->d->prompts[row]=nullptr;guard->d->refresh();
        });
        refresh();static_cast<QDialog *>(prompt)->open();
    }
    bool searchInstances(QString *outError){
        auto fail=[&](const QString &text){if(outError)*outError=text;return false;};
        if(closed)return fail(u("Діалог уже закрито."));
        if(discovery&&discovery->searching())return fail(u("Пошук уже триває."));
        auto client=new DiscoveryClient(dialog);discovery=client;const auto serial=++scanGeneration;const QPointer<RemoteDialog> guard(dialog);
        QString configurationError;if(!client->setEndpoints(instanceRegistry().searchConfiguration.value("endpoints").toArray(),&configurationError)){discovery=nullptr;client->deleteLater();return fail(configurationError);}
        QObject::connect(client,&DiscoveryClient::foundInstance,dialog,[guard,serial](const QJsonObject &entry){
            if(guard&&!guard->d->closed&&guard->d->scanGeneration==serial)guard->d->rememberInstance(entry);
        });
        QObject::connect(client,&DiscoveryClient::finished,dialog,[guard,serial,client]{
            if(!guard||guard->d->closed||guard->d->scanGeneration!=serial)return;
            for(const auto &entry:client->instances())guard->d->rememberInstance(entry);
            guard->d->lastDiscoveryStats=client->statistics();
            guard->d->discovery=nullptr;client->deleteLater();guard->d->refresh();emit guard->discoveryFinished();
        });
        QString problem;if(!client->start(&problem)){discovery=nullptr;client->deleteLater();return fail(problem);}
        for(int row=0;row<endpoints->rowCount();++row){
            QHostAddress address;if(address.setAddress(static_cast<QLineEdit *>(endpoints->cellWidget(row,1))->text().trimmed())&&address.protocol()==QAbstractSocket::IPv4Protocol)client->probeEndpoint(address.toString(),quint16(static_cast<QSpinBox *>(endpoints->cellWidget(row,2))->value()));
        }
        interactionError.clear();refresh();if(outError)outError->clear();return true;
    }
    void shutdown(){
        if(closed)return;closed=true;++generation;++scanGeneration;instanceRegistry().listeners.remove(dialog);
        if(discovery){discovery->disconnect(dialog);discovery->deleteLater();discovery=nullptr;}
        for(int row=0;row<project.nodes.size();++row)invalidateRow(row);
    }
    QJsonObject endpointConfiguration() const {
        QJsonArray nodes;for(int row=0;row<endpoints->rowCount();++row)nodes.append(QJsonObject{{"id",project.nodes[row].id},
            {"host",static_cast<QLineEdit *>(endpoints->cellWidget(row,1))->text().trimmed()},
            {"port",static_cast<QSpinBox *>(endpoints->cellWidget(row,2))->value()}});
        return {{"format","obrii-mesh-endpoints"},{"version",1},{"transport",bearer->currentData().toString()},{"nodes",nodes}};
    }
    bool setEndpointConfiguration(const QJsonObject &object,QString *outError){
        auto fail=[&](const QString &text){if(outError)*outError=text;return false;};
        if(closed||controller.state!=Controller::Idle)return fail(u("Спочатку від'єднайте поточний сеанс."));
        if(object.value("format")!="obrii-mesh-endpoints"||!integer(object.value("version"),1,1)||!object.value("nodes").isArray())return fail(u("Невідомий формат адрес."));
        const auto transport=object.value("transport").toString();if(transport!="data"&&transport!="iq")return fail(u("Невідомий транспорт."));
        const auto nodes=object.value("nodes").toArray();if(nodes.size()!=project.nodes.size())return fail(u("Адреси мають точно відповідати вузлам знімка проєкту."));
        QMap<QString,QJsonObject> byId;QSet<QString> addresses;
        for(const auto &value:nodes){
            const auto node=value.toObject();const auto id=node.value("id").toString(),host=node.value("host").toString().trimmed();
            const auto address=host.toLower()+"/"+QString::number(node.value("port").toInt());
            if(!validId(id)||byId.contains(id)||!hostValid(host)||!integer(node.value("port"),1,65535)||addresses.contains(address))return fail(u("Некоректні або повторні ID / адреси / порти."));
            addresses.insert(address);byId.insert(id,node);
        }
        for(const auto &node:project.nodes)if(!byId.contains(node.id))return fail(u("У файлі відсутній вузол: ")+node.id);
        for(int row=0;row<project.nodes.size();++row){
            endpointEdited(row);const auto node=byId.value(project.nodes[row].id);static_cast<QLineEdit *>(endpoints->cellWidget(row,1))->setText(node.value("host").toString().trimmed());
            static_cast<QSpinBox *>(endpoints->cellWidget(row,2))->setValue(node.value("port").toInt());
            static_cast<QLineEdit *>(endpoints->cellWidget(row,3))->clear();
        }
        bearer->setCurrentIndex(bearer->findData(transport));if(outError)outError->clear();return true;
    }
    QJsonObject experimentResults() const {
        QJsonArray nodes;for(int row=0;row<project.nodes.size();++row){
            auto node=row<controller.telemetry.size()?controller.telemetry[row]:QJsonObject();node["nodeId"]=project.nodes[row].id;
            node["controllerState"]=row<controller.nodeStates.size()?controller.nodeStates[row]:u("Не приєднано");nodes.append(node);
        }
        return {{"format","obrii-mesh-remote-results"},{"version",1},{"exportedUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"endpoints",experimentEndpoints.isEmpty()?endpointConfiguration():experimentEndpoints},{"topology",topology},{"nodes",nodes},{"results",controller.results},{"errors",controller.events},
            {"controllerResultLimit",MaximumResults},{"rfEnabled",false}};
    }
    bool executeProtocolCommand(int row,const QJsonObject &args,QString *outError){
        auto fail=[&](const QString &message){if(outError)*outError=message;return false;};
        if(closed||row<0||row>=project.nodes.size()||!controller.canSend(row))return fail(u("Вузол не приєднано або попередній запит ще виконується."));
        const auto type=project.nodes[row].protocol.value("type").toString("lab");if(type=="lab")return fail(u("Для лабораторного режиму доступна панель пакетів."));
        if(QJsonDocument(args).toJson(QJsonDocument::Compact).size()>16384)return fail(u("Native-команда перевищує 16 КіБ."));
        interactionError.clear();controller.protocol(row,args);if(outError)outError->clear();return true;
    }
    int payloadMaximum() const {
        int maximum=4096;
        if(project.links.isEmpty()&&bearer->currentData()=="iq")maximum=240;
        for(const auto &link:project.links)if(link.enabled&&(link.bearer=="iq"||(link.bearer=="inherit"&&bearer->currentData()=="iq")))maximum=240;
        const int row=source->currentIndex();if(row>=0&&row<controller.telemetry.size()){
            const auto value=controller.telemetry[row].value("nativeProtocol").toObject().value("maxPayload");if(integer(value,1,4096))maximum=qMin(maximum,value.toInt());
        }
        return maximum;
    }
    void attach(){
        if(closed)return;
        if(!pairingKeys.isEmpty()){interactionError=u("Дочекайтеся завершення реєстрації.");refresh();return;}
        QString problem=topologyError(project);QVector<Endpoint> values;QSet<QString> addresses;
        if(bearer->currentData()!="data"&&bearer->currentData()!="iq")problem=u("Фізичне радіо недоступне.");
        for(int row=0;row<project.nodes.size()&&problem.isEmpty();++row){
            Endpoint endpoint;endpoint.id=project.nodes[row].id;endpoint.host=static_cast<QLineEdit *>(endpoints->cellWidget(row,1))->text().trimmed();
            endpoint.port=static_cast<QSpinBox *>(endpoints->cellWidget(row,2))->value();endpoint.token=static_cast<QLineEdit *>(endpoints->cellWidget(row,3))->text();
            if(known.contains(assigned[row])){
                const auto entry=known.value(assigned[row]);endpoint.instance=entry.value("instanceId").toString();
                if(project.nodes[row].enabled&&!entry.value("availableBearers").toArray().contains(bearer->currentData().toString()))problem=u("Знайдений екземпляр не підтримує вибраний транспорт: ")+endpoint.id;
            }
            const auto address=endpoint.host.toLower()+"/"+QString::number(endpoint.port);
            if(project.nodes[row].enabled&&(!hostValid(endpoint.host)||endpoint.token.isEmpty()||addresses.contains(address)))problem=u("Перевірте адресу й токен вузла; кожна адреса з портом має бути окремою: ")+endpoint.id;
            if(project.nodes[row].enabled)addresses.insert(address);values.append(endpoint);
        }
        if(!problem.isEmpty()){error->setText(problem);return;}
        if(controller.state!=Controller::Idle)return;
        interactionError.clear();experimentEndpoints=endpointConfiguration();controller.begin(values,topology,bearer->currentData().toString());
    }
    QByteArray inputBytes(QString *problem=nullptr) const {
        if(inputEncoding->currentIndex()==0)return input->text().toUtf8();
        QString hex=input->text();hex.remove(QRegularExpression("\\s"));
        if(hex.size()%2||hex.contains(QRegularExpression("[^0-9A-Fa-f]"))){if(problem)*problem=u("HEX потребує парної кількості шістнадцяткових цифр.");return {};}
        return QByteArray::fromHex(hex.toLatin1());
    }
    void refreshLimit(){
        QString problem;const auto bytes=inputBytes(&problem);const int maximum=payloadMaximum();
        limit->setText(problem.isEmpty()?u("Корисні дані: %1 / %2 байтів").arg(bytes.size()).arg(maximum):problem);
        limit->setToolTip(u("Непорожні дані; ліміт рахується в байтах, не символах."));
    }
    void sendPacket(){
        if(!controller.canSend(source->currentIndex()))return;
        const bool native=project.nodes.value(source->currentIndex()).protocol.value("type").toString("lab")!="lab";
        QString problem;const auto bytes=inputBytes(&problem);const int maximum=payloadMaximum();
        if(problem.isEmpty()&&!native&&!QRegularExpression("^[A-Za-z][A-Za-z0-9_.-]{0,63}$").match(packetId->text()).hasMatch())problem=u("ID: літера, далі до 63 літер, цифр або символів _ . -.");
        if(problem.isEmpty()&&(bytes.isEmpty()||bytes.size()>maximum))problem=u("Корисні дані мають містити 1–%1 байтів.").arg(maximum);
        if(!problem.isEmpty()){error->setText(problem);return;}
        if(native&&(nativeDestination->text().trimmed().isEmpty()||nativeDestination->text().contains(QRegularExpression("[\\x00-\\x1f]")))){interactionError=u("Вкажіть нативну адресу призначення.");refresh();return;}
        if(native){
            const auto type=project.nodes[source->currentIndex()].protocol.value("type").toString();const auto address=nativeDestination->text().trimmed();
            if((type=="meshtastic"&&!QRegularExpression("^(?:![0-9A-Fa-f]{1,8}|0[xX][0-9A-Fa-f]{1,8})$").match(address).hasMatch())||
                (type=="reticulum"&&!QRegularExpression("^[0-9A-Fa-f]{32}$").match(address).hasMatch())){interactionError=u("Перевірте формат native-адреси: Meshtastic !xxxxxxxx / 0x…, Reticulum 32 hex-цифри.");refresh();return;}
        }
        controller.send(source->currentIndex(),destination->currentData().toString(),packetId->text(),ttl->value(),bytes,native?nativeDestination->text().trimmed():QString());
        packetId->setText("packet_"+uuid());
    }
    void refresh(){
        const bool idle=controller.state==Controller::Idle;
        for(int row=0;row<project.links.size();++row)if(auto checkbox=qobject_cast<QCheckBox *>(graph->cellWidget(row,2))){const QSignalBlocker block(checkbox);checkbox->setChecked(project.links[row].enabled);checkbox->setEnabled(!closed&&(idle||controller.state==Controller::Attached));}
        bool prompting=false;for(const auto &prompt:prompts)prompting=prompting||!prompt.isNull();
        endpoints->setEnabled(!closed&&(idle||controller.state==Controller::Attached));bearer->setEnabled(idle&&!closed);importButton->setEnabled(idle&&!closed);
        start->setEnabled(idle&&!closed&&pairingKeys.isEmpty()&&!prompting&&topologyError(project).isEmpty());
        const bool searching=discovery&&discovery->searching();searchButton->setEnabled(!closed&&!searching);
        discoveryState->setText(searching?u("Пошук..."):u("Знайдено: %1").arg(known.size()));
        discoveryState->setToolTip(u("Відповіді UDP: %1 · TCP: %2 · каталогів: %3 · невдалі запити: %4").arg(lastDiscoveryStats.value("udpReplies").toInt()).arg(lastDiscoveryStats.value("tcpReplies").toInt()).arg(lastDiscoveryStats.value("directoryReplies").toInt()).arg(lastDiscoveryStats.value("failedProbes").toInt()));
        if(auto button=dialog->findChild<QPushButton *>("remoteDiscoverySettings"))button->setEnabled(!closed&&!searching);
        for(int row=0;row<project.nodes.size();++row){
            const bool editable=!closed&&controller.canEditEndpoint(row);
            for(int column=1;column<5;++column)endpoints->cellWidget(row,column)->setEnabled(editable);
            auto checkbox=static_cast<QCheckBox *>(endpoints->cellWidget(row,6));const QSignalBlocker block(checkbox);checkbox->setChecked(project.nodes[row].enabled);checkbox->setEnabled(!closed&&(idle||controller.state==Controller::Attached));
            auto button=static_cast<QPushButton *>(endpoints->cellWidget(row,5));
            button->setEnabled(editable&&known.contains(assigned[row])&&!pairingKeys.contains(assigned[row])&&!prompts[row]);
            button->setIcon(dialog->style()->standardIcon(pairing[row]?QStyle::SP_BrowserReload:QStyle::SP_DialogApplyButton));
        }
        const int sourceRow=source->currentIndex();const bool nativeSource=sourceRow>=0&&sourceRow<project.nodes.size()&&project.nodes[sourceRow].protocol.value("type").toString("lab")!="lab";
        const auto sourceStatus=sourceRow>=0&&sourceRow<controller.telemetry.size()?controller.telemetry[sourceRow].value("nativeProtocol").toObject():QJsonObject();
        nativeDestination->setEnabled(nativeSource);destination->setEnabled(!nativeSource);ttl->setEnabled(!nativeSource);packetId->setEnabled(!nativeSource);
        stop->setEnabled(!idle&&controller.state!=Controller::Stopping);send->setEnabled(controller.canSend(sourceRow)&&(!nativeSource||sourceStatus.value("ready").toBool()));
        const int protocolRow=protocolNode->currentIndex();const auto native=protocolRow>=0&&protocolRow<controller.telemetry.size()?controller.telemetry[protocolRow].value("nativeProtocol").toObject():QJsonObject();
        const auto configured=protocolRow>=0&&protocolRow<project.nodes.size()?project.nodes[protocolRow].protocol.value("type").toString("lab"):QString("lab");
        protocolSend->setEnabled(configured!="lab"&&controller.canSend(protocolRow));
        nativeState->setText(configured=="lab"?u("Лабораторний режим"):native.isEmpty()?u("Очікування native-статусу: ")+configured:
            native.value("type").toString(configured)+" · "+(native.value("ready").toBool()?u("Готовий"):u("Backend не готовий / недоступний"))+" · "+(native.value("nativeAddress").isString()?native.value("nativeAddress").toString():counter(native.value("nativeAddress"))));
        static const char *states[]{"idle","checking","configuring","attached","stopping"};dialog->setProperty("remoteState",states[int(controller.state)]);
        state->setText(controller.message);error->setText(interactionError.isEmpty()?controller.lastError:interactionError);refreshLimit();
        for(int row=0;row<project.nodes.size();++row){
            const auto node=row<controller.telemetry.size()?controller.telemetry[row]:QJsonObject();
            telemetry->item(row,1)->setText(row<controller.nodeStates.size()?controller.nodeStates[row]:u("Не приєднано"));
            const char *keys[]{"queue","tx","rx","forwarded","delivered","dropped","iqTxSamples","iqRxSamples","resultsEvicted"};
            for(int index=0;index<9;++index)telemetry->item(row,index+2)->setText(counter(node.value(keys[index])));
            telemetry->item(row,11)->setText(node.value("instanceId").toString());telemetry->item(row,12)->setText(node.value("active").toBool()?u("Так"):u("Ні"));
            telemetry->item(row,1)->setToolTip(telemetry->item(row,1)->text());
        }
        portsTable->setRowCount(0);
        for(int node=0;node<controller.telemetry.size();++node)for(const auto &value:controller.telemetry[node].value("ports").toArray()){
            const auto port=value.toObject();const int row=portsTable->rowCount();portsTable->insertRow(row);
            const QStringList fields{project.nodes[node].id,port.value("id").toString(),port.value("direction").toString(),port.value("backend").toString(),port.value("enabled").toBool()?u("Так"):u("Ні"),port.value("ready").toBool()?u("Так"):u("Ні"),port.value("receiverId").toString(),port.value("channelId").toString()};
            for(int column=0;column<fields.size();++column)portsTable->setItem(row,column,cell(fields[column]));
        }
        if(displayed!=controller.results){
            const QSignalBlocker block(results);
            QString selected;if(results->currentRow()>=0)selected=QString::fromUtf8(QJsonDocument(results->item(results->currentRow(),0)->data(Qt::UserRole).toJsonObject()).toJson(QJsonDocument::Compact));
            displayed=controller.results;results->setRowCount(displayed.size());int selection=-1;
            for(int row=0;row<displayed.size();++row){const auto result=displayed[row].toObject();
                    const QStringList fields{scalarText(result.contains("id")?result.value("id"):result.value("nativeId")),scalarText(result.contains("source")?result.value("source"):result.value("from")),scalarText(result.contains("destination")?result.value("destination"):result.value("to")),result.value("node").toString(),
                    resultName(result.value("status").toString()),counter(result.value("localMs")),pathText(result.value("path").toArray())};
                for(int column=0;column<fields.size();++column)results->setItem(row,column,cell(fields[column]));
                results->item(row,0)->setData(Qt::UserRole,result);
                if(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))==selected)selection=row;
            }
            if(selection>=0)results->selectRow(selection);else if(!displayed.isEmpty())results->selectRow(displayed.size()-1);showPayload();
        }
    }
    void showPayload(){
        const int row=results->currentRow();if(row<0||!results->item(row,0)){payload->clear();return;}
        const auto result=results->item(row,0)->data(Qt::UserRole).toJsonObject();const auto bytes=QByteArray::fromBase64(result.value("payloadBase64").toString(result.value("wireBase64").toString()).toLatin1());
        QString text=outputEncoding->currentIndex()==0?QString::fromUtf8(bytes):QString::fromLatin1(bytes.toHex(' '));
        if(bytes.isEmpty()&&outputEncoding->currentIndex()==0)text=result.value("text").toString(result.value("payloadUtf8").toString());
        if(result.contains("event"))text=QString::fromUtf8(QJsonDocument(result).toJson())+"\n"+text;
        if(result.contains("error"))text=result.value("error").toString()+"\n"+text;payload->setPlainText(text);
    }
    void importEndpoints(){
        const auto path=QFileDialog::getOpenFileName(dialog,u("Імпорт адрес без токенів"),{},"JSON (*.json)");if(path.isEmpty())return;
        QFile file(path);if(!file.open(QIODevice::ReadOnly)){error->setText(u("Не вдалося відкрити файл адрес."));return;}
        if(file.size()>MaximumReply){error->setText(u("Файл адрес перевищує 128 КіБ."));return;}
        QJsonParseError parse;const auto object=QJsonDocument::fromJson(file.readAll(),&parse);QString problem;
        if(parse.error!=QJsonParseError::NoError||!object.isObject())problem=u("Некоректний JSON адрес.");
        else dialog->setEndpointConfiguration(object.object(),&problem);
        error->setText(problem);
    }
    void save(bool experiment){
        const auto path=QFileDialog::getSaveFileName(dialog,experiment?u("Результати експерименту"):u("Адреси без токенів"),experiment?"remote-results.json":"remote-endpoints.json","JSON (*.json)");
        if(path.isEmpty())return;QSaveFile file(path);const auto bytes=QJsonDocument(experiment?experimentResults():endpointConfiguration()).toJson();
        if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())error->setText(u("Не вдалося зберегти JSON."));
    }
};
RemoteDialog::RemoteDialog(const Project &project,QWidget *parent):QDialog(parent),d(std::make_unique<Private>(this,project)){
    setObjectName("remoteMesh");setWindowTitle(u("Реальні екземпляри · віддалена mesh-мережа"));setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);setAttribute(Qt::WA_DontShowOnScreen,parent&&parent->testAttribute(Qt::WA_DontShowOnScreen));
    resize(1120,800);setMinimumSize(660,620);d->setup();
}
RemoteDialog::~RemoteDialog(){d->controller.changed={};d->controller.protocolReply={};d->shutdown();d->controller.stop();}
QJsonObject RemoteDialog::endpointConfiguration() const {return d->endpointConfiguration();}
bool RemoteDialog::setEndpointConfiguration(const QJsonObject &object,QString *error){return d->setEndpointConfiguration(object,error);}
QJsonObject RemoteDialog::experimentResults() const {return d->experimentResults();}
bool RemoteDialog::searchInstances(QString *error){return d->searchInstances(error);}
QJsonObject RemoteDialog::routedDiscoveryConfiguration() const {return instanceRegistry().searchConfiguration;}
bool RemoteDialog::setRoutedDiscoveryConfiguration(const QJsonObject &configuration,QString *error){
    if(d->closed||(d->discovery&&d->discovery->searching())){if(error)*error=u("Дочекайтеся завершення пошуку.");return false;}
    if(!routedDiscoveryConfigurationValid(configuration,error))return false;
    QSettings settings(QSettings::defaultFormat(),QSettings::UserScope,"Obrii","ObriiMeshDesigner");
    settings.setValue("meshRemote/routedDiscovery",QJsonDocument(configuration).toJson(QJsonDocument::Compact));settings.sync();
    if(settings.status()!=QSettings::NoError){if(error)*error=u("Не вдалося зберегти адреси пошуку.");return false;}
    instanceRegistry().searchConfiguration=configuration;registryChanged();if(error)error->clear();return true;
}
QList<QJsonObject> RemoteDialog::discoveredInstances() const {return d->known.values();}
bool RemoteDialog::assignDiscoveredInstance(int row,const QJsonObject &entry,QString *error){return d->assignDiscoveredInstance(row,entry,error);}
bool RemoteDialog::registerDiscoveredInstance(int row,const QJsonObject &entry,const QString &code,QString *error){return d->registerDiscoveredInstance(row,entry,code,error);}
bool RemoteDialog::executeProtocolCommand(int row,const QJsonObject &args,QString *error){return d->executeProtocolCommand(row,args,error);}
bool RemoteDialog::sessionActive() const {return d->controller.state!=Controller::Idle;}
void RemoteDialog::stopSession(){d->controller.stop();}
bool RemoteDialog::canEditTopology() const {return d->controller.state==Controller::Idle||d->controller.state==Controller::Attached;}
QJsonObject RemoteDialog::nodeState(const QString &id) const {for(int i=0;i<d->project.nodes.size();++i)if(d->project.nodes[i].id==id){auto value=d->controller.telemetry.value(i);value["available"]=d->controller.nodeAvailable(i);value["controllerState"]=d->controller.nodeStates.value(i);return value;}return {};}
Project RemoteDialog::currentProject() const {return d->project;}
bool RemoteDialog::updateProject(const Project &project,QString *error){const auto before=d->topology;if(!d->replaceProject(project,error))return false;if(before!=d->topology)emit projectChanged();return true;}
bool RemoteDialog::setNodeEnabled(const QString &id,bool enabled,QString *error){
    for(int row=0;row<d->project.nodes.size();++row)if(d->project.nodes[row].id==id){
        if(d->controller.state==Controller::Attached){
            if(enabled){const auto endpoint=d->endpoint(row);if(!hostValid(endpoint.host)||endpoint.token.isEmpty()){if(error)*error=u("Вкажіть адресу й токен цього вузла.");return false;}
                if(!d->controller.setEndpoint(row,endpoint)&&!d->controller.nodeEnabled(row)){if(error)*error=u("Дочекайтеся завершення запитів цього вузла.");return false;}}
            if(!d->controller.setEnabled(row,enabled)){if(error)*error=u("Дочекайтеся завершення звільнення або приєднання вузла.");return false;}
        }else if(d->controller.state!=Controller::Idle){if(error)*error=u("Дочекайтеся завершення поточної операції.");return false;}
        d->project.nodes[row].enabled=enabled;d->topology=topologySnapshot(d->project);d->experimentEndpoints=d->endpointConfiguration();
        d->interactionError.clear();d->refresh();emit projectChanged();if(error)error->clear();return true;
    }if(error)*error=u("Невідомий вузол.");return false;
}
bool RemoteDialog::setLinkEnabled(const QString &tx,const QString &rx,bool enabled,QString *error){
    if(d->controller.state!=Controller::Idle&&d->controller.state!=Controller::Attached){if(error)*error=u("Дочекайтеся завершення поточної операції.");return false;}
    for(auto &link:d->project.links)if(link.tx==tx&&link.rx==rx){link.enabled=enabled;d->topology=topologySnapshot(d->project);
        if(d->controller.state==Controller::Attached)d->controller.updateTopology(d->topology);
        d->interactionError.clear();d->refresh();emit projectChanged();if(error)error->clear();return true;}
    if(error)*error=u("Невідомий зв'язок.");return false;
}
void RemoteDialog::closeEvent(QCloseEvent *event){d->shutdown();d->controller.stop();QDialog::closeEvent(event);}
void RemoteDialog::done(int result){d->shutdown();d->controller.stop();QDialog::done(result);}
}
