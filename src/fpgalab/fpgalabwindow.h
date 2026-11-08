#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QString>
#include <QVector>
#include "fpgapipelinecompiler.h"
#include "openxc7project.h"

class QCloseEvent;
class QComboBox;
class QAction;
class QDoubleSpinBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QCheckBox;
class QLineEdit;
class QTabWidget;

class FpgaLabWindow final : public QMainWindow {
public:
    explicit FpgaLabWindow(QWidget *parent = nullptr);
    bool openPath(const QString &path);
    bool addCustomStageFromFile(const QString &path, QString *error = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    using Stage = FpgaPipelineStage;

    bool confirmDiscard();
    bool saveSource();
    bool savePipeline();
    bool loadSource(const QString &path);
    void loadPipeline();
    QJsonObject pipelineJson() const;
    bool applyPipelineJson(const QJsonObject &root);
    void scanProject();
    void drawPipeline();
    void updateInspector();
    void updateProjectSummary();
    void startBuild();
    void stopBuild();
    void refreshOpenXc7Report();
    void startOpenXc7Preflight();
    void startOpenXc7ModuleCheck();
    void startOpenXc7BitstreamBuild();
    void synthesizePipelineRtl();
    void launchOpenXc7Script(const QString &scriptPath, const QString &kind);
    void setBuildBusy(bool busy);
    QString openXc7OutputDir() const;
    QString pipelinePath() const;

    QString projectRoot_;
    QString projectFile_;
    QString buildTclFile_;
    QString binaryFile_;
    QString sourceFile_;
    QByteArray sourceHash_;
    bool sourceBom_ = false;
    bool sourceCrLf_ = false;
    QString vivadoPath_;
    QString buildScriptPath_;
    QString activeBuildKind_;
    OpenXc7ProjectReport openXc7Report_;
    QVector<Stage> stages_;
    bool pipelineDirty_ = false;
    QVector<QPushButton *> pipelineEditButtons_;
    QAction *savePipelineAction_ = nullptr;
    QAction *vivadoBuildAction_ = nullptr;
    QAction *stopBuildAction_ = nullptr;
    QProcess *buildProcess_ = nullptr;
    QTreeWidget *fileTree_ = nullptr;
    QPlainTextEdit *sourceEditor_ = nullptr;
    QPlainTextEdit *buildLog_ = nullptr;
    QPlainTextEdit *openXc7ReportView_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QLineEdit *moduleTopEdit_ = nullptr;
    QCheckBox *nixCheck_ = nullptr;
    QPushButton *bitstreamButton_ = nullptr;
    QVector<QPushButton *> openXc7Buttons_;
    QGraphicsScene *pipelineScene_ = nullptr;
    QGraphicsView *pipelineView_ = nullptr;
    QComboBox *stageType_ = nullptr;
    QComboBox *pipelinePreset_ = nullptr;
    QDoubleSpinBox *stageFrequency_ = nullptr;
    QDoubleSpinBox *stageBandwidth_ = nullptr;
    QDoubleSpinBox *inputRate_ = nullptr;
    QDoubleSpinBox *clockRate_ = nullptr;
    QSpinBox *stageFactor_ = nullptr;
    QPushButton *synthPipelineButton_ = nullptr;
    QPushButton *logicEditorButton_ = nullptr;
    QPushButton *stageLogicButton_ = nullptr;
    QLabel *stageLogicName_ = nullptr;
    QLabel *projectSummary_ = nullptr;
    QLabel *pipelineSummary_ = nullptr;
    int selectedStage_ = -1;
};
