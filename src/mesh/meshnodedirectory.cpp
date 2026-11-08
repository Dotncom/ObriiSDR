#include "meshnodedirectory.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <utility>

namespace Mesh {
namespace {
constexpr qint64 EntryTtlMs = 90000;
constexpr int MaximumEntries = 64;
constexpr int MaximumReplyBytes = 64 * 1024;
constexpr int StageTimeoutMs = 1500;
constexpr int RefreshIntervalMs = 30000;

bool integer(const QJsonValue &value, int minimum, int maximum)
{
    const double number = value.toDouble();
    return value.isDouble() && std::isfinite(number) && number >= minimum &&
           number <= maximum && std::floor(number) == number;
}

bool numericUnicastIPv4(const QJsonValue &value)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    const QHostAddress address(text);
    // Roundtrip comparison excludes DNS, abbreviated, integer, padded and hex
    // spellings accepted by some address parsers. Loopback is intentionally valid.
    if (address.protocol() != QAbstractSocket::IPv4Protocol || address.toString() != text)
        return false;
    const quint32 firstOctet = address.toIPv4Address() >> 24;
    return firstOctet > 0 && firstOctet < 224;
}

QString uuid(const QJsonValue &value)
{
    if (!value.isString()) return {};
    QString text = value.toString();
    const QUuid parsed(text);
    if (parsed.isNull()) return {};
    if (text.startsWith('{') && text.endsWith('}')) text = text.mid(1, text.size() - 2);
    const auto canonical = parsed.toString(QUuid::WithoutBraces);
    return text.compare(canonical, Qt::CaseInsensitive) == 0 ? canonical : QString{};
}

bool onlyKeys(const QJsonObject &object, const QSet<QString> &allowed)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!allowed.contains(it.key())) return false;
    return true;
}

QJsonObject failure(const char *error)
{
    return {{"ok", false}, {"error", QLatin1String(error)}};
}

QString freshUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
} // namespace

QJsonObject publicDiscoveryEntry(const QJsonObject &metadata, const QString &fallbackHost)
{
    if ((metadata.contains("protocol") && metadata.value("protocol") != "obrii-mesh-node") ||
        (metadata.contains("version") && !integer(metadata.value("version"), 1, 1)))
        return {};
    const QString id = uuid(metadata.value("instanceId"));
    const QJsonValue host = metadata.contains("host") ? metadata.value("host") : QJsonValue(fallbackHost);
    const auto name = metadata.value("name");
    if (id.isEmpty() || !numericUnicastIPv4(host) || !integer(metadata.value("port"), 1, 65535) ||
        !name.isString() || name.toString().size() > 128 || !metadata.value("active").isBool() ||
        !metadata.value("availableBearers").isArray())
        return {};
    const auto bearers = metadata.value("availableBearers").toArray();
    if (bearers.isEmpty() || bearers.size() > 2) return {};
    QSet<QString> seen;
    for (const auto &bearer : bearers) {
        if (!bearer.isString() || (bearer != "data" && bearer != "iq") || seen.contains(bearer.toString()))
            return {};
        seen.insert(bearer.toString());
    }
    if (metadata.contains("directoryVersion") && !integer(metadata.value("directoryVersion"), 0, 1))
        return {};
    QJsonArray normalizedBearers;
    for (const auto &bearer : {QStringLiteral("data"), QStringLiteral("iq")})
        if (seen.contains(bearer)) normalizedBearers.append(bearer);
    QJsonObject result{{"protocol", "obrii-mesh-node"}, {"version", 1}, {"instanceId", id},
                       {"host", host}, {"port", metadata.value("port")}, {"name", name},
                       {"active", metadata.value("active")}, {"availableBearers", normalizedBearers}};
    if (metadata.contains("directoryVersion")) result["directoryVersion"] = metadata.value("directoryVersion");
    return result;
}

void NodeDirectory::setEnabled(bool enabled)
{
    m_enabled = enabled;
    if (!enabled) m_records.clear();
}

bool NodeDirectory::enabled() const { return m_enabled; }

void NodeDirectory::expire(qint64 nowMs)
{
    for (auto it = m_records.begin(); it != m_records.end();) {
        if (nowMs >= it->refreshedMs && nowMs - it->refreshedMs >= EntryTtlMs)
            it = m_records.erase(it);
        else
            ++it;
    }
}

QJsonObject NodeDirectory::put(const QJsonObject &entry, const QString &owner, qint64 nowMs)
{
    if (!m_enabled) return failure("directory_disabled");
    if (nowMs < 0) return failure("invalid_time");
    expire(nowMs);
    if (owner.isEmpty()) return failure("invalid_owner");
    static const QSet<QString> allowed{"protocol", "version", "instanceId", "host", "port", "name",
                                       "active", "availableBearers", "directoryVersion"};
    if (!onlyKeys(entry, allowed) || !entry.contains("protocol") || !entry.contains("version"))
        return failure("invalid_entry");
    const auto normalized = publicDiscoveryEntry(entry);
    if (normalized.isEmpty()) return failure("invalid_entry");
    const auto id = normalized.value("instanceId").toString();
    const auto existing = m_records.constFind(id);
    if (existing != m_records.constEnd() && existing->owner != owner) return failure("owner_conflict");
    if (existing == m_records.constEnd() && m_records.size() >= MaximumEntries) return failure("directory_full");
    m_records[id] = {normalized, owner, nowMs};
    return {{"ok", true}, {"registered", true}, {"expiresInMs", EntryTtlMs}};
}

QJsonArray NodeDirectory::entries(qint64 nowMs)
{
    if (!m_enabled || nowMs < 0) return {};
    expire(nowMs);
    QJsonArray result;
    for (const auto &record : m_records) result.append(record.entry);
    return result;
}

void NodeDirectory::revoke(const QString &owner)
{
    for (auto it = m_records.begin(); it != m_records.end();) {
        if (it->owner == owner) it = m_records.erase(it);
        else ++it;
    }
}

int NodeDirectory::count(qint64 nowMs) { return entries(nowMs).size(); }

struct DirectoryAnnouncer::Private {
    DirectoryAnnouncer *q;
    std::function<QJsonObject()> metadata;
    QJsonObject configuration;
    QTimer refresh;
    QPointer<QTimer> deadline;
    QPointer<QTcpSocket> socket;
    QByteArray buffer;
    QString requestId, nonce, catalogId, error;
    quint64 epoch = 0;
    qint64 lastSuccessUtcMs = 0;
    QElapsedTimer successAge;
    bool registered = false;
    bool registering = false;

    Private(DirectoryAnnouncer *owner, std::function<QJsonObject()> callback)
        : q(owner), metadata(std::move(callback))
    {
        refresh.setParent(q);
        refresh.setInterval(RefreshIntervalMs);
        QObject::connect(&refresh, &QTimer::timeout, q, [this] { begin(); });
    }

    void cancelStage()
    {
        ++epoch;
        if (deadline) deadline->stop();
        deadline.clear();
        if (socket) {
            auto *old = socket.data();
            socket.clear();
            QObject::disconnect(old, nullptr, q, nullptr);
            old->abort();
            old->deleteLater();
        }
        buffer.clear();
    }

    void clear()
    {
        refresh.stop();
        cancelStage();
        configuration = {};
        requestId.clear();
        nonce.clear();
        catalogId.clear();
        error.clear();
        registered = false;
        lastSuccessUtcMs = 0;
        successAge.invalidate();
    }

    void fail(const char *kind)
    {
        cancelStage();
        registered = false;
        error = QLatin1String(kind);
    }

    bool current(QTcpSocket *candidate, quint64 generation) const
    {
        return socket == candidate && epoch == generation && !configuration.isEmpty();
    }

    void begin()
    {
        if (configuration.isEmpty() || socket) return;
        nonce = freshUuid();
        catalogId.clear();
        open(false);
    }

    void open(bool registration)
    {
        cancelStage();
        registering = registration;
        requestId = freshUuid();
        auto *connection = new QTcpSocket(q);
        socket = connection;
        connection->setReadBufferSize(MaximumReplyBytes + 1);
        const auto generation = epoch;
        auto *timeout = new QTimer(connection);
        deadline = timeout;
        timeout->setSingleShot(true);
        timeout->setTimerType(Qt::PreciseTimer);
        timeout->setInterval(StageTimeoutMs);
        QObject::connect(timeout, &QTimer::timeout, q, [this, connection, generation] {
            if (current(connection, generation)) fail("timeout");
        });
        QObject::connect(connection, &QTcpSocket::connected, q, [this, connection, generation] {
            if (!current(connection, generation)) return;
            QJsonObject request;
            if (!registering) {
                request = {{"op", "discover"}, {"protocol", "obrii-mesh-discovery"}, {"version", 1},
                           {"nonce", nonce}, {"requestId", requestId}};
            } else {
                // The callback may reconfigure/stop its owner: recheck the epoch
                // after invoking application code before using private config.
                const auto raw = metadata ? metadata() : QJsonObject{};
                if (!current(connection, generation)) return;
                auto advertised = raw;
                if (configuration.contains("advertiseHost"))
                    advertised["host"] = configuration.value("advertiseHost");
                else if (advertised.value("host").isString() && advertised.value("host").toString().isEmpty())
                    advertised.remove("host");
                const auto entry = publicDiscoveryEntry(advertised, connection->localAddress().toString());
                if (entry.isEmpty()) { fail("invalid_metadata"); return; }
                request = {{"op", "directoryRegister"}, {"instanceId", catalogId},
                           {"token", configuration.value("token")}, {"requestId", requestId}, {"entry", entry}};
            }
            const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
            if (connection->write(bytes) != bytes.size()) fail("transport_error");
        });
        QObject::connect(connection, &QTcpSocket::readyRead, q, [this, connection, generation] {
            if (current(connection, generation)) receive();
        });
        QObject::connect(connection, &QTcpSocket::disconnected, q, [this, connection, generation] {
            if (!current(connection, generation)) return;
            receive();
            if (current(connection, generation)) fail(buffer.isEmpty() ? "transport_error" : "invalid_reply");
        });
        QObject::connect(connection, &QAbstractSocket::errorOccurred, q,
                         [this, connection, generation](QAbstractSocket::SocketError) {
            if (!current(connection, generation)) return;
            // A peer may close immediately after writing its complete reply.
            receive();
            if (current(connection, generation)) fail("transport_error");
        });
        timeout->start();
        connection->connectToHost(QHostAddress(configuration.value("host").toString()),
                                  quint16(configuration.value("port").toInt()));
    }

    void receive()
    {
        if (!socket) return;
        if (socket->bytesAvailable() + buffer.size() > MaximumReplyBytes) {
            fail("reply_too_large");
            return;
        }
        buffer += socket->read(MaximumReplyBytes + 1 - buffer.size());
        if (buffer.size() > MaximumReplyBytes) { fail("reply_too_large"); return; }
        const int newline = buffer.indexOf('\n');
        if (newline < 0) return;
        if (!buffer.mid(newline + 1).trimmed().isEmpty()) { fail("invalid_reply"); return; }
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(buffer.left(newline), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) { fail("invalid_reply"); return; }
        const auto reply = document.object();
        if (reply.value("requestId") != requestId) { fail("invalid_reply"); return; }
        if (reply.value("ok") != true) {
            fail(registering ? "registration_rejected" : "discovery_rejected");
            return;
        }
        if (!registering) {
            if (reply.value("protocol") != "obrii-mesh-discovery" || !integer(reply.value("version"), 1, 1) ||
                reply.value("op") != "instance" || reply.value("nonce") != nonce ||
                !integer(reply.value("directoryVersion"), 1, 1)) {
                fail("invalid_reply");
                return;
            }
            auto node = reply;
            node["protocol"] = "obrii-mesh-node";
            const auto entry = publicDiscoveryEntry(node);
            if (entry.isEmpty()) { fail("invalid_reply"); return; }
            catalogId = entry.value("instanceId").toString();
            open(true);
        } else {
            if (reply.value("registered") != true || !integer(reply.value("expiresInMs"), EntryTtlMs, EntryTtlMs) ||
                uuid(reply.value("instanceId")) != catalogId) {
                fail("invalid_reply");
                return;
            }
            cancelStage();
            registered = true;
            lastSuccessUtcMs = QDateTime::currentMSecsSinceEpoch();
            successAge.start();
            error.clear();
        }
    }
};

DirectoryAnnouncer::DirectoryAnnouncer(std::function<QJsonObject()> metadata, QObject *parent)
    : QObject(parent), d(new Private(this, std::move(metadata)))
{
}

DirectoryAnnouncer::~DirectoryAnnouncer() { d->clear(); }

bool DirectoryAnnouncer::validateConfiguration(const QJsonObject &configuration, QString *error)
{
    if (error) error->clear();
    if (configuration.isEmpty()) return true;
    static const QSet<QString> allowed{"host", "port", "token", "advertiseHost"};
    const auto token = configuration.value("token");
    if (!onlyKeys(configuration, allowed) || !numericUnicastIPv4(configuration.value("host")) ||
        !integer(configuration.value("port"), 1, 65535) || !token.isString() ||
        token.toString().isEmpty() || token.toString().size() > 4096 ||
        (configuration.contains("advertiseHost") && !numericUnicastIPv4(configuration.value("advertiseHost")))) {
        if (error) *error = "invalid_configuration";
        return false;
    }
    return true;
}

bool DirectoryAnnouncer::configure(const QJsonObject &configuration, QString *error)
{
    if (!validateConfiguration(configuration, error)) return false;
    d->clear();
    if (configuration.isEmpty()) return true;
    d->configuration = configuration;
    d->refresh.start();
    d->begin();
    return true;
}

void DirectoryAnnouncer::stop() { d->clear(); }

QJsonObject DirectoryAnnouncer::status() const
{
    return {{"configured", !d->configuration.isEmpty()},
            {"registered", d->registered && d->successAge.isValid() && d->successAge.elapsed() < EntryTtlMs},
            {"lastSuccessUtcMs", d->lastSuccessUtcMs}, {"error", d->error}};
}

} // namespace Mesh
