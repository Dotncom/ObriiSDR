#pragma once
#include "meshproject.h"
#include "meshruntime.h"
#include <QMainWindow>
#include <QMap>
#include <QPointer>
class QGraphicsScene;
class QGraphicsView;
class QGraphicsItem;
class QLineEdit;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QStackedWidget;
class QDockWidget;

namespace Mesh {
class SpectrumView;
class PacketDialog;
class RemoteDialog;
class Window : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(const QString &libraryFile,QWidget *parent=nullptr);
    ~Window() override;
    bool loadProjectFile(const QString &,QString *error);
    bool saveProjectFile(const QString &,QString *error);
    const Project &currentProject() const { return project; }
protected:
    void closeEvent(QCloseEvent *) override;
    void changeEvent(QEvent *) override;
private:
    void rebuild();
    void linksGeometry();
    void selection();
    void editDesired();
    void editNodeOptions();
    void editLinkOptions();
    void applyNodeJson(bool ports);
    void copyRxModem();
    void refreshRuntime();
    void refreshSpectrum();
    void refreshTopology();
    void showPacketLab();
    void showRemoteMesh();
    void editLibrary();
    void editMedium();
    void addNode();
    void addLink();
    void removeSelected();
    void dirty();
    bool maybeSave();
    QString selectedNode() const;
    QString libraryPath,projectPath;
    QVector<HostPreset> presets;
    Project project;
    Runtime runtime;
    bool modified=false,formBusy=false,closing=false,topologyPending=false;
    QString selectedLink;
    QGraphicsScene *scene=nullptr;
    QGraphicsView *view=nullptr;
    QMap<QString,QGraphicsItem *> nodeItems,linkItems;
    QMap<QString,QJsonObject> confirmedLinks;
    QPointer<PacketDialog> packetDialog;
    QPointer<RemoteDialog> remoteDialog;
    QStackedWidget *properties=nullptr;
    QLineEdit *nodeName=nullptr;
    QComboBox *device=nullptr,*mode=nullptr;
    QComboBox *rxBackend=nullptr,*txBackend=nullptr,*nodeProtocol=nullptr,*nodeRole=nullptr;
    QLineEdit *rxPortId=nullptr,*txPortId=nullptr,*receiverId=nullptr,*channelId=nullptr;
    QLineEdit *rxChannelId=nullptr,*protocolProfile=nullptr;
    QDoubleSpinBox *rxCenter=nullptr;
    QPlainTextEdit *portsJson=nullptr,*protocolJson=nullptr;
    QPushButton *portsJsonApply=nullptr,*protocolJsonApply=nullptr,*rxModemCopy=nullptr;
    QCheckBox *rxPortEnabled=nullptr,*txPortEnabled=nullptr;
    QComboBox *linkBearer=nullptr,*linkModem=nullptr,*loraBandwidth=nullptr,*loraCodingRate=nullptr,*loraOversampling=nullptr;
    QLineEdit *linkTxPort=nullptr,*linkRxPort=nullptr;
    QSpinBox *loraSf=nullptr,*loraPreamble=nullptr,*loraSyncWord=nullptr;
    QLabel *protocolHint=nullptr;
    QDoubleSpinBox *frequency=nullptr,*bandwidth=nullptr,*level=nullptr,*gain=nullptr;
    QDoubleSpinBox *nodeNumber=nullptr;
    QSpinBox *delay=nullptr;
    QCheckBox *enabled=nullptr;
    QLabel *actual=nullptr,*linkTitle=nullptr,*status=nullptr;
    QLabel *linkActual=nullptr;
    QPushButton *applyLinkButton=nullptr;
    QPlainTextEdit *logs=nullptr;
    QPushButton *applyButton=nullptr,*testButton=nullptr;
    QPushButton *startRxButton=nullptr,*stopRxButton=nullptr,*stopTxButton=nullptr;
    QDockWidget *spectrumDock=nullptr;
    SpectrumView *spectrumView=nullptr;
    QCheckBox *previewEnabled=nullptr;
    QCheckBox *nodeEnabled=nullptr;
    QLabel *spectrumTitle=nullptr;
    QAction *startAction=nullptr,*stopAction=nullptr,*addAction=nullptr,*connectAction=nullptr,*removeAction=nullptr,*libraryAction=nullptr,*mediumAction=nullptr;
    QAction *packetsAction=nullptr;
};
}
