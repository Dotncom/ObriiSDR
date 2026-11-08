#pragma once
#include <QObject>
#include <QJsonObject>
#include <QStringList>
#include <memory>

namespace Mesh {
struct ProtocolTransmission {
    quint64 ticket=0;
    QString interfaceId;
    QByteArray wire;
};
// Owned by one node worker. Transport never interprets/re-routes native frames.
class ProtocolAdapter : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool start(const QJsonObject &configuration,const QStringList &interfaces,
                       const QString &localStateDirectory,QString *error)=0;
    virtual QJsonObject command(const QJsonObject &request,qint64 nowMs)=0;
    virtual void receive(const QString &interfaceId,const QByteArray &wire,qint64 nowMs)=0;
    virtual void step(qint64 nowMs)=0;
    virtual void complete(quint64 ticket,bool sent,qint64 nowMs)=0;
    virtual QJsonObject status() const=0;
    virtual void stop()=0;
signals:
    void transmit(quint64 ticket,const QString &interfaceId,const QByteArray &wire);
    void event(const QJsonObject &);
    void failed(const QString &);
};
std::unique_ptr<ProtocolAdapter> makeMeshtasticAdapter(QObject *parent=nullptr);
std::unique_ptr<ProtocolAdapter> makeReticulumAdapter(QObject *parent=nullptr);
std::unique_ptr<ProtocolAdapter> makeLoRaWanAdapter(QObject *parent=nullptr);
}
