#ifndef COMMANDCONSOLEDIALOG_H
#define COMMANDCONSOLEDIALOG_H

#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QStringList>
#include <functional>

class CommandDispatcher;
class CommandControlServer;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTcpSocket;
class QTimer;
class QToolButton;

class CommandConsoleDialog final : public QDialog {
    Q_OBJECT
public:
    // The translator receives a stable key and an English fallback.
    using Translator = std::function<QString(const QString &, const QString &)>;
    explicit CommandConsoleDialog(CommandDispatcher *dispatcher, Translator translator = {},
                                  QWidget *parent = nullptr);
    ~CommandConsoleDialog() override;
    void setControlServer(CommandControlServer *server);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QString text(const char *key, const char *fallback) const;
    QString redacted(QString value) const;
    QJsonValue sanitized(const QJsonValue &value, int depth = 0) const;
    QString inputEcho(const QString &line, const QJsonObject &parsed) const;
    QJsonObject parseRemoteText(const QString &line) const;
    void rememberSecret(const QString &secret);
    void appendReply(const QJsonObject &reply);
    void setStatus(const QString &message, bool error = false);
    void setIdentity(const QString &identity);
    void updateControls();
    void updatePolling();
    void refreshCatalog(bool display = false);
    void applyCatalog(const QJsonObject &reply);
    void showHelp();
    void executeLine(const QString &line, bool poll = false);
    void sendRemote(const QJsonObject &request);
    void readRemote(QTcpSocket *socket);
    void finishRemote(const QJsonObject &reply);
    void failRemote(const char *code, const QString &message);
    void openAgentSettings();
    void refreshAgentSettings();

    CommandDispatcher *dispatcher_ = nullptr;
    Translator translator_;
    QPointer<CommandControlServer> controlServer_;
    QComboBox *target_ = nullptr;
    QWidget *remoteFields_ = nullptr;
    QLineEdit *host_ = nullptr;
    QSpinBox *port_ = nullptr;
    QLineEdit *token_ = nullptr;
    QComboBox *catalog_ = nullptr;
    QToolButton *help_ = nullptr;
    QToolButton *send_ = nullptr;
    QToolButton *agentButton_ = nullptr;
    QCheckBox *metrics_ = nullptr;
    QPlainTextEdit *output_ = nullptr;
    QLineEdit *input_ = nullptr;
    QLabel *identity_ = nullptr;
    QLabel *status_ = nullptr;
    QTimer *pollTimer_ = nullptr;
    QTimer *requestTimer_ = nullptr;
    QTcpSocket *socket_ = nullptr;
    QByteArray incoming_;
    QString activeToken_;
    QStringList secrets_;
    QStringList history_;
    QString historyDraft_;
    int historyIndex_ = 0;
    QString identityText_;
    bool busy_ = false;
    bool metricsSupported_ = false;
    bool remoteCatalog_ = false;

    QDialog *agentDialog_ = nullptr;
    QSpinBox *agentPort_ = nullptr;
    QCheckBox *agentControl_ = nullptr;
    QLineEdit *agentAddress_ = nullptr;
    QLineEdit *agentToken_ = nullptr;
    QLabel *agentStatus_ = nullptr;
    QToolButton *agentStart_ = nullptr;
    QToolButton *agentStop_ = nullptr;
    QToolButton *agentCopy_ = nullptr;
};

#endif
