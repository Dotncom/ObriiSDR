#pragma once
#include "meshpacketengine.h"
#include <QDialog>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
class QComboBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class QTableWidget;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class QDoubleSpinBox;

namespace Mesh {
class IqBearer;
class PacketDialog : public QDialog {
    Q_OBJECT
public:
    explicit PacketDialog(const Project &,QWidget *parent=nullptr);
    ~PacketDialog() override;
    const PacketEngine &simulation() const { return engine; }
private:
    void populate();
    void refresh();
    void send();
    void displayPayload();
    void updateLink(const QString &,const QString &,QCheckBox *,QSpinBox *);
    void editNodeMotion();
    void save(bool results);
    void open();
    void restartMode();
    QJsonObject iqTopology() const;
    PacketEngine engine;
    std::unique_ptr<IqBearer> bearer;
    QMap<QString,double> gains;
    QElapsedTimer iqClock;
    QTimer timer;
    bool busy=false;
    quint64 shown=0;
    QComboBox *source=nullptr,*destination=nullptr,*encoding=nullptr,*mode=nullptr;
    QLineEdit *text=nullptr;
    QSpinBox *ttl=nullptr,*retries=nullptr,*airBitrate=nullptr;
    QDoubleSpinBox *radioRange=nullptr;
    QPushButton *editMotion=nullptr;
    QCheckBox *avoidFailedLinks=nullptr;
    QLabel *metrics=nullptr,*routeLabel=nullptr,*errorLabel=nullptr;
    QTableWidget *packets=nullptr,*nodes=nullptr,*links=nullptr;
    QPlainTextEdit *payload=nullptr;
    QPushButton *run=nullptr,*step=nullptr,*sendButton=nullptr,*loadButton=nullptr,*scenarioButton=nullptr;
};
}
