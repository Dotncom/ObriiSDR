#pragma once
#include "meshports.h"
#include <QObject>
#include <QJsonObject>
#include <memory>
#include <functional>

namespace Mesh {
struct LinkJob {
    quint64 ticket=0;
    QByteArray wire;
    QJsonObject legacyPacket;
    bool native=false;
    QJsonObject radioFrame;
};
// One bounded queue per directed port/link, independently paced and cancellable.
class LinkSender : public QObject {
    Q_OBJECT
public:
    LinkSender(const LinkBinding &,const QJsonObject &peer,const QString &session,const QString &key,QObject *parent=nullptr);
    ~LinkSender() override;
    bool submit(const LinkJob &,QString *error);
    void setStartAuthorization(std::function<QJsonObject(quint64)>);
    int pending() const;
    void cancel(const QString &reason={});
    QJsonObject status() const;
signals:
    void completed(quint64 ticket,bool sent,const QString &reason,quint64 samples);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
}
