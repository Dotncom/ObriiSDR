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

namespace Mesh {
struct Runtime::Entry {
    Node node;
    QString cli,instanceId,phase="Starting",detail;
    QJsonObject applied;
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
    // Last-resort cleanup for abnormal window teardown: never enumerate external PIDs.
    for(auto &e:entries) {
        if(e->socket){e->socket->disconnect();e->socket->abort();}
        for(auto p:{e->rx,e->tx})if(p){p->disconnect();if(p->state()!=QProcess::NotRunning){p->kill();p->waitForFinished(100);}}
    }
    if(hub){hub->disconnect();if(hub->state()!=QProcess::NotRunning){hub->kill();hub->waitForFinished(100);}}
}
Runtime::Entry *Runtime::entry(const QString &id) const { for(auto &e:entries)if(e->node.id==id)return e.get();return nullptr; }
int Runtime::ownedProcessCount() const { int n=hub&&hub->state()!=QProcess::NotRunning;for(auto &e:entries)for(auto p:{e->rx,e->tx})if(p&&p->state()!=QProcess::NotRunning)++n;return n; }
QJsonObject Runtime::snapshot(const QString &id) const {
    const auto e=entry(id);if(!e)return {};
    return {{"phase",e->phase},{"detail",e->detail},{"applied",e->applied},{"instanceId",e->instanceId},
        {"controlPort",e->controlPort},{"rxPort",e->rxPort},{"pid",double(e->rx?e->rx->processId():0)},
        {"txRunning",e->tx&&e->tx->state()!=QProcess::NotRunning}};
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
    hubPort=port();
    for(const auto &n:p.nodes) {
        auto e=std::make_unique<Entry>();e->node=n;e->cli=executable(hosts.value(n.presetId),"ObriiCli");
        e->controlPort=port();e->rxPort=port();e->viewPort=port();
        if(!hubPort||!e->controlPort||!e->rxPort||!e->viewPort){entries.clear();return fail(error,"Cannot reserve loopback ports");}
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
            if(command=="status") {e->applied=reply;e->ready=true;e->phase="Ready";e->detail.clear();}
            else {e->detail=command+": applied";emit log(id+" "+e->detail);}
        } else {
            e->detail=reply.value("error").toString("Control request failed");
            if(command=="status"){e->ready=false;e->phase="Control unavailable";}
            if(command!="status")emit log(id+": "+e->detail);
        }
        emit nodeChanged(id);
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
    for(auto &e:entries){ready&=e->ready;if(!e->socket&&e->requests.isEmpty())enqueue(e.get(),"status",{});}
    if(state_==Starting&&ready&&!hub)startHub();
}
void Runtime::startHub() {
    QJsonArray links;for(const auto &l:project.links)links.append(l.json());
    const auto config=session->filePath("links.json");QString error;
    if(!saveJson(config,{{"version",1},{"defaultRoute",false},{"links",links}},&error)){failure(error);return;}
    auto process=new QProcess(this);hub=process;process->setWorkingDirectory(session->path());
    QStringList args{"--listen","127.0.0.1:"+QString::number(hubPort),"--scenario",config};
    for(const auto &e:entries)args<<"--rx"<<(e->node.id+"@127.0.0.1:"+QString::number(e->rxPort));
    if(project.mixIq)args<<"--mix-rate"<<QString::number(project.sampleRate)<<"--mix-center"<<QString::number(qRound64(project.centerHz))<<"--mix-max-blocks"<<"256";
    connect(process,&QProcess::readyReadStandardOutput,this,[this,process]{readLog(process,"medium");});
    connect(process,&QProcess::readyReadStandardError,this,[this,process]{readLog(process,"medium");});
    connect(process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){if(error==QProcess::FailedToStart)failure("Cannot start IQ hub");});
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this](int code,QProcess::ExitStatus){
        if(state_!=Stopping&&state_!=Idle)failure("IQ hub exited: "+QString::number(code));else checkStopped();
    });
    connect(process,&QProcess::started,this,[this,startupGeneration=generation]{
        QTimer::singleShot(250,this,[this,startupGeneration]{if(startupGeneration==generation&&state_==Starting&&hub&&hub->state()==QProcess::Running){state_=Running;emit changed();emit log(QString::fromUtf8("Віртуальне середовище запущено. Зв'язки статичні; mesh-маршрутизації ще немає."));}});
    });
    process->start(cliPath,args);
}
bool Runtime::apply(const Node &node,QString *error) {
    auto e=entry(node.id);if(state_!=Running||!e||!e->ready)return fail(error,"Node is not ready");
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
void Runtime::failure(const QString &reason) { emit log("ERROR: "+reason);stop(); }
void Runtime::stop() {
    if(state_==Idle||state_==Stopping)return;++generation;state_=Stopping;polling.stop();emit changed();
    for(auto &e:entries){e->requests.clear();if(e->socket){for(auto timer:e->socket->findChildren<QTimer *>())timer->stop();e->socket->disconnect();e->socket->abort();e->socket->deleteLater();e->socket=nullptr;}
        for(auto p:{e->tx,e->rx})if(p&&p->state()!=QProcess::NotRunning)p->terminate();}
    if(hub&&hub->state()!=QProcess::NotRunning)hub->terminate();killDeadline.start(1500);checkStopped();
}
void Runtime::checkStopped() {
    if(state_!=Stopping||ownedProcessCount()!=0)return;killDeadline.stop();state_=Idle;
    for(auto &e:entries){e->phase="Stopped";e->ready=false;emit nodeChanged(e->node.id);}emit changed();
}
}
