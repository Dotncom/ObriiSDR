#include "meshradioegress.h"
#include "meshwiremodem.h"
#include "meshnodecommands.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <cstdio>

namespace {
int errors=0;
void check(bool okay,const char *label){std::printf("%s %s\n",okay?"PASS":"FAIL",label);std::fflush(stdout);if(!okay)++errors;}
bool wait(const std::function<bool()> &f,int timeout=5000){QElapsedTimer timer;timer.start();while(!f()&&timer.elapsed()<timeout){QCoreApplication::processEvents();QThread::msleep(2);}return f();}
struct Capture {QList<QByteArray> frames;std::atomic<bool> entered{false},cancelled{false};};
class CaptureSink : public Mesh::BurstSink {
    std::shared_ptr<Capture> state;
    bool hold;
public:
    CaptureSink(std::shared_ptr<Capture> s,bool h):state(std::move(s)),hold(h){}
    bool send(const Mesh::PortBinding &p,const QJsonObject &,const QVector<std::complex<float>> &wave,const std::atomic<bool> &cancel,QString *) override {
        state->entered=true;
        if(hold){while(!cancel.load())QThread::msleep(2);state->cancelled=true;return false;}
        Mesh::WireModem rx(p.modem);QVector<float> iq(wave.size()*2);for(int i=0;i<wave.size();++i){iq[i*2]=wave[i].real();iq[i*2+1]=wave[i].imag();}
        const auto decoded=rx.receive(iq.constData(),wave.size());
        QMetaObject::invokeMethod(QCoreApplication::instance(),[s=state,decoded]{s->frames.append(decoded);},Qt::QueuedConnection);
        return !decoded.isEmpty();
    }
};
}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);
    Mesh::PortBinding port;port.direction="tx";port.backend="radio";port.centerHz=145500000;
    port.modem={{"type","lora"},{"sf",7},{"bw",125000},{"os",1},{"preamble",16}};
    QJsonObject profile{{"backend","uhd"},{"minHz",145000000},{"maxHz",146000000}};QString error;
    check(Mesh::RadioEgress::validate(port,profile,&error),"bounded local UHD profile validates without opening hardware");
    auto invalid=profile;invalid["minHz"]=146000000;check(!Mesh::RadioEgress::validate(port,invalid,&error),"remote port cannot escape locally permitted frequency interval");
    invalid=profile;invalid["gainDb"]=1000;check(!Mesh::RadioEgress::validate(port,invalid,&error),"excessive local RF gain rejected");
    invalid=profile;invalid["program"]="anything";check(!Mesh::RadioEgress::validate(port,invalid,&error),"RF profiles cannot launch arbitrary programs");
    auto capture=std::make_shared<Capture>();int completed=0;bool emitted=false;
    {
        Mesh::RadioEgress output(port,profile,nullptr,[capture]{return std::make_unique<CaptureSink>(capture,false);},5000);
        QObject::connect(&output,&Mesh::RadioEgress::completed,&app,[&](quint64 ticket,bool sent,const QString &,quint64 count){++completed;emitted=sent&&ticket==42&&count>0;});
        check(output.submit(42,QByteArray("native\0radio",12),&error),"native frame accepted by optional RF worker");
        check(wait([&]{return completed==1&&capture->frames.size()==1;})&&emitted&&capture->frames[0]==QByteArray("native\0radio",12),"RF worker produces exact LoRa IQ through injectable non-RF sink");
    }
    auto blocked=std::make_shared<Capture>();
    {
        Mesh::RadioEgress output(port,profile,nullptr,[blocked]{return std::make_unique<CaptureSink>(blocked,true);},5000);
        check(output.submit(1,"one",&error)&&wait([&]{return blocked->entered.load();}),"slow backend isolated in its own worker");
        check(output.submit(2,"two",&error)&&output.submit(3,"three",&error)&&output.submit(4,"four",&error)&&!output.submit(5,"five",&error),"RF queue is capped at four including active burst");
        QElapsedTimer stop;stop.start();output.stop();check(stop.elapsed()<500&&output.status().value("state")=="disarmed","disarm cancels active work and clears queued RF without GUI blocking");
    }
    {
        auto expiring=std::make_shared<Capture>();
        Mesh::RadioEgress output(port,profile,nullptr,[expiring]{return std::make_unique<CaptureSink>(expiring,true);},250);
        check(output.submit(1,"deadline",&error)&&wait([&]{return expiring->entered.load();}),"deadline fixture enters cooperative backend");
        QThread::msleep(350);
        check(expiring->cancelled.load()&&output.status().value("state")=="expired"&&!output.submit(2,"late",&error),"authorization expires without running the Qt event loop or node watchdog");
    }
    {
        Mesh::RadioEgress output(port,profile);
        check(!output.submit(1,"no authorization",&error),"missing authorization starts no RF worker and accepts no bursts");
        auto idle=std::make_shared<Capture>();QElapsedTimer timer;timer.start();
        for(int i=0;i<64;++i){Mesh::RadioEgress stress(port,profile,nullptr,[idle]{return std::make_unique<CaptureSink>(idle,false);},5000);stress.stop();}
        check(timer.elapsed()<3000,"repeated immediate stop wakes idle workers without lost notifications");
    }
    CommandDispatcher commands("test");registerMeshNodeCommands(commands,&app);
    check(!commands.executeText("mesh.rf arm 10",{}).value("ok").toBool(),"untrusted command context cannot authorize RF");
    auto human=CommandDispatcher::Context::human();human.allowTransmit=true;
    auto start=commands.execute({{"command","mesh.service"},{"args",QJsonObject{{"operation","start"},{"port",0},{"radioTxProfiles",QJsonObject{{"radio",profile}}}}}},human);
    check(start.value("ok").toBool()&&!start.value("radioTxAllowed").toBool(),"approved profile alone never arms RF");
    check(commands.executeText("mesh.rf arm 1",human).value("radioTxAllowed").toBool(),"separate local transmit-authorized command arms for limited lifetime");
    check(commands.executeText("mesh.service status",human).value("radioTxAllowed").toBool(),"local status immediately reflects successful RF authorization");
    QElapsedTimer authorization;authorization.start();
    check(wait([&]{return !commands.executeText("mesh.service status",human).value("radioTxAllowed").toBool();},2000),"RF watchdog automatically removes authorization");
    check(authorization.elapsed()>=800,"authorization expiry test waits for the actual watchdog, not a stale pre-arm snapshot");
    commands.executeText("mesh.service stop",human);
    return errors?1:0;
}
