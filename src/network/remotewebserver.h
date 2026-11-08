#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QJsonObject;
class RemoteVfoService;

// Small dependency-free HTTP/WebSocket gateway for the remote VFO service.
// It is intentionally LAN-oriented; Internet exposure requires an authenticated
// TLS reverse proxy or VPN.
class RemoteWebServer final : public QObject {
    Q_OBJECT
public:
    explicit RemoteWebServer(RemoteVfoService *service, QObject *parent = nullptr);
    ~RemoteWebServer() override;

    bool start(const QString &bindAddress, quint16 port);
    void stop();
    bool isListening() const;
    quint16 port() const;
    QString statusText() const;

signals:
    void statusChanged(const QString &status);

private:
    void acceptConnections();
    void readSocket(QTcpSocket *socket);
    void closeSocket(QTcpSocket *socket);
    bool processHttpRequest(QTcpSocket *socket, QByteArray &buffer);
    bool processWebSocketFrames(QTcpSocket *socket, QByteArray &buffer);
    void sendHttp(QTcpSocket *socket,
                  int status,
                  const QByteArray &contentType,
                  const QByteArray &body);
    void sendWebSocketFrame(QTcpSocket *socket, quint8 opcode, const QByteArray &payload);
    void sendJsonToOwner(const QString &ownerId, const QJsonObject &frame);
    void sendBinaryToOwner(const QString &ownerId,
                           const QJsonObject &frame,
                           const QByteArray &payload);
    static QByteArray browserPage();

    RemoteVfoService *service_ = nullptr;
    QTcpServer *server_ = nullptr;
    QHash<QTcpSocket *, QByteArray> buffers_;
    QHash<QTcpSocket *, QString> owners_;
    QHash<QString, QTcpSocket *> ownerSockets_;
    quint64 nextOwner_ = 1;
    QString status_ = QStringLiteral("Browser server stopped");
};
