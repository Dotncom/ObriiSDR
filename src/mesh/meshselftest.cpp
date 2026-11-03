#include "meshwindow.h"
#include "meshspectrum.h"
#include "meshpacketdialog.h"
#include "meshremotedialog.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsItem>
#include <QAction>
#include <QDialog>
#include <QTableWidget>
#include <QCheckBox>
#include <QDockWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QTcpSocket>
#include <QHostAddress>
#include <iostream>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
bool waitFor(const std::function<bool()> &predicate,int ms=20000) {
    QElapsedTimer t;t.start();while(t.elapsed()<ms){QCoreApplication::processEvents(QEventLoop::AllEvents,25);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);if(predicate())return true;QThread::msleep(5);}return predicate();
}
QJsonObject command(Mesh::Runtime &runtime,const QString &id,const QString &name,const QJsonObject &args) {
    QTcpSocket socket;socket.setReadBufferSize(65536);
    const auto s=runtime.snapshot(id);
    const QJsonObject request{{"command",name},{"args",args},{"targetInstanceId",s.value("instanceId")},{"id","mesh-selftest"}};
    QObject::connect(&socket,&QTcpSocket::connected,&socket,[&]{socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');});
    socket.connectToHost(QHostAddress::LocalHost,quint16(s.value("controlPort").toInt()));
    if(!waitFor([&]{return socket.canReadLine();},3000))return {};
    return QJsonDocument::fromJson(socket.readLine()).object();
}
QJsonObject appliedLink(Mesh::Runtime &runtime,const QString &tx,const QString &rx) {
    for(const auto &v:runtime.hubSnapshot().value("links").toArray()){
        const auto l=v.toObject();if(l.value("tx")==tx&&l.value("rx")==rx)return l;
    }
    return {};
}
double filteredFrames(Mesh::Runtime &runtime,const QString &id) {
    for(const auto &v:runtime.hubSnapshot().value("receivers").toArray()){
        const auto c=v.toObject();if(c.value("id")==id)return c.value("filteredFrames").toString().toDouble();
    }
    return -1;
}
}
int main(int argc,char **argv) {
#ifdef Q_OS_WIN
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
#endif
    QApplication app(argc,argv);QTemporaryDir temp;int failures=0;
    auto check=[&](bool ok,const char *name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<std::endl;if(!ok)++failures;};
    Mesh::HostPreset host{"local","Local test","local",{}, {},argc>1?QString::fromLocal8Bit(argv[1]):QCoreApplication::applicationDirPath(),22};
    QVector<Mesh::HostPreset> library{host,{"pi5","Raspberry Pi 5","ssh-posix","192.0.2.10","admin","/opt/obrii/bin",22}};
    QString error;const auto libraryFile=temp.filePath("devices.json");
    check(Mesh::saveLibrary(libraryFile,library,&error),"device library save");QVector<Mesh::HostPreset> restored;
    check(Mesh::loadLibrary(libraryFile,&restored,&error)&&restored.size()==2&&restored[1].host=="192.0.2.10"&&restored[1].directory=="/opt/obrii/bin","device addresses and executable paths roundtrip");
    auto project=Mesh::example("local");check(project.validate(library).isEmpty(),"example topology valid");
    auto invalid=project;invalid.nodes[1].id=invalid.nodes[0].id;check(!invalid.validate(library).isEmpty(),"duplicate node IDs rejected");
    invalid=project;invalid.links[0].rx="missing";check(!invalid.validate(library).isEmpty(),"dangling links rejected");
    invalid=project;invalid.nodes[0].listeningHz+=1e6;check(!invalid.validate(library).isEmpty(),"out-of-band radio setting rejected");
    Mesh::Project parsed;check(Mesh::Project::parse(project.json(),&parsed,&error)&&parsed.json()==project.json(),"project JSON roundtrip");
    auto bad=project.json();bad["version"]=9;check(!Mesh::Project::parse(bad,&parsed,&error),"future schema rejected");
    {
        Mesh::SpectrumView graph;
        QJsonObject trace{{"startHz",145476000.0},{"binHz",12000.0},{"offset",0},{"totalBins",4},
            {"truncated",false},{"levelsDbfs",QJsonArray{-100,-40,-60,-110}}};
        check(graph.setTrace(trace)&&graph.binCount()==4,"full spectrum accepted by display adapter");
        trace["totalBins"]=8;
        check(!graph.setTrace(trace)&&graph.binCount()==0,"partial spectrum rejected instead of mislabeling frequencies");
        trace["totalBins"]=4;trace["levelsDbfs"]=QJsonArray{-100,QJsonValue(),-60,-110};
        check(!graph.setTrace(trace)&&graph.binCount()==0,"invalid bins cannot leave a stale plot");
    }
    library[1].user="-oProxyCommand";check(!Mesh::validatePresets(library).isEmpty(),"invalid SSH user rejected");library=restored;
    {
        Mesh::Window window(libraryFile);window.setAttribute(Qt::WA_DontShowOnScreen);window.show();app.processEvents();
        const auto file=temp.filePath("network.json");check(window.saveProjectFile(file,&error)&&window.loadProjectFile(file,&error),"production designer save/load");
        auto view=window.findChild<QGraphicsView *>("topologyCanvas");check(view&&view->scene()->items().size()==5,"production canvas has three nodes and two links");
        for(auto item:view->scene()->items())if(item->data(1)=="node2")item->setSelected(true);
        app.processEvents();
        if(argc>2)check(window.grab().save(QString::fromLocal8Bit(argv[2])),"designer screenshot");
        for(auto action:window.findChildren<QAction *>())if(action->text().contains(QString::fromUtf8("Бібліотека пристроїв"))) {
            QTimer::singleShot(100,&window,[&]{
                auto dialog=window.findChild<QDialog *>();auto table=dialog?dialog->findChild<QTableWidget *>("deviceLibrary"):nullptr;
                check(table&&table->rowCount()==2&&table->item(1,3)->text()=="192.0.2.10","production device preset library UI");
                if(argc>2&&dialog)check(dialog->grab().save(QString::fromLocal8Bit(argv[2])+"-devices.png"),"device library screenshot");
                if(dialog)dialog->reject();
            });action->trigger();break;
        }
        auto remoteAction=window.findChild<QAction *>("remoteMeshAction");if(remoteAction)remoteAction->trigger();app.processEvents();
        auto remote=window.findChild<Mesh::RemoteDialog *>();
        check(remoteAction&&remote&&remote->property("remoteState")=="idle","constructor opens distributed-mode window without starting endpoints or RF");
        if(remote){if(argc>2)check(remote->grab().save(QString::fromLocal8Bit(argv[2])+"-remote.png"),"production distributed window screenshot");remote->close();app.processEvents();}
        auto packetAction=window.findChild<QAction *>("packetLabAction");if(packetAction)packetAction->trigger();app.processEvents();
        auto lab=window.findChild<Mesh::PacketDialog *>("packetLab");
        auto packetOwner=window.findChild<Mesh::Runtime *>();
        check(packetAction&&lab&&packetOwner&&packetOwner->state()==Mesh::Runtime::Idle&&packetOwner->ownedProcessCount()==0,"constructor opens optional packet lab without starting IQ");
        if(lab){
            auto send=lab->findChild<QPushButton *>("packetSend");auto from=lab->findChild<QComboBox *>("packetSource");auto to=lab->findChild<QComboBox *>("packetDestination");
            auto input=lab->findChild<QLineEdit *>("packetText");auto ttl=lab->findChild<QSpinBox *>("packetTtl");
            if(from)from->setCurrentIndex(from->findData("node1"));if(to)to->setCurrentIndex(to->findData("node3"));if(input)input->setText("PACKET TEST");
            if(send)send->click();
            check(send&&waitFor([&]{return lab->simulation().snapshot(false).value("delivered")=="1";}),"production packet controls deliver A-B-C without IQ workers");
            auto linkTable=lab->findChild<QTableWidget *>("packetLinks");QCheckBox *edge=nullptr;
            if(linkTable)for(int row=0;row<linkTable->rowCount();++row)if(linkTable->item(row,0)->text()=="node2"&&linkTable->item(row,1)->text()=="node3")edge=qobject_cast<QCheckBox *>(linkTable->cellWidget(row,2));
            if(edge)edge->setChecked(false);if(send)send->click();
            check(edge&&waitFor([&]{return lab->simulation().snapshot(false).value("dropped")=="1";}),"packet lab can partition its own link graph");
            if(edge)edge->setChecked(true);if(send)send->click();
            check(waitFor([&]{return lab->simulation().snapshot(false).value("delivered")=="2";}),"packet lab restores delivery after repairing edge");
            if(ttl)ttl->setValue(1);if(send)send->click();
            check(ttl&&waitFor([&]{return lab->simulation().snapshot(false).value("dropReasons").toObject().value("ttl_expired")=="1";}),"production TTL control limits forwarding");
            check(lab->simulation().pending()==0&&window.currentProject().links.last().enabled,"packet controls leave constructor IQ topology unchanged");
            auto run=lab->findChild<QPushButton *>("packetRun");auto step=lab->findChild<QPushButton *>("packetStep");
            if(run)run->setChecked(false);if(ttl)ttl->setValue(2);if(send)send->click();
            check(run&&step&&step->isEnabled()&&lab->simulation().pending()==1,"paused packet lab retains pending datagram without a running timer");
            if(step)step->click();check(lab->simulation().snapshot(false).value("delivered")=="2","one step only forwards from source");
            if(step){step->click();step->click();}
            check(lab->simulation().snapshot(false).value("delivered")=="3","manual steps reach destination through relay");
            const auto snapshot=lab->simulation().snapshot();
            check(snapshot.value("results").toArray().first().toObject().value("path").toArray()==QJsonArray{"node1","node2","node3"},"packet inspector records actual relay path");
            auto table=lab->findChild<QTableWidget *>("packetResults");if(table)table->selectRow(0);
            if(argc>2){check(lab->grab().save(QString::fromLocal8Bit(argv[2])+"-packets.png"),"packet lab screenshot");lab->resize(760,560);app.processEvents();check(lab->grab().save(QString::fromLocal8Bit(argv[2])+"-packets-compact.png"),"compact packet lab screenshot");}
            auto reset=lab->findChild<QPushButton *>("packetReset");if(reset)reset->click();
            check(reset&&lab->simulation().pending()==0&&lab->simulation().snapshot(false).value("submitted")=="0"&&table&&table->rowCount()==0,"reset clears packet state and pending work");
            auto bearerMode=lab->findChild<QComboBox *>("packetBearer");
            if(bearerMode)bearerMode->setCurrentIndex(1);
            check(bearerMode&&waitFor([&]{return send->isEnabled();}),"production IQ modem mode becomes ready");
            if(send)send->click();
            check(waitFor([&]{return lab->simulation().snapshot(false).value("delivered")=="1";},20000)&&lab->simulation().snapshot(false).value("model")=="ax25-afsk1200-fm-virtual-iq","production packet controls deliver through real virtual IQ modems");
            if(argc>2)check(lab->grab().save(QString::fromLocal8Bit(argv[2])+"-packet-iq.png"),"IQ packet lab screenshot");
            if(send)send->click();
            check(waitFor([&]{return lab->simulation().snapshot(false).value("iqPending").toInt()>0;}),"IQ cancellation test has an actual modem hop in flight");
            if(reset)reset->click();
            check(lab->simulation().pending()==0&&lab->simulation().snapshot(false).value("submitted")=="0","IQ reset cancels the old session without stale delivery");
            check(waitFor([&]{return send->isEnabled();}),"IQ mode restarts after cancellation");if(send)send->click();
            check(waitFor([&]{return lab->simulation().snapshot(false).value("delivered")=="1";},20000)&&lab->simulation().snapshot(false).value("submitted")=="1","fresh IQ session delivers only its own packet after reset");
            if(bearerMode)bearerMode->setCurrentIndex(0);
            lab->close();app.processEvents();
        }
    }
    if(argc>1) {
        Mesh::Runtime runtime;QStringList events;QObject::connect(&runtime,&Mesh::Runtime::log,[&](const QString &s){events<<s;});
        check(runtime.start(project,library,&error),"launch scheduled for real local CLI nodes");
        const bool ready=waitFor([&]{return runtime.state()==Mesh::Runtime::Running;});check(ready,"real hub and three virtual SDRs ready");
        if(!ready){std::cerr<<events.join('\n').toStdString()<<std::endl;runtime.stop();waitFor([&]{return runtime.state()==Mesh::Runtime::Idle;});}
        if(ready) {
            check(runtime.ownedProcessCount()==4,"only four owned processes: three RX and hub");
            const auto hubInstance=runtime.hubSnapshot().value("instanceId");
            check(!hubInstance.toString().isEmpty()&&runtime.hubSnapshot().value("revision")=="0"&&!runtime.hubSnapshot().contains("token"),"session readiness requires authenticated hub snapshot without exposing key");
            auto rejected=command(runtime,"node2","cli.preview",{{"ttlMs",4000}});
            check(rejected.contains("ok")&&!rejected.value("ok").toBool(),"preview rejects unbounded leases");
            rejected=command(runtime,"node2","cli.preview",{{"ttlMs",1000},{"intervalMs",99}});
            check(rejected.contains("ok")&&!rejected.value("ok").toBool(),"preview rejects excessive polling rate");
            runtime.inspectNode("node2");
            check(runtime.testSignal("node1",&error),"virtual-only test CW starts");
            check(waitFor([&]{return runtime.snapshot("node2").value("applied").toObject().value("samples").toDouble()>0;}),"IQ reaches linked node B");
            check(waitFor([&]{return runtime.snapshot("node2").value("trace").toObject().value("levelsDbfs").toArray().size()==1024;}),"selected node receives a full live trace through production CLI");
            check(runtime.snapshot("node1").value("traceRequests").toDouble()==0&&runtime.snapshot("node3").value("traceRequests").toDouble()==0,"unselected nodes receive no spectrum polling");
            runtime.inspectNode({});
            const double traceRequests=runtime.snapshot("node2").value("traceRequests").toDouble();
            QElapsedTimer quiet;quiet.start();waitFor([&]{return quiet.elapsed()>1200;},1800);
            check(runtime.snapshot("node2").value("traceRequests").toDouble()==traceRequests,"disabled inspection issues no further trace requests");
            check(waitFor([&]{return runtime.snapshot("node2").value("applied").toObject().value("previewLeaseMs").toDouble(-1)==0;}),"disabled inspector releases its FFT lease");
            check(command(runtime,"node2","cli.preview",{{"ttlMs",500},{"intervalMs",500}}).value("ok").toBool(),"finite standalone preview lease accepted");
            QElapsedTimer lease;lease.start();waitFor([&]{return lease.elapsed()>1200;},1800);
            check(command(runtime,"node2","status",{}).value("previewLeaseMs").toDouble(-1)==0,"orphaned preview expires without client cancellation");
            check(runtime.snapshot("node3").value("applied").toObject().value("samples").toDouble()==0,"no fake automatic relay to node C");
            const auto nodePid=runtime.snapshot("node2").value("pid");
            const auto instance=runtime.snapshot("node2").value("instanceId");
            check(!runtime.setNodeRunning("missing",false,&error),"unknown node cannot control a receiver");
            check(waitFor([&]{return runtime.setNodeRunning("node2",false,&error);}),"independent RX stop queued");
            check(waitFor([&]{const auto s=runtime.snapshot("node2");return !s.value("runningPending").toBool()&&s.value("phase")=="Paused"&&!s.value("applied").toObject().value("running").toBool();}),"stopped RX confirmed without terminating its CLI");
            check(runtime.state()==Mesh::Runtime::Running&&runtime.snapshot("node1").value("applied").toObject().value("running").toBool()&&runtime.snapshot("node3").value("applied").toObject().value("running").toBool(),"other nodes and medium remain running");
            check(runtime.stopSignal("node1",&error)&&waitFor([&]{return !runtime.snapshot("node1").value("txRunning").toBool();}),"node TX can be stopped without stopping RX session");
            check(waitFor([&]{return runtime.setNodeRunning("node2",true,&error);}),"independent RX restart queued");
            check(waitFor([&]{const auto s=runtime.snapshot("node2");return !s.value("runningPending").toBool()&&s.value("applied").toObject().value("running").toBool();}),"RX restart confirmed");
            check(runtime.snapshot("node2").value("pid")==nodePid&&runtime.snapshot("node2").value("instanceId")==instance,"RX pause/resume preserves process and session identity");
            const double before=runtime.snapshot("node2").value("applied").toObject().value("samples").toDouble();
            check(runtime.testSignal("node1",&error)&&waitFor([&]{return runtime.snapshot("node2").value("applied").toObject().value("samples").toDouble()>before;}),"resumed RX receives new IQ without restarting other nodes");
            check(runtime.stopSignal("node1",&error)&&waitFor([&]{return !runtime.snapshot("node1").value("txRunning").toBool();}),"prepare live topology test");
            QElapsedTimer drain;drain.start();waitFor([&]{return drain.elapsed()>1000;},1600);
            auto link=project.links.first();link.enabled=false;
            check(runtime.applyLink(link,&error)&&waitFor([&]{return !runtime.hubSnapshot().value("pending").toBool()&&runtime.hubSnapshot().value("revision")=="1"&&!appliedLink(runtime,"node1","node2").value("enabled").toBool(true);}),"live link disabled and acknowledged");
            const double isolated=command(runtime,"node2","status",{}).value("samples").toDouble();
            const double filtered=filteredFrames(runtime,"node2");
            check(runtime.testSignal("node1",&error)&&waitFor([&]{return filteredFrames(runtime,"node2")>filtered;}),"new TX is filtered by disabled live link");
            check(command(runtime,"node2","status",{}).value("samples").toDouble()==isolated,"disabled edge delivers no new IQ after queued tail drains");
            check(runtime.stopSignal("node1",&error)&&waitFor([&]{return !runtime.snapshot("node1").value("txRunning").toBool();}),"disabled-link stimulus stops cleanly");
            link.enabled=true;link.gainDb=-18;link.delayMs=20;
            check(runtime.applyLink(link,&error)&&waitFor([&]{const auto a=appliedLink(runtime,"node1","node2");return !runtime.hubSnapshot().value("pending").toBool()&&runtime.hubSnapshot().value("revision")=="2"&&a.value("enabled").toBool()&&a.value("gainDb").toDouble()==-18&&a.value("delayMs").toInt()==20;}),"gain, delay and restore applied without session restart");
            auto invalidLink=link;invalidLink.delayMs=501;
            check(!runtime.applyLink(invalidLink,&error)&&runtime.hubSnapshot().value("revision")=="2","invalid designer link cannot mutate running topology");
            check(runtime.testSignal("node1",&error)&&waitFor([&]{return runtime.snapshot("node2").value("applied").toObject().value("samples").toDouble()>isolated;}),"restored edge carries new IQ");
            check(runtime.hubSnapshot().value("instanceId")==hubInstance&&runtime.snapshot("node2").value("pid")==nodePid&&runtime.state()==Mesh::Runtime::Running,"live topology edits preserve hub and receiver processes");
            auto n=project.nodes[1];n.listeningHz+=1000;n.bandwidthHz=8000;n.mode="am";check(runtime.apply(n,&error),"live radio changes queued");
            const bool applied=waitFor([&]{const auto a=runtime.snapshot("node2").value("applied").toObject();return a.value("audioBandwidthHz").toDouble()==8000&&a.value("modulationType").toInt()==0&&a.value("listeningFrequencyHz").toDouble()==n.listeningHz;});
            check(applied,"live changes confirmed by real CLI");
            if(!applied)std::cerr<<QJsonDocument(runtime.snapshot("node2")).toJson().toStdString()<<'\n'<<events.join('\n').toStdString()<<std::endl;
            runtime.stop();check(waitFor([&]{return runtime.state()==Mesh::Runtime::Idle;},8000)&&runtime.ownedProcessCount()==0,"stop cleans up all owned processes including TX");
            auto remote=project;remote.nodes[0].presetId="pi5";check(!runtime.start(remote,library,&error)&&runtime.ownedProcessCount()==0,"remote launch explicitly unavailable before creating processes");
            check(runtime.start(project,library,&error)&&waitFor([&]{return runtime.state()==Mesh::Runtime::Running;}),"session can restart with fresh endpoints");
            auto pendingLink=project.links.first();pendingLink.enabled=false;
            check(runtime.applyLink(pendingLink,&error),"link request queued before immediate stop");
            runtime.stop();check(waitFor([&]{return runtime.state()==Mesh::Runtime::Idle;},8000)&&!runtime.hubSnapshot().value("pending").toBool(),"stop cancels pending link control without stale callbacks");
            check(runtime.start(project,library,&error),"begin cancellable startup");runtime.stop();check(waitFor([&]{return runtime.state()==Mesh::Runtime::Idle;},8000),"cancel during startup leaves no child processes");
        }
        {
            Mesh::Window window(libraryFile);window.setAttribute(Qt::WA_DontShowOnScreen);window.show();app.processEvents();
            auto view=window.findChild<QGraphicsView *>("topologyCanvas");
            for(auto item:view->scene()->items())if(item->data(1)=="node2")item->setSelected(true);
            auto live=window.findChild<Mesh::Runtime *>();
            auto graph=window.findChild<Mesh::SpectrumView *>("nodeSpectrum");
            auto preview=window.findChild<QCheckBox *>("nodePreviewEnabled");
            const bool started=live&&live->start(project,library,&error)&&waitFor([&]{return live->state()==Mesh::Runtime::Running;});
            check(started,"production window starts virtual session");
            if(started) {
                check(live->testSignal("node1",&error)&&waitFor([&]{return graph&&graph->binCount()==1024&&live->snapshot("node2").value("applied").toObject().value("samples").toDouble()>0;}),"production node inspector displays real CLI bins");
                if(argc>2) {
                    check(window.grab().save(QString::fromLocal8Bit(argv[2])+"-live.png"),"live inspector screenshot");
                    window.resize(960,640);app.processEvents();
                    check(window.grab().save(QString::fromLocal8Bit(argv[2])+"-compact.png"),"compact inspector screenshot");
                }
                check(live->stopSignal("node1",&error)&&waitFor([&]{return graph->binCount()==0;},6000),"unchanged cached trace is not relabeled as live after TX ends");
                if(preview)preview->setChecked(false);
                const double requests=live->snapshot("node2").value("traceRequests").toDouble();
                QElapsedTimer quiet;quiet.start();waitFor([&]{return quiet.elapsed()>1200;},1800);
                check(preview&&graph->binCount()==0&&live->snapshot("node2").value("traceRequests").toDouble()==requests,"production preview toggle clears plot and stops polling");
                check(waitFor([&]{return !live->snapshot("node1").value("txRunning").toBool()&&live->snapshot("node2").value("applied").toObject().value("previewLeaseMs").toDouble(-1)==0;}),"production preview releases FFT demand");
                const auto before=command(*live,"node2","status",{});
                const auto completed=before.value("fftWorker").toObject().value("completedWindows");
                const double samples=before.value("samples").toDouble();
                check(live->testSignal("node1",&error)&&waitFor([&]{return live->snapshot("node2").value("applied").toObject().value("samples").toDouble()>samples;}),"IQ continues with spectrum dock disabled");
                check(command(*live,"node2","status",{}).value("fftWorker").toObject().value("completedWindows")==completed,"disabled preview does no FFT work on arriving IQ");
                view->scene()->clearSelection();
                for(auto item:view->scene()->items())if(item->data(0)=="link"&&item->data(1)=="node1/node2")item->setSelected(true);
                auto enabled=window.findChild<QCheckBox *>("linkEnabled");auto apply=window.findChild<QPushButton *>("applyLink");
                if(enabled)enabled->setChecked(false);
                check(enabled&&apply&&apply->isEnabled()&&live->hubSnapshot().value("revision")=="0","production link editor stages desired values until Apply");
                if(apply)apply->click();
                check(waitFor([&]{return live->hubSnapshot().value("revision")=="1"&&!live->hubSnapshot().value("pending").toBool()&&!appliedLink(*live,"node1","node2").value("enabled").toBool(true);}),"production Apply button changes actual hub link");
                auto label=window.findChild<QLabel *>("appliedLinkState");
                check(label&&apply&&label->text().contains(QString::fromUtf8("Вимкнений"))&&!apply->isEnabled(),"production link inspector shows confirmed state");
                if(argc>2)check(window.grab().save(QString::fromLocal8Bit(argv[2])+"-link.png"),"live link editor screenshot");
            }
            if(live){live->stop();check(waitFor([&]{return live->state()==Mesh::Runtime::Idle;},8000),"production inspector session cleans up");}
        }
    }
    return failures?1:0;
}
