#pragma once
#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QQueue>
#include <functional>
#include <memory>

namespace Mesh {
constexpr quint16 DiscoveryPort=17699;
class DiscoveryResponder : public QObject {
public:
    explicit DiscoveryResponder(std::function<QJsonObject()> metadata,QObject *parent=nullptr);
    bool start();
    bool listening() const { return socket.state()==QAbstractSocket::BoundState; }
private:
    void receive();
    QUdpSocket socket;
    std::function<QJsonObject()> metadata;
    QElapsedTimer window;
    int responses=0;
    QQueue<QString> recent;
};
class DiscoveryClient : public QObject {
    Q_OBJECT
public:
    explicit DiscoveryClient(QObject *parent=nullptr);
    ~DiscoveryClient() override;
    bool setEndpoints(const QJsonArray &,QString *error=nullptr);
    bool start(QString *error=nullptr);
    void probe(const QString &numericHost);
    void probeEndpoint(const QString &numericHost,quint16 port,bool directory=false);
    void cancel();
    QJsonObject statistics() const;
    bool searching() const { return timer.isActive(); }
    QList<QJsonObject> instances() const { return found.values(); }
signals:
    void foundInstance(const QJsonObject &);
    void finished();
private:
    void receive();
    void finish();
    struct RoutedState;
    std::unique_ptr<RoutedState> routed;
    QUdpSocket socket;
    QTimer timer;
    QByteArray request;
    QString nonce;
    QMap<QString,QJsonObject> found;
};
}
