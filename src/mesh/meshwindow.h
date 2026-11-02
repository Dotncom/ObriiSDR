#pragma once
#include "meshproject.h"
#include "meshruntime.h"
#include <QMainWindow>
#include <QMap>
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

namespace Mesh {
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
private:
    void rebuild();
    void linksGeometry();
    void selection();
    void editDesired();
    void refreshRuntime();
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
    QStackedWidget *properties=nullptr;
    QLineEdit *nodeName=nullptr;
    QComboBox *device=nullptr,*mode=nullptr;
    QDoubleSpinBox *frequency=nullptr,*bandwidth=nullptr,*level=nullptr,*gain=nullptr;
    QSpinBox *delay=nullptr;
    QCheckBox *enabled=nullptr;
    QLabel *actual=nullptr,*linkTitle=nullptr,*status=nullptr;
    QPlainTextEdit *logs=nullptr;
    QPushButton *applyButton=nullptr,*testButton=nullptr;
    QAction *startAction=nullptr,*stopAction=nullptr,*addAction=nullptr,*connectAction=nullptr,*removeAction=nullptr,*libraryAction=nullptr,*mediumAction=nullptr;
};
}
