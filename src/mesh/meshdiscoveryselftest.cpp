#include "meshdiscovery.h"
#include "meshnode.h"
#include <QCoreApplication>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QThread>
#include <QUuid>
#include <functional>
#include <cstdio>

namespace {
int failures=0;
void check(bool yes,const char *s){std::printf("%s %s\n",yes?"PASS":"FAIL",s);std::fflush(stdout);if(!yes)++failures;}
bool waitFor(const std::function<bool()> &condition,int ms=4000){QElapsedTimer t;t.start();while(t.elapsed()<ms){QCoreApplication::processEvents(QEventLoop::AllEvents,20);if(condition())return true;QThread::msleep(2);}return condition();}
QJsonObject rpc(const QJsonObject &e,QJsonObject r,const QString &token={}){
    r["instanceId"]=e.value("instanceId");if(!token.isEmpty())r["token"]=token;
    QTcpSocket s;s.connectToHost("127.0.0.1",quint16(e.value("port").toInt()));if(!s.waitForConnected(2000))return {};
    s.write(QJsonDocument(r).toJson(QJsonDocument::Compact)+'\n');QByteArray bytes;QElapsedTimer t;t.start();
    while(t.elapsed()<3000&&!bytes.contains('\n')){if(s.bytesAvailable()||s.waitForReadyRead(50))bytes+=s.readAll();if(s.state()==QAbstractSocket::UnconnectedState)break;}
    return QJsonDocument::fromJson(bytes.trimmed()).object();
}
}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);app.setApplicationName("Discovery self-test");
    Mesh::NodeService a,b,c;QVector<QJsonObject> endpoints;
    for(auto service:{&a,&b,&c})endpoints.append(service->command({{"operation","start"},{"bind","127.0.0.1"},{"port",0}}));
    bool okay=true;for(const auto &e:endpoints)okay&=e.value("ok").toBool()&&e.value("discovery").toBool();check(okay,"three optional services share the LAN discovery port");
    Mesh::DiscoveryClient scan;QString error;check(scan.start(&error),"bounded UDP discovery starts");
    bool finished=false;QObject::connect(&scan,&Mesh::DiscoveryClient::finished,&app,[&]{finished=true;});
    check(waitFor([&]{return finished;})&&!scan.searching(),"discovery finishes without a background scan timer");
    bool all=true,privateData=false;
    for(const auto &e:endpoints){bool found=false;for(const auto &entry:scan.instances())if(entry.value("instanceId")==e.value("instanceId")){found=true;const auto bytes=QJsonDocument(entry).toJson();privateData|=bytes.contains(e.value("token").toString().toUtf8())||bytes.contains(e.value("pairingSecret").toString().toUtf8())||entry.contains("directory");}all&=found;}
    check(all,"broadcast finds every same-PC instance, not only the last UDP listener");
    check(!privateData,"discovery advertises addresses/capabilities but no keys or local paths");
    const auto endpoint=endpoints[0];const auto client=QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto paired=rpc(endpoint,{{"op","pair"},{"clientId",client},{"clientName","controller one"},{"code",endpoint.value("pairingSecret")}});
    check(paired.value("ok").toBool()&&paired.value("token").toString()!=endpoint.value("token").toString(),"local pairing code grants a distinct client registration token");
    auto state=rpc(endpoint,{{"op","status"}},paired.value("token").toString());check(state.value("ok").toBool()&&state.value("registrations")==1,"registered credential authenticates subsequent node requests");
    const auto secondId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto second=rpc(endpoint,{{"op","pair"},{"clientId",secondId},{"clientName","controller two"},{"code",endpoint.value("pairingSecret")}});
    const QString session=QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject topology{{"nodes",QJsonArray{QJsonObject{{"id","A"},{"name","A"}}}},{"links",QJsonArray{}}};
    QJsonObject peers{{"A",QJsonObject{{"host","127.0.0.1"},{"port",endpoint.value("port")},{"instanceId",endpoint.value("instanceId")}}}};
    const auto configured=rpc(endpoint,{{"op","configure"},{"session",session},{"key",QString(64,'x')},{"nodeId","A"},{"transport","data"},{"topology",topology},{"peers",peers}},paired.value("token").toString());
    check(configured.value("ok").toBool(),"registered controller can own a configured graph node");
    const auto observer=rpc(endpoint,{{"op","status"}},second.value("token").toString());
    check(observer.value("active").toBool()&&!observer.contains("session")&&!observer.contains("results")&&!rpc(endpoint,{{"op","release"},{"session",session}},second.value("token").toString()).value("ok").toBool(),"another registered client cannot inspect payloads or release the owner's session");
    auto registrations=a.command({{"operation","registrations"}});const auto encoded=QJsonDocument(registrations).toJson();
    check(registrations.value("clients").toArray().size()==2&&!encoded.contains(paired.value("token").toString().toUtf8()),"local registration inventory contains names and IDs, never credentials");
    check(a.command({{"operation","revoke"},{"clientId",client}}).value("ok").toBool()&&rpc(endpoint,{{"op","status"}},paired.value("token").toString()).isEmpty(),"local revocation invalidates the client's credential");
    check(!rpc(endpoint,{{"op","status"}},second.value("token").toString()).value("active").toBool(),"revoking the owner cancels only its mesh session");
    for(int i=0;i<5;++i)rpc(endpoints[1],{{"op","pair"},{"clientId",client},{"clientName","wrong code"},{"code","bad"}});
    check(!rpc(endpoints[1],{{"op","pair"},{"clientId",client},{"clientName","rate test"},{"code",endpoints[1].value("pairingSecret")}}).value("ok").toBool(),"pairing failures have a bounded attempt rate");
    for(auto service:{&a,&b,&c})service->command({{"operation","stop"}});
    scan.start();finished=false;bool stopped=waitFor([&]{return finished;});
    for(const auto &entry:scan.instances())for(const auto &e:endpoints)if(entry.value("instanceId")==e.value("instanceId"))stopped=false;
    check(stopped,"stopped services no longer advertise themselves");
    return failures?1:0;
}
