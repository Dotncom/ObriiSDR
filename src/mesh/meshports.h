#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QVector>

namespace Mesh {
struct PortBinding {
    QString id,direction,backend="network",receiverId="main",channelId="rx0";
    bool enabled=true;
    double centerHz=145500000;
    QJsonObject modem;
    QJsonObject json() const;
};
struct LinkBinding {
    QString tx,rx,txPort="radio",rxPort="radio",bearer="data";
    bool enabled=true;
    int delayMs=0;
    double gainDb=-6;
    QJsonObject modem;
    QJsonObject json() const;
};
class PortPlan {
public:
    bool configure(const QJsonObject &topology,const QString &fallback,QString *error);
    const LinkBinding *link(const QString &tx,const QString &rx) const;
    const PortBinding *port(const QString &node,const QString &id,const QString &direction) const;
    QVector<LinkBinding> outgoing(const QString &node,const QString &port={}) const;
    QVector<PortBinding> ports(const QString &node) const { return bindings.value(node); }
    QJsonObject protocol(const QString &node) const {return protocols.value(node);}
    QJsonObject routingTopology() const;
    QJsonObject topology() const;
    QStringList nodes() const {return bindings.keys();}
private:
    QMap<QString,QVector<PortBinding>> bindings;
    QMap<QString,QJsonObject> protocols;
    QVector<LinkBinding> links;
    QJsonArray names;
};
}
