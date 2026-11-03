#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <functional>
#include <memory>

namespace Mesh {

// Returns an empty object on invalid metadata. Missing protocol/version are
// supplied; missing host uses fallbackHost. All non-public fields are discarded.
QJsonObject publicDiscoveryEntry(const QJsonObject &metadata, const QString &fallbackHost = {});

// Confined to its caller's thread. nowMs is a nonnegative monotonic clock value.
class NodeDirectory {
public:
    void setEnabled(bool enabled);
    bool enabled() const;
    // Unlike publicDiscoveryEntry, put requires the public envelope and rejects
    // every unknown field. Returns {ok,registered,expiresInMs} or {ok,error}.
    QJsonObject put(const QJsonObject &entry, const QString &owner, qint64 nowMs);
    QJsonArray entries(qint64 nowMs);
    void revoke(const QString &owner);
    int count(qint64 nowMs);

private:
    struct Record {
        QJsonObject entry;
        QString owner;
        qint64 refreshedMs = 0;
    };
    void expire(qint64 nowMs);
    bool m_enabled = false;
    QMap<QString, Record> m_records;
};

// Outbound TCP only. Configuration is private and never included in status.
// Use a dedicated registered credential where possible, rather than an admin
// token. Like other Qt network objects, use this object only on its own thread.
class DirectoryAnnouncer : public QObject {
public:
    explicit DirectoryAnnouncer(std::function<QJsonObject()> metadata, QObject *parent = nullptr);
    ~DirectoryAnnouncer() override;
    static bool validateConfiguration(const QJsonObject &configuration, QString *error = nullptr);
    // Invalid configuration leaves the current configuration unchanged.
    bool configure(const QJsonObject &configuration, QString *error = nullptr);
    void stop();
    QJsonObject status() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace Mesh
