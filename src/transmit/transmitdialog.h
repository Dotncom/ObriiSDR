#ifndef TRANSMITDIALOG_H
#define TRANSMITDIALOG_H

#include "transmitterbackend.h"
#include "hackrftransmitterbackend.h"
#include "transmitwaveformgenerator.h"
#include "signallabgenerator.h"

#include <QDialog>
#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QVector>

#include <complex>
#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QSlider;
class QTimer;
class QToolButton;
class QIODevice;
class TxWaveformPreviewWidget;
class DmrTransmitEncoder;
class SignalLabDialog;
#ifdef _WIN32
class WindowsMicrophoneCapture;
#endif

#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
class QAudioInput;
#endif

class TransmitDialog final : public QDialog {
public:
    explicit TransmitDialog(const QString &language, QWidget *parent = nullptr);
    ~TransmitDialog() override;

    using HackRfReceiverUseChecker = std::function<bool(int)>;

    void setLanguage(const QString &language);
    void setHackRfReceiverUseChecker(HackRfReceiverUseChecker checker);
    void prepareFt8Message(const QString &message, double dialFrequencyHz,
                           double audioFrequencyHz, int slotParity);
    bool startPreparedFt8Automatically(QString *error = nullptr);
    void cancelAutomaticFt8();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QString text(const QString &english, const QString &ukrainian) const;
    TxConfiguration configurationFromUi() const;
    TxModulation selectedModulation() const;
    double signalBandwidthFromUi() const;
    bool validateSignalBandwidth(const TxConfiguration &configuration);
    bool writeActiveIq(const std::complex<float> *samples, int count, QString *error);
    void applyLiveSignalShape();
    QString signalShapeSummary(const TxConfiguration &configuration) const;
    void buildUi();
    void populateAudioInputs();
    void populateHackRfDevices();
    void applyMicrophoneLevel();
    void updateMicrophoneLevelText();
    void updateModeUi();
    void applyModulationPreset(TxModulation modulation);
    void updateTexts();
    void loadSettings();
    void saveSettings() const;
    void startTransmission();
    void startLaboratoryTransmission(const SignalLabGenerationResult &result);
    void stopTransmission(const QString &reason = QString());
    void startTextSimulation(const TxConfiguration &configuration);
    void beginTextWaveform();
    void startMicrophoneSimulation(TxConfiguration configuration);
    void startManualCw(TxConfiguration configuration);
    void startAudioFile(TxConfiguration configuration);
    void startImageTransmission(TxConfiguration configuration);
    void readMicrophone();
    void processMicrophoneBytes(const QByteArray &bytes);
    void processLiveSource();
    QVector<std::complex<float>> modulateActiveAudio(const QVector<float> &audio,
                                                     QString *error);
    bool configureDmrEncoder(const TxConfiguration &configuration, QString *error);
    bool beginDmrCall(QString *error);
    bool finishDmrCall(QString *error);
    void setLiveKey(bool pressed);
    void chooseMediaFile();
    void editMediaImage();
    void updateProgress();
    void exportIq();
    void setStatus(const QString &message, bool error = false);
    bool hackRfSelected() const;
    bool selectedHackRfBusyForReception() const;
    TransmitterBackend *selectedBackend();

    QString language;
    HackRfReceiverUseChecker hackRfReceiverUseChecker;
    SimulatorTransmitterBackend simulator;
    HackRfTransmitterBackend hackRf;
    TransmitterBackend *activeBackend = nullptr;
    TxConfiguration activeConfiguration;
    TxModulatorState modulatorState;
    TxBandwidthLimiterState bandwidthLimiterState;
    QVector<std::complex<float>> bandwidthLimitedIq;
    std::unique_ptr<DmrTransmitEncoder> dmrEncoder;
    bool dmrCallActive = false;
    bool dmrFinishing = false;

    QComboBox *backendCombo = nullptr;
    QComboBox *deviceCombo = nullptr;
    QComboBox *modulationCombo = nullptr;
    QComboBox *sourceCombo = nullptr;
    QComboBox *mediaModeCombo = nullptr;
    QComboBox *audioInputCombo = nullptr;
    QComboBox *ft8SlotCombo = nullptr;
    QToolButton *audioRefreshButton = nullptr;
    QSlider *microphoneLevelSlider = nullptr;
    QLabel *microphoneLevelValueLabel = nullptr;
    QComboBox *sampleRateCombo = nullptr;
    QComboBox *deviceSampleRateCombo = nullptr;
    QComboBox *bandwidthCombo = nullptr;
    QComboBox *signalBandwidthCombo = nullptr;
    QDoubleSpinBox *frequencySpin = nullptr;
    QSpinBox *levelSpin = nullptr;
    QSpinBox *txGainSpin = nullptr;
    QSpinBox *watchdogSpin = nullptr;
    QCheckBox *rfAmpCheck = nullptr;
    QCheckBox *armCheck = nullptr;
    QDoubleSpinBox *deviationSpin = nullptr;
    QDoubleSpinBox *toneSpin = nullptr;
    QSpinBox *cwWpmSpin = nullptr;
    QDoubleSpinBox *symbolRateSpin = nullptr;
    QDoubleSpinBox *gaussianBtSpin = nullptr;
    QSpinBox *modulationDepthSpin = nullptr;
    QCheckBox *invertDataCheck = nullptr;
    QCheckBox *ft8UtcCheck = nullptr;
    QGroupBox *dmrBox = nullptr;
    QSpinBox *dmrColorCodeSpin = nullptr;
    QComboBox *dmrTimeslotCombo = nullptr;
    QSpinBox *dmrSourceIdSpin = nullptr;
    QSpinBox *dmrDestinationIdSpin = nullptr;
    QComboBox *dmrCallTypeCombo = nullptr;
    QComboBox *dmrPrivacyCombo = nullptr;
    QSpinBox *dmrPrivacyKeyIdSpin = nullptr;
    QLineEdit *dmrPrivacyKeyEdit = nullptr;
    QPlainTextEdit *messageEdit = nullptr;
    QPushButton *startButton = nullptr;
    QPushButton *stopButton = nullptr;
    QPushButton *exportButton = nullptr;
    QPushButton *signalLabButton = nullptr;
    QPushButton *mediaBrowseButton = nullptr;
    QPushButton *mediaEditButton = nullptr;
    QPushButton *mediaPlayButton = nullptr;
    QPushButton *mediaPauseButton = nullptr;
    QPushButton *mediaStopButton = nullptr;
    QPushButton *liveKeyButton = nullptr;
    QLabel *rfLockLabel = nullptr;
    QLabel *backendLabel = nullptr;
    QLabel *rfLabel = nullptr;
    QLabel *deviceLabel = nullptr;
    QLabel *basebandLabel = nullptr;
    QLabel *deviceRateLabel = nullptr;
    QLabel *bandwidthLabel = nullptr;
    QLabel *levelLabel = nullptr;
    QLabel *txGainLabel = nullptr;
    QLabel *watchdogLabel = nullptr;
    QLabel *modeLabel = nullptr;
    QLabel *sourceLabel = nullptr;
    QLabel *mediaPathLabel = nullptr;
    QLabel *mediaModeLabel = nullptr;
    QLabel *audioInputLabel = nullptr;
    QLabel *microphoneLevelLabel = nullptr;
    QLabel *deviationLabel = nullptr;
    QLabel *signalBandwidthLabel = nullptr;
    QLabel *toneLabel = nullptr;
    QLabel *cwSpeedLabel = nullptr;
    QLabel *symbolRateLabel = nullptr;
    QLabel *gaussianBtLabel = nullptr;
    QLabel *modulationDepthLabel = nullptr;
    QLabel *dmrColorCodeLabel = nullptr;
    QLabel *dmrTimeslotLabel = nullptr;
    QLabel *dmrSourceIdLabel = nullptr;
    QLabel *dmrDestinationIdLabel = nullptr;
    QLabel *dmrCallTypeLabel = nullptr;
    QLabel *dmrPrivacyLabel = nullptr;
    QLabel *dmrPrivacyKeyIdLabel = nullptr;
    QLabel *dmrPrivacyKeyLabel = nullptr;
    int amDepthPercent = 90;
    int askDepthPercent = 70;
    QLabel *statusLabel = nullptr;
    QLabel *statsLabel = nullptr;
    QProgressBar *progressBar = nullptr;
    TxWaveformPreviewWidget *preview = nullptr;
    SignalLabDialog *signalLabDialog = nullptr;
    QTimer *progressTimer = nullptr;
    QTimer *ft8StartTimer = nullptr;
    QTimer *sourceTimer = nullptr;
    QTimer *microphoneLevelApplyTimer = nullptr;
    QElapsedTimer elapsedTimer;
    int hackRfStoppedStreamPolls = 0;
    double plannedDurationSeconds = 0.0;
    QVector<std::complex<float>> pendingTextIq;
    bool textSimulation = false;
    bool automaticFt8Active = false;
    bool pttMode = false;
    bool liveKeyPressed = false;
    float microphoneRmsDb = -120.0f;
    float microphonePeakDb = -120.0f;
    float microphoneSoftwareGain = 1.0f;
    bool mediaPaused = false;
    QString mediaPath;
    QImage preparedMediaImage;
    QVector<float> mediaAudio;
    QVector<std::complex<float>> atvFrame;
    int mediaPosition = 0;
    double manualCwPhase = 0.0;

#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
    QAudioInput *audioInput = nullptr;
    QIODevice *audioDevice = nullptr;
    int audioCaptureChannels = 1;
    int audioCaptureSampleSize = 16;
    int audioCaptureSampleType = 0;
    bool audioCaptureLittleEndian = true;
    QByteArray audioCapturePendingBytes;
    QElapsedTimer microphoneDiagnosticTimer;
    qint64 microphoneDiagnosticFrames = 0;
    qint64 microphoneDiagnosticNonZeroFrames = 0;
    float microphoneDcPreviousInput = 0.0f;
    float microphoneDcPreviousOutput = 0.0f;
    bool microphoneSilenceWarned = false;
#endif
#ifdef _WIN32
    WindowsMicrophoneCapture *windowsMicrophoneCapture = nullptr;
#endif
};

#endif // TRANSMITDIALOG_H
