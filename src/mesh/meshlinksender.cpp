#include "meshlinksender.h"
#include "meshwiremodem.h"
#include "../openradio/virtualsignaltransport.h"
#include <QTcpSocket>
#include <QQueue>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <cmath>

namespace Mesh {
namespace {QByteArray line(const QJsonObject &o){return QJsonDocument(o).toJson(QJsonDocument::Compact)+'\n';}}
struct LinkSender::Private {
    LinkSender *owner;
    LinkBinding link;
    QJsonObject peer;
    QString session,key;
    QQueue<LinkJob> queue;
    QTcpSocket *socket=nullptr;
    QTimer timer;
    QElapsedTimer clock,pace;
    LinkJob job;
    std::function<QJsonObject(quint64)> authorize;
    QByteArray buffer;
    std::unique_ptr<WireModem> modem;
    QVector<std::complex<float>> wave;
    qint64 deadline=0,delayUntil=0;
    int offset=0,sequence=0;
    quint64 sentSamples=0,totalSamples=0;
    bool ready=false,submitted=false,accepted=false,reported=false;
    qint64 airtimeMs=0;
    Private(LinkSender *o,const LinkBinding &l,const QJsonObject &p,const QString &s,const QString &k):owner(o),link(l),peer(p),session(s),key(k){clock.start();timer.setInterval(5);timer.setTimerType(Qt::PreciseTimer);QObject::connect(&timer,&QTimer::timeout,owner,[this]{tick();});}
    void finish(bool sent,const QString &reason){
        if(!socket)return;auto old=socket;socket=nullptr;old->disconnect(owner);if(sent)old->disconnectFromHost();else old->abort();old->deleteLater();wave.clear();modem.reset();
        if(!reported)emit owner->completed(job.ticket,sent,reason,sentSamples);if(queue.isEmpty())timer.stop();
    }
    void begin(){
        job=queue.dequeue();offset=sequence=0;sentSamples=0;ready=submitted=accepted=reported=false;buffer.clear();airtimeMs=0;
        if(!job.radioFrame.isEmpty())airtimeMs=(job.radioFrame.value("airtimeUs").toString().toLongLong()+999)/1000;
        if(link.bearer=="iq"){
            try{modem=std::make_unique<WireModem>(job.radioFrame.isEmpty()?link.modem:job.radioFrame.value("modem").toObject());QString error;wave=modem->modulate(job.wire,&error);if(wave.isEmpty()){emit owner->completed(job.ticket,false,error,0);modem.reset();return;}}
            catch(const std::exception &e){emit owner->completed(job.ticket,false,QString::fromUtf8(e.what()),0);return;}
            const float gain=float(std::pow(10.0,link.gainDb/20.0));for(auto &sample:wave)sample*=gain;
        }
        auto s=new QTcpSocket(owner);socket=s;s->setReadBufferSize(65536);s->setSocketOption(QAbstractSocket::LowDelayOption,1);
        deadline=clock.elapsed()+qMax<qint64>(15000,modem?qint64(wave.size())*1000/modem->sampleRate()+5000:0);delayUntil=clock.elapsed()+link.delayMs;
        QObject::connect(s,&QTcpSocket::connected,owner,[this,s]{if(socket==s){auto request=QJsonObject{{"op","peer"},{"session",session},{"key",key},{"from",link.tx},{"next",link.rx},{"txPort",link.txPort},{"rxPort",link.rxPort},{"bearer",link.bearer},{"modem",job.radioFrame.isEmpty()?link.modem:job.radioFrame.value("modem")},{"instanceId",peer.value("instanceId")}};if(!job.radioFrame.isEmpty())request["radioFrame"]=job.radioFrame;s->write(line(request));}});
        QObject::connect(s,&QTcpSocket::readyRead,owner,[this,s]{
            if(socket!=s)return;buffer+=s->readAll();if(buffer.size()>65536){finish(false,"invalid_peer_response");return;}
            while(socket&&buffer.contains('\n')){int end=buffer.indexOf('\n');const auto doc=QJsonDocument::fromJson(buffer.left(end));buffer.remove(0,end+1);
                if(!doc.isObject()||!doc.object().value("ok").toBool()){finish(false,"peer_rejected");return;}
                if(doc.object().value("ready").toBool())ready=true;
                else if(doc.object().value("accepted").toBool()){accepted=true;if(job.radioFrame.isEmpty()&&(!job.native||link.bearer=="data")){finish(true,{});return;}}
            }
        });
        QObject::connect(s,&QTcpSocket::disconnected,owner,[this,s]{if(socket==s)finish(false,"link_disconnected");});
        QObject::connect(s,&QTcpSocket::errorOccurred,owner,[this,s](QAbstractSocket::SocketError){if(socket==s)finish(false,"link_error");});
        s->connectToHost(peer.value("host").toString(),quint16(peer.value("port").toInt()));
    }
    void tick(){
        if(!socket){if(queue.isEmpty()){timer.stop();return;}begin();}
        if(!socket)return;if(clock.elapsed()>deadline){finish(false,"modem_timeout");return;}
        if(!ready||clock.elapsed()<delayUntil)return;
        if(socket->bytesToWrite()>256*1024){finish(false,"backpressure");return;}
        if(!submitted){submitted=true;
            if(!job.radioFrame.isEmpty()){
                const auto authorization=authorize?authorize(job.ticket):QJsonObject();
                if(!authorization.value("ok").toBool()){finish(false,"radio_start_not_authorized");return;}
            }
            pace.start();
            if(link.bearer=="data"){socket->write(line(job.native?QJsonObject{{"op","wire"},{"payloadBase64",QString::fromLatin1(job.wire.toBase64())}}:QJsonObject{{"op","packet"},{"packet",job.legacyPacket}}));return;}
        }
        if(link.bearer!="iq"){
            if(!job.radioFrame.isEmpty()&&pace.elapsed()>=airtimeMs){if(!reported){reported=true;emit owner->completed(job.ticket,true,{},0);}if(accepted)finish(true,{});}
            return;
        }
        const int rate=modem->sampleRate(),block=qBound(1024,(rate+199)/200,8192);
        for(int budget=0;budget<2&&offset<wave.size();++budget){if(offset>pace.elapsed()*rate/1000)break;
            const int count=qMin(block,wave.size()-offset);const auto bytes=virtualSignalComplexFloat32Payload(wave.constData()+offset,count);
            socket->write(line({{"op","iq"},{"sequence",sequence++},{"samples",QString::fromLatin1(bytes.toBase64())}}));offset+=count;sentSamples+=quint64(count);totalSamples+=quint64(count);
        }
        if(!job.radioFrame.isEmpty()&&!reported&&pace.elapsed()>=airtimeMs&&offset>=qMin<qint64>(wave.size(),airtimeMs*rate/1000)&&socket->bytesToWrite()==0){reported=true;emit owner->completed(job.ticket,true,{},sentSamples);}
        if(job.native&&offset>=wave.size()&&socket->bytesToWrite()==0&&(job.radioFrame.isEmpty()||reported))finish(true,{});
    }
};
LinkSender::LinkSender(const LinkBinding &l,const QJsonObject &p,const QString &s,const QString &key,QObject *parent):QObject(parent),d(new Private(this,l,p,s,key)){}
LinkSender::~LinkSender(){cancel();}
bool LinkSender::submit(const LinkJob &job,QString *error){
    if(pending()>=16){if(error)*error="Per-link TX queue is full";return false;}
    if(job.wire.size()>4096||job.legacyPacket.size()>16){if(error)*error="Invalid bounded link job";return false;}
    d->queue.enqueue(job);if(!d->timer.isActive())d->timer.start();return true;
}
void LinkSender::setStartAuthorization(std::function<QJsonObject(quint64)> f){d->authorize=std::move(f);}
int LinkSender::pending() const{return d->queue.size()+(d->socket?1:0);}
void LinkSender::cancel(const QString &reason){
    d->timer.stop();QList<QPair<quint64,quint64>> cancelled;
    if(d->socket&&!d->reported)cancelled.append({d->job.ticket,d->sentSamples});
    while(!d->queue.isEmpty())cancelled.append({d->queue.dequeue().ticket,0});
    if(d->socket){auto socket=d->socket;d->socket=nullptr;socket->disconnect(this);socket->abort();delete socket;}
    d->wave.clear();d->modem.reset();
    // Shutdown is silent; live link cancellation must retire native/lab tickets.
    if(!reason.isEmpty())for(const auto &job:cancelled)emit completed(job.first,false,reason,job.second);
}
QJsonObject LinkSender::status() const{return {{"tx",d->link.tx},{"rx",d->link.rx},{"txPort",d->link.txPort},{"rxPort",d->link.rxPort},{"bearer",d->link.bearer},{"modem",d->link.modem},{"queued",pending()},{"samples",QString::number(d->totalSamples)}};}
}
