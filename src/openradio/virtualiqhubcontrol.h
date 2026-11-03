#pragma once
#include "virtualiqhub.h"
#include "virtualiqmedium.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QTcpServer>
#include <QSet>

// Optional, loopback-only control plane. No control work in the IQ hot path.
class VirtualIqHubControl : public QObject {
public:
    VirtualIqHubControl(VirtualIqHub &hub, VirtualIqMedium *medium,
                        const QJsonArray &initialLinks, bool defaultRoute, QObject *parent=nullptr);
    ~VirtualIqHubControl() override;
    bool listen(quint16 port, const QString &key, QString *error=nullptr);
    quint16 port() const { return server.serverPort(); }
private:
    QJsonObject execute(const QJsonObject &request);
    QJsonObject status() const;
    VirtualIqHub &hub;
    VirtualIqMedium *medium;
    QTcpServer server;
    QSet<QTcpSocket *> clients;
    QMap<QString,QJsonObject> links;
    QString key,instance;
    quint64 revision=0;
    bool defaultRoute;
    QElapsedTimer rateWindow;
    int requestsThisWindow=0;
};
