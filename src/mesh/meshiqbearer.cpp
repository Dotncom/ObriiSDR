#include "meshiqbearer.h"
#include "../cli/clivirtualiqsource.h"
#include "../openradio/virtualiqmedium.h"
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <memory>
#include <vector>
#include <cmath>

namespace Mesh {
namespace {
bool fail(QString *error,const char *message){if(error)*error=QLatin1String(message);return false;}
class IqWorker : public QObject {
    Q_OBJECT
public:
    QJsonObject topology;
    QUuid epoch;
    struct Node {
        QString id;bool enabled=true;
        std::unique_ptr<CliVirtualIqSource> rx;
        QTcpSocket *feed=nullptr;
        IqReceiver modem;
        quint64 samples=0,decoded=0;
    };
    struct Tx { QString from;QByteArray wire; };
    std::vector<std::unique_ptr<Node>> nodes;
    std::unique_ptr<VirtualIqMedium> medium;
    QTcpServer ingress;
    QTimer pump;
    QElapsedTimer clock,txClock,statsClock;
    QTcpSocket *publisher=nullptr;
    QVector<std::complex<float>> waveform;
    QQueue<Tx> queue;
    Tx current;
    int offset=0,publishers=0;
    quint32 sequence=0;
    quint64 sentSamples=0,sentBursts=0,decodedPackets=0,epochs=0;
    qint64 channelEpochMs=0;
    bool announced=false,broken=false;
    explicit IqWorker(const QJsonObject &t,const QUuid &e):topology(t),epoch(e) {ingress.setParent(this);pump.setParent(this);}
    ~IqWorker() override {
        pump.stop();ingress.disconnect();ingress.close();
        for(auto &n:nodes){n->rx->onDataAvailable={};n->rx->stop();}
        const auto sockets=findChildren<QTcpSocket *>();for(auto s:sockets){s->disconnect();s->abort();delete s;}
    }
    Node *node(const QString &id){for(auto &n:nodes)if(n->id==id)return n.get();return nullptr;}
    void wake(){if(!pump.isActive()&&!broken)pump.start();}
    void error(const char *text){if(broken)return;broken=true;pump.stop();emit failed(QLatin1String(text));}
    bool resetMedium() {
        VirtualIqMediumSettings settings;settings.sampleRate=IqModem::Rate;settings.centerFrequencyHz=145500000;
        settings.blockSamples=1024;settings.playoutDelayMs=100;settings.maximumBufferedBlocks=256;settings.defaultRoute=false;
        medium=std::make_unique<VirtualIqMedium>(settings);
        for(auto &n:nodes)if(!medium->addSubscriber(n->id))return false;
        for(const auto &v:topology.value("links").toArray()){
            const auto l=v.toObject();auto tx=node(l.value("tx").toString()),rx=node(l.value("rx").toString());
            if(!tx||!rx||!medium->setLink(tx->id,rx->id,{l.value("enabled").toBool()&&tx->enabled&&rx->enabled,l.value("delayMs").toInt(),float(l.value("gainDb").toDouble(-6))}))return false;
        }
        channelEpochMs=clock.elapsed();++epochs;return true;
    }
    void start() {
        clock.start();statsClock.start();pump.setInterval(5);pump.setTimerType(Qt::PreciseTimer);
        connect(&pump,&QTimer::timeout,this,[this]{tick();});
        for(const auto &v:topology.value("nodes").toArray()){
            const auto o=v.toObject();auto n=std::make_unique<Node>();n->id=o.value("id").toString();n->enabled=o.value("enabled").toBool(true);
            n->rx=std::make_unique<CliVirtualIqSource>();VirtualSignalEndpoint endpoint;endpoint.mode=VirtualSignalTransportMode::Tcp;endpoint.port=0;
            QString problem;if(!n->rx->start(endpoint,IqModem::Rate,145500000,2*1024*1024,&problem)){error("Cannot start virtual IQ receiver");return;}
            n->rx->onDataAvailable=[this]{wake();};n->feed=new QTcpSocket(this);n->feed->setSocketOption(QAbstractSocket::LowDelayOption,1);
            n->feed->connectToHost(QHostAddress::LocalHost,n->rx->listeningPort());nodes.push_back(std::move(n));
        }
        if(!resetMedium()||!ingress.listen(QHostAddress::LocalHost,0)){error("Cannot start virtual IQ medium");return;}
        connect(&ingress,&QTcpServer::newConnection,this,[this]{
            while(auto socket=ingress.nextPendingConnection()){
                if(publishers>=2){socket->abort();socket->deleteLater();continue;}++publishers;
                socket->setReadBufferSize(2*1024*1024);auto parser=std::make_shared<VirtualSignalStreamParser>();
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,parser]{
                    parser->append(socket->read(2*1024*1024));VirtualSignalFrame frame;QString problem;
                    while(parser->takeFrame(&frame,&problem)){
                        const auto source=frame.metadata.value("hubNodeId").toString();
                        if(!node(source)||!medium->publish(source,frame,clock.nsecsElapsed()+1000000000LL,&problem)){error("Virtual IQ medium rejected input");return;}
                    }
                    if(!problem.isEmpty()||parser->bufferedBytes()>2*1024*1024){error("Invalid virtual IQ publisher stream");return;}wake();
                });
                connect(socket,&QTcpSocket::disconnected,this,[this,socket]{--publishers;socket->deleteLater();});
            }
        });pump.start();
    }
    void send(const QString &from,const QByteArray &wire) {
        if(broken||!node(from)||queue.size()>=8){error("IQ transmitter queue rejected a packet");return;}
        queue.enqueue({from,wire});wake();
    }
    void beginTx() {
        if(queue.isEmpty()||publisher||!waveform.isEmpty())return;
        // The shared medium enforces a finite timestamp epoch; renew only at idle.
        if(clock.elapsed()-channelEpochMs>45000){
            if(medium->totalCounters().bufferedBlocks)return;
            for(const auto &n:nodes)if(n->rx->status().value("queuedBytes").toDouble()>0)return;
            if(!resetMedium()){error("Cannot renew IQ channel epoch");return;}
        }
        current=queue.dequeue();QString problem;waveform=IqModem::modulate(current.wire,&problem);
        if(waveform.isEmpty()){error("Cannot modulate AX.25 IQ packet");return;}offset=0;sequence=0;
        publisher=new QTcpSocket(this);auto socket=publisher;socket->setSocketOption(QAbstractSocket::LowDelayOption,1);
        connect(socket,&QTcpSocket::connected,this,[this]{txClock.start();wake();});
        connect(socket,&QTcpSocket::errorOccurred,this,[this](QAbstractSocket::SocketError e){if(e!=QAbstractSocket::RemoteHostClosedError)error("Virtual IQ transmitter connection failed");});
        connect(socket,&QTcpSocket::disconnected,this,[this,socket]{
            if(publisher==socket){publisher=nullptr;waveform.clear();}socket->deleteLater();wake();
        });socket->connectToHost(QHostAddress::LocalHost,ingress.serverPort());
    }
    qint64 firstTimestamp=0;
    void tick() {
        if(broken)return;
        if(!announced){
            bool ready=true;for(auto &n:nodes)ready&=n->feed->state()==QAbstractSocket::ConnectedState&&n->rx->status().value("connected").toBool();
            if(ready){announced=true;emit readySignal();}else if(clock.elapsed()>3000){error("Virtual IQ receiver startup timeout");return;}
        }
        beginTx();
        if(publisher&&publisher->state()==QAbstractSocket::ConnectedState&&!waveform.isEmpty()){
            for(int budget=0;budget<2&&offset<waveform.size();++budget){
                if(offset>txClock.elapsed()*IqModem::Rate/1000)break;
                if(publisher->bytesToWrite()>256*1024){error("IQ publisher backpressure exceeded");return;}
                if(offset==0){firstTimestamp=clock.nsecsElapsed()+1000000000LL;++sentBursts;}
                const int count=qMin(1024,waveform.size()-offset);VirtualSignalFrame frame;
                frame.payloadType=VirtualSignalPayloadType::ComplexFloat32;frame.sampleRate=IqModem::Rate;frame.centerFrequencyHz=145500000;
                frame.timestampNs=firstTimestamp+qint64(offset)*1000000000LL/IqModem::Rate;frame.sequence=sequence++;
                frame.streamId="mesh-iq";frame.protocolId="raw-iq";frame.metadata.insert("hubNodeId",current.from);
                frame.payload=virtualSignalComplexFloat32Payload(waveform.constData()+offset,count);
                const auto bytes=VirtualSignalStreamParser::wrap(frame);
                if(bytes.isEmpty()||publisher->write(bytes)!=bytes.size()){error("Cannot send virtual IQ samples");return;}
                offset+=count;sentSamples+=count;
            }
            if(offset>=waveform.size())publisher->disconnectFromHost();
        }
        float buffer[2048];bool receiving=false,decodedNow=false;
        for(auto &n:nodes){
            if(n->feed->bytesToWrite()>512*1024){error("Virtual IQ receiver backpressure exceeded");return;}
            for(const auto &frame:medium->takeReady(n->id,clock.nsecsElapsed()+1000000000LL,16)){
                const auto wire=VirtualSignalStreamParser::wrap(frame);if(n->feed->write(wire)!=wire.size()){error("Virtual RX feed failed");return;}
            }
            for(int budget=0;budget<16;++budget){
                const int count=int(n->rx->read(buffer,1024));if(!count)break;n->samples+=quint64(count);receiving=true;
                const auto frames=n->modem.append(buffer,count,n->rx->takeDiscontinuity());
                for(const auto &wire:frames){IqEnvelope envelope;QString problem;
                    if(IqModem::decode(wire,&envelope,&problem)&&envelope.session==epoch&&envelope.next==n->id){++decodedPackets;++n->decoded;decodedNow=true;emit decoded(n->id,wire);}
                }
            }
        }
        const bool idle=announced&&!publisher&&queue.isEmpty()&&medium->totalCounters().bufferedBlocks==0&&!receiving;
        if(decodedNow||idle||statsClock.elapsed()>=100){
        statsClock.restart();QJsonArray counters;for(const auto &n:nodes)counters.append(QJsonObject{{"id",n->id},{"samples",QString::number(n->samples)},
            {"decoded",QString::number(n->decoded)},{"fcsFrames",QString::number(n->modem.validFrames)},
            {"badBursts",QString::number(n->modem.badBursts)},{"discontinuities",QString::number(n->modem.resets)},
            {"queueDrops",n->rx->status().value("droppedSamples")}});
        emit statistics({{"sampleRate",IqModem::Rate},{"active",!idle},{"modem","AX25-AFSK1200-NFM"},{"txBursts",QString::number(sentBursts)},
            {"txSamples",QString::number(sentSamples)},{"decoded",QString::number(decodedPackets)},{"epochs",QString::number(epochs)},{"nodes",counters}});
        }
        if(idle)pump.stop();
    }
    void link(const QString &tx,const QString &rx,bool enabled,int delay,double gain) {
        auto ls=topology.value("links").toArray();bool found=false;
        for(int i=0;i<ls.size();++i){auto l=ls[i].toObject();if(l.value("tx")==tx&&l.value("rx")==rx){l["enabled"]=enabled;l["delayMs"]=delay;l["gainDb"]=gain;ls[i]=l;found=true;break;}}
        if(!found||!node(tx)||!node(rx)||!medium->setLink(tx,rx,{enabled&&node(tx)->enabled&&node(rx)->enabled,delay,float(gain)})){error("Cannot update IQ link");return;}
        topology["links"]=ls;wake();
    }
    void enable(const QString &id,bool active) {
        auto n=node(id);if(!n){error("Unknown IQ node");return;}n->enabled=active;n->modem.reset();
        for(const auto &v:topology.value("links").toArray()){const auto l=v.toObject();
            auto tx=node(l.value("tx").toString()),rx=node(l.value("rx").toString());
            if(!medium->setLink(tx->id,rx->id,{l.value("enabled").toBool()&&tx->enabled&&rx->enabled,l.value("delayMs").toInt(),float(l.value("gainDb").toDouble(-6))})){error("Cannot update IQ node");return;}
        }wake();
    }
signals:
    void readySignal();
    void decoded(const QString &,const QByteArray &);
    void statistics(const QJsonObject &);
    void failed(const QString &);
};
}
IqBearer::IqBearer(QObject *parent):QObject(parent) {}
IqBearer::~IqBearer(){stop();}
bool IqBearer::start(const QJsonObject &topology,QString *error) {
    stop();PacketEngine validate;if(!validate.configure(topology,error))return false;
    if(topology.value("nodes").toArray().size()>8)return fail(error,"IQ modem mode supports at most eight nodes");
    for(const auto &v:topology.value("links").toArray()){const auto l=v.toObject();const double gain=l.value("gainDb").toDouble(-6);
        if(l.value("delayMs").toInt()>500||!std::isfinite(gain)||gain< -120||gain>60)return fail(error,"IQ link delay must be 0..500 ms and gain -120..60 dB");
    }
    epoch=QUuid::createUuid();const auto currentGeneration=++generation;auto w=new IqWorker(topology,epoch);worker=w;w->moveToThread(&thread);
    connect(&thread,&QThread::started,w,&IqWorker::start);connect(&thread,&QThread::finished,w,&QObject::deleteLater);
    connect(w,&IqWorker::readySignal,this,[this,currentGeneration]{if(generation==currentGeneration){ready_=true;emit readyChanged();}});
    connect(w,&IqWorker::decoded,this,[this,currentGeneration](const QString &node,const QByteArray &wire){if(generation==currentGeneration)emit decoded(node,wire);});
    connect(w,&IqWorker::statistics,this,[this,currentGeneration](const QJsonObject &s){if(generation==currentGeneration)stats=s;});
    connect(w,&IqWorker::failed,this,[this,currentGeneration](const QString &s){if(generation==currentGeneration){ready_=false;emit failed(s);}});
    thread.start();return true;
}
void IqBearer::stop(){++generation;ready_=false;if(worker)worker->disconnect(this);thread.quit();thread.wait();worker=nullptr;stats={};}
bool IqBearer::transmit(quint64 flight,const QString &from,const QString &to,const PacketEngine::Datagram &packet,QString *error) {
    if(!ready_||!worker)return fail(error,"IQ modem is not ready");
    const auto wire=IqModem::encode({epoch,flight,from,to,packet},error);if(wire.isEmpty())return false;
    auto w=static_cast<IqWorker *>(worker);QMetaObject::invokeMethod(w,[w,from,wire]{w->send(from,wire);},Qt::QueuedConnection);return true;
}
void IqBearer::updateLink(const QString &from,const QString &to,bool on,int delay,double gain){if(!worker)return;auto w=static_cast<IqWorker *>(worker);QMetaObject::invokeMethod(w,[=]{w->link(from,to,on,delay,gain);},Qt::QueuedConnection);}
void IqBearer::updateNode(const QString &id,bool on){if(!worker)return;auto w=static_cast<IqWorker *>(worker);QMetaObject::invokeMethod(w,[=]{w->enable(id,on);},Qt::QueuedConnection);}
}
#include "meshiqbearer.moc"
