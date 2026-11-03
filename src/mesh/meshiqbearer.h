#pragma once
#include "meshiqmodem.h"
#include <QObject>
#include <QThread>
#include <QJsonObject>

namespace Mesh {
class IqBearer : public QObject {
    Q_OBJECT
public:
    explicit IqBearer(QObject *parent=nullptr);
    ~IqBearer() override;
    bool start(const QJsonObject &topology,QString *error);
    void stop();
    bool ready() const { return ready_; }
    bool transmit(quint64 flight,const QString &from,const QString &to,const PacketEngine::Datagram &,QString *error);
    void updateLink(const QString &from,const QString &to,bool enabled,int delayMs,double gainDb);
    void updateNode(const QString &id,bool enabled);
    QJsonObject statistics() const { return stats; }
signals:
    void readyChanged();
    void decoded(const QString &receiver,const QByteArray &wire);
    void failed(const QString &error);
private:
    QThread thread;
    QObject *worker=nullptr;
    QUuid epoch;
    quint64 generation=0;
    bool ready_=false;
    QJsonObject stats;
};
}
