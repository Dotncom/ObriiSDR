#ifndef OBRII_USRP_LAB_JOB_H
#define OBRII_USRP_LAB_JOB_H

#include <QJsonObject>
#include <QObject>
#include <QProcess>

class UsrpLabJob final : public QObject {
    Q_OBJECT
public:
    explicit UsrpLabJob(QObject *parent = nullptr);
    ~UsrpLabJob() override;
    static UsrpLabJob *forOwner(QObject *owner);
    static QJsonObject arguments(const QStringList &words);
    QJsonObject command(const QJsonObject &request);
    QJsonObject status() const;

signals:
    void changed();
    void completed(const QJsonObject &result);

private:
    void finish(int exitCode, const QString &processError = {});
    void readDiagnostics();
    bool requestCancel();

    QProcess process_;
    QString directory_;
    QString action_;
    QByteArray diagnostics_;
    QJsonObject result_;
    bool active_ = false;
    bool cancelling_ = false;
};

#endif
