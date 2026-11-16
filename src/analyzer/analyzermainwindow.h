#ifndef ANALYZERMAINWINDOW_H
#define ANALYZERMAINWINDOW_H

#include "analyzerframechannel.h"
#include "analyzerreceiver.h"
#include "instrumentsweepengine.h"
#include "spectrumframerecorder.h"
#include "spectrumscienceanalyzer.h"

#include <QMainWindow>
#include <QFutureWatcher>
#include <QElapsedTimer>
#include <memory>

class AnalyzerSpectrumWidget;
class AnalyzerInstrumentDialog;
class SpectrumMarkerPairsDialog;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QSlider;
class QTimer;

class AnalyzerMainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit AnalyzerMainWindow(bool connectOnStart, QWidget *parent = nullptr);
    ~AnalyzerMainWindow() override;

private:
    void buildUi();
    void applyAnalyzerSettings();
    void consumeFrame(const AnalyzerSpectrumFrame &frame);
    void consumeRecordedFrame(const SpectrumFrameRecord &frame);
    void refreshDisplay();
    void refreshMetrics();
    void refreshMarkers();
    void openRecording();
    void togglePlayback();
    void readNextRecordedFrame();
    void exportCurrentCsv();
    void connectLive();
    void refreshReceivers();
    void toggleStandaloneReceiver();
    void toggleWideSweep();
    AnalyzerReceiverSettings currentReceiverSettings(double centerHz) const;
    bool consumeWideSweepFrame(const AnalyzerSpectrumFrame &frame);
    void updateSourceControls();
    void updateReceiverControlRanges();
    void populateSampleRates();
    void populateReceiverInputs();
    double selectedSampleRateHz() const;
    void setSelectedSampleRateHz(double sampleRateHz);
    double selectedCenterFrequencyHz() const;
    void setCenterFrequencyHz(double frequencyHz);
    void updateCenterFrequencyUnit();
    void scheduleStandaloneReceiverRestart();
    int selectedFftLength() const;
    int selectedDisplayReductionRatio() const;
    void synchronizeResolutionFields();
    void updateResolutionControls();
    void saveSettings();
    void applyFrequencyView();
    void setStatus(const QString &message, const QColor &color);
    QString uiText(const QString &english) const;
    void translateStaticUi();

    AnalyzerFrameClient liveClient;
    AnalyzerReceiver standaloneReceiver;
    SpectrumScienceAnalyzer analyzer;
    AnalyzerSpectrumWidget *spectrum = nullptr;
    QLabel *statusLabel = nullptr;
    QLabel *sourceLabel = nullptr;
    QLabel *rbwLabel = nullptr;
    QLabel *metricsLabel = nullptr;
    QLabel *markerLabel = nullptr;
    QPushButton *connectButton = nullptr;
    QPushButton *receiverStartButton = nullptr;
    QPushButton *receiverRefreshButton = nullptr;
    QPushButton *playButton = nullptr;
    QComboBox *detectorCombo = nullptr;
    QSpinBox *detectorFrames = nullptr;
    QDoubleSpinBox *vbwSpin = nullptr;
    QDoubleSpinBox *sweepStartSpin = nullptr;
    QDoubleSpinBox *sweepStopSpin = nullptr;
    QDoubleSpinBox *sweepOverlapSpin = nullptr;
    QDoubleSpinBox *sweepEdgeSpin = nullptr;
    QSpinBox *sweepAverageFramesSpin = nullptr;
    QComboBox *sweepFobosModeCombo = nullptr;
    QCheckBox *sweepContinuousCheck = nullptr;
    QPushButton *sweepButton = nullptr;
    QDoubleSpinBox *scaleTopSpin = nullptr;
    QDoubleSpinBox *scaleBottomSpin = nullptr;
    QSlider *scaleTopSlider = nullptr;
    QSlider *scaleBottomSlider = nullptr;
    QSlider *frequencyZoomSlider = nullptr;
    QSlider *frequencyZoomDivisorSlider = nullptr;
    QLabel *scaleTopValueLabel = nullptr;
    QLabel *scaleBottomValueLabel = nullptr;
    QLabel *frequencyZoomValueLabel = nullptr;
    QLabel *frequencyZoomDivisorValueLabel = nullptr;
    QDoubleSpinBox *centerSpin = nullptr;
    QComboBox *centerUnitCombo = nullptr;
    QComboBox *sampleRateCombo = nullptr;
    QDoubleSpinBox *viewMinimumSpin = nullptr;
    QDoubleSpinBox *viewMaximumSpin = nullptr;
    QDoubleSpinBox *cycleSpin = nullptr;
    QCheckBox *maxHoldCheck = nullptr;
    QCheckBox *minHoldCheck = nullptr;
    QCheckBox *averageCheck = nullptr;
    QCheckBox *currentTraceCheck = nullptr;
    QCheckBox *p50Check = nullptr;
    QCheckBox *p90Check = nullptr;
    QCheckBox *p99Check = nullptr;
    QComboBox *markerCombo = nullptr;
    SpectrumMarkerPairsDialog *markerPairsDialog = nullptr;
    AnalyzerInstrumentDialog *instrumentDialog = nullptr;
    QComboBox *receiverCombo = nullptr;
    QComboBox *receiverInputCombo = nullptr;
    QComboBox *receiverFftCombo = nullptr;
    QCheckBox *hzPerBinCheck = nullptr;
    QDoubleSpinBox *hzPerBinSpin = nullptr;
    QLabel *resolutionActualLabel = nullptr;
    QComboBox *displayReductionRatioCombo = nullptr;
    QComboBox *displayReductionMethodCombo = nullptr;
    QLabel *displayReductionInfoLabel = nullptr;
    QLabel *receiverCapabilityLabel = nullptr;
    QLabel *receiverLegacyGainLabel = nullptr;
    QSpinBox *receiverLnaSpin = nullptr;
    QSpinBox *receiverVgaSpin = nullptr;
    QWidget *uhdSettingsPanel = nullptr;
    QDoubleSpinBox *uhdGainSpin = nullptr;
    QComboBox *uhdAntennaCombo = nullptr;
    QLineEdit *uhdImageEdit = nullptr;
    QCheckBox *automaticViewCheck = nullptr;
    QCheckBox *automaticAttachCheck = nullptr;
    QTimer *playbackTimer = nullptr;
    QTimer *attachRetryTimer = nullptr;
    QTimer *receiverRestartTimer = nullptr;
    std::unique_ptr<SpectrumFrameReader> reader;
    QString recordingPath;
    bool playbackRunning = false;
    int activeMarker = 0;
    qint64 previousFrameElapsedMs = -1;
    std::vector<float> currentFrequencies;
    std::vector<float> currentLevels;
    std::vector<float> sourceFrequencies;
    std::vector<float> sourceLevels;
    int currentFftLength = 0;
    double sourceBinWidthHz = 0.0;
    double centerUnitFactor = 1.0e6;
    bool synchronizingResolutionFields = false;
    QVector<AnalyzerReceiverDevice> receiverDevices;
    QFutureWatcher<QVector<AnalyzerReceiverDevice>> *receiverScanWatcher = nullptr;
    InstrumentSweepEngine sweepEngine;
    bool sweepActive = false;
    int sweepSegmentIndex = -1;
    int sweepSettleFrames = 0;
    int sweepFramesAccumulated = 0;
    int sweepPassCount = 0;
    bool sweepUsesNativeAgileScan = false;
    bool sweepRestartPending = false;
    QVector<int> sweepSegmentFrameCounts;
    QElapsedTimer sweepDisplayTimer;
    double sweepOriginalCenterHz = 0.0;
    bool ukrainianUi = false;
};

#endif
