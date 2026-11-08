#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <deque>
#include <queue>
#include <vector>
#include <functional>

namespace Mesh {
struct Project;
class PacketEngine {
public:
    static constexpr int MaximumNodes=64, MaximumPayload=4096, QueuePerNode=256;
    static constexpr int MaximumEvents=8192, MaximumActions=10000;
    static constexpr qint64 MaximumTimeMs=3600000;
    struct Datagram {
        QString id,source,destination;
        QByteArray payload;
        int ttl=16;
    };
    using IqTransmit=std::function<bool(quint64,const QString &,const QString &,const Datagram &,QString *)>;
    bool attachIqBearer(IqTransmit,QString *error);
    bool receiveIq(quint64 flight,const QString &from,const QString &at,const Datagram &,QString *error);
    void abortIq();
    bool configure(const Project &,QString *error);
    bool configure(const QJsonObject &topology,QString *error);
    bool submit(const Datagram &,QString *error);
    QString send(const QString &source,const QString &destination,const QByteArray &,int ttl,QString *error);
    bool setLink(const QString &tx,const QString &rx,bool enabled,int delayMs,QString *error);
    bool setNodeEnabled(const QString &id,bool enabled,QString *error);
    bool advanceTo(qint64 targetMs,int budget=512);
    bool step();
    qint64 now() const { return timeMs; }
    int pending() const { return int(events.size()+flights.size()); }
    QStringList route(const QString &source,const QString &destination) const;
    QJsonObject topology() const;
    QJsonObject initialTopology() const { return initial; }
    QJsonObject snapshot(bool includeHistory=true) const;
    QJsonArray results(quint64 after=0) const;
    QJsonObject scenario();
    static bool replay(const QJsonObject &,PacketEngine *output,QString *error);
private:
    struct Node {
        QString name;
        bool enabled=true;
        int queued=0;
        quint64 received=0,forwarded=0,delivered=0,dropped=0;
        QSet<QString> seen;
        QQueue<QString> seenOrder;
    };
    struct Link { QString tx,rx; bool enabled=true; int delayMs=0; };
    struct Event {
        qint64 due=0,created=0;
        quint64 order=0;
        QString at,from;
        Datagram packet;
        QStringList path;
    };
    struct Later { bool operator()(const Event &a,const Event &b) const { return a.due==b.due?a.order>b.order:a.due>b.due; } };
    struct Flight { Event arrival; qint64 deadline=0; };
    qint64 nextDue() const;
    void process(Event);
    bool enqueue(Event);
    void finish(const Event &,const QString &reason);
    void trace(const Event &,const QString &kind,const QString &next={});
    bool record(const QJsonObject &,QString *error);
    void changed();
    QMap<QString,Node> nodes;
    QMap<QString,Link> links;
    mutable QMap<QString,QMap<QString,QString>> parents;
    std::priority_queue<Event,std::vector<Event>,Later> events;
    QMap<quint64,Flight> flights;
    IqTransmit iqTransmit;
    quint64 nextFlight=0,iqRejected=0;
    QJsonObject initial;
    QJsonArray actions;
    int actionBytes=0;
    std::deque<QJsonObject> history,steps;
    qint64 timeMs=0;
    quint64 order=0,nextPacket=0,revision=0,serial=0;
    quint64 submitted=0,delivered=0,forwarded=0,dropped=0,bytes=0,delayTotal=0;
    quint64 historyEvicted=0,traceEvicted=0;
    QMap<QString,quint64> dropReasons;
};
}
