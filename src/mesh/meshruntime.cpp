#include "meshruntime.h"
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQueue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QPointer>
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
    QProcess *rx=nullptr,*tx=nullptr;
    QTcpSocket *socket=nullptr;
    quint16 controlPort=0,rxPort=0,viewPort=0;
    bool ready=false;
    QQueue<QJsonObject> requests;
};
namespace {
bool fail(QString *error,const QString &text) { if(error)*error=text;return false; }
QString numeric(double v) { return QString::number(v,'f',3); }
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
        for(auto p:{e->rx,e->tx})if(p){p->disconnect();if(p->state()!=QProcess::NotRunning){p->kill();p->waitForFinished(100);}}
    }
    if(hub){hub->disconnect();if(hub->state()!=QProcess::NotRunning){hub->kill();hub->waitForFinished(100);}}
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
        if(!QFileInfo::exists(executable(host,"ObriiCli")))return fail(error,"Missing ObriiCli: "+host.directory);
    }
    const auto hubPath=executable(hosts.value(p.nodes.first().presetId),"ObriiIqHub");
    if(!QFileInfo::exists(hubPath))return fail(error,"Missing ObriiIqHub: "+hubPath);
    ++generation;
    cancelHubRequest();hubState={};hubInstance.clear();hubKey.clear();hasQueuedLink=linkChangePending=false;
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
        e->controlPort=port();e->rxPort=port();e->viewPort=port();
        if(!hubPort||!hubControlPort||!e->controlPort||!e->rxPort||!e->viewPort){entries.clear();return fail(error,"Cannot reserve loopback ports");}
        entries.push_back(std::move(e));
    }
    state_=Starting;startedMs=QDateTime::currentMSecsSinceEpoch();emit changed();
    reservations.clear();
    for(auto &ptr:entries) {
        if(state_!=Starting)break;
        auto e=ptr.get();auto process=new QProcess(this);e->rx=process;
        process->setWorkingDirectory(session->path());
        connect(process,&QProcess::readyReadStandardOutput,this,[this,process,e]{readLog(process,e->node.id);});
        connect(process,&QProcess::readyReadStandardError,this,[this,process,e]{readLog(process,e->node.id);});
        connect(process,&QProcess::started,this,[this,e]{enqueue(e,"status",{});});
        connect(process,&QProcess::errorOccurred,this,[this,e](QProcess::ProcessError error){if(error==QProcess::FailedToStart&&state_!=Stopping)failure("Cannot start "+e->node.id);});
        connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this,e](int code,QProcess::ExitStatus){
            e->phase="Stopped";e->ready=false;emit nodeChanged(e->node.id);
            if(state_!=Stopping&&state_!=Idle)failure(e->node.id+" exited: "+QString::number(code));else checkStopped();
        });
        QStringList args{"run","--backend","virtual","--input","rf","--virtual-rx-transport","tcp","--virtual-rx-bind","127.0.0.1",
            "--virtual-rx-port",QString::number(e->rxPort),"--control-port",QString::number(e->controlPort),
            "--freq",numeric(p.centerHz),"--rate",QString::number(p.sampleRate),"--mode",e->node.mode,
            "--listen",numeric(e->node.listeningHz),"--bandwidth",numeric(e->node.bandwidthHz),"--serve","--bind","127.0.0.1",
            "--network-port",QString::number(e->viewPort),"--fft","1024"};
        // Use each entry's own settings, not a shell command or a physical backend.
        args[args.indexOf("--mode")+1]=e->node.mode;
        args[args.indexOf("--listen")+1]=numeric(e->node.listeningHz);
        args[args.indexOf("--bandwidth")+1]=numeric(e->node.bandwidthHz);
        process->start(e->cli,args);
    }
    if(state_!=Starting)return fail(error,"Node process failed during startup");
    polling.start();return true;
}
void Runtime::enqueue(Entry *e,const QString &command,const QJsonObject &args) {
    if(state_==Stopping||state_==Idle||e->requests.size()>=12)return;
    if(command=="trace")++e->traceRequests;
    e->requests.enqueue({{"command",command},{"args",args},{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)}});pump(e);
}
void Runtime::pump(Entry *e) {
    if(state_==Idle||state_==Stopping||e->socket||e->requests.isEmpty()||!e->rx||e->rx->state()!=QProcess::Running)return;
    auto request=e->requests.dequeue();if(!e->instanceId.isEmpty())request["targetInstanceId"]=e->instanceId;
    auto socket=new QTcpSocket(this);e->socket=socket;socket->setReadBufferSize(1024*1024+1);
    auto deadline=new QTimer(socket);deadline->setSingleShot(true);
    auto finished=std::make_shared<bool>(false);
    const QString id=e->node.id,command=request.value("command").toString();
    const auto requestGeneration=generation;
    auto complete=[this,e,socket,finished,id,command,requestGeneration](const QJsonObject &reply) {
        if(*finished)return;*finished=true;
        for(auto timer:socket->findChildren<QTimer *>())timer->stop();
        socket->disconnect();socket->abort();socket->deleteLater();
        if(requestGeneration!=generation||state_==Idle||state_==Stopping)return;
        e->socket=nullptr;
        if(reply.value("ok").toBool()) {
            const auto identity=reply.value("instanceId").toString();
            if(identity.isEmpty()||(!e->instanceId.isEmpty()&&identity!=e->instanceId)){failure("Control instance identity mismatch: "+id);return;}
            e->instanceId=identity;
            if(command=="status") {
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
            else e->detail=reply.value("error").toString("Control request failed");
            if(command=="start"||command=="stop"||(command=="status"&&e->runningCommandDone)){
                e->runningPending=false;e->runningCommandDone=false;
            }
            if(command=="status"){e->ready=false;e->phase="Control unavailable";}
            if(command!="status"&&command!="trace"&&command!="cli.preview")emit log(id+": "+e->detail);
        }
        if(command!="trace"&&command!="cli.preview")emit nodeChanged(id);
        QTimer::singleShot(0,this,[this,id]{if(auto n=entry(id))pump(n);});
    };
    auto receive=[socket,complete] {
        if(socket->bytesAvailable()>1024*1024){complete({{"ok",false},{"error","Control response too large"}});return;}
        if(!socket->canReadLine())return;
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(socket->readLine(),&error);
        complete(error.error==QJsonParseError::NoError&&doc.isObject()?doc.object():QJsonObject{{"ok",false},{"error","Invalid control JSON"}});
    };
    connect(socket,&QTcpSocket::connected,socket,[socket,request]{socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');});
    connect(socket,&QTcpSocket::readyRead,socket,receive);
    connect(socket,&QTcpSocket::disconnected,socket,[socket,receive,complete]{receive();complete({{"ok",false},{"error",socket->errorString()}});});
    connect(deadline,&QTimer::timeout,socket,[complete]{complete({{"ok",false},{"error","Control timeout"}});});
    deadline->start(2500);socket->connectToHost(QHostAddress::LocalHost,e->controlPort);
}
void Runtime::poll() {
    if(state_==Starting&&QDateTime::currentMSecsSinceEpoch()-startedMs>15000){failure("Node startup timeout");return;}
    bool ready=!entries.empty();
    for(auto &e:entries){
        ready&=e->ready;
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
        if(hasQueuedLink)sendQueuedLink();else hubRequest("status");
    }
}
void Runtime::startHub() {
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
    if(command=="setLink"){request.insert("args",args);request.insert("expectedRevision",hubState.value("revision"));}
    auto socket=new QTcpSocket(this);hubSocket=socket;socket->setReadBufferSize(256*1024+1);
    auto deadline=new QTimer(socket);deadline->setSingleShot(true);
    auto finished=std::make_shared<bool>(false);
    auto complete=[this,socket,finished,epoch,requestId,command](const QJsonObject &reply){
        if(*finished)return;*finished=true;
        for(auto timer:socket->findChildren<QTimer *>())timer->stop();
        socket->disconnect();socket->abort();socket->deleteLater();
        if(epoch!=generation||state_==Stopping||state_==Idle)return;
        hubSocket=nullptr;
        if(command=="setLink")linkChangePending=false;
        if(reply.value("ok").toBool()) {
            const auto identity=reply.value("instanceId").toString();
            if(reply.value("id")!=requestId||identity.isEmpty()||(!hubInstance.isEmpty()&&hubInstance!=identity)){
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
            } else {
                auto links=hubState.value("links").toArray();const auto link=reply.value("link").toObject();bool found=false;
                for(int i=0;i<links.size();++i)if(links[i].toObject().value("tx")==link.value("tx")&&links[i].toObject().value("rx")==link.value("rx")){links[i]=link;found=true;break;}
                if(!found)links.append(link);
                hubState.insert("links",links);hubState.insert("revision",reply.value("revision"));hubState.remove("lastLinkError");
                emit log(QString::fromUtf8("Зв'язок застосовано: ")+link.value("tx").toString()+" -> "+link.value("rx").toString()+"; revision "+reply.value("revision").toString());
            }
            hubState.remove("controlError");
        } else {
            const auto error=reply.value("error").toString("Hub control failed").left(200);
            hubState.insert(command=="setLink"?"lastLinkError":"controlError",error);
            if(command=="setLink")emit log("Link not applied: "+error);
        }
        emit topologyChanged();
        if(hasQueuedLink)QTimer::singleShot(0,this,[this]{sendQueuedLink();});
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
void Runtime::sendQueuedLink() {
    if(!hasQueuedLink||hubSocket||state_!=Running)return;
    hasQueuedLink=false;hubRequest("setLink",queuedLink.json());
}
bool Runtime::applyLink(const Link &link,QString *error) {
    if(state_!=Running||hubInstance.isEmpty())return fail(error,"Hub is not ready");
    if(hubState.contains("controlError"))return fail(error,"Hub control is unavailable; wait for reconnection");
    if(linkChangePending)return fail(error,"A link change is still pending");
    if(!entry(link.tx)||!entry(link.rx)||link.tx==link.rx||!std::isfinite(link.gainDb)||link.gainDb< -120||link.gainDb>60||link.delayMs<0||link.delayMs>500)
        return fail(error,"Invalid link parameters");
    queuedLink=link;hasQueuedLink=linkChangePending=true;hubState.remove("lastLinkError");emit topologyChanged();sendQueuedLink();return true;
}
bool Runtime::apply(const Node &node,QString *error) {
    auto e=entry(node.id);if(state_!=Running||!e||!e->ready)return fail(error,"Node is not ready");
    if(e->runningPending)return fail(error,"RX start/stop is still pending");
    Project check=project;for(auto &n:check.nodes)if(n.id==node.id)n=node;
    QVector<HostPreset> dummy;QSet<QString> ids;for(auto &n:check.nodes)if(!ids.contains(n.presetId)){ids.insert(n.presetId);HostPreset p;p.id=n.presetId;p.name=p.id;p.directory=".";dummy<<p;}
    const auto validation=check.validate(dummy);if(!validation.isEmpty())return fail(error,validation);
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
    if(e->tx&&e->tx->state()!=QProcess::NotRunning)return fail(error,"This node is already transmitting");
    // Launch the worker directly so stopping its QProcess cannot orphan a child
    // underneath the CLI module-launching wrapper.
    const auto txPath=executable(HostPreset{"worker","worker","local",{}, {},QFileInfo(e->cli).absolutePath(),22},"ObriiCliTxModule");
    if(!QFileInfo::exists(txPath))return fail(error,"Missing ObriiCliTxModule: "+txPath);
    if(e->tx){e->tx->disconnect();e->tx->deleteLater();}
    auto process=new QProcess(this);e->tx=process;process->setWorkingDirectory(session->path());
    connect(process,&QProcess::readyReadStandardOutput,this,[this,process,id]{readLog(process,id+" TX");});
    connect(process,&QProcess::readyReadStandardError,this,[this,process,id]{readLog(process,id+" TX");});
    connect(process,&QProcess::errorOccurred,this,[this,e,id](QProcess::ProcessError){e->detail="TX process error";emit nodeChanged(id);});
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this,e,id](int code,QProcess::ExitStatus){
        e->detail="Test TX finished: "+QString::number(code);emit log(id+": "+e->detail);emit nodeChanged(id);if(state_==Stopping)checkStopped();
    });
    process->start(txPath,{"tx","cw","TEST","--tx-sink","virtual","--freq",numeric(project.centerHz),"--rate",QString::number(project.sampleRate),
        "--virtual-host","127.0.0.1","--virtual-port",QString::number(hubPort),"--virtual-node-id",id,"--tx-level",numeric(e->node.txLevel)});
    emit nodeChanged(id);return true;
}
bool Runtime::setNodeRunning(const QString &id,bool running,QString *error) {
    auto e=entry(id);
    if(state_!=Running||!e||!e->ready)return fail(error,"Node is not ready");
    if(e->runningPending||!e->requests.isEmpty())return fail(error,"Previous commands are still pending");
    e->runningPending=true;e->runningCommandDone=false;e->trace={};emit spectrumChanged(id);
    enqueue(e,running?"start":"stop",{});enqueue(e,"status",{});
    emit nodeChanged(id);return true;
}
bool Runtime::stopSignal(const QString &id,QString *error) {
    auto e=entry(id);
    if(state_!=Running||!e)return fail(error,"Node is not ready");
    if(e->tx&&e->tx->state()!=QProcess::NotRunning) {
        QPointer<QProcess> process=e->tx;process->terminate();
        QTimer::singleShot(1500,this,[process]{if(process&&process->state()!=QProcess::NotRunning)process->kill();});
    }
    return true;
}
void Runtime::failure(const QString &reason) { emit log("ERROR: "+reason);stop(); }
void Runtime::stop() {
    if(state_==Idle||state_==Stopping)return;++generation;state_=Stopping;polling.stop();emit changed();
    cancelHubRequest();hasQueuedLink=linkChangePending=false;
    for(auto &e:entries){e->requests.clear();if(e->socket){for(auto timer:e->socket->findChildren<QTimer *>())timer->stop();e->socket->disconnect();e->socket->abort();e->socket->deleteLater();e->socket=nullptr;}
        for(auto p:{e->tx,e->rx})if(p&&p->state()!=QProcess::NotRunning)p->terminate();}
    if(hub&&hub->state()!=QProcess::NotRunning)hub->terminate();killDeadline.start(1500);checkStopped();
}
void Runtime::checkStopped() {
    if(state_!=Stopping||ownedProcessCount()!=0)return;killDeadline.stop();state_=Idle;
    if(session)QFile::remove(session->filePath("hub-control.json"));hubKey.clear();
    for(auto &e:entries){e->phase="Stopped";e->ready=false;e->runningPending=false;e->trace={};emit spectrumChanged(e->node.id);emit nodeChanged(e->node.id);}emit changed();
}
}
