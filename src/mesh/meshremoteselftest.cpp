#include "meshremotedialog.h"
#include "meshnode.h"
#include "meshwindow.h"
#include <QCheckBox>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHeaderView>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <functional>
#include <iostream>

namespace {
bool waitFor(const std::function<bool()> &condition,int milliseconds=8000){
    QElapsedTimer elapsed;elapsed.start();
    while(elapsed.elapsed()<milliseconds){
        QCoreApplication::processEvents(QEventLoop::AllEvents,20);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(condition())return true;QThread::msleep(5);
    }
    return condition();
}
Mesh::Project project(){
    Mesh::Project result;
    for(const auto &id:{"A","B","C"}){Mesh::Node node;node.id=id;node.name=QString::fromUtf8("Вузол ")+id;result.nodes.append(node);}
    Mesh::Link first;first.tx="A";first.rx="B";first.gainDb=0;result.links.append(first);
    Mesh::Link second;second.tx="B";second.rx="C";second.gainDb=0;result.links.append(second);return result;
}
bool isState(Mesh::RemoteDialog &dialog,const char *state){return dialog.property("remoteState").toString()==QLatin1String(state);}
void click(Mesh::RemoteDialog &dialog,const char *name){if(auto button=dialog.findChild<QPushButton *>(name)){waitFor([&]{return button->isEnabled();});button->click();}}
void initialize(Mesh::RemoteDialog &dialog,const QVector<QJsonObject> &services){
    dialog.setAttribute(Qt::WA_DeleteOnClose,false);dialog.setAttribute(Qt::WA_DontShowOnScreen);
    const auto nodes=dialog.endpointConfiguration().value("nodes").toArray();
    for(int index=0;index<nodes.size()&&index<services.size();++index){
        const auto id=nodes[index].toObject().value("id").toString();
        dialog.findChild<QSpinBox *>("remotePort_"+id)->setValue(services[index].value("port").toInt());
        dialog.findChild<QLineEdit *>("remoteToken_"+id)->setText(services[index].value("token").toString());
    }
    dialog.show();QCoreApplication::processEvents();
}
bool hasResult(Mesh::RemoteDialog &dialog,const QString &id,const QString &status){
    for(const auto &value:dialog.experimentResults().value("results").toArray()){
        const auto result=value.toObject();if(result.value("id")==id&&result.value("status")==status)return true;
    }
    return false;
}
bool allInactive(Mesh::NodeService &a,Mesh::NodeService &b,Mesh::NodeService &c){
    return !a.status().value("active").toBool()&&!b.status().value("active").toBool()&&!c.status().value("active").toBool();
}
bool allListening(Mesh::NodeService &a,Mesh::NodeService &b,Mesh::NodeService &c){
    return a.status().value("listening").toBool()&&b.status().value("listening").toBool()&&c.status().value("listening").toBool();
}
void editLine(QLineEdit *field,const QString &text){field->setText(text);QMetaObject::invokeMethod(field,"textEdited",Q_ARG(QString,text));}
QJsonObject discovered(const QJsonObject &service,const QString &name="Loopback test"){
    return {{"protocol","obrii-mesh-node"},{"version",1},{"host","127.0.0.1"},{"port",service.value("port")},
        {"instanceId",service.value("instanceId")},{"name",name},{"active",false},{"availableBearers",QJsonArray{"data","iq"}}};
}
bool omitsSecrets(const QByteArray &document,const QStringList &secrets){
    for(const auto &secret:secrets)if(!secret.isEmpty()&&document.contains(secret.toUtf8()))return false;
    for(const auto key:{"token","key","pairingSecret","code","peers"})if(document.contains(QByteArray('"'+QByteArray(key)+'"')))return false;
    return true;
}

// Delays applying configure, not just its reply, to exercise release-before-configure.
class DelayedService : public QObject {
public:
    QTcpServer server;
    quint64 revision=0;
    QString token="test-only-token-never-export",instance=QUuid::createUuid().toString(QUuid::WithoutBraces),session,key,node,transport;
    int configures=0,releases=0,statuses=0,delay=250,version=1;
    bool oversized=false,silent=false;
    bool allowPairing=false,pairHadToken=false;
    int pairs=0;
    QString pairCode="123456",pairedToken="test-paired-credential-never-export";
    bool advertiseOptions=false;
    int protocolCommands=0;
    QJsonObject configuredTopology,mockNative,lastProtocolArgs,lastSend;
    QJsonArray mockEvents,mockPorts,modems=QJsonArray{"afsk1200","lora"};
    explicit DelayedService(){
        server.listen(QHostAddress::LocalHost,0);
        connect(&server,&QTcpServer::newConnection,this,[this]{while(auto socket=server.nextPendingConnection()){
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                if(!socket->canReadLine())return;
                const auto request=QJsonDocument::fromJson(socket->readLine()).object();
                const auto operation=request.value("op").toString();
                if(operation=="pair"&&allowPairing){
                    ++pairs;pairHadToken=request.contains("token");const QPointer<QTcpSocket> guard(socket);
                    const auto valid=request.value("instanceId")==instance&&request.value("code")==pairCode&&!QUuid(request.value("clientId").toString()).isNull();
                    QTimer::singleShot(delay,this,[this,guard,request,valid]{
                        if(!guard)return;
                        if(!valid){respond(guard,request,{{"ok",false},{"error","invalid pairing"}});return;}
                        respond(guard,request,{{"ok",true},{"protocol","obrii-mesh-node"},{"version",1},{"instanceId",instance},{"token",pairedToken},{"registrationId",request.value("clientId")}});
                    });return;
                }
                if(request.value("token")!=token){socket->abort();return;}
                if(operation=="status"){
                    ++statuses;if(silent)return;
                    if(oversized){socket->write(QByteArray(128*1024+1,'x')+'\n');socket->disconnectFromHost();return;}
                    respond(socket,request,status());return;
                }
                if(request.value("instanceId")!=instance){respond(socket,request,{{"ok",false},{"error","instance mismatch"}});return;}
                if(operation=="configure"){
                    ++configures;configuredTopology=request.value("topology").toObject();const QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(delay,this,[this,guard,request]{
                        if(!session.isEmpty()){if(guard)respond(guard,request,{{"ok",false},{"error","active"}});return;}
                        session=request.value("session").toString();key=request.value("key").toString();node=request.value("nodeId").toString();transport=request.value("transport").toString();revision=1;
                        if(guard)respond(guard,request,status());
                    });return;
                }
                if(operation=="updateTopology"){
                    if(!advertiseOptions||request.value("session")!=session||request.value("expectedRevision").toString()!=QString::number(revision)){respond(socket,request,{{"ok",false},{"error","invalid topology ownership/revision"}});return;}
                    configuredTopology=request.value("topology").toObject();++revision;respond(socket,request,status());return;
                }
                if(operation=="protocol"){
                    ++protocolCommands;lastProtocolArgs=request.value("args").toObject();auto reply=status();
                    reply["protocolReply"]=QJsonObject{{"ok",true},{"pending",true},{"nativeId",77},{"key","MOCK-PEER-KEY"},{"pythonPath","C:/private/python.exe"}};
                    respond(socket,request,reply);return;
                }
                if(operation=="send"){lastSend=request;respond(socket,request,status());return;}
                if(operation=="release"){
                    ++releases;if(!session.isEmpty()&&request.value("session")!=session){respond(socket,request,{{"ok",false},{"error","ownership"}});return;}
                    session.clear();respond(socket,request,status());
                }
            });
        }});
    }
    QJsonObject credentials() const {return {{"token",token},{"port",server.serverPort()},{"instanceId",instance}};}
private:
    QJsonObject status() const {
        QJsonObject result{{"ok",true},{"protocol","obrii-mesh-node"},{"version",version},{"instanceId",instance},
            {"listening",true},{"active",!session.isEmpty()},{"session",session},{"nodeId",node},{"transport",transport},{"results",mockEvents}};
        if(advertiseOptions){result["topologyRevision"]=QString::number(revision);result["liveTopologyVersion"]=1;result["portBindingsVersion"]=1;result["perLinkBearers"]=true;result["nativeProtocols"]=QJsonArray{"lab","meshtastic","reticulum"};result["availableModems"]=modems;result["availableBearers"]=QJsonArray{"data","iq"};}
        if(!mockNative.isEmpty())result["nativeProtocol"]=mockNative;if(!mockPorts.isEmpty())result["ports"]=mockPorts;return result;
    }
    static void respond(QTcpSocket *socket,const QJsonObject &request,QJsonObject response){
        response["requestId"]=request.value("requestId");socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact)+'\n');socket->disconnectFromHost();
    }
};
}

int main(int argc,char **argv){
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))qputenv("QT_QPA_PLATFORM","offscreen");
    QApplication app(argc,argv);QTemporaryDir settingsDirectory;int failures=0;
    if(!settingsDirectory.isValid())return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsDirectory.path());
    auto check=[&](bool ok,const char *name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<std::endl;if(!ok)++failures;};
    {
        const auto legacy=Mesh::example("local").json();Mesh::Project restored;QString error;
        check(Mesh::Project::parse(legacy,&restored,&error)&&restored.json()==legacy&&restored.nodes[0].ports.isEmpty()&&restored.links[0].bearer=="inherit"&&restored.links[0].txPort=="radio","v1 projects roundtrip without adding optional fields");
        restored.nodes[0].ports=Mesh::defaultNodePorts();restored.nodes[0].protocol={{"type","meshtastic"},{"nodeNumber",256},{"role","router"},{"privateKey","NODE-PRIVATE-SECRET"},{"pythonPath","C:/private/python.exe"}};
        restored.links[0].bearer="iq";restored.links[0].modem={{"type","lora"},{"sf",7},{"bw",125000},{"cr",1},{"os",1},{"preamble",16},{"syncWord",43},{"key","MODEM-SECRET"}};
        const auto extended=restored.json();Mesh::Project parsed;
        check(Mesh::validateNodeOptions(restored.nodes[0]).isEmpty()&&Mesh::Project::parse(extended,&parsed,&error)&&parsed.json()==extended,"optional directional ports, native protocol and LoRa modem parameters roundtrip in schema v1");
        check(omitsSecrets(QJsonDocument(extended).toJson(),{"NODE-PRIVATE-SECRET","MODEM-SECRET","C:/private/python.exe"}),"project persistence excludes private protocol keys and host-local Python paths");
        auto invalid=restored.nodes[0];invalid.ports.append(invalid.ports.first());check(!Mesh::validateNodeOptions(invalid).isEmpty(),"duplicate same-direction ports are rejected while RX and TX may share radio ID");
        auto bad=restored.links[0];bad.modem["sf"]=13;check(!Mesh::validateLinkOptions(bad).isEmpty(),"out-of-range LoRa parameters are rejected before RPC");
        Mesh::RemoteDialog snapshot(restored);snapshot.setAttribute(Qt::WA_DeleteOnClose,false);snapshot.setAttribute(Qt::WA_DontShowOnScreen);
        const auto topology=snapshot.experimentResults().value("topology").toObject();
        check(topology.value("nodes").toArray().first().toObject().value("ports").toArray().size()==2&&topology.value("nodes").toArray().first().toObject().value("protocol").toObject().value("type")=="meshtastic"&&topology.value("links").toArray().first().toObject().value("modem").toObject().value("type")=="lora","remote topology snapshot preserves ports, protocols and per-link modem selectors");
        check(omitsSecrets(QJsonDocument(snapshot.experimentResults()).toJson(),{"NODE-PRIVATE-SECRET","MODEM-SECRET","C:/private/python.exe"}),"remote snapshot exports no protocol private keys or host-local runtime paths");
    }
    {
        Mesh::Window window(settingsDirectory.filePath("ui-devices.json"));window.setAttribute(Qt::WA_DontShowOnScreen);window.show();app.processEvents();
        auto view=window.findChild<QGraphicsView *>("topologyCanvas");for(auto item:view->scene()->items())if(item->data(0)=="node"&&item->data(1)=="node1")item->setSelected(true);
        auto rx=window.findChild<QComboBox *>("nodeRxBackend"),tx=window.findChild<QComboBox *>("nodeTxBackend");
        check(rx&&tx&&rx->currentData()=="network"&&tx->currentData()=="network"&&qobject_cast<QStandardItemModel *>(rx->model())->item(1)->isEnabled()&&qobject_cast<QStandardItemModel *>(tx->model())->item(1)->isEnabled(),"node inspector exposes independent RX/TX configuration; remote attach requires local radio permissions");
        editLine(window.findChild<QLineEdit *>("nodeRxPortId"),"listen");editLine(window.findChild<QLineEdit *>("nodeTxPortId"),"uplink");window.findChild<QCheckBox *>("nodeRxPortEnabled")->setChecked(false);
        const auto ports=window.currentProject().nodes[0].ports;
        check(ports.size()==2&&ports[0].toObject().value("id")=="listen"&&!ports[0].toObject().value("enabled").toBool()&&ports[1].toObject().value("id")=="uplink"&&ports[1].toObject().value("enabled").toBool(),"RX and TX settings do not overwrite the opposite direction");
        auto protocol=window.findChild<QComboBox *>("nodeProtocol");protocol->setCurrentIndex(protocol->findData("meshtastic"));
        check(window.currentProject().nodes[0].protocol.value("nodeNumber")==256&&window.currentProject().nodes[0].protocol.value("relayRole")=="router"&&qobject_cast<QStandardItemModel *>(protocol->model())->item(3)->isEnabled(),"Meshtastic defaults avoid reserved IDs; LoRaWAN selection still requires the host capability");
        view->scene()->clearSelection();for(auto item:view->scene()->items())if(item->data(0)=="link"&&item->data(1)=="node1/node2")item->setSelected(true);
        auto bearer=window.findChild<QComboBox *>("linkBearer"),modem=window.findChild<QComboBox *>("linkModem");bearer->setCurrentIndex(bearer->findData("iq"));modem->setCurrentIndex(modem->findData("lora"));window.findChild<QSpinBox *>("linkLoraSf")->setValue(9);
        editLine(window.findChild<QLineEdit *>("linkTxPort"),"uplink");
        check(window.currentProject().links[0].bearer=="iq"&&window.currentProject().links[0].modem.value("type")=="lora"&&window.currentProject().links[0].modem.value("sf")==9&&window.currentProject().links[0].txPort=="uplink","link inspector edits bearer, port binding and LoRa parameters");
        if(argc>1)check(window.grab().save(QString::fromLocal8Bit(argv[1])+"-ports-inspector.png"),"offscreen ports and modem inspector screenshot");
        QString error;check(window.saveProjectFile(settingsDirectory.filePath("ports-ui.json"),&error),"new inspector settings save through the production v1 project path");
    }
    Mesh::NodeService a,b,c;const QJsonObject start{{"operation","start"},{"bind","127.0.0.1"},{"port",0}};
    const QVector<QJsonObject> services{a.command(start),b.command(start),c.command(start)};
    check(services[0].value("ok").toBool()&&services[1].value("ok").toBool()&&services[2].value("ok").toBool(),"three independent worker-thread services start on ephemeral loopback ports");
    if(!allListening(a,b,c))return 1;
    QStringList privateValues;for(const auto &service:services)privateValues<<service.value("token").toString()<<service.value("pairingSecret").toString();
    QVector<QJsonObject> entries;for(int index=0;index<services.size();++index)entries.append(discovered(services[index],"Local service "+QString::number(index+1)));
    QString firstRegisteredToken;
    {
        Mesh::RemoteDialog registration(project());initialize(registration,{});bool finished=false;
        QObject::connect(&registration,&Mesh::RemoteDialog::discoveryFinished,&app,[&]{finished=true;});
        QString problem;check(registration.searchInstances(&problem)&&waitFor([&]{return finished;},5000),"bounded LAN discovery completes asynchronously");
        bool foundAll=true;
        for(int index=0;index<services.size();++index){bool found=false;for(const auto &entry:registration.discoveredInstances())if(entry.value("instanceId")==services[index].value("instanceId")){entries[index]=entry;found=true;break;}foundAll&=found;}
        check(foundAll,"search auto-populates the three explicitly started loopback services");
        check(registration.findChildren<QInputDialog *>().isEmpty(),"discovery does not spontaneously prompt for credentials");
        QJsonArray publicEntries;for(const auto &entry:registration.discoveredInstances())publicEntries.append(entry);
        check(omitsSecrets(QJsonDocument(publicEntries).toJson(),privateValues),"discovery entries contain no tokens or pairing codes");
        check(registration.assignDiscoveredInstance(0,entries[0],&problem)&&registration.findChild<QLineEdit *>("remoteHost_A")->text()==entries[0].value("host").toString()&&
            registration.findChild<QSpinBox *>("remotePort_A")->value()==services[0].value("port").toInt()&&registration.findChild<QLineEdit *>("remoteToken_A")->text().isEmpty(),"assigning an instance fills address and port without copying an unrelated token");
        auto selector=registration.findChild<QComboBox *>("remoteInstance_A");check(selector&&selector->count()>=4&&selector->currentIndex()>0,"each node has a discovered-instance selector and a manual option");
        int replies=0;bool lastOk=false;
        QObject::connect(&registration,&Mesh::RemoteDialog::instanceRegistrationFinished,&app,[&](int,bool ok,const QString &){++replies;lastOk=ok;});
        check(registration.registerDiscoveredInstance(0,entries[0],services[0].value("pairingSecret").toString(),&problem)&&waitFor([&]{return replies==1;})&&lastOk,"service-local pairing code obtains a client-specific credential from a real service");
        firstRegisteredToken=registration.findChild<QLineEdit *>("remoteToken_A")->text();privateValues<<firstRegisteredToken;
        check(!firstRegisteredToken.isEmpty()&&firstRegisteredToken!=services[0].value("token").toString(),"registration credential is distinct from the manual admin token");
        check(registration.assignDiscoveredInstance(1,entries[0],&problem)&&registration.findChild<QLineEdit *>("remoteToken_B")->text()==firstRegisteredToken,"the same assigned instance reuses its cached credential in another row");
        click(registration,"remoteStart");check(isState(registration,"idle")&&allInactive(a,b,c),"registered instance cannot occupy two graph roles");
        const auto code=services[1].value("pairingSecret").toString();const auto wrongCode=code=="000000"?QString("000001"):QString("000000");
        check(registration.registerDiscoveredInstance(1,entries[1],wrongCode,&problem)&&waitFor([&]{return replies==2;})&&!lastOk&&registration.findChild<QLineEdit *>("remoteToken_B")->text().isEmpty(),"invalid local code is rejected without caching credentials");
        check(registration.registerDiscoveredInstance(1,entries[1],code,&problem)&&waitFor([&]{return replies==3;})&&lastOk,"user can retry registration manually after a bad code");
        check(registration.registerDiscoveredInstance(2,entries[2],services[2].value("pairingSecret").toString(),&problem)&&waitFor([&]{return replies==4;})&&lastOk,"third real instance registers independently");
        privateValues<<registration.findChild<QLineEdit *>("remoteToken_B")->text()<<registration.findChild<QLineEdit *>("remoteToken_C")->text();
        click(registration,"remotePair_A");auto prompt=registration.findChild<QInputDialog *>("remotePairingCode");
        check(prompt&&prompt->textEchoMode()==QLineEdit::Password&&prompt->testAttribute(Qt::WA_DontShowOnScreen),"explicit registration prompt is masked and remains offscreen in tests");if(prompt)prompt->reject();app.processEvents();
        click(registration,"remoteStart");check(waitFor([&]{return isState(registration,"attached");}),"registered credentials authenticate ordinary configure requests");
        if(isState(registration,"attached")){
            registration.findChild<QLineEdit *>("remotePacketId")->setText("paired_data");click(registration,"remoteSend");
            check(waitFor([&]{return hasResult(registration,"paired_data","delivered");}),"registered services deliver real A-B-C packets without starting RF");
            check(omitsSecrets(QJsonDocument(registration.experimentResults()).toJson(),privateValues)&&omitsSecrets(QJsonDocument(registration.endpointConfiguration()).toJson(),privateValues),"registered experiment and endpoint exports omit all credentials and codes");
        }
        click(registration,"remoteStop");check(waitFor([&]{return isState(registration,"idle")&&allInactive(a,b,c);}),"registered owner releases only its own session");
        if(argc>1){const auto prefix=QString::fromLocal8Bit(argv[1]);check(registration.grab().save(prefix+"-discovery.png"),"discovery and pairing desktop screenshot");registration.resize(660,620);app.processEvents();check(registration.grab().save(prefix+"-discovery-compact.png"),"discovery and pairing compact screenshot");}
    }
    {
        Mesh::RemoteDialog reopened(project());initialize(reopened,{});QString problem;
        check(reopened.discoveredInstances().size()>=3&&reopened.assignDiscoveredInstance(0,entries[0],&problem)&&reopened.findChild<QLineEdit *>("remoteToken_A")->text()==firstRegisteredToken,"discovered registry and credentials survive closing and reopening the dialog in the same process");
        QSettings settings(QSettings::defaultFormat(),QSettings::UserScope,"Obrii","ObriiMeshDesigner");settings.sync();
        check(settings.allKeys()==QStringList{"meshRemote/clientId"}&&!QUuid(settings.value("meshRemote/clientId").toString()).isNull(),"only a stable non-secret client UUID is persisted in settings");
        auto imported=reopened.endpointConfiguration();QString error;
        check(reopened.setEndpointConfiguration(imported,&error)&&reopened.findChild<QLineEdit *>("remoteToken_A")->text().isEmpty(),"imported endpoint addresses never restore in-memory credentials implicitly");
    }
    {
        auto snapshot=project();Mesh::RemoteDialog dialog(snapshot);initialize(dialog,services);snapshot.links.clear();
        check(!dialog.isModal()&&allInactive(a,b,c),"modeless constructor does not configure or start any radio");
        check(dialog.experimentResults().value("topology").toObject().value("links").toArray().size()==2,"project topology is an immutable snapshot");
        bool masked=true;for(const auto &node:project().nodes)masked&=dialog.findChild<QLineEdit *>("remoteToken_"+node.id)->echoMode()==QLineEdit::Password;
        check(masked,"all admin tokens use password echo mode");
        const auto endpoints=dialog.endpointConfiguration();const auto encoded=QJsonDocument(endpoints).toJson();bool credentialFree=!encoded.contains("token");
        for(const auto &service:services)credentialFree&=!encoded.contains(service.value("token").toString().toUtf8());
        check(credentialFree,"endpoint document contains no credentials");
        auto host=dialog.findChild<QLineEdit *>("remoteHost_A");auto port=dialog.findChild<QSpinBox *>("remotePort_A");auto token=dialog.findChild<QLineEdit *>("remoteToken_A");
        host->setText("localhost");check(token->text().isEmpty(),"changing endpoint host clears its old admin token");
        host->setText("127.0.0.1");token->setText(services[0].value("token").toString());port->setValue(port->value()==65535?65534:port->value()+1);
        check(token->text().isEmpty(),"changing endpoint port clears its old admin token");port->setValue(services[0].value("port").toInt());token->setText(services[0].value("token").toString());
        auto bearer=dialog.findChild<QComboBox *>("remoteBearer");auto model=qobject_cast<QStandardItemModel *>(bearer->model());
        check(model&&!model->item(2)->isEnabled(),"physical SDR and LoRa option is unavailable");
        click(dialog,"remoteStart");check(waitFor([&]{return isState(dialog,"attached");}),"controller authenticates and pins all three independent instances before configuring");
        if(isState(dialog,"attached")){
            const auto ownSession=a.status().value("session").toString();
            Mesh::RemoteDialog contender(project());initialize(contender,services);click(contender,"remoteStart");
            check(waitFor([&]{return isState(contender,"attached")&&!contender.findChild<QLabel *>("remoteErrorLabel")->text().isEmpty();})&&a.status().value("session")==ownSession&&b.status().value("session")==ownSession&&c.status().value("session")==ownSession,
                "busy endpoints are rejected without stealing or releasing the existing owner");
            auto id=dialog.findChild<QLineEdit *>("remotePacketId");auto input=dialog.findChild<QLineEdit *>("remotePayloadInput");auto ttl=dialog.findChild<QSpinBox *>("remoteTtl");
            id->setText("remote_data");input->setText("REMOTE DATA");click(dialog,"remoteSend");
            check(waitFor([&]{return hasResult(dialog,"remote_data","delivered");}),"real service nodes independently forward A-B-C data");
            bool path=false,payload=false;for(const auto &value:dialog.experimentResults().value("results").toArray()){
                const auto result=value.toObject();if(result.value("id")=="remote_data"&&result.value("status")=="delivered"){
                    path=result.value("path").toArray()==QJsonArray{"A","B","C"};payload=QByteArray::fromBase64(result.value("payloadBase64").toString().toLatin1())=="REMOTE DATA";
                }
            }
            check(path&&payload,"telemetry inspector preserves actual path and payload");
            const auto exportData=QJsonDocument(dialog.experimentResults()).toJson();credentialFree=!exportData.contains(ownSession.toUtf8())&&!exportData.contains("\"key\"")&&!exportData.contains("\"token\"");
            for(const auto &service:services)credentialFree&=!exportData.contains(service.value("token").toString().toUtf8());
            check(credentialFree,"experiment export omits tokens and session ownership");
            input->setText(QString(4097,'x'));id->setText("too_large");click(dialog,"remoteSend");
            check(!dialog.findChild<QLabel *>("remoteErrorLabel")->text().isEmpty()&&!hasResult(dialog,"too_large","delivered"),"data payload over 4096 bytes is rejected before RPC");
            input->setText(QString(4096,'x'));id->setText("maximum_data");click(dialog,"remoteSend");
            check(waitFor([&]{return hasResult(dialog,"maximum_data","delivered");}),"maximum 4096-byte data payload fits bounded status RPC");
            check(waitFor([&]{return dialog.findChild<QPushButton *>("remoteSend")->isEnabled();}),"send control recovers after async reply");
            input->setText("TTL");id->setText("zero_ttl");ttl->setValue(0);click(dialog,"remoteSend");
            check(waitFor([&]{return hasResult(dialog,"zero_ttl","ttl_expired");}),"TTL zero reports a node-side forwarding error");ttl->setValue(16);
            if(argc>1){
                const auto prefix=QString::fromLocal8Bit(argv[1]);check(dialog.grab().save(prefix+"-desktop.png"),"desktop offscreen screenshot");
                dialog.resize(660,620);app.processEvents();check(dialog.grab().save(prefix+"-compact.png"),"compact offscreen screenshot");
                auto results=dialog.findChild<QTableWidget *>("remoteResults");check(!results->geometry().intersects(input->geometry()),"compact packet form and results do not overlap");
            }
        }
        click(dialog,"remoteStop");check(waitFor([&]{return isState(dialog,"idle")&&allInactive(a,b,c);}),"stop releases all owned sessions without stopping services");
        check(allListening(a,b,c),"user-owned services remain listening after detach");
        QString error;check(dialog.setEndpointConfiguration(endpoints,&error)&&dialog.findChild<QLineEdit *>("remoteToken_A")->text().isEmpty(),"address import clears credentials instead of loading or preserving stale tokens");
    }
    {
        Mesh::RemoteDialog duplicate(project());initialize(duplicate,services);
        duplicate.findChild<QLineEdit *>("remoteHost_B")->setText("localhost");
        duplicate.findChild<QSpinBox *>("remotePort_B")->setValue(services[0].value("port").toInt());
        duplicate.findChild<QLineEdit *>("remoteToken_B")->setText(services[0].value("token").toString());
        click(duplicate,"remoteStart");
        check(waitFor([&]{return isState(duplicate,"idle");})&&allInactive(a,b,c)&&!duplicate.findChild<QLabel *>("remoteErrorLabel")->text().isEmpty(),"aliases of the same instance are rejected before configuring any endpoint");
    }
    {
        Mesh::RemoteDialog unauthorized(project());initialize(unauthorized,services);unauthorized.findChild<QLineEdit *>("remoteToken_B")->setText("wrong-token");
        click(unauthorized,"remoteStart");check(waitFor([&]{return isState(unauthorized,"attached")&&a.status().value("active").toBool()&&!b.status().value("active").toBool()&&c.status().value("active").toBool();})&&!unauthorized.findChild<QLabel *>("remoteErrorLabel")->text().isEmpty(),"authentication failure isolates only that endpoint while authorized endpoints stay attached");
        click(unauthorized,"remoteStop");check(waitFor([&]{return isState(unauthorized,"idle")&&allInactive(a,b,c);}),"partial attach releases only its authorized owned endpoints");
    }
    {
        Mesh::RemoteDialog iq(project());initialize(iq,services);iq.findChild<QComboBox *>("remoteBearer")->setCurrentIndex(1);click(iq,"remoteStart");
        check(waitFor([&]{return isState(iq,"attached");}),"IQ remote mode configures the same independent services");
        if(isState(iq,"attached")){
            auto input=iq.findChild<QLineEdit *>("remotePayloadInput");auto id=iq.findChild<QLineEdit *>("remotePacketId");
            input->setText(QString(241,'q'));id->setText("iq_oversize");click(iq,"remoteSend");
            check(!iq.findChild<QLabel *>("remoteErrorLabel")->text().isEmpty(),"IQ mode rejects payload over 240 bytes");
            input->setText("IQ REMOTE");id->setText("iq_remote");click(iq,"remoteSend");
            const bool delivered=waitFor([&]{return hasResult(iq,"iq_remote","delivered");},25000);
            if(!delivered){
                std::cerr<<QJsonDocument(iq.experimentResults()).toJson().toStdString();
                for(auto service:{&a,&b,&c}){auto state=service->status();state.remove("session");std::cerr<<QJsonDocument(state).toJson().toStdString();}
            }
            check(delivered,"peer-to-peer CF32 AFSK/FM delivers A-B-C through worker threads");
            check(a.status().value("iqTxSamples").toString().toULongLong()>0&&c.status().value("iqRxSamples").toString().toULongLong()>0,"IQ telemetry includes actual transmitted and received samples");
        }
        click(iq,"remoteStop");check(waitFor([&]{return isState(iq,"idle")&&allInactive(a,b,c);}),"IQ detach also leaves user services alive");
    }
    auto one=project();one.nodes.resize(1);one.links.clear();
    {
        DelayedService old;auto configured=one;configured.nodes[0].ports=Mesh::defaultNodePorts();configured.nodes[0].protocol={{"type","meshtastic"}};
        Mesh::RemoteDialog unsupported(configured);initialize(unsupported,{old.credentials()});click(unsupported,"remoteStart");
        check(waitFor([&]{return isState(unsupported,"attached")&&!unsupported.findChild<QLabel *>("remoteErrorLabel")->text().isEmpty();})&&old.configures==0,"legacy service without port/protocol capabilities is isolated before configure");
    }
    {
        DelayedService a,b;auto configured=project();configured.nodes.resize(2);configured.links.resize(1);configured.links[0].bearer="iq";configured.links[0].modem={{"type","lora"}};
        Mesh::RemoteDialog unsupported(configured);initialize(unsupported,{a.credentials(),b.credentials()});click(unsupported,"remoteStart");
        check(waitFor([&]{return isState(unsupported,"attached")&&!unsupported.findChild<QLabel *>("remoteErrorLabel")->text().isEmpty();})&&a.configures==0&&b.configures==0,"per-link overrides cannot be silently ignored by old services");
    }
    {
        DelayedService a,b;a.advertiseOptions=b.advertiseOptions=true;a.modems=QJsonArray{"afsk1200"};auto configured=project();configured.nodes.resize(2);configured.links.resize(1);configured.links[0].bearer="iq";configured.links[0].modem={{"type","lora"}};
        Mesh::RemoteDialog unsupported(configured);initialize(unsupported,{a.credentials(),b.credentials()});click(unsupported,"remoteStart");
        check(waitFor([&]{return isState(unsupported,"attached")&&!unsupported.findChild<QLabel *>("remoteErrorLabel")->text().isEmpty();})&&a.configures==0,"unsupported LoRa endpoint is isolated without forcing the other endpoint to stop");
    }
    {
        DelayedService service;service.advertiseOptions=true;
        service.mockNative={{"type","meshtastic"},{"ready",true},{"nativeAddress","!00000100"},{"nodeNumber",256},{"maxPayload",240},{"privateKey","NATIVE-PRIVATE-KEY"},{"pythonPath","C:/private/python.exe"}};
        for(const auto &value:Mesh::defaultNodePorts()){auto port=value.toObject();port["ready"]=true;port["token"]="PORT-PRIVATE-TOKEN";service.mockPorts.append(port);}
        service.mockEvents={QJsonObject{{"protocol","meshtastic"},{"node","A"},{"localMs",10},{"serial","1"},{"status","received"},{"nativeId",77},{"from",257},{"to",256},{"payloadBase64",QString::fromLatin1(QByteArray("NATIVE UI").toBase64())},{"privateKey","EVENT-PRIVATE-KEY"}}};
        auto configured=one;configured.nodes[0].ports=Mesh::defaultNodePorts();configured.nodes[0].protocol={{"type","meshtastic"}};
        Mesh::RemoteDialog native(configured);initialize(native,{service.credentials()});click(native,"remoteStart");
        check(waitFor([&]{return isState(native,"attached");}),"capability-confirmed native node attaches with independent directional ports");
        if(isState(native,"attached")){
            const auto node=service.configuredTopology.value("nodes").toArray().first().toObject();
            check(node.value("ports").toArray().size()==2&&node.value("protocol").toObject().value("nodeNumber")==256&&node.value("protocol").toObject().value("relayRole")=="router","configure RPC preserves ports and supplies deterministic Meshtastic defaults");
            const auto events=native.experimentResults().value("results").toArray();const auto event=events.isEmpty()?QJsonObject():events.first().toObject();
            check(event.value("nativeId")==77&&event.value("from")==257&&event.value("to")==256&&!event.contains("path")&&!event.contains("source"),"native results keep real protocol IDs without inventing lab paths or graph addresses");
            check(native.findChild<QTableWidget *>("remotePorts")->rowCount()==2&&native.findChild<QLabel *>("remoteNativeState")->text().contains("!00000100"),"native identity and independently reported ports are visible in separate panels");
            bool replied=false,accepted=false;QJsonObject publicReply;QString error;
            QObject::connect(&native,&Mesh::RemoteDialog::protocolCommandFinished,&app,[&](int,bool ok,const QJsonObject &reply,const QString &){replied=true;accepted=ok;publicReply=reply;});
            check(waitFor([&]{return native.findChild<QPushButton *>("remoteProtocolSend")->isEnabled();})&&native.executeProtocolCommand(0,{{"op","send"},{"text","NATIVE COMMAND"},{"privateKey","COMMAND-PRIVATE-KEY"}},&error)&&waitFor([&]{return replied;})&&accepted&&service.lastProtocolArgs.value("text")=="NATIVE COMMAND"&&publicReply.value("protocolReply").toObject().value("pending").toBool(),"protocol panel sends args asynchronously and decodes wrapped pending command replies");
            check(omitsSecrets(QJsonDocument(publicReply).toJson(),{"MOCK-PEER-KEY","COMMAND-PRIVATE-KEY","C:/private/python.exe"}),"native command replies are explicitly projected without peer keys or host paths");
            native.findChild<QLineEdit *>("remoteNativeDestination")->setText("!00000101");click(native,"remoteSend");
            check(waitFor([&]{return service.lastSend.value("nativeDestination")=="!00000101"&&native.findChild<QPushButton *>("remoteSend")->isEnabled();}),"native source send carries an explicit nativeDestination without altering the lab send contract");
            check(omitsSecrets(QJsonDocument(native.experimentResults()).toJson(),{"NATIVE-PRIVATE-KEY","PORT-PRIVATE-TOKEN","EVENT-PRIVATE-KEY","COMMAND-PRIVATE-KEY","C:/private/python.exe"}),"native telemetry and results export no keys, command bodies or host-local sidecar paths");
            if(argc>1)check(native.grab().save(QString::fromLocal8Bit(argv[1])+"-native-protocol.png"),"native protocol offscreen screenshot");
        }
        click(native,"remoteStop");check(waitFor([&]{return isState(native,"idle")&&service.session.isEmpty();}),"native command session releases cleanly");
    }
    {
        DelayedService service;service.allowPairing=true;Mesh::RemoteDialog changing(one);initialize(changing,{});QString error;bool replied=false,accepted=true;
        QObject::connect(&changing,&Mesh::RemoteDialog::instanceRegistrationFinished,&app,[&](int,bool ok,const QString &){replied=true;accepted=ok;});
        changing.findChild<QLineEdit *>("remoteToken_A")->setText("old-unrelated-admin-credential");
        check(changing.registerDiscoveredInstance(0,discovered(service.credentials()),service.pairCode,&error)&&waitFor([&]{return service.pairs==1;}),"pairing request can remain in flight");
        changing.findChild<QLineEdit *>("remoteHost_A")->setText("127.0.0.2");
        check(waitFor([&]{return replied;})&&!accepted&&!service.pairHadToken&&changing.findChild<QLineEdit *>("remoteToken_A")->text().isEmpty(),"changing host discards a late pairing response and never sends the old admin token");
        check(changing.assignDiscoveredInstance(0,discovered(service.credentials()),&error)&&changing.findChild<QLineEdit *>("remoteToken_A")->text().isEmpty(),"cancelled registration is not added to the process credential cache");
    }
    {
        DelayedService service;service.allowPairing=true;QPointer<Mesh::RemoteDialog> closing=new Mesh::RemoteDialog(one);initialize(*closing,{});closing->setAttribute(Qt::WA_DeleteOnClose);QString error;
        check(closing->registerDiscoveredInstance(0,discovered(service.credentials()),service.pairCode,&error)&&waitFor([&]{return service.pairs==1;}),"close guard test has an outstanding registration");
        closing->close();Mesh::RemoteDialog reopened(one);initialize(reopened,{});reopened.assignDiscoveredInstance(0,discovered(service.credentials()),&error);
        check(waitFor([&]{return closing.isNull()&&reopened.findChild<QPushButton *>("remotePair_A")->isEnabled();})&&reopened.findChild<QLineEdit *>("remoteToken_A")->text().isEmpty(),"closing the origin dialog discards the token and clears the process-wide pending guard");
    }
    {
        Mesh::NodeService restarting;auto original=restarting.command(start);Mesh::RemoteDialog restart(one);initialize(restart,{});QString error;int replies=0;bool accepted=false;
        QObject::connect(&restart,&Mesh::RemoteDialog::instanceRegistrationFinished,&app,[&](int,bool ok,const QString &){++replies;accepted=ok;});
        check(original.value("ok").toBool()&&restart.registerDiscoveredInstance(0,discovered(original),original.value("pairingSecret").toString(),&error)&&waitFor([&]{return replies==1;})&&accepted,"restart fixture registers a real service");
        const auto previous=restart.findChild<QLineEdit *>("remoteToken_A")->text();restarting.command({{"operation","stop"}});
        const auto replacement=restarting.command({{"operation","start"},{"bind","127.0.0.1"},{"port",original.value("port")}});
        check(replacement.value("ok").toBool()&&replacement.value("instanceId")!=original.value("instanceId")&&restart.assignDiscoveredInstance(0,discovered(replacement),&error)&&restart.findChild<QLineEdit *>("remoteToken_A")->text().isEmpty(),"new instanceId at the same host and port invalidates the old cached credential");
        check(restart.registerDiscoveredInstance(0,discovered(replacement),replacement.value("pairingSecret").toString(),&error)&&waitFor([&]{return replies==2;})&&accepted&&restart.findChild<QLineEdit *>("remoteToken_A")->text()!=previous,"restarted service requires registration and returns a fresh credential");
    }
    {
        DelayedService service;Mesh::RemoteDialog delayed(one);initialize(delayed,{service.credentials()});click(delayed,"remoteStart");
        check(waitFor([&]{return service.configures==1;}),"configure can be held in flight");click(delayed,"remoteStop");
        check(waitFor([&]{return isState(delayed,"idle")&&service.releases>=2&&service.session.isEmpty();}),"stop releases immediately and again after a late configure succeeds");
        const int polls=service.statuses;QTimer heartbeat;int ticks=0;QObject::connect(&heartbeat,&QTimer::timeout,&app,[&]{++ticks;});heartbeat.start(10);
        check(waitFor([&]{return ticks>120;},2500)&&service.statuses==polls,"detach stops polling while the GUI event loop remains responsive");
    }
    {
        DelayedService service;QPointer<Mesh::RemoteDialog> closing=new Mesh::RemoteDialog(one);initialize(*closing,{service.credentials()});
        closing->setAttribute(Qt::WA_DeleteOnClose);click(*closing,"remoteStart");check(waitFor([&]{return service.configures==1;}),"destructor test has an outstanding configure");
        closing->close();check(waitFor([&]{return closing.isNull()&&service.releases>=2&&service.session.isEmpty();}),"closing dialog preserves captured-owner cleanup after controller destruction");
    }
    {
        DelayedService service;service.silent=true;Mesh::RemoteDialog timedOut(one);initialize(timedOut,{service.credentials()});
        QTimer heartbeat;int ticks=0;QObject::connect(&heartbeat,&QTimer::timeout,&app,[&]{++ticks;});heartbeat.start(10);click(timedOut,"remoteStart");
        check(waitFor([&]{return isState(timedOut,"attached")&&!timedOut.findChild<QLabel *>("remoteErrorLabel")->text().isEmpty();})&&ticks>100&&service.configures==0,"RPC timeout marks the endpoint unavailable while the GUI and session stay responsive");
    }
    {
        DelayedService service;service.oversized=true;Mesh::RemoteDialog large(one);initialize(large,{service.credentials()});click(large,"remoteStart");
        check(waitFor([&]{return isState(large,"attached")&&large.findChild<QLabel *>("remoteErrorLabel")->text().contains("128");})&&service.configures==0,"oversized reply is rejected without globally stopping the controller");
    }
    {
        DelayedService service;service.version=2;Mesh::RemoteDialog future(one);initialize(future,{service.credentials()});click(future,"remoteStart");
        check(waitFor([&]{return isState(future,"attached")&&!future.findChild<QLabel *>("remoteErrorLabel")->text().isEmpty();})&&service.configures==0,"unsupported endpoint is isolated before configure");
    }
    {
        Mesh::RemoteDialog saved(one);initialize(saved,{});QString problem;
        const auto empty=saved.routedDiscoveryConfiguration();
        QJsonObject config{{"version",1},{"endpoints",QJsonArray{QJsonObject{{"enabled",true},{"role","directory"},{"name","Test directory"},{"host","192.0.2.10"},{"port",17690}}}}};
        check(saved.findChild<QPushButton *>("remoteDiscoverySettings")&&saved.setRoutedDiscoveryConfiguration(config,&problem),"production dialog saves explicit routed discovery addresses");
        Mesh::RemoteDialog reopened(one);initialize(reopened,{});
        QSettings settings(QSettings::defaultFormat(),QSettings::UserScope,"Obrii","ObriiMeshDesigner");settings.sync();
        check(reopened.routedDiscoveryConfiguration()==config&&QJsonDocument::fromJson(settings.value("meshRemote/routedDiscovery").toByteArray()).object()==config,"saved address book survives reopening and is persisted without credentials");
        auto invalid=config;invalid["token"]="must-not-be-saved";
        check(!saved.setRoutedDiscoveryConfiguration(invalid,&problem)&&saved.routedDiscoveryConfiguration()==config,"invalid credential-bearing discovery profile leaves saved addresses unchanged");
        check(!QJsonDocument(saved.endpointConfiguration()).toJson().contains("Test directory")&&!QJsonDocument(saved.experimentResults()).toJson().contains("Test directory"),"search address book is not silently included in public project or result exports");
        check(saved.setRoutedDiscoveryConfiguration(empty,&problem),"test restores its isolated search preferences");
    }
    check(allInactive(a,b,c)&&allListening(a,b,c),"all tests leave only inactive, listening test-owned services");
    std::cout<<failures<<" failure(s)"<<std::endl;return failures?1:0;
}
