#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPoint>
#include <vector>
#include <memory>
#include <QSet>
#include "radiosettings.h"
#include "dspgraphmodel.h"
#include "dspstreammanager.h"
#include "spectrumcomputeoptions.h"
#include <QPointer>
class DspBranchEngine;
class DspBranchAudioOutput;
class DspComputeDialog;
class AudioFilterChainWidget;

class DspFlowScene;
class QCloseEvent;
class QResizeEvent;
class QComboBox;
class QDialog;
class QLabel;
class QGraphicsView;
class QMenu;
class QPushButton;
class QTimer;
class QToolButton;

class DspFlowPanel final : public QDialog {
    Q_OBJECT

public:
    explicit DspFlowPanel(QWidget *parent = nullptr);
    ~DspFlowPanel() override;

    void setLanguage(bool ukrainian);
    QString configurationJson() const;
    bool setConfigurationJson(const QString &json);
    bool importExperimentLayout(const QString &json, QString *error = nullptr);
    void setMainAudioFilterChainWidget(AudioFilterChainWidget *widget);
    bool canUndoBoard() const;
    bool canRedoBoard() const;
    void undoBoard();
    void redoBoard();
    void setControlStates(bool receiverRunning, const QHash<QString, bool> &enabledByBlockType);
    void setFineTuneRangeHz(double rangeHz);
    bool shouldUpdateMultiVfoSpectrum();
    void updateMultiVfoSpectrum(const QString &configurationJson,
                                const std::vector<float> &frequencies,
                                const std::vector<float> &levels);
    bool shouldUpdateWorkspaceSpectrum();
    int workspaceSpectrumTargetBins() const;
    void updateWorkspaceSpectrum(const std::vector<float> &frequencies,
                                 const std::vector<float> &levels,
                                 double centerHz,
                                 double listeningHz,
                                 double sampleRate,
                                 double bandwidthHz,
                                 int modulationType);
    void setWorkspaceMode(bool enabled);
    void setAnalogPeakMeterEnabled(bool enabled);
    void setAnalogPeakMeterStyle(int style);
    void setAnalogPeakMeterTarget(double frequencyHz, bool valid);
    void setWorkspaceVisualizationSettings(const QJsonObject &settings);
    bool isWorkspaceMode() const { return workspaceMode_; }
    void addMultiVfoBranch(int channelIndex = -1);
    void addDdcBranch();
    int vfoIndexForBlock(const QString &blockId) const;
    bool assignVfoIndexToBlock(const QString &blockId, int channelIndex);
    QJsonObject blockSettings(const QString &blockId) const;
    QString blockTypeForId(const QString &blockId) const;
    QString boundWorkspaceSettingsBlockId(const QString &viewBlockId) const;
    QJsonArray workspaceDisplayTargets(const QString &settingsBlockId) const;
    bool bindWorkspaceSettingsBlock(const QString &settingsBlockId, const QString &viewBlockId);
    bool setBlockSettings(const QString &blockId, const QJsonObject &settings);
    int workspaceSectorCount() const;
    bool dockSettingsDialog(const QString &blockId, QDialog *dialog, int sectorIndex = -1);
    void setBranchSourceContext(const RadioSettings &settings, bool running, int updateMs = 100, int overlapPercent = 0);
    QString branchStatus(const QString &blockId) const;
    void setExternalEndpoint(const DspStreamEndpoint &endpoint);
    DspStreamEndpoint externalEndpoint() const { return externalEndpoint_; }
    SpectrumComputeOptions spectrumComputeOptions() const { return spectrumComputeOptions_; }
    void setSpectrumComputeOptions(const SpectrumComputeOptions &options);
    QString spectrumComputeStatus() const { return spectrumComputeStatus_; }
    void setSpectrumComputeStatus(const QString &status);
    void showComputeDialog();
    bool startBranchRecording(const QString &id, const QString &directory);
    void stopBranchRecording(const QString &id);
    bool branchRecordingRequested(const QString &id) const;

signals:
    void externalEndpointChanged();
    void spectrumComputeOptionsChanged();
    void spectrumComputeStatusChanged(const QString &status);
    void configurationChanged(const QString &json);
    void blockActivated(const QString &type, const QString &id);
    void blockCreated(const QString &type, const QString &id);
    void blockRemoved(const QString &type, const QString &id);
    void controlTriggered(const QString &controlType,
                          const QString &controlId,
                          const QString &targetType,
                          const QString &targetId);
    void controlStateRefreshRequested();
    void fineTuneDeltaRequested(double deltaHz);
    void workspaceScaleChanged(int direction);
    void workspacePanRequested(int deltaPixels, int widthPixels);
    void workspaceTuneContextRequested(double frequency, const QPoint &globalPos);
    void workspaceAutoTuneRequested(double frequency);
    void workspaceScienceMarkerRequested(double frequency);
    void workspaceMultiVfoSelectionRequested(double lowHz, double highHz);
    void workspaceListeningFrequencyRequested(double frequency);
    void workspaceCenterFrequencyRequested(double frequency);
    void workspaceTuningRequested(double listeningFrequency, double centerFrequency);
    void workspaceAnalogPeakMeterToggled(bool enabled);
    void workspaceAnalogPeakMeterStyleChanged(int style);
    void workspaceModeExitRequested();
    void workspaceCloseRequested();

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuildAddMenu();
    void refreshWorkspaceProfiles(const QString &selectedName = QString());
    void saveWorkspaceProfile();
    void loadWorkspaceProfile(int index);
    void deleteWorkspaceProfile();
    void scheduleConfigurationChanged();
    void addBlock(const QString &type);
    QString blockTitle(const QString &type) const;
    void refreshSectorControls();
    void syncWorkspaceViewport();
    void refreshBranches();
    void queueBoardHistory(const QString &mergeKey);
    void commitBoardHistory();
    void resetBoardHistory();
    void trimBoardHistory();
    void updateHistoryActions();
    void restoreBoardHistory(int index);
    QByteArray captureBoardHistory() const;
    QByteArray captureFilterHistory(const QByteArray &board) const;
    void refreshCurrentFilterHistory();
    friend struct DspFlowHistoryTestAccess;

    struct BoardHistoryEntry {
        QByteArray board;
        QByteArray filters;
    };
    QVector<BoardHistoryEntry> boardHistory_;
    int boardHistoryIndex_ = -1;
    bool restoringHistory_ = false;
    bool historyPending_ = false;
    QString historyMergeKey_;
    QString pendingHistoryMergeKey_;
    QElapsedTimer historyMergeTimer_;
    QTimer *historyTimer_ = nullptr;
    QToolButton *undoButton_ = nullptr;
    QToolButton *redoButton_ = nullptr;
    QPointer<AudioFilterChainWidget> mainAudioFilterChain_;

    bool ukrainian_ = false;
    bool loading_ = false;
    DspFlowScene *scene_ = nullptr;
    QGraphicsView *view_ = nullptr;
    QToolButton *addButton_ = nullptr;
    QLabel *profileLabel_ = nullptr;
    QComboBox *profileCombo_ = nullptr;
    QPushButton *saveProfileButton_ = nullptr;
    QPushButton *deleteProfileButton_ = nullptr;
    QPushButton *openButton_ = nullptr;
    QPushButton *renameButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    QPushButton *defaultButton_ = nullptr;
    QPushButton *fitButton_ = nullptr;
    QLabel *sectorLayoutLabel_ = nullptr;
    QComboBox *sectorLayoutCombo_ = nullptr;
    QComboBox *sectorIndexCombo_ = nullptr;
    QPushButton *placeInSectorButton_ = nullptr;
    QPushButton *fullScreenButton_ = nullptr;
    QPushButton *returnToMainButton_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QMenu *addMenu_ = nullptr;
    QTimer *saveTimer_ = nullptr;
    QElapsedTimer multiVfoSpectrumUpdateTimer_;
    QElapsedTimer workspaceSpectrumUpdateTimer_;
    bool workspaceMode_ = false;
    bool analogPeakMeterEnabled_ = false;
    int analogPeakMeterStyle_ = 0;
    bool analogPeakMeterTargetValid_ = false;
    QHash<QString, QString> knownFilterBlocks_;
    std::unique_ptr<DspBranchEngine> branchEngine_;
    std::shared_ptr<DspBranchAudioOutput> branchAudioOutput_;
    DspGraphPlan branchPlan_;
    RadioSettings branchSource_;
    bool branchSourceRunning_ = false;
    int mainFftUpdateMs_ = 100, mainFftOverlapPercent_ = 0;
    bool branchPlanDirty_ = true;
    DspStreamEndpoint externalEndpoint_;
    SpectrumComputeOptions spectrumComputeOptions_;
    QString spectrumComputeStatus_;
    QPointer<DspComputeDialog> computeDialog_;
    QHash<QString, QString> branchRecordings_;
    double analogPeakMeterTargetHz_ = 0.0;
};
