#include "meshremotedialog.h"
#include "meshnode.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
void events(){QCoreApplication::processEvents(QEventLoop::AllEvents,20);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
bool waitFor(const std::function<bool()> &condition,int milliseconds=8000){
    QElapsedTimer timer;timer.start();
    do {events();if(condition())return true;QThread::msleep(5);}while(timer.elapsed()<milliseconds);
    return condition();
}
void require(bool okay,const char *name){
    if(!okay)throw std::runtime_error(name);
    std::printf("PASS %s\n",name);std::fflush(stdout);
}
template<class T> T *widget(Mesh::RemoteDialog &dialog,const QString &name){
    auto result=dialog.findChild<T *>(name);
    if(!result)throw std::runtime_error(("missing RemoteDialog control: "+name).toStdString());
    return result;
}
void click(Mesh::RemoteDialog &dialog,const QString &name){
    auto button=widget<QPushButton>(dialog,name);
    require(button->isEnabled(),"requested controller action is available in the UI");button->click();
}
bool attached(const Mesh::RemoteDialog &dialog){return dialog.sessionActive()&&dialog.property("remoteState").toString()=="attached";}
quint64 count(const QJsonValue &value){return value.isString()?value.toString().toULongLong():quint64(value.toDouble());}
Mesh::Node node(const QString &id){Mesh::Node result;result.id=id;result.name="Node "+id;return result;}
Mesh::Link link(const QString &tx,const QString &rx){Mesh::Link result;result.tx=tx;result.rx=rx;result.gainDb=0;result.delayMs=2;return result;}
Mesh::Project initialProject(){
    Mesh::Project result;for(const auto &id:{"A","B","C"})result.nodes.append(node(id));
    for(const auto &edge:{link("A","B"),link("B","C"),link("C","B"),link("B","A"),link("A","C"),link("C","A")})result.links.append(edge);
    return result;
}
bool nodeEnabled(const Mesh::Project &project,const QString &id){
    for(const auto &node:project.nodes)if(node.id==id)return node.enabled;
    throw std::runtime_error("node missing from currentProject");
}
bool linkEnabled(const Mesh::Project &project,const QString &tx,const QString &rx){
    for(const auto &link:project.links)if(link.tx==tx&&link.rx==rx)return link.enabled;
    throw std::runtime_error("link missing from currentProject");
}
QJsonObject result(const Mesh::RemoteDialog &dialog,const QString &id,const QString &status){
    for(const auto &value:dialog.experimentResults().value("results").toArray()){
        const auto event=value.toObject();if(event.value("id")==id&&event.value("status")==status)return event;
    }return {};
}
bool failureFor(const Mesh::RemoteDialog &dialog,const QString &id,int after){
    const auto errors=dialog.experimentResults().value("errors").toArray();
    for(int i=after;i<errors.size();++i){
        const auto event=errors[i].toObject();if(event.value("node")==id&&!event.value("error").toString().isEmpty())return true;
    }return false;
}
bool publicFields(const QJsonValue &value){
    if(value.isArray()){for(const auto &entry:value.toArray())if(!publicFields(entry))return false;}
    if(value.isObject()){
        const auto object=value.toObject();
        for(auto it=object.begin();it!=object.end();++it){
            const auto name=it.key().toLower();
            if(QStringList{"token","key","session","sessionkey","pairingsecret","code","peers","pskbase64","password","secret","protocolstatedirectory"}.contains(name)
                ||!publicFields(it.value()))return false;
        }
    }return true;
}

class Fixture {
public:
    struct Endpoint {
        QString id;
        QJsonObject startup;
        std::unique_ptr<Mesh::NodeService> service;
    };
    QTemporaryDir state;
    std::vector<std::unique_ptr<Endpoint>> endpoints;
    std::unique_ptr<Mesh::RemoteDialog> dialog;
    QTimer monitor;
    QString session;
    QJsonObject lastDirectToggle;
    int projectChanges=0,ticks=0;
    bool watch=false,isolated=true;
    Fixture(){require(state.isValid(),"isolated temporary service state root");}
    ~Fixture(){
        monitor.stop();watch=false;
        if(dialog){
            if(auto stop=dialog->findChild<QPushButton *>("remoteStop"))if(stop->isEnabled())stop->click();
            waitFor([&]{return dialog->property("remoteState").toString()=="idle";},5000);
            dialog.reset();
        }
        for(auto &endpoint:endpoints)endpoint->service->command({{"operation","stop"}});
        events();
    }
    void add(const QString &id){
        auto endpoint=std::make_unique<Endpoint>();endpoint->id=id;endpoint->service=std::make_unique<Mesh::NodeService>();
        const auto directory=state.filePath("host-"+id);require(QDir().mkpath(directory),"create test-owned service state directory");
        endpoint->startup=endpoint->service->command({{"operation","start"},{"bind","127.0.0.1"},{"port",0},{"protocolStateDirectory",directory}});
        require(endpoint->startup.value("ok").toBool()&&endpoint->startup.value("port").toInt()>0
            &&!endpoint->startup.value("token").toString().isEmpty()&&!endpoint->startup.value("instanceId").toString().isEmpty(),
            "actual NodeService starts on an ephemeral loopback port without SDR or RF");
        endpoints.push_back(std::move(endpoint));
    }
    Endpoint &endpoint(const QString &id){
        for(auto &value:endpoints)if(value->id==id)return *value;
        throw std::runtime_error("unknown test endpoint");
    }
    QJsonObject status(const QString &id){return endpoint(id).service->status();}
    bool retained(const QString &id){const auto state=status(id);return state.value("active").toBool()&&state.value("listening").toBool()
        &&state.value("session")==session&&state.value("instanceId")==endpoint(id).startup.value("instanceId");}
    void fields(const QString &id){
        const auto startup=endpoint(id).startup;
        auto host=widget<QLineEdit>(*dialog,"remoteHost_"+id);auto port=widget<QSpinBox>(*dialog,"remotePort_"+id);auto token=widget<QLineEdit>(*dialog,"remoteToken_"+id);
        require(host->isEnabled()&&port->isEnabled()&&token->isEnabled(),"unattached node endpoint fields remain editable");
        host->setText("127.0.0.1");port->setValue(startup.value("port").toInt());token->setText(startup.value("token").toString());
    }
    void begin(){
        for(const auto &id:{"A","B","C"})add(id);
        dialog=std::make_unique<Mesh::RemoteDialog>(initialProject());dialog->setAttribute(Qt::WA_DeleteOnClose,false);dialog->setAttribute(Qt::WA_DontShowOnScreen);
        QObject::connect(dialog.get(),&Mesh::RemoteDialog::projectChanged,dialog.get(),[this]{++projectChanges;});
        dialog->show();events();
        for(const auto &id:{"A","B","C"})fields(id);
        auto bearer=widget<QComboBox>(*dialog,"remoteBearer");const int data=bearer->findData("data");require(data>=0,"DATA transport is exposed in the controller");bearer->setCurrentIndex(data);
        click(*dialog,"remoteStart");
        require(waitFor([&]{return attached(*dialog)&&status("A").value("active").toBool()&&status("B").value("active").toBool()&&status("C").value("active").toBool();}),
            "controller attaches three independent actual loopback services");
        session=status("A").value("session").toString();
        require(!session.isEmpty()&&retained("A")&&retained("B")&&retained("C"),"initial endpoints share one owned DATA session with pinned instances");
        QObject::connect(&monitor,&QTimer::timeout,dialog.get(),[this]{
            ++ticks;if(watch&&(!attached(*dialog)||!retained("A")||!retained("C")))isolated=false;
        });
        monitor.start(5);watch=true;
    }
    void privacy(){
        const QVector<QJsonObject> documents{dialog->experimentResults(),dialog->endpointConfiguration(),dialog->currentProject().json()};
        for(const auto &document:documents){
            require(publicFields(document),"exports contain no credential, peer-key or session ownership fields");
            const auto bytes=QJsonDocument(document).toJson(QJsonDocument::Compact);
            bool clean=!bytes.contains(state.path().toUtf8())&&(session.isEmpty()||!bytes.contains(session.toUtf8()));
            for(const auto &endpoint:endpoints)for(const auto key:{"token","pairingSecret"}){
                const auto secret=endpoint->startup.value(key).toString();if(!secret.isEmpty())clean&=!bytes.contains(secret.toUtf8());
            }
            require(clean,"exports omit actual service tokens, pairing codes, session and private state paths");
        }
    }
    QJsonObject routeSnapshot(){
        QJsonObject services;QJsonArray controllers;
        for(const auto &endpoint:endpoints){
            const auto state=endpoint->service->status();QJsonObject summary;
            for(const auto key:{"active","topologyRevision","queue","tx","rx","forwarded","delivered"})summary[key]=state.value(key);
            services[endpoint->id]=summary;
        }
        for(const auto &value:dialog->experimentResults().value("nodes").toArray()){
            const auto state=value.toObject();QJsonObject summary;
            for(const auto key:{"nodeId","active","topologyRevision","controllerState"})summary[key]=state.value(key);
            controllers.append(summary);
        }
        return {{"services",services},{"controller",controllers},{"remoteState",dialog->property("remoteState").toString()}};
    }
    void send(const QString &from,const QString &to,const QString &id,const QJsonArray &path){
        auto source=widget<QComboBox>(*dialog,"remoteSource"),destination=widget<QComboBox>(*dialog,"remoteDestination");
        const int sourceIndex=source->findData(from),destinationIndex=destination->findData(to);
        require(sourceIndex>=0&&destinationIndex>=0,"source and destination controls include the requested retained or appended nodes");
        source->setCurrentIndex(sourceIndex);destination->setCurrentIndex(destinationIndex);
        require(waitFor([&]{return widget<QPushButton>(*dialog,"remoteSend")->isEnabled();}),"working source remains sendable while the controller is attached");
        const auto payload=QByteArray::fromHex("006c6976652d72656d6f74652dff");
        auto encoding=widget<QComboBox>(*dialog,"remoteInputEncoding");const int hex=encoding->findText("HEX");require(hex>=0,"binary HEX entry is available");encoding->setCurrentIndex(hex);
        widget<QLineEdit>(*dialog,"remotePayloadInput")->setText(QString::fromLatin1(payload.toHex()));
        widget<QLineEdit>(*dialog,"remotePacketId")->setText(id);widget<QSpinBox>(*dialog,"remoteTtl")->setValue(4);
        const auto atSend=routeSnapshot();
        click(*dialog,"remoteSend");
        require(waitFor([&]{return !result(*dialog,id,"delivered").isEmpty();}),"UI send delivers DATA through actual NodeServices");
        const auto event=result(*dialog,id,"delivered");
        const auto actualPayload=QByteArray::fromBase64(event.value("payloadBase64").toString().toLatin1());
        const bool matches=event.value("path").toArray()==path&&event.value("source")==from&&event.value("destination")==to&&actualPayload==payload;
        if(!matches){
            QJsonObject actual,nodes,links;
            for(const auto key:{"id","source","destination","path","node","status","serial"})actual[key]=event.value(key);
            actual["payloadBytes"]=actualPayload.size();actual["payloadMatches"]=actualPayload==payload;
            const auto project=dialog->currentProject();
            for(const auto &node:project.nodes)nodes[node.id]=node.enabled;
            for(const auto &link:project.links)links[link.tx+"/"+link.rx]=link.enabled;
            const QJsonObject diagnostic{{"expected",QJsonObject{{"id",id},{"source",from},{"destination",to},{"path",path},{"payloadBytes",payload.size()}}},
                {"actual",actual},{"requestedNodes",nodes},{"requestedLinks",links},{"lastDirectToggle",lastDirectToggle},
                {"atSend",atSend},{"atMismatch",routeSnapshot()}};
            // Whitelist routing metadata; never dump credentials, session ownership or payloads.
            std::fprintf(stderr,"ROUTE_MISMATCH %s\n",QJsonDocument(diagnostic).toJson(QJsonDocument::Compact).constData());std::fflush(stderr);
        }
        require(matches,"controller results preserve exact binary bytes and the actual route");
        require(attached(*dialog)&&retained("A")&&retained("C")&&isolated,"send never detaches or replaces unaffected A/C sessions");
        privacy();
    }
    void disabledSource(const QString &id){
        auto source=widget<QComboBox>(*dialog,"remoteSource");const int index=source->findData(id);
        if(index>=0){source->setCurrentIndex(index);require(!widget<QPushButton>(*dialog,"remoteSend")->isEnabled(),"disabled or failed source cannot send through the UI");}
    }
    void toggleNode(const QString &id,bool enabled,bool viaWidget){
        const int changed=projectChanges;QString error;
        if(viaWidget){
            auto checkbox=widget<QCheckBox>(*dialog,"remoteEnabled_"+id);
            require(checkbox->isEnabled()&&checkbox->isChecked()!=enabled,"node checkbox is actionable during an attached session");checkbox->click();
        }else require(dialog->setNodeEnabled(id,enabled,&error),"setNodeEnabled accepts an explicit live node toggle");
        if(!enabled){
            // Do not process events here: the asynchronous release callback must still be pending.
            const auto disabled=dialog->currentProject();const int disabledChanges=projectChanges;auto reenable=disabled;
            for(auto &node:reenable.nodes)if(node.id==id)node.enabled=true;
            QString problem;
            require(!dialog->updateProject(reenable,&problem)&&!problem.isEmpty(),"updateProject rejects re-enable while this row's release is pending");
            require(dialog->currentProject().json()==disabled.json()&&projectChanges==disabledChanges
                &&!widget<QCheckBox>(*dialog,"remoteEnabled_"+id)->isChecked()&&attached(*dialog)&&retained("A")&&retained("C"),
                "rejected pending-release re-enable does not commit the project, emit projectChanged or disturb unaffected sessions");
        }
        require(waitFor([&]{return projectChanges>changed&&nodeEnabled(dialog->currentProject(),id)==enabled
            &&status(id).value("active").toBool()==enabled&&widget<QCheckBox>(*dialog,"remoteEnabled_"+id)->isChecked()==enabled;}),
            "node API, checkbox, projectChanged and actual session ownership converge");
        require(attached(*dialog)&&retained("A")&&retained("C")&&isolated,"node toggle does not detach unaffected endpoints");
        if(enabled)require(retained(id),"explicit re-enable joins the original session and pinned instance");else disabledSource(id);
    }
    void toggleDirect(bool enabled,bool viaWidget){
        const auto aRevision=count(status("A").value("topologyRevision")),cRevision=count(status("C").value("topologyRevision"));
        lastDirectToggle={{"enabled",enabled},{"before",QJsonObject{{"A",QString::number(aRevision)},{"C",QString::number(cRevision)}}}};
        const int changed=projectChanges;QString error;
        if(viaWidget){
            auto checkbox=widget<QCheckBox>(*dialog,"remoteLinkEnabled_A_C");
            require(checkbox->isEnabled()&&checkbox->isChecked()!=enabled,"link checkbox is actionable during an attached session");checkbox->click();
        }else require(dialog->setLinkEnabled("A","C",enabled,&error),"setLinkEnabled accepts an explicit live link toggle");
        require(waitFor([&]{return projectChanges>changed&&linkEnabled(dialog->currentProject(),"A","C")==enabled
            &&widget<QCheckBox>(*dialog,"remoteLinkEnabled_A_C")->isChecked()==enabled
            &&count(status("A").value("topologyRevision"))>aRevision&&count(status("C").value("topologyRevision"))>cRevision;}),
            "link API, checkbox and topology revisions converge at both unaffected services");
        lastDirectToggle["after"]=QJsonObject{{"A",status("A").value("topologyRevision")},{"C",status("C").value("topologyRevision")}};
    }
    void reject(const Mesh::Project &candidate,const char *label){
        const auto before=dialog->currentProject().json();const int changed=projectChanges;QString error;
        require(!dialog->updateProject(candidate,&error)&&!error.isEmpty(),label);
        require(dialog->currentProject().json()==before&&projectChanges==changed&&attached(*dialog)&&retained("A")&&retained("C"),
            "rejected project edit preserves the attached project, signal count and unaffected ownership");
    }
};

void liveController(){
    Fixture rig;rig.begin();auto &dialog=*rig.dialog;
    rig.send("A","C","initial_direct",{"A","C"});
    rig.toggleDirect(false,false);rig.send("A","C","via_b",{"A","B","C"});
    rig.toggleDirect(true,true);rig.send("A","C","direct_restored",{"A","C"});

    const auto initial=dialog.currentProject();
    auto removed=initial;removed.nodes.removeLast();
    for(int i=removed.links.size()-1;i>=0;--i)if(removed.links[i].tx=="C"||removed.links[i].rx=="C")removed.links.removeAt(i);
    rig.reject(removed,"live updateProject rejects removing an existing node");
    auto reordered=initial;const auto first=reordered.nodes[0];reordered.nodes[0]=reordered.nodes[1];reordered.nodes[1]=first;
    rig.reject(reordered,"live updateProject rejects reindexing existing nodes");
    auto ports=initial;ports.nodes[0].ports=Mesh::defaultNodePorts();ports.nodes[0].ports.append(QJsonObject{{"id","extra"},{"direction","tx"},{"backend","network"},{"enabled",true}});
    rig.reject(ports,"live updateProject rejects changes to a retained node's local ports");
    auto protocol=initial;for(int i=0;i<protocol.nodes.size();++i)protocol.nodes[i].protocol={{"type","meshtastic"},{"nodeNumber",256+i}};
    rig.reject(protocol,"live updateProject rejects changing retained native protocol configuration");
    auto oversized=initial;for(const auto &id:{"D","E","F","G","H","I"})oversized.nodes.append(node(id));
    rig.reject(oversized,"live updateProject rejects more than eight nodes");

    rig.toggleNode("B",false,false);rig.send("A","C","b_disabled",{"A","C"});
    rig.toggleNode("B",true,true);
    rig.toggleDirect(false,true);rig.send("A","C","b_rejoined",{"A","B","C"});rig.toggleDirect(true,false);
    rig.send("C","A","reverse_before_failure",{"C","A"});

    // A real listener failure, not a fabricated reply. B is never restarted or unpinned.
    const auto pinnedB=rig.endpoint("B").startup.value("instanceId");const int ticks=rig.ticks;
    const int errorsBeforeStop=dialog.experimentResults().value("errors").toArray().size();
    rig.endpoint("B").service->command({{"operation","stop"}});
    require(waitFor([&]{return failureFor(dialog,"B",errorsBeforeStop);}),"real B service failure is reported for B without a fail-all transition");
    require(attached(dialog)&&rig.retained("A")&&rig.retained("C")&&rig.isolated&&rig.ticks>ticks&&!rig.status("B").value("listening").toBool(),
        "GUI remains responsive and attached with the original A/C sessions after B stops");
    rig.disabledSource("B");rig.send("A","C","after_b_failure",{"A","C"});rig.send("C","A","reverse_after_failure",{"C","A"});
    require(rig.endpoint("B").startup.value("instanceId")==pinnedB&&!rig.status("B").value("listening").toBool(),"failure isolation never restarts B or implicitly assigns a replacement instance");

    const int beforeDisable=rig.projectChanges;QString error;
    require(dialog.setNodeEnabled("B",false,&error),"unavailable B can be explicitly disabled without stopping the network");
    require(waitFor([&]{return rig.projectChanges>beforeDisable&&!nodeEnabled(dialog.currentProject(),"B")
        &&!widget<QCheckBox>(dialog,"remoteEnabled_B")->isChecked();}),"explicit disable of failed B is reflected in the project and checkbox");
    rig.send("A","C","failed_b_disabled",{"A","C"});

    const auto tokenA=widget<QLineEdit>(dialog,"remoteToken_A")->text(),tokenC=widget<QLineEdit>(dialog,"remoteToken_C")->text();
    rig.add("D");auto appended=dialog.currentProject();appended.nodes.append(node("D"));appended.links.append(link("C","D"));appended.links.append(link("D","C"));
    const int beforeAppend=rig.projectChanges;
    require(dialog.updateProject(appended,&error),"updateProject appends D and new links while the controller remains attached");
    require(waitFor([&]{return rig.projectChanges>beforeAppend&&dialog.currentProject().nodes.size()==4
        &&dialog.findChild<QCheckBox *>("remoteEnabled_D")&&dialog.findChild<QLineEdit *>("remoteToken_D");}),"appended endpoint row and node checkbox appear without a new dialog or attach");
    require(!nodeEnabled(dialog.currentProject(),"D")&&!widget<QCheckBox>(dialog,"remoteEnabled_D")->isChecked()
        &&widget<QLineEdit>(dialog,"remoteToken_D")->text().isEmpty()&&!rig.status("D").value("active").toBool(),
        "new append defaults disabled despite the candidate's enabled default and has no borrowed token");
    require(widget<QLineEdit>(dialog,"remoteToken_A")->text()==tokenA&&widget<QLineEdit>(dialog,"remoteToken_C")->text()==tokenC,
        "appending a row preserves unrelated endpoint credentials in memory");
    require(widget<QTableWidget>(dialog,"remoteEndpoints")->rowCount()==4&&widget<QTableWidget>(dialog,"remoteTelemetry")->rowCount()==4
        &&widget<QCheckBox>(dialog,"remoteLinkEnabled_C_D")->isChecked()&&widget<QCheckBox>(dialog,"remoteLinkEnabled_D_C")->isChecked(),
        "endpoint, telemetry and link controls include the appended topology");
    rig.send("A","C","d_still_disabled",{"A","C"});
    rig.fields("D");rig.send("A","C","d_credentials_only",{"A","C"});
    require(!rig.status("D").value("active").toBool()&&!nodeEnabled(dialog.currentProject(),"D"),"entering D credentials alone does not enable or configure it");
    rig.toggleNode("D",true,true);
    rig.send("A","D","deliver_new_d",{"A","C","D"});rig.send("D","A","reply_new_d",{"D","C","A"});
    require(!rig.status("B").value("listening").toBool()&&rig.retained("D")&&rig.isolated,
        "D joins the original session while failed B stays stopped and A/C never detach");
    require(!result(dialog,"via_b","delivered").isEmpty()&&!result(dialog,"after_b_failure","delivered").isEmpty(),"controller retains pre-failure and post-failure delivery history after append");
    rig.privacy();

    rig.watch=false;click(dialog,"remoteStop");
    require(waitFor([&]{return dialog.property("remoteState").toString()=="idle"&&!dialog.sessionActive()
        &&!rig.status("A").value("active").toBool()&&!rig.status("C").value("active").toBool()&&!rig.status("D").value("active").toBool();}),
        "explicit stop releases retained and appended nodes even when B release cannot be confirmed");
    require(rig.status("A").value("listening").toBool()&&rig.status("C").value("listening").toBool()&&rig.status("D").value("listening").toBool()
        &&!rig.status("B").value("listening").toBool(),"detach leaves only test-owned A/C/D listeners alive and does not resurrect B");
    rig.privacy();
}
}

int main(int argc,char **argv){
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))qputenv("QT_QPA_PLATFORM","offscreen");
    QApplication app(argc,argv);QTemporaryDir settings;
    if(!settings.isValid())return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    std::printf("CASE live RemoteDialog failure isolation and topology edits\n");std::fflush(stdout);
    try{liveController();}catch(const std::exception &error){std::fprintf(stderr,"FAIL live remote controller: %s\n",error.what());return 1;}
    std::printf("0 failure(s)\n");return 0;
}
