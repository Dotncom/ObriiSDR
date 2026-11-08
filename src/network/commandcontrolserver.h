#ifndef OBRII_COMMAND_CONTROL_SERVER_H
#define OBRII_COMMAND_CONTROL_SERVER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <array>

class CommandDispatcher;
class QTcpServer;
class QTcpSocket;
class QTimer;

class CommandControlServer final : public QObject {
    Q_OBJECT
public:
    explicit CommandControlServer(CommandDispatcher *dispatcher, QObject *parent = nullptr);
    ~CommandControlServer() override;

    bool start(quint16 port = 0, QString *error = nullptr);
    void stop();
    bool isListening() const;
    quint16 port() const;
    QString token() const;
    void setAllowControl(bool allow);
    bool allowControl() const;
    QJsonObject status() const;

signals:
    void statusChanged();

private:
    struct Client {
        QByteArray input;
        QTimer *timer = nullptr;
        qint64 deadlineMs = 0;
        bool processing = false;
        bool responding = false;
        bool rejected = false;
    };
    void acceptConnections();
    void readClient(QTcpSocket *socket);
    void dispatchRequest(QTcpSocket *socket, const QJsonObject &request,
                         bool controlAllowed, quint64 generation);
    void expireClient(QTcpSocket *socket);
    void removeClient(QTcpSocket *socket);
    void reject(QTcpSocket *socket, const QString &code, const QString &message);
    void markRejected(QTcpSocket *socket);
    void respond(QTcpSocket *socket, const QJsonObject &response);
    bool authentic(const QString &supplied) const;
    bool admitCommand();

    CommandDispatcher *dispatcher_ = nullptr;
    QTcpServer *server_ = nullptr;
    QHash<QTcpSocket *, Client> clients_;
    QString token_;
    QElapsedTimer clock_;
    std::array<qint64, 20> commandTimes_{};
    std::size_t rateHead_ = 0;
    std::size_t rateCount_ = 0;
    quint64 generation_ = 0;
    quint64 requests_ = 0;
    quint64 authFailures_ = 0;
    quint64 rejected_ = 0;
    bool allowControl_ = false;
    bool dispatching_ = false;
};

#endif
