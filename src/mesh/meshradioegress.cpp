#include "meshradioegress.h"
#include "meshwiremodem.h"
#include <QMetaObject>
#include <QQueue>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>

namespace Mesh {
struct RadioEgress::Private {
    RadioEgress *owner;
    PortBinding port;
    QJsonObject profile;
    Factory factory;
    struct Job {quint64 ticket;QByteArray wire;};
    std::mutex mutex;
    std::condition_variable wake;
    QQueue<Job> queue;
    std::atomic<bool> cancel{false};
    std::atomic<bool> expired{false};
    std::chrono::steady_clock::time_point expires;
    std::atomic<int> pending{0};
    std::atomic<quint64> frames{0},samples{0},failed{0};
    std::thread thread,watchdog;
    Private(RadioEgress *o,const PortBinding &p,const QJsonObject &v,Factory f,qint64 milliseconds):owner(o),port(p),profile(v),factory(std::move(f)),expires(std::chrono::steady_clock::now()+std::chrono::milliseconds(qBound<qint64>(0,milliseconds,3600000))){}
    void expire(){
        std::unique_lock<std::mutex> lock(mutex);
        if(wake.wait_until(lock,expires,[&]{return cancel.load();}))return;
        expired=true;cancel=true;lock.unlock();wake.notify_all();
    }
    void run(){
        auto sink=factory?factory():makeRadioBurstSink();
        while(!cancel.load()){
            Job job;
            {std::unique_lock<std::mutex> lock(mutex);wake.wait(lock,[&]{return cancel.load()||!queue.isEmpty();});if(cancel.load())break;
                if(std::chrono::steady_clock::now()>=expires){expired=true;cancel=true;wake.notify_all();break;}job=queue.dequeue();}
            QString error;quint64 count=0;bool okay=false;
            try{WireModem modem(port.modem);const auto wave=modem.modulate(job.wire,&error);count=quint64(wave.size());
                if(!wave.isEmpty()&&!cancel.load()&&sink)okay=sink->send(port,profile,wave,cancel,&error);
            }catch(const std::exception &e){error=QString::fromUtf8(e.what());}
            --pending;if(okay){++frames;samples+=count;}else ++failed;
            if(!cancel.load())QMetaObject::invokeMethod(owner,[o=owner,ticket=job.ticket,okay,error,count]{emit o->completed(ticket,okay,error.left(256),okay?count:0);},Qt::QueuedConnection);
        }
    }
};
bool RadioEgress::validate(const PortBinding &p,const QJsonObject &v,QString *error){
    auto fail=[&](const QString &why){if(error)*error=why;return false;};
    if(!std::isfinite(p.centerHz)||p.centerHz<=0)return fail("RF TX frequency must be finite and positive");
    for(auto i=v.begin();i!=v.end();++i)if(!QStringList{"backend","deviceIndex","channel","gainDb","deviceRate","bandwidthHz","antenna","minHz","maxHz"}.contains(i.key()))return fail("Unknown local RF TX profile field");
    const auto backend=v.value("backend").toString();
    if(backend!="uhd"&&backend!="hackrf")return fail("RF TX profile requires native UHD or HackRF");
    auto number=[&](const char *key,double low,double high,double fallback,bool integer=false){const auto x=v.value(key);const double n=x.toDouble(fallback);return (!v.contains(key)||x.isDouble())&&std::isfinite(n)&&n>=low&&n<=high&&(!integer||std::floor(n)==n);};
    if(!v.contains("minHz")||!v.contains("maxHz")||!number("minHz",1,6e9,0,true)||!number("maxHz",1,6e9,0,true)||v.value("minHz").toDouble()>v.value("maxHz").toDouble()||p.centerHz<v.value("minHz").toDouble()||p.centerHz>v.value("maxHz").toDouble())return fail("Port frequency is outside the locally approved RF range");
    if(!number("deviceIndex",0,63,0,true)||!number("channel",0,7,0,true)||!number("gainDb",0,backend=="hackrf"?47:100,0,backend=="hackrf")||!number("bandwidthHz",0,56e6,0)||!number("deviceRate",2e6,20e6,2e6,true)||(v.contains("antenna")&&(!v.value("antenna").isString()||v.value("antenna").toString().size()>64)))return fail("Invalid bounded local RF TX profile");
    const double occupied=p.modem.value("type")=="lora"?p.modem.value("bw").toDouble(250000):12500;
    if(p.centerHz-occupied/2<v.value("minHz").toDouble()||p.centerHz+occupied/2>v.value("maxHz").toDouble())return fail("Occupied RF band exceeds the locally approved frequency interval");
    if(backend=="hackrf"&&(p.centerHz<1e6||v.value("channel").toInt()!=0))return fail("HackRF requires 1 MHz..6 GHz and channel zero");
    try{WireModem m(p.modem);if(m.sampleRate()<8000)return fail("Unsupported RF modem rate");}catch(const std::exception &e){return fail(QString::fromUtf8(e.what()));}
    return true;
}
RadioEgress::RadioEgress(const PortBinding &p,const QJsonObject &v,QObject *parent,Factory factory,qint64 authorizationMs):QObject(parent),d(new Private(this,p,v,std::move(factory),authorizationMs)){
    if(authorizationMs<=0||authorizationMs>3600000){d->cancel=true;return;}
    try{d->thread=std::thread([this]{d->run();});d->watchdog=std::thread([this]{d->expire();});}
    catch(...){requestStop();if(d->thread.joinable())d->thread.join();throw;}
}
RadioEgress::~RadioEgress(){stop();}
bool RadioEgress::submit(quint64 ticket,const QByteArray &wire,QString *error){
    std::lock_guard<std::mutex> lock(d->mutex);
    if(d->cancel.load()||std::chrono::steady_clock::now()>=d->expires||d->pending.load()>=4||wire.isEmpty()||wire.size()>4096){if(error)*error="RF port disarmed/expired, frame invalid or bounded TX queue full";return false;}
    ++d->pending;d->queue.enqueue({ticket,wire});d->wake.notify_one();return true;
}
void RadioEgress::requestStop(){{std::lock_guard<std::mutex> lock(d->mutex);d->cancel=true;}d->wake.notify_all();}
void RadioEgress::stop(){requestStop();if(d->watchdog.joinable())d->watchdog.join();if(d->thread.joinable())d->thread.join();std::lock_guard<std::mutex> lock(d->mutex);d->queue.clear();d->pending=0;}
QJsonObject RadioEgress::status() const{return {{"port",d->port.id},{"state",d->expired.load()?"expired":d->cancel.load()?"disarmed":"armed"},{"queue",d->pending.load()},{"frames",QString::number(d->frames.load())},{"samples",QString::number(d->samples.load())},{"failed",QString::number(d->failed.load())}};}
}
