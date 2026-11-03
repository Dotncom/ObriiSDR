#pragma once
#include "meshproject.h"
#include <QDialog>
#include <QList>
#include <memory>

namespace Mesh {
class RemoteDialog : public QDialog {
    Q_OBJECT
public:
    explicit RemoteDialog(const Project &,QWidget *parent=nullptr);
    ~RemoteDialog() override;

    // These documents deliberately exclude credentials and session ownership.
    QJsonObject endpointConfiguration() const;
    bool setEndpointConfiguration(const QJsonObject &,QString *error=nullptr);
    QJsonObject experimentResults() const;
    bool searchInstances(QString *error=nullptr);
    QList<QJsonObject> discoveredInstances() const;
    bool assignDiscoveredInstance(int row,const QJsonObject &,QString *error=nullptr);
    bool registerDiscoveredInstance(int row,const QJsonObject &,const QString &code,QString *error=nullptr);
    bool executeProtocolCommand(int row,const QJsonObject &args,QString *error=nullptr);
signals:
    void discoveryFinished();
    void instanceRegistrationFinished(int row,bool ok,const QString &error);
    void protocolCommandFinished(int row,bool ok,const QJsonObject &reply,const QString &error);
protected:
    void closeEvent(QCloseEvent *) override;
    void done(int result) override;
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
