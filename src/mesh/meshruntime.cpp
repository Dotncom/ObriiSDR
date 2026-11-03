#include "meshruntime.h"
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQueue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <QSet>
#include <QDir>
#include <QFile>
#include <cmath>

namespace Mesh {
struct Runtime::Entry {
    Node node;
    QString cli,instanceId,phase="Starting",detail;
    QJsonObject applied;
    QJsonObject trace;
    QString traceSequence,previewError;
    quint64 traceRequests=0;
    bool runningPending=false;
    bool runningCommandDone=false;
    bool failed=false,txStopping=false;
    bool registered=true,registrationPending=false,registrationQuery=false;
    quint64 epoch=0;
    qint64 startedMs=0;
    QProcess *rx=nullptr,*tx=nullptr;
    QTcpSocket *socket=nullptr;
    quint16 controlPort=0,rxPort=0,viewPort=0;
    bool ready=false;
    QQueue<QJsonObject> requests;
};
namespace {
bool fail(QString *error,const QString &text) { if(error)*error=text;return false; }
QString numeric(double v) { return QString::number(v,'f',3); }
void releaseProcess(QProcess *process) {
    if(!process)return;
    process->disconnect();
    if(process->state()==QProcess::NotRunning)return;
    // QProcess destruction waits; let an active owned child finish asynchronously.
    process->setParent(nullptr);
    QObject::connect(process,&QProcess::stateChanged,process,[process](QProcess::ProcessState state){if(state==QProcess::NotRunning)process->deleteLater();});
    QObject::connect(process,&QProcess::started,process,[process]{process->kill();});process->kill();
}
}
Runtime::Runtime(QObject *parent):QObject(parent) {
    polling.setInterval(500);
    connect(&polling,&QTimer::timeout,this,&Runtime::poll);
    killDeadline.setSingleShot(true);
    connect(&killDeadline,&QTimer::timeout,this,[this]{
        for(auto &e:entries)for(auto p:{e->rx,e->tx})if(p&&p->state()!=QProcess::NotRunning)p->kill();
        if(hub&&hub->state()!=QProcess::NotRunning)hub->kill();checkStopped();
    });
}
Runtime::~Runtime() {
    ++generation;state_=Stopping;
    polling.stop();killDeadline.stop();
    cancelHubRequest();
    // Last-resort cleanup for abnormal window teardown: never enumerate external PIDs.
    for(auto &e:entries) {
        if(e->socket){e->socket->disconnect();e->socket->abort();}
        for(auto p:{e->rx,e->tx})releaseProcess(p);
    }
    releaseProcess(hub);
}
Runtime::Entry *Runtime::entry(const QString &id) const { for(auto &e:entries)if(e->node.id==id)return e.get();return nullptr; }
int Runtime::ownedProcessCount() const { int n=hub&&hub->state()!=QProcess::NotRunning;for(auto &e:entries)for(auto p:{e->rx,e->tx})if(p&&p->state()!=QProcess::NotRunning)++n;return n; }
QJsonObject Runtime::hubSnapshot() const {
    auto result=hubState;result.insert("pending",linkChangePending);return result;
}
QJsonObject Runtime::snapshot(const QString &id) const {
    const auto e=entry(id);if(!e)return {};
    return {{"phase",e->phase},{"detail",e->detail},{"applied",e->applied},{"instanceId",e->instanceId},
        {"controlPort",e->controlPort},{"rxPort",e->rxPort},{"pid",double(e->rx?e->rx->processId():0)},
        {"trace",e->trace},{"traceRequests",double(e->traceRequests)},{"runningPending",e->runningPending},
        {"failed",e->failed},{"ready",e->ready},{"enabled",e->node.enabled},
        {"registered",e->registered},{"registrationPending",e->registrationPending},
        {"previewError",e->previewError},
        {"txRunning",e->tx&&e->tx->state()!=QProcess::NotRunning}};
}
void Runtime::inspectNode(const QString &id) {
    if(inspectedNode==id)return;
    if(auto old=entry(inspectedNode))enqueue(old,"cli.preview",{{"ttlMs",0}});
    inspectedNode=id;
}
void Runtime::readLog(QProcess *process,const QString &tag) {
    QByteArray output=process->readAllStandardOutput()+process->readAllStandardError();
    if(!output.isEmpty())emit log(tag+": "+QString::fromUtf8(output.left(3000)).trimmed());
}
bool Runtime::start(const Project &p,const QVector<HostPreset> &presets,QString *error) {
    if(state_!=Idle)return fail(error,"Session already active");
    const auto validation=p.validate(presets);if(!validation.isEmpty())return fail(error,validation);
    if(p.nodes.isEmpty())return fail(error,"Add at least one node");
    if(p.nodes.size()>(p.mixIq?8:32))return fail(error,p.mixIq?"IQ medium currently supports at most 8 RX processes":"Frame hub currently supports at most 32 RX processes");
    QMap<QString,HostPreset> hosts;for(const auto &v:presets)hosts[v.id]=v;
    for(const auto &n:p.nodes) {
        const auto host=hosts.value(n.presetId);
        if(host.kind!="local")return fail(error,QString::fromUtf8("Віддалений профіль збережено; SSH-запуск ще не реалізовано: ")+host.name);
        if(n.enabled&&!QFileInfo::exists(executable(host,"ObriiCli")))return fail(error,"Missing ObriiCli: "+host.directory);
    }
    const auto hubPath=executable(hosts.value(p.nodes.first().presetId),"ObriiIqHub");
    if(!QFileInfo::exists(hubPath))return fail(error,"Missing ObriiIqHub: "+hubPath);
    ++generation;
    cancelHubRequest();hubState={};hubInstance.clear();hubKey.clear();hasQueuedLink=linkChangePending=false;receiverRegistrations.clear();
    for(auto &e:entries)for(auto proc:{e->rx,e->tx})if(proc){proc->disconnect();proc->deleteLater();}
    entries.clear();if(hub){hub->disconnect();hub->deleteLater();hub=nullptr;}
    session.reset(new QTemporaryDir(QDir::tempPath()+"/obrii-mesh-XXXXXX"));
    if(!session->isValid())return fail(error,"Cannot create session workspace");
    project=p;cliPath=hubPath;
    // Reserve distinct local ports together. CLI/hub receive fixed ports because their
    // current public launch interface does not return OS-assigned port numbers.
    std::vector<std::unique_ptr<QTcpServer>> reservations;
    auto port=[&]()->quint16 {
        auto server=std::make_unique<QTcpServer>();if(!server->listen(QHostAddress::LocalHost,0))return 0;
        const auto result=server->serverPort();reservations.push_back(std::move(server));return result;
    };
    hubPort=port();hubControlPort=port();
    for(const auto &n:p.nodes) {
        auto e=std::make_unique<Entry>();e->node=n;e->cli=executable(hosts.value(n.presetId),"ObriiCli");
        if(!n.enabled)e->phase="Disabled";
        e->controlPort=port();e->rxPort=port();e->viewPort=port();
        if(!hubPort||!hubControlPort||!e->controlPort||!e->rxPort||!e->viewPort){entries.clear();return fail(error,"Cannot reserve loopback ports");}
        entries.push_back(std::move(e));
    }
    state_=Starting;startedMs=QDateTime::currentMSecsSinceEpoch();emit changed();
    reservations.clear();
    for(auto &ptr:entries) {
        if(state_!=Starting)break;
        if(ptr->node.enabled)startRx(ptr.get());
    }
    if(state_!=Starting)return fail(error,"Session stopped during startup");
    polling.start();return true;
}
bool Runtime::addNode(const Node &node,const QVector<HostPreset> &presets,QString *error) {
    if(state_!=Running||hubInstance.isEmpty())return fail(error,"Hub is not ready");
    if(entry(node.id))return fail(error,"Node ID already exists in this session");
    if(entries.size()>=size_t(project.mixIq?8:32))return fail(error,"Session receiver capacity reached");
    int pending=0;for(const auto &e:entries)if(e->registrationPending)++pending;
    if(pending>=8)return fail(error,"Receiver registration queue is full");
    Node descriptor=node;descriptor.enabled=false;
    Project check=project;check.nodes.push_back(descriptor);
    const auto validation=check.validate(presets);if(!validation.isEmpty())return fail(error,validation);
    HostPreset host;for(const auto &preset:presets)if(preset.id==node.presetId)host=preset;
    if(host.kind!="local")return fail(error,"Live local nodes require a local host preset");
    QSet<quint16> used{hubPort,hubControlPort};
    for(const auto &e:entries)for(auto port:{e->controlPort,e->rxPort,e->viewPort})used.insert(port);
    std::vector<std::unique_ptr<QTcpServer>> reservations;
    auto port=[&]()->quint16 {
        for(int attempt=0;attempt<64;++attempt) {
            auto server=std::make_unique<QTcpServer>();if(!server->listen(QHostAddress::LocalHost,0))return 0;
            const auto value=server->serverPort();if(used.contains(value))continue;
            used.insert(value);reservations.push_back(std::move(server));return value;
        }
        return 0;
    };
    auto e=std::make_unique<Entry>();e->node=descriptor;e->cli=executable(host,"ObriiCli");
    e->controlPort=port();e->rxPort=port();e->viewPort=port();
    if(!e->controlPort||!e->rxPort||!e->viewPort)return fail(error,"Cannot reserve loopback ports for the new node");
    e->registered=false;e->registrationPending=true;e->phase="Registering";
    e->detail="Waiting for hub receiver registration";e->startedMs=QDateTime::currentMSecsSinceEpoch();
    entries.push_back(std::move(e));project.nodes.push_back(descriptor);receiverRegistrations.enqueue(node.id);
    reservations.clear();emit nodeChanged(node.id);emit topologyChanged();sendQueuedHubChange();return true;
}
void Runtime::startRx(Entry *e) {
    ++e->epoch;e->startedMs=QDateTime::currentMSecsSinceEpoch();
    const auto epoch=generation,nodeEpoch=e->epoch;
    auto process=new QProcess(this);e->rx=process;
    process->setWorkingDirectory(session->path());
    auto current=[this,e,process,epoch,nodeEpoch]{return epoch==generation&&e->epoch==nodeEpoch&&e->rx==process&&!e->failed&&e->node.enabled&&state_!=Idle&&state_!=Stopping;};
    connect(process,&QProcess::readyReadStandardOutput,this,[this,process,e,current]{if(current())readLog(process,e->node.id);else process->readAllStandardOutput();});
    connect(process,&QProcess::readyReadStandardError,this,[this,process,e,current]{if(current())readLog(process,e->node.id);else process->readAllStandardError();});
    connect(process,&QProcess::started,this,[this,e,process,current]{if(current())enqueue(e,"status",{});else process->kill();});
    connect(process,&QProcess::errorOccurred,this,[this,e,process,current](QProcess::ProcessError){
        if(current())nodeFailure(e,"RX process error: "+process->errorString());else checkStopped();
    });
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this,e,process,current,epoch](int code,QProcess::ExitStatus){
        if(current())nodeFailure(e,"RX exited: "+QString::number(code));
        else {
            checkStopped();
            if(epoch==generation&&e->rx==process&&(e->failed||!e->node.enabled))emit nodeChanged(e->node.id);
        }
    });
    QStringList args{"run","--backend","virtual","--input","rf","--virtual-rx-transport","tcp","--virtual-rx-bind","127.0.0.1",
        "--virtual-rx-port",QString::number(e->rxPort),"--control-port",QString::number(e->controlPort),
        "--freq",numeric(project.centerHz),"--rate",QString::number(project.sampleRate),"--mode",e->node.mode,
        "--listen",numeric(e->node.listeningHz),"--bandwidth",numeric(e->node.bandwidthHz),"--serve","--bind","127.0.0.1",
        "--network-port",QString::number(e->viewPort),"--fft","1024"};
    process->start(e->cli,args);
}
bool Runtime::restartRx(Entry *e,QString *error) {
    if(!e->registered)return retryRegistration(e,error);
    for(auto p:{e->rx,e->tx})if(p&&p->state()!=QProcess::NotRunning)
        return fail(error,"Node's previous workers are still stopping; retry RX start shortly");
    // The hub reconnects the same receiver endpoint; never substitute a new node or TX.
    QTcpServer control,rx,view;
    if(!control.listen(QHostAddress::LocalHost,e->controlPort)||!rx.listen(QHostAddress::LocalHost,e->rxPort)||!view.listen(QHostAddress::LocalHost,e->viewPort))
        return fail(error,"Cannot reclaim node's loopback ports");
    cancelNodeRequest(e);
    for(auto p:{e->rx,e->tx})if(p){p->disconnect();p->deleteLater();}
    e->rx=e->tx=nullptr;e->node.enabled=true;e->failed=false;e->txStopping=false;e->ready=false;
    e->instanceId.clear();e->applied={};e->trace={};e->traceSequence.clear();e->previewError.clear();
    e->runningPending=true;e->runningCommandDone=false;e->phase="Starting";e->detail="Explicit RX restart";
    control.close();rx.close();view.close();startRx(e);
    emit spectrumChanged(e->node.id);emit nodeChanged(e->node.id);return true;
}
bool Runtime::retryRegistration(Entry *e,QString *error) {
    if(e->registrationPending)return fail(error,"Receiver registration is still pending");
    int pending=0;for(const auto &candidate:entries)if(candidate->registrationPending)++pending;
    if(pending>=8)return fail(error,"Receiver registration queue is full");
    ++e->epoch;e->failed=false;e->node.enabled=false;e->registrationPending=true;e->registrationQuery=true;
    e->phase="Registering";e->detail="Explicit retry: checking the hub's receiver endpoint";
    e->startedMs=QDateTime::currentMSecsSinceEpoch();receiverRegistrations.enqueue(e->node.id);
    emit nodeChanged(e->node.id);sendQueuedHubChange();return true;
}
void Runtime::cancelNodeRequest(Entry *e) {
    e->requests.clear();
    if(!e->socket)return;
    auto socket=e->socket;e->socket=nullptr;
    for(auto timer:socket->findChildren<QTimer *>())timer->stop();
    socket->disconnect();socket->abort();socket->deleteLater();
}
void Runtime::nodeFailure(Entry *e,const QString &reason) {
    if(state_==Idle||state_==Stopping||e->failed)return;
    e->registrationPending=false;receiverRegistrations.removeAll(e->node.id);
    e->failed=true;stopNode(e,"Failed",reason);
}
void Runtime::stopNode(Entry *e,const QString &phase,const QString &detail) {
    ++e->epoch;e->ready=false;e->phase=phase;e->detail=detail.left(300);
    cancelNodeRequest(e);e->runningPending=e->runningCommandDone=false;
    e->applied={};e->trace={};e->traceSequence.clear();e->previewError.clear();
    e->txStopping=true;
    // Never send further control after failure/disable; stop only owned workers.
    for(auto p:{e->tx,e->rx})if(p&&p->state()!=QProcess::NotRunning)p->kill();
    emit log(e->node.id+": "+e->detail);emit spectrumChanged(e->node.id);emit nodeChanged(e->node.id);
}
void Runtime::enqueue(Entry *e,const QString &command,const QJsonObject &args) {
    if(state_==Stopping||state_==Idle||e->failed||!e->registered||!e->node.enabled||e->requests.size()>=12)return;
    if(command!="status"&&!e->ready)return;
    if(command=="trace")++e->traceRequests;
    e->requests.enqueue({{"command",command},{"args",args},{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)}});pump(e);
}
void Runtime::pump(Entry *e) {
    if(state_==Idle||state_==Stopping||e->failed||!e->registered||!e->node.enabled||e->socket||e->requests.isEmpty()||!e->rx||e->rx->state()!=QProcess::Running)return;
    auto request=e->requests.dequeue();if(!e->instanceId.isEmpty())request["targetInstanceId"]=e->instanceId;
    auto socket=new QTcpSocket(this);e->socket=socket;socket->setReadBufferSize(1024*1024+1);
    auto deadline=new QTimer(socket);deadline->setSingleShot(true);
    auto finished=std::make_shared<bool>(false);
    const QString id=e->node.id,command=request.value("command").toString();
    const auto requestGeneration=generation,nodeEpoch=e->epoch;
    const auto requestId=request.value("id");
    auto complete=[this,e,socket,finished,id,command,requestGeneration,nodeEpoch,requestId](const QJsonObject &reply,bool transportError=false) {
        if(*finished)return;*finished=true;
        for(auto timer:socket->findChildren<QTimer *>())timer->stop();
        socket->disconnect();socket->abort();socket->deleteLater();
        if(requestGeneration!=generation||state_==Idle||state_==Stopping||e->epoch!=nodeEpoch||e->socket!=socket||e->failed)return;
        e->socket=nullptr;
        if(transportError) {
            const auto reason=reply.value("error").toString("Control unavailable").left(300);
            // A new CLI may not have opened control yet. Only startup gets retries.
            if(!e->ready&&e->phase=="Starting") {e->detail=reason;emit nodeChanged(id);}
            else nodeFailure(e,"Control unavailable: "+reason);
            return;
        }
        const auto identity=reply.value("instanceId").toString();
        if(reply.value("id")!=requestId||identity.isEmpty()||(!e->instanceId.isEmpty()&&identity!=e->instanceId)){
            nodeFailure(e,"Control response identity mismatch: "+reply.value("error").toString());return;
        }
        e->instanceId=identity;
        if(reply.value("ok").toBool()) {
            if(command=="status") {
                if(!e->ready){e->detail.clear();e->runningPending=false;}
                e->applied=reply;e->ready=true;
                if(e->runningCommandDone){e->runningPending=false;e->runningCommandDone=false;}
                e->phase=reply.value("running").toBool()?"Ready":"Paused";
                if(!reply.value("running").toBool()) {e->trace={};emit spectrumChanged(id);}
            }
            else if(command=="trace") {
                if(inspectedNode==id&&e->applied.value("running").toBool()&&!e->runningPending) {
                    const QString sequence=reply.value("sequence").toString();
                    const qint64 computed=reply.value("computedUtcMs").toString().toLongLong();
                    const qint64 now=QDateTime::currentMSecsSinceEpoch();
                    if(!sequence.isEmpty()&&sequence!=e->traceSequence&&computed>0&&now-computed>=0&&now-computed<=2000){
                        e->traceSequence=sequence;e->previewError.clear();
                        e->trace=reply;e->trace.insert("receivedMs",double(computed));emit spectrumChanged(id);
                    }
                }
            }
            else if(command=="cli.preview")e->previewError.clear();
            else {
                if(command=="start"||command=="stop")e->runningCommandDone=true;
                e->detail=command+": applied";emit log(id+" "+e->detail);
            }
        } else {
            if(command=="trace") {
                e->previewError=reply.value("error").toString();
                if(inspectedNode==id) {e->trace={};emit spectrumChanged(id);}
            } else if(command=="cli.preview")e->previewError=reply.value("error").toString();
            else {nodeFailure(e,command+": "+reply.value("error").toString("Control request failed"));return;}
        }
        if(command!="trace"&&command!="cli.preview")emit nodeChanged(id);
        QTimer::singleShot(0,this,[this,id,requestGeneration,nodeEpoch]{if(requestGeneration==generation)if(auto n=entry(id))if(n->epoch==nodeEpoch)pump(n);});
    };
    auto receive=[socket,complete] {
        if(socket->bytesAvailable()>1024*1024){complete({{"ok",false},{"error","Control response too large"}});return;}
        if(!socket->canReadLine())return;
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(socket->readLine(),&error);
        complete(error.error==QJsonParseError::NoError&&doc.isObject()?doc.object():QJsonObject{{"ok",false},{"error","Invalid control JSON"}});
    };
    connect(socket,&QTcpSocket::connected,socket,[socket,request]{socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');});
    connect(socket,&QTcpSocket::readyRead,socket,receive);
    connect(socket,&QTcpSocket::errorOccurred,socket,[socket,receive,complete](QAbstractSocket::SocketError){receive();complete({{"ok",false},{"error",socket->errorString()}},true);});
    connect(socket,&QTcpSocket::disconnected,socket,[socket,receive,complete]{receive();complete({{"ok",false},{"error",socket->errorString()}},true);});
    connect(deadline,&QTimer::timeout,socket,[complete]{complete({{"ok",false},{"error","Control timeout"}},true);});
    deadline->start(2500);socket->connectToHost(QHostAddress::LocalHost,e->controlPort);
}
void Runtime::poll() {
    if(state_!=Starting&&state_!=Running)return;
    const auto now=QDateTime::currentMSecsSinceEpoch();
    if(state_==Starting&&hub&&now-startedMs>15000){failure("Hub startup timeout");return;}
    bool ready=!entries.empty();
    for(auto &e:entries){
        if(!e->registered) {
            if(e->registrationPending&&now-e->startedMs>15000)nodeFailure(e.get(),"Hub receiver registration timeout");
            continue;
        }
        if(e->node.enabled&&!e->failed&&!e->ready&&now-e->startedMs>15000)nodeFailure(e.get(),"RX startup timeout");
        ready&=e->ready||e->failed||!e->node.enabled;
        if(e->failed||!e->node.enabled)continue;
        if(!e->socket&&e->requests.isEmpty()) {
            enqueue(e.get(),"status",{});
            if(state_==Running&&inspectedNode==e->node.id&&e->ready&&
               e->applied.value("running").toBool()&&!e->runningPending) {
                enqueue(e.get(),"cli.preview",{{"ttlMs",1500},{"intervalMs",500}});
                enqueue(e.get(),"trace",{{"source","live"},{"offset",0},{"limit",1024}});
            }
        }
    }
    if(state_==Starting&&ready&&!hub)startHub();
    else if((state_==Starting||state_==Running)&&hub&&hub->state()==QProcess::Running&&!hubSocket) {
        if(hubState.contains("controlError")||(!hasQueuedLink&&receiverRegistrations.isEmpty()))hubRequest("status");
        else sendQueuedHubChange();
    }
}
void Runtime::startHub() {
    startedMs=QDateTime::currentMSecsSinceEpoch();
    QJsonArray links;for(const auto &l:project.links)links.append(l.json());
    const auto config=session->filePath("links.json");QString error;
    if(!saveJson(config,{{"version",1},{"defaultRoute",false},{"links",links}},&error)){failure(error);return;}
    hubKey=QUuid::createUuid().toString(QUuid::WithoutBraces)+QUuid::createUuid().toString(QUuid::WithoutBraces);hubKey.remove('-');
    const auto keyFile=session->filePath("hub-control.json");
    if(!saveJson(keyFile,{{"token",hubKey}},&error)||!QFile::setPermissions(keyFile,QFileDevice::ReadOwner|QFileDevice::WriteOwner)){
        failure("Cannot create private hub control key file");return;
    }
    auto process=new QProcess(this);hub=process;process->setWorkingDirectory(session->path());
    QStringList args{"--listen","127.0.0.1:"+QString::number(hubPort),"--scenario",config,
        "--control-port",QString::number(hubControlPort),"--control-key-file",keyFile};
    for(const auto &e:entries)args<<"--rx"<<(e->node.id+"@127.0.0.1:"+QString::number(e->rxPort));
    if(project.mixIq)args<<"--mix-rate"<<QString::number(project.sampleRate)<<"--mix-center"<<QString::number(qRound64(project.centerHz))<<"--mix-max-blocks"<<"256";
    connect(process,&QProcess::readyReadStandardOutput,this,[this,process]{readLog(process,"medium");});
    connect(process,&QProcess::readyReadStandardError,this,[this,process]{readLog(process,"medium");});
    connect(process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){if(error==QProcess::FailedToStart)failure("Cannot start IQ hub");});
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this](int code,QProcess::ExitStatus){
        if(state_!=Stopping&&state_!=Idle)failure("IQ hub exited: "+QString::number(code));else checkStopped();
    });
    connect(process,&QProcess::started,this,[this]{hubRequest("status");});
    process->start(cliPath,args);
}
void Runtime::cancelHubRequest() {
    if(!hubSocket)return;
    for(auto timer:hubSocket->findChildren<QTimer *>())timer->stop();
    hubSocket->disconnect();hubSocket->abort();hubSocket->deleteLater();hubSocket=nullptr;
}
void Runtime::hubRequest(const QString &command,const QJsonObject &args) {
    if(hubSocket||(state_!=Starting&&state_!=Running)||!hub||hub->state()!=QProcess::Running)return;
    const auto epoch=generation;
    const auto requestId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject request{{"version",1},{"id",requestId},{"command",command},{"token",hubKey}};
    if(!hubInstance.isEmpty())request.insert("targetInstanceId",hubInstance);
    if(command=="setLink"||command=="addReceiver"){request.insert("args",args);request.insert("expectedRevision",hubState.value("revision"));}
    if(command=="getReceiver")request.insert("args",args);
    const bool receiverRequest=command=="addReceiver"||command=="getReceiver";
    const auto receiverId=receiverRequest?args.value("id").toString():QString();
    const auto receiver=entry(receiverId);const quint64 receiverEpoch=receiver?receiver->epoch:0;
    const auto expectedRevision=request.value("expectedRevision").toString();
    const auto changedLink=queuedLink;
    auto socket=new QTcpSocket(this);hubSocket=socket;socket->setReadBufferSize(256*1024+1);
    auto deadline=new QTimer(socket);deadline->setSingleShot(true);
    auto finished=std::make_shared<bool>(false);
    auto complete=[this,socket,finished,epoch,requestId,command,args,receiverRequest,receiverId,receiverEpoch,expectedRevision,changedLink](const QJsonObject &reply){
        if(*finished)return;*finished=true;
        for(auto timer:socket->findChildren<QTimer *>())timer->stop();
        socket->disconnect();socket->abort();socket->deleteLater();
        if(epoch!=generation||state_==Stopping||state_==Idle||hubSocket!=socket)return;
        hubSocket=nullptr;
        if(command=="setLink")linkChangePending=false;
        auto receiver=entry(receiverId);
        const bool currentReceiver=receiver&&receiver->epoch==receiverEpoch&&receiver->registrationPending;
        if(reply.value("ok").toBool()) {
            const auto identity=reply.value("instanceId").toString();
            if(reply.value("id")!=requestId||identity.isEmpty()||(!hubInstance.isEmpty()&&hubInstance!=identity)){
                if(receiverRequest) {
                    if(currentReceiver)nodeFailure(receiver,"Hub receiver registration identity mismatch");
                    hubState.insert("controlError","Hub receiver registration identity mismatch");emit topologyChanged();return;
                }
                failure("Hub control identity mismatch");return;
            }
            hubInstance=identity;
            if(command=="status") {
                const auto lastError=hubState.value("lastLinkError");hubState=reply;
                if(!lastError.isUndefined())hubState.insert("lastLinkError",lastError);
                if(state_==Starting){
                    QFile::remove(session->filePath("hub-control.json"));
                    state_=Running;emit changed();emit log(QString::fromUtf8("Віртуальне середовище готове. Живі зміни зв'язків доступні; автоматичної ретрансляції немає."));
                }
            } else if(command=="getReceiver") {
                if(currentReceiver) {
                    const QJsonObject endpoint{{"id",receiverId},{"host","127.0.0.1"},{"port",receiver->rxPort}};
                    bool validRevision=false;const auto revision=reply.value("revision").toString();
                    const auto parsedRevision=revision.toULongLong(&validRevision);
                    if(!reply.value("registered").isBool()||!validRevision||QString::number(parsedRevision)!=revision) {
                        nodeFailure(receiver,"Invalid receiver reconciliation response");
                    } else if(reply.value("registered").toBool()) {
                        if(reply.value("receiver").toObject()!=endpoint)nodeFailure(receiver,"Receiver ID belongs to a different or unverified endpoint");
                        else {
                            receiver->registered=true;receiver->registrationPending=false;receiver->phase="Disabled";
                            receiver->detail="Receiver registration reconciled; enable RX explicitly";emit nodeChanged(receiverId);
                            auto receivers=hubState.value("receivers").toArray();bool found=false;
                            for(const auto &value:receivers)if(value.toObject().value("id")==receiverId){found=true;break;}
                            if(!found)receivers.append(endpoint);hubState.insert("receivers",receivers);
                        }
                    } else {
                        receiver->registrationQuery=false;receiverRegistrations.enqueue(receiverId);
                    }
                    if(validRevision&&QString::number(parsedRevision)==revision)hubState.insert("revision",revision);
                }
            } else if(command=="addReceiver") {
                bool validRevision=false;const auto previousRevision=expectedRevision.toULongLong(&validRevision);
                if(reply.value("receiver").toObject()!=args||!validRevision||
                   reply.value("revision").toString()!=QString::number(previousRevision+1)) {
                    if(currentReceiver)nodeFailure(receiver,"Hub acknowledged a different receiver endpoint or revision");
                    emit topologyChanged();return;
                }
                auto receivers=hubState.value("receivers").toArray();
                receivers.append(args);hubState.insert("receivers",receivers);hubState.insert("revision",reply.value("revision"));
                if(currentReceiver) {
                    receiver->registered=true;receiver->registrationPending=false;receiver->phase="Disabled";
                    receiver->detail="Registered with hub; enable RX explicitly";emit nodeChanged(receiverId);
                }
            } else {
                auto links=hubState.value("links").toArray();const auto link=reply.value("link").toObject();bool found=false;
                for(int i=0;i<links.size();++i)if(links[i].toObject().value("tx")==link.value("tx")&&links[i].toObject().value("rx")==link.value("rx")){links[i]=link;found=true;break;}
                if(!found)links.append(link);
                hubState.insert("links",links);hubState.insert("revision",reply.value("revision"));hubState.remove("lastLinkError");
                bool updated=false;
                for(auto &existing:project.links)if(existing.tx==changedLink.tx&&existing.rx==changedLink.rx){existing=changedLink;updated=true;break;}
                if(!updated)project.links.push_back(changedLink);
                emit log(QString::fromUtf8("Зв'язок застосовано: ")+link.value("tx").toString()+" -> "+link.value("rx").toString()+"; revision "+reply.value("revision").toString());
            }
            hubState.remove("controlError");
        } else {
            const auto error=reply.value("error").toString("Hub control failed").left(200);
            if(receiverRequest) {if(currentReceiver)nodeFailure(receiver,"Hub receiver registration failed: "+error);}
            else hubState.insert(command=="setLink"?"lastLinkError":"controlError",error);
            if(command=="setLink")emit log("Link not applied: "+error);
        }
        emit topologyChanged();
        if(hasQueuedLink||!receiverRegistrations.isEmpty())QTimer::singleShot(0,this,[this,epoch]{if(epoch==generation)sendQueuedHubChange();});
    };
    auto receive=[socket,complete]{
        if(socket->bytesAvailable()>256*1024){complete({{"ok",false},{"error","Hub response too large"}});return;}
        if(!socket->canReadLine())return;
        QJsonParseError parse;const auto doc=QJsonDocument::fromJson(socket->readLine(),&parse);
        complete(parse.error==QJsonParseError::NoError&&doc.isObject()?doc.object():QJsonObject{{"ok",false},{"error","Invalid hub response"}});
    };
    connect(socket,&QTcpSocket::connected,socket,[socket,request]{socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');});
    connect(socket,&QTcpSocket::readyRead,socket,receive);
    connect(socket,&QTcpSocket::errorOccurred,socket,[socket,receive,complete](QAbstractSocket::SocketError){receive();complete({{"ok",false},{"error",socket->errorString()}});});
    connect(socket,&QTcpSocket::disconnected,socket,[receive,complete]{receive();complete({{"ok",false},{"error","Hub disconnected"}});});
    connect(deadline,&QTimer::timeout,socket,[complete]{complete({{"ok",false},{"error","Hub control timeout"}});});
    deadline->start(2500);socket->connectToHost(QHostAddress::LocalHost,hubControlPort);
}
void Runtime::sendQueuedHubChange() {
    if(hubSocket||state_!=Running||hubState.contains("controlError"))return;
    if(hasQueuedLink){
        hasQueuedLink=false;
        hubRequest("setLink",{{"tx",queuedLink.tx},{"rx",queuedLink.rx},{"gainDb",queuedLink.gainDb},{"delayMs",queuedLink.delayMs},{"enabled",queuedLink.enabled}});return;
    }
    while(!receiverRegistrations.isEmpty()) {
        const auto id=receiverRegistrations.dequeue();auto e=entry(id);
        if(!e||!e->registrationPending||e->failed)continue;
        if(e->registrationQuery)hubRequest("getReceiver",{{"id",id}});
        else hubRequest("addReceiver",{{"id",id},{"host","127.0.0.1"},{"port",e->rxPort}});return;
    }
}
bool Runtime::applyLink(const Link &link,QString *error) {
    if(state_!=Running||hubInstance.isEmpty())return fail(error,"Hub is not ready");
    if(hubState.contains("controlError"))return fail(error,"Hub control is unavailable; wait for reconnection");
    if(linkChangePending)return fail(error,"A link change is still pending");
    const auto tx=entry(link.tx),rx=entry(link.rx);
    if(!tx||!rx||!tx->registered||!rx->registered)return fail(error,"Link endpoints must be registered with the hub");
    if(link.tx==link.rx||!std::isfinite(link.gainDb)||link.gainDb< -120||link.gainDb>60||link.delayMs<0||link.delayMs>500)
        return fail(error,"Invalid link parameters");
    const auto validation=validateLinkOptions(link);if(!validation.isEmpty())return fail(error,validation);
    queuedLink=link;hasQueuedLink=linkChangePending=true;hubState.remove("lastLinkError");emit topologyChanged();sendQueuedHubChange();return true;
}
bool Runtime::apply(const Node &node,QString *error) {
    auto e=entry(node.id);if(state_!=Running||!e)return fail(error,"Node is not ready");
    if(node.presetId!=e->node.presetId)return fail(error,"Changing a running node's host requires a new session");
    Project check=project;for(auto &n:check.nodes)if(n.id==node.id)n=node;
    QVector<HostPreset> dummy;QSet<QString> ids;for(auto &n:check.nodes)if(!ids.contains(n.presetId)){ids.insert(n.presetId);HostPreset p;p.id=n.presetId;p.name=p.id;p.directory=".";dummy<<p;}
    const auto validation=check.validate(dummy);if(!validation.isEmpty())return fail(error,validation);
    if(!node.enabled){e->node=node;return setNodeEnabled(node.id,false,error);}
    if(e->failed||!e->node.enabled){
        const auto previous=e->node;e->node=node;
        if(restartRx(e,error))return true;e->node=previous;return false;
    }
    if(!e->ready)return fail(error,"Node is not ready");
    if(e->runningPending)return fail(error,"RX start/stop is still pending");
    if(e->requests.size()>7)return fail(error,"Previous changes are still pending");
    e->node=node;
    // The public demod command restores the mode's default bandwidth. Reconfigure
    // from the common center, then set final BW and restore the desired listener.
    enqueue(e,"listen",{{"frequencyHz",project.centerHz}});
    enqueue(e,"demod",{{"mode",node.mode}});
    enqueue(e,"bandwidth",{{"bandwidthHz",node.bandwidthHz}});
    enqueue(e,"listen",{{"frequencyHz",node.listeningHz}});
    enqueue(e,"status",{});return true;
}
bool Runtime::testSignal(const QString &id,QString *error) {
    auto e=entry(id);if(state_!=Running||!e)return fail(error,"Session is not ready");
    if(e->failed||!e->node.enabled||!e->ready||e->runningPending)return fail(error,"RX is unavailable or changing; explicitly restart RX before TX");
    if(e->tx&&e->tx->state()!=QProcess::NotRunning)return fail(error,"This node is already transmitting");
    // Launch the worker directly so stopping its QProcess cannot orphan a child
    // underneath the CLI module-launching wrapper.
    const auto txPath=executable(HostPreset{"worker","worker","local",{}, {},QFileInfo(e->cli).absolutePath(),22},"ObriiCliTxModule");
    if(!QFileInfo::exists(txPath))return fail(error,"Missing ObriiCliTxModule: "+txPath);
    if(e->tx){e->tx->disconnect();e->tx->deleteLater();}
    auto process=new QProcess(this);e->tx=process;e->txStopping=false;process->setWorkingDirectory(session->path());
    const auto epoch=generation,nodeEpoch=e->epoch;
    auto current=[this,e,process,epoch,nodeEpoch]{return epoch==generation&&e->epoch==nodeEpoch&&e->tx==process&&!e->failed&&e->node.enabled&&state_==Running;};
    connect(process,&QProcess::readyReadStandardOutput,this,[this,process,id,current]{if(current())readLog(process,id+" TX");else process->readAllStandardOutput();});
    connect(process,&QProcess::readyReadStandardError,this,[this,process,id,current]{if(current())readLog(process,id+" TX");else process->readAllStandardError();});
    connect(process,&QProcess::started,this,[this,e,process,id,current]{if(!current()||e->txStopping)process->kill();else emit nodeChanged(id);});
    connect(process,&QProcess::errorOccurred,this,[this,e,process,current](QProcess::ProcessError){
        if(current()&&!e->txStopping)nodeFailure(e,"TX process error: "+process->errorString());else checkStopped();
    });
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this,e,process,id,current,epoch](int code,QProcess::ExitStatus status){
        if(current()) {
            if(!e->txStopping&&(code!=0||status!=QProcess::NormalExit)){nodeFailure(e,"TX exited: "+QString::number(code));return;}
            e->detail=e->txStopping?"Test TX stopped":"Test TX finished: "+QString::number(code);
            e->txStopping=false;emit log(id+": "+e->detail);emit nodeChanged(id);
        } else {
            checkStopped();
            if(epoch==generation&&e->tx==process&&(e->failed||!e->node.enabled))emit nodeChanged(id);
        }
    });
    process->start(txPath,{"tx","cw","TEST","--tx-sink","virtual","--freq",numeric(project.centerHz),"--rate",QString::number(project.sampleRate),
        "--virtual-host","127.0.0.1","--virtual-port",QString::number(hubPort),"--virtual-node-id",id,"--tx-level",numeric(e->node.txLevel)});
    emit nodeChanged(id);return true;
}
bool Runtime::setNodeRunning(const QString &id,bool running,QString *error) {
    auto e=entry(id);
    if(state_!=Running||!e)return fail(error,"Node is not ready");
    if(!e->node.enabled)return running?setNodeEnabled(id,true,error):true;
    if(e->failed)return running?restartRx(e,error):fail(error,"Node has failed; RX is already unavailable");
    if(!e->ready)return fail(error,"Node is not ready");
    if(e->runningPending||!e->requests.isEmpty())return fail(error,"Previous commands are still pending");
    e->runningPending=true;e->runningCommandDone=false;e->trace={};emit spectrumChanged(id);
    enqueue(e,running?"start":"stop",{});enqueue(e,"status",{});
    emit nodeChanged(id);return true;
}
bool Runtime::setNodeEnabled(const QString &id,bool enabled,QString *error) {
    auto e=entry(id);
    if(state_!=Running||!e)return fail(error,"Node is not ready");
    if(!e->registered) {
        if(enabled)return retryRegistration(e,error);
        e->node.enabled=false;return true;
    }
    if(enabled){if(e->node.enabled&&!e->failed)return true;return restartRx(e,error);}
    e->node.enabled=false;e->failed=false;stopNode(e,"Disabled","Node disabled explicitly");return true;
}
bool Runtime::stopSignal(const QString &id,QString *error) {
    auto e=entry(id);
    if(state_!=Running||!e)return fail(error,"Node is not ready");
    if(e->tx&&!e->txStopping&&e->tx->state()!=QProcess::NotRunning) {
        e->txStopping=true;
        auto process=e->tx;process->terminate();
        auto deadline=new QTimer(process);deadline->setSingleShot(true);
        connect(deadline,&QTimer::timeout,process,[process]{if(process->state()!=QProcess::NotRunning)process->kill();});
        connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),deadline,&QObject::deleteLater);
        deadline->start(1500);
    }
    return true;
}
void Runtime::failure(const QString &reason) { emit log("ERROR: "+reason);stop(); }
void Runtime::stop() {
    if(state_==Idle||state_==Stopping)return;++generation;state_=Stopping;polling.stop();emit changed();
    cancelHubRequest();hasQueuedLink=linkChangePending=false;receiverRegistrations.clear();
    for(auto &e:entries){e->registrationPending=false;cancelNodeRequest(e.get());
        for(auto p:{e->tx,e->rx})if(p&&p->state()!=QProcess::NotRunning)p->terminate();}
    if(hub&&hub->state()!=QProcess::NotRunning)hub->terminate();killDeadline.start(1500);checkStopped();
}
void Runtime::checkStopped() {
    if(state_!=Stopping||ownedProcessCount()!=0)return;killDeadline.stop();state_=Idle;
    if(session)QFile::remove(session->filePath("hub-control.json"));hubKey.clear();
    for(auto &e:entries){e->phase="Stopped";e->failed=false;e->ready=false;e->runningPending=e->runningCommandDone=false;e->trace={};emit spectrumChanged(e->node.id);emit nodeChanged(e->node.id);}emit changed();
}
}
