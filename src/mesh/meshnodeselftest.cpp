#include "meshnodecommands.h"
#include "meshproject.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QThread>
#include <QDir>
#include <QFileInfo>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
int failures=0;
void check(bool ok,const char *s){std::printf("%s %s\n",ok?"PASS":"FAIL",s);std::fflush(stdout);if(!ok)++failures;}
QJsonObject rpc(int port,const QString &token,QJsonObject request){
    request["token"]=token;QTcpSocket socket;socket.connectToHost("127.0.0.1",quint16(port));if(!socket.waitForConnected(2000))return {};
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');QByteArray buffer;QElapsedTimer t;t.start();
    while(t.elapsed()<3000&&!buffer.contains('\n')){if(socket.bytesAvailable()||socket.waitForReadyRead(100))buffer+=socket.readAll();if(socket.state()==QAbstractSocket::UnconnectedState)break;}
    return QJsonDocument::fromJson(buffer.trimmed()).object();
}
}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);CommandDispatcher commands("test");registerMeshNodeCommands(commands,&app);
    const auto human=CommandDispatcher::Context::human();
    auto started=commands.executeText("mesh.service start 127.0.0.1 0",human);
    check(started.value("ok").toBool()&&!started.value("token").toString().isEmpty(),"same typed command starts an optional embedded node service");
    check(!QJsonDocument(commands.executeText("mesh.service status",human)).toJson().contains(started.value("token").toString().toLatin1()),"ordinary mesh status never returns startup credentials");
    check(!commands.execute({{"command","mesh.service"},{"args",QJsonObject{{"operation","start"},{"port",0}}}},{}).value("ok").toBool(),"unprivileged agent cannot expose mesh listener");
    commands.execute({{"command","mesh.service"},{"args",QJsonObject{{"operation","stop"}}}},human);
    const auto binary=app.arguments().value(1,QDir(app.applicationDirPath()).filePath("ObriiMeshNodeHost"
#ifdef Q_OS_WIN
        ".exe"
#endif
    ));
    std::vector<std::unique_ptr<QProcess>> processes;QVector<QJsonObject> endpoints;
    for(int i=0;i<3;++i){
        auto p=std::make_unique<QProcess>();QStringList launch{"--mesh-port","0","--seconds","90"};
        if(QFileInfo(binary).baseName().compare("ObriiCli",Qt::CaseInsensitive)==0)launch.prepend("mesh-node");
        p->start(binary,launch);
        QByteArray bytes;QElapsedTimer wait;wait.start();
        while(wait.elapsed()<7000&&!bytes.contains('\n')){p->waitForReadyRead(100);bytes+=p->readAllStandardOutput();}
        const auto endpoint=QJsonDocument::fromJson(bytes.trimmed()).object();check(endpoint.value("ok").toBool(),"independent Obrii node process ready without receiver/RF");
        if(!endpoint.value("ok").toBool()){for(auto &owned:processes){owned->kill();owned->waitForFinished();}p->kill();p->waitForFinished();return 1;}
        endpoints.append(endpoint);processes.push_back(std::move(p));
    }
    const QStringList ids{"A","B","C"};QJsonArray nodes,links;
    for(const auto &id:ids)nodes.append(QJsonObject{{"id",id},{"name",id}});
    links.append(QJsonObject{{"tx","A"},{"rx","B"},{"enabled",true},{"delayMs",10},{"gainDb",-6}});
    links.append(QJsonObject{{"tx","B"},{"rx","C"},{"enabled",true},{"delayMs",20},{"gainDb",-12}});
    const QJsonObject topology{{"nodes",nodes},{"links",links}};QJsonObject peers;
    for(int i=0;i<3;++i)peers[ids[i]]=QJsonObject{{"host","127.0.0.1"},{"port",endpoints[i].value("port")},{"instanceId",endpoints[i].value("instanceId")}};
    auto call=[&](int i,QJsonObject r){r["instanceId"]=endpoints[i].value("instanceId");return rpc(endpoints[i].value("port").toInt(),endpoints[i].value("token").toString(),r);};
    auto configure=[&](const QString &mode,const QString &session){bool okay=true;const QString runKey=QUuid::createUuid().toString()+QUuid::createUuid().toString();
        for(int i=0;i<3;++i)okay&=call(i,{{"op","configure"},{"session",session},{"key",runKey},{"nodeId",ids[i]},{"transport",mode},{"topology",topology},{"peers",peers}}).value("ok").toBool();return okay;};
    auto pollUntil=[&](const QString &session,const QString &field,int n,int milliseconds){QElapsedTimer t;t.start();while(t.elapsed()<milliseconds){QJsonObject target;for(int i=0;i<3;++i){auto s=call(i,{{"op","status"},{"session",session}});if(i==2)target=s;}if(target.value(field).toString().toInt()>=n)return target;QThread::msleep(25);}return QJsonObject();};
    auto release=[&](const QString &session){bool okay=true;for(int i=0;i<3;++i)okay&=call(i,{{"op","release"},{"session",session}}).value("ok").toBool();return okay;};
    QString session=QUuid::createUuid().toString(QUuid::WithoutBraces);check(configure("data",session),"configure three pinned data nodes");
    check(!call(1,{{"op","configure"}}).value("ok").toBool(),"active node cannot be stolen by another configuration");
    check(!rpc(endpoints[0].value("port").toInt(),endpoints[0].value("token").toString(),{{"op","release"},{"session",session},{"instanceId",QUuid::createUuid().toString()}}).value("ok").toBool(),"stale instance identity cannot release a new process");
    const QByteArray payload("distributed\0packet",18);
    check(call(0,{{"op","send"},{"session",session},{"id","p1"},{"destination","C"},{"ttl",2},{"payloadBase64",QString::fromLatin1(payload.toBase64())}}).value("ok").toBool(),"source accepts binary data packet");
    auto state=pollUntil(session,"delivered",1,5000);const auto outcomes=state.value("results").toArray();
    check(!outcomes.isEmpty()&&outcomes.last().toObject().value("path").toArray()==QJsonArray{"A","B","C"}&&QByteArray::fromBase64(outcomes.last().toObject().value("payloadBase64").toString().toLatin1())==payload,"A-B-C processes independently relay exact data without IQ traffic");
    check(call(1,{{"op","status"},{"session",session}}).value("forwarded")=="1"&&state.value("iqRxSamples")=="0","data forwarding actually happens at relay B without modulation");
    check(call(2,{{"op","status"},{"session",session},{"after",state.value("lastResultSerial")}}).value("results").toArray().isEmpty(),"incremental telemetry does not resend unchanged packet payloads");
    check(rpc(endpoints[0].value("port").toInt(),"wrong",{{"op","status"}}).isEmpty(),"unauthorized network requests receive no node state");
    check(!call(0,{{"op","release"},{"session","foreign"}}).value("ok").toBool(),"foreign session cannot release active node");
    check(release(session),"session stop releases nodes but keeps their processes/listeners alive");
    check(!configure("rf",QUuid::createUuid().toString()),"physical RF is not silently substituted for a virtual bearer");
    session=QUuid::createUuid().toString(QUuid::WithoutBraces);check(configure("iq",session),"restart same processes in distributed IQ mode");
    check(call(0,{{"op","send"},{"session",session},{"id","iq1"},{"destination","C"},{"ttl",2},{"payloadBase64",QString::fromLatin1(payload.toBase64())}}).value("ok").toBool(),"source accepts IQ modem packet");
    state=pollUntil(session,"delivered",1,12000);
    check(!state.isEmpty()&&state.value("iqRxSamples").toString().toULongLong()>0&&call(1,{{"op","status"},{"session",session}}).value("iqTxSamples").toString().toULongLong()>0,"A-B-C external processes decode and retransmit real CF32 IQ");
    check(release(session),"distributed IQ session stops independently from hosts");
    session=QUuid::createUuid().toString(QUuid::WithoutBraces);check(configure("data",session),"prepare orphan-ownership expiry test");
    QElapsedTimer expiry;expiry.start();bool expired=false;
    while(expiry.elapsed()<18000){expired=true;for(int i=0;i<3;++i)expired&=!call(i,{{"op","status"}}).value("active").toBool(true);if(expired)break;QThread::msleep(100);}
    check(expired,"nodes release orphan sessions without controller keepalive");
    for(auto &p:processes){p->terminate();if(!p->waitForFinished(3000)){p->kill();p->waitForFinished(3000);}}
    return failures?1:0;
}
