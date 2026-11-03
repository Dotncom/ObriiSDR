#pragma once
#include "meshports.h"
#include <QObject>
#include <atomic>
#include <complex>
#include <functional>
#include <memory>

namespace Mesh {
class BurstSink {
public:
    virtual ~BurstSink()=default;
    virtual bool send(const PortBinding &,const QJsonObject &localProfile,
                      const QVector<std::complex<float>> &,const std::atomic<bool> &cancel,
                      QString *error)=0;
};
class RadioEgress : public QObject {
    Q_OBJECT
public:
    using Factory=std::function<std::unique_ptr<BurstSink>()>;
    RadioEgress(const PortBinding &,const QJsonObject &localProfile,QObject *parent=nullptr,Factory factory={},qint64 authorizationMs=0);
    ~RadioEgress() override;
    static bool validate(const PortBinding &,const QJsonObject &localProfile,QString *error);
    bool submit(quint64 ticket,const QByteArray &,QString *error);
    void requestStop();
    void stop();
    QJsonObject status() const;
signals:
    void completed(quint64 ticket,bool sent,const QString &reason,quint64 samples);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
std::unique_ptr<BurstSink> makeRadioBurstSink();
}
