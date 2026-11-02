#include "meshwindow.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsItem>
#include <QAction>
#include <QDialog>
#include <QTableWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
bool waitFor(const std::function<bool()> &predicate,int ms=20000) {
    QElapsedTimer t;t.start();while(t.elapsed()<ms){QCoreApplication::processEvents(QEventLoop::AllEvents,25);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);if(predicate())return true;QThread::msleep(5);}return predicate();
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
    }
    if(argc>1) {
        Mesh::Runtime runtime;QStringList events;QObject::connect(&runtime,&Mesh::Runtime::log,[&](const QString &s){events<<s;});
        check(runtime.start(project,library,&error),"launch scheduled for real local CLI nodes");
        const bool ready=waitFor([&]{return runtime.state()==Mesh::Runtime::Running;});check(ready,"real hub and three virtual SDRs ready");
        if(!ready){std::cerr<<events.join('\n').toStdString()<<std::endl;runtime.stop();waitFor([&]{return runtime.state()==Mesh::Runtime::Idle;});}
        if(ready) {
            check(runtime.ownedProcessCount()==4,"only four owned processes: three RX and hub");
            check(runtime.testSignal("node1",&error),"virtual-only test CW starts");
            check(waitFor([&]{return runtime.snapshot("node2").value("applied").toObject().value("samples").toDouble()>0;}),"IQ reaches linked node B");
            check(runtime.snapshot("node3").value("applied").toObject().value("samples").toDouble()==0,"no fake automatic relay to node C");
            auto n=project.nodes[1];n.listeningHz+=1000;n.bandwidthHz=8000;n.mode="am";check(runtime.apply(n,&error),"live radio changes queued");
            const bool applied=waitFor([&]{const auto a=runtime.snapshot("node2").value("applied").toObject();return a.value("audioBandwidthHz").toDouble()==8000&&a.value("modulationType").toInt()==0&&a.value("listeningFrequencyHz").toDouble()==n.listeningHz;});
            check(applied,"live changes confirmed by real CLI");
            if(!applied)std::cerr<<QJsonDocument(runtime.snapshot("node2")).toJson().toStdString()<<'\n'<<events.join('\n').toStdString()<<std::endl;
            runtime.stop();check(waitFor([&]{return runtime.state()==Mesh::Runtime::Idle;},8000)&&runtime.ownedProcessCount()==0,"stop cleans up all owned processes including TX");
            auto remote=project;remote.nodes[0].presetId="pi5";check(!runtime.start(remote,library,&error)&&runtime.ownedProcessCount()==0,"remote launch explicitly unavailable before creating processes");
            check(runtime.start(project,library,&error)&&waitFor([&]{return runtime.state()==Mesh::Runtime::Running;}),"session can restart with fresh endpoints");runtime.stop();check(waitFor([&]{return runtime.state()==Mesh::Runtime::Idle;},8000),"restarted session stops");
            check(runtime.start(project,library,&error),"begin cancellable startup");runtime.stop();check(waitFor([&]{return runtime.state()==Mesh::Runtime::Idle;},8000),"cancel during startup leaves no child processes");
        }
    }
    return failures?1:0;
}
