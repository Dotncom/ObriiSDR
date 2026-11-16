#ifndef OBRII_COMMAND_DISPATCHER_H
#define OBRII_COMMAND_DISPATCHER_H

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QThread>
#include <functional>

class CommandDispatcher {
public:
    enum class Access { Read, Control, Files, Transmit, Local };
    struct Context {
        QString origin = QStringLiteral("local");
        bool allowControl = false;
        bool allowFiles = false;
        bool allowTransmit = false;
        static Context human();
    };
    struct Spec {
        QString name;
        QString description;
        Access access = Access::Read;
        QJsonObject inputSchema;
        QStringList positional;
    };
    using Handler = std::function<QJsonObject(const QJsonObject &)>;

    explicit CommandDispatcher(const QString &frontend);
    CommandDispatcher(const CommandDispatcher &) = delete;
    CommandDispatcher &operator=(const CommandDispatcher &) = delete;
    bool add(const Spec &spec, Handler handler);
    bool addStandard(const QString &name, Handler handler);
    QJsonObject execute(const QJsonObject &request, const Context &context);
    QJsonObject executeText(const QString &text, const Context &context);
    QJsonObject catalog(const Context &context) const;
    QJsonObject parseText(const QString &text) const;
    QString instanceId() const { return instanceId_; }
    QString frontend() const { return frontend_; }
    static Spec standardSpec(const QString &name);
    static QJsonObject failure(const QString &code, const QString &message);

private:
    struct Entry { Spec spec; Handler handler; };
    bool allowed(Access access, const Context &context) const;
    QString validate(const QJsonObject &arguments, const QJsonObject &schema) const;
    QJsonObject finish(QJsonObject reply, const QString &command, const QJsonValue &id,
                       const Context &context, qint64 durationUs);
    QMap<QString, Entry> commands_;
    QString instanceId_;
    QString frontend_;
    QThread *thread_ = nullptr;
    bool executing_ = false;
    quint64 sequence_ = 0;
    QJsonArray events_;
};

#endif
