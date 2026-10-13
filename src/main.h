#ifndef MAIN_H
#define MAIN_H

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QTimer>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QSlider>
#include <QDial>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QGroupBox>
#include <QMap>
#include <QHash>
#include <QPointer>
#include <memory>
#include <QScrollArea>
#include <QCheckBox>
#include <QMainWindow>
#include <QDockWidget>
#include <QPlainTextEdit>
#include <QVector>
#include <QWheelEvent>
#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QCloseEvent>
#include <QMessageBox>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <QApplication>
#include <QDateTime>
#include <QCoreApplication>
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <QRadioButton>
#include <QButtonGroup>
#include <QElapsedTimer>
#include <QUdpSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSerialPort>
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif
#include "appconstants.h"
#include "fobosdevicecatalog.h"
#include "fft.h"
#include "dataprocessor.h"
#include "digitaldecoder.h"
#include "audioprocessor.h"
#include "networkcontroller.h"
#include "playbackmanager.h"
#include "recordingmanager.h"
#include "remoteaudioplayer.h"
#include "spectrumframerecorder.h"
#include "spectrumiqeventrecorder.h"
#include "spectrumscienceanalyzer.h"
#include "receivercalibrationtable.h"
#include "radiosettings.h"
#include "videoprocessor.h"
#include "videowidget.h"
#include "dmrhunterdetector.h"
#include "dmrprivacyutils.h"
#include "fpvhunterdetector.h"
#include "digitalvideohunterdetector.h"
#include "spectrumhuntercontrols.h"
#include "scanvisualassembler.h"
#include "frequencycontrol.h"
#include "fobosbackend.h"
#include "rtlsdrbackend.h"
#include "gnssacquisition.h"
#include "gnsssignalmonitor.h"
#include "qthlocator.h"
#include "scalewidget.h"
#include "MyGraphWidget.h"
#include "MyWaterfallWidget.h"
#include <fftw3.h>

class ZeroSpanDialog;
class ResearchAnalysisDialog;
class WirelessLabDialog;
class PropagationLabDialog;
class ZoomSpectrumDialog;
class ZoomDensityDialog;
class TransmitDialog;
class Ft8StationDialog;
class AudioFilterChainWidget;
class DspFlowPanel;
class MultiVfoWidget;
class GnssSdrPvtDialog;
class LrptDecodeDialog;

extern fobos_dev_t *device;
extern float* dataq;
extern float* iqData;
extern double globalFrequency; 
extern double actualFrequency; 
extern double listeningFrequency;
extern double globalSampleRate;
extern double globalBandwidth;
extern int globalModulationType;
extern int globalMode;
extern int fftLength;
extern double currentScale;
extern double minFrequency;
extern double maxFrequency;
extern float sensitivity;
extern float contrast;
class FFTResult;
class SpectrumFftWorker;
class FineTuneScaleWidget;
class QDialog;
class QFile;
class QStackedWidget;
class QToolButton;
class QTableWidget;
class QTableWidgetItem;
class QthMapWidget;
class DsdNeoBridge;
class GopherTrunkBridge;
extern int deviceID;

struct GnssNmeaSatellite {
    QString key;
    QString source;
    QString system;
    QString talker;
    int prn = -1;
    int elevationDeg = -1;
    int azimuthDeg = -1;
    int cn0DbHz = -1;
    bool usedInFix = false;
    qint64 lastSeenMs = 0;
};

struct SpectrumEventIqFrame {
    QByteArray data;
    double sampleRate = 0.0;
    int sampleCount = 0;
    bool channelized = true;
    qint64 utcMs = 0;
};

class YourClassName : public QMainWindow {
    Q_OBJECT

public:
   explicit YourClassName(QWidget *parent = nullptr);
    ~YourClassName();
    void showConfiguredInterface();
     std::unique_ptr<FFTResult> fftResult;
     std::unique_ptr<SpectrumFftWorker> spectrumFftWorker;
    void onFrequencyEntered();
    void onListeningFrequencyEntered();
    void onScaleChanged(int value);
    void onAdditionalScaleDivisorChanged(int value);
    void updateSpectrum();
    void onLnaGainChanged(int value);
    void onVgaGainChanged(int value);
    void onRtlAgcChanged(int state);
    void onRtlGainChanged(int value);
    void onSensitivityChanged(int value);
    void onContrastChanged(int value);
    void onLevelMinChanged(int value);
    void onLevelMaxChanged(int value);
    void onFineTuneDialChanged(int value);
    void onFineTuneDialReleased();
    void populateSampleRates();
    void populateAudioDevices();
private slots:
	void settingRange();
    void onDirectSamplingChanged(int index);
    void listFobosDevices();
    void startFobosProcessing();
    void stopFobosProcessing();
    void onSampleRateChanged(int index);
    void onfftLengthEntered();
    void onFftBinWidthModeChanged(bool checked);
    void onFftBinWidthChanged(double value);
    void onClkChanged(int index);
    void onCheckboxStateChanged(int state);
    void onAudioDeviceChanged(int index);
    void syncEnable(bool checked);
	void onModulationChanged(int id);
    void onBandwidthChanged();
    void updateFrequency();
    void updateCentralFrequency();
    void updateTuningFromScale(double tunedListeningFrequency, double tunedCenterFrequency);
    void panSpectrumView(int deltaPixels, int widthPixels);
    void doubleGraphEnable(bool checked);
    void colorGraphEnable(bool checked);
    void pollStopCompletion();
    void checkStreamStartup();
    void openNetworkSettingsDialog();
    void showHackRfRxSettings();
    void onNetworkStatusChanged(const QString &status);
    void onNetworkControlCommandReceived(const QJsonObject &command);
    void onAudioEnabledChanged(bool checked);
    void onDigitalTextDecoded(const QString &text);
    void onDigitalDecoderStatusChanged(const QString &status);
    void onFt8Decoded(const QDateTime &utc,
                      double rfFrequencyHz,
                      double audioFrequencyHz,
                      double dtSeconds,
                      int snrDbEstimate,
                      double qualityDb,
                      int costasHits,
                      const QString &message);
    void onDmrMetadataDetected(int colorCode,
                               int timeslot,
                               quint32 targetId,
                               quint32 sourceId,
                               int flco);
    void startRecording(bool momentary);
    void stopRecording(bool momentaryRelease);
    void startSpectrumFrameRecording();
    void stopSpectrumFrameRecording();
    void openSpectrumFrameReplay();
    void updateSpectrumFrameRecordingStatus(const QString &status);
    bool spectrumEventIqProducerNeeded() const;
    bool spectrumEventIqChannelized() const;
    void handleSpectrumEventIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount, bool channelized);
    void startPlayback();
    void stopPlayback();
    void refreshPlaybackFiles();
    void onPlaybackStarted(const QString &filePath, PlaybackManager::WavInfo info);
    void onPlaybackStopped();
    void onPlaybackStatusChanged(const QString &status);
    void onVideoStatusChanged(const QString &status);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
	void onWaterfallScaleChanged(int delta);
    void wheelEvent(QWheelEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    double effectiveScalePercent() const;
    QWidget *centralWidget;
    QStringList getFobosDevices();
    void refreshFobosDeviceList();
    void requestUsbDeviceRefresh();
    void continueUsbDeviceRefresh();
    QString formatFobosDeviceLabel(const FobosDeviceInfo &info) const;
    FobosDeviceInfo selectedFobosDeviceInfo() const;
    bool isRtlTcpSelected() const;
    bool isRtlSdrNativeSelected() const;
    int selectedRtlSdrNativeIndex() const;
    bool isSoapySdrSelected() const;
    bool isBladeRfNativeSelected() const;
    int selectedBladeRfNativeIndex() const;
    bool isHackRfNativeSelected() const;
    int selectedHackRfNativeIndex() const;
    bool isRtlBackendSelected() const;
    bool isExternalReceiverBackendSelected() const;
    bool normalizeRtlSdrSettings();
    void rebuildReceiverModeChoices();
    void updateReceiverSpecificControls();
    ReceiverStreamDescriptor makeRtlTcpStreamDescriptor(bool queueAudioBlocks,
                                                        bool publishIqSnapshot,
                                                        bool emitIqFrames) const;
    ReceiverStreamDescriptor makeRtlSdrNativeStreamDescriptor(bool queueAudioBlocks,
                                                              bool publishIqSnapshot,
                                                              bool emitIqFrames) const;
    ReceiverStreamDescriptor makeSoapySdrStreamDescriptor(bool queueAudioBlocks,
                                                          bool publishIqSnapshot,
                                                          bool emitIqFrames) const;
    ReceiverStreamDescriptor makeBladeRfNativeStreamDescriptor(bool queueAudioBlocks,
                                                               bool publishIqSnapshot,
                                                               bool emitIqFrames) const;
    ReceiverStreamDescriptor makeHackRfNativeStreamDescriptor(bool queueAudioBlocks,
                                                              bool publishIqSnapshot,
                                                              bool emitIqFrames) const;
    bool restartStreamForHardwareChange();
    void abandonFobosSessionWithoutClose(const char *reason);
    bool openFobosSession();
    bool closeFobosSession(bool clearIq = true);
    bool prepareFobosSessionFromSettings(const QString &reason);
    bool applyFobosSettings(bool forceFrequencyApply = false);
    double calibratedHardwareFrequency(double logicalFrequencyHz) const;
    double effectiveFrequencyCalibrationOffsetHz(double logicalFrequencyHz) const;
    ReceiverCalibrationContext receiverCalibrationContext(const RadioSettings &settings) const;
    double effectiveAmplitudeCalibrationOffsetDb(double frequencyHz) const;
    int setCalibratedActiveFrequencySafely(double logicalFrequencyHz, double *logicalActualFrequencyHz);
    void applyReceiverCalibrationLive();
    bool applyAgileStartupFrequencyKick(const QString &reason);
    bool applyAgileScanSettings(bool forceStop = false);
    bool applyStandardScanSettings(bool forceStop = false);
    QVector<double> agileScanFrequencyList(QString *error = nullptr) const;
    double agileScanAutoStepMhz() const;
    void applyAgileScanAutoStep(bool updateSpin = true);
    QVector<double> standardScanFrequencyList(QString *error = nullptr) const;
    QVector<double> listeningScanFrequencyList(QString *error = nullptr) const;
    double currentAgileScanCenterFrequencyHz() const;
    double currentStandardScanCenterFrequencyHz() const;
    void resetStandardScanState(bool clearSegments = true);
    void resetListeningScanState();
    bool applyStandardScanRetune(double targetFrequencyHz, const char *reason);
    bool applyListeningScanTarget(double targetFrequencyHz, const char *reason);
    bool applyListeningScanSettings(bool forceStop = false);
    bool scheduleLiveAgileCenterRetune(const QString &reason);
    void flushQueuedLiveAgileCenterRetune();
    void advanceStandardScanIfNeeded();
    void advanceListeningScanIfNeeded();
    void normalizeStandardScanCentersUi(bool requireTwoCenters = false);
    void applyStandardScanRangeToCenters();
    void updateAgileScanControls();
    void updateListeningScanControls();
    void saveAgileScanPreset();
    void deleteAgileScanPreset();
    void updateScanMeasurement(const std::vector<float> &frequencies,
                               const std::vector<float> &magnitudes);
    std::vector<float> scanMeasurementOverlay(const std::vector<float> &frequencies,
                                              int dataCount) const;
    void updateScanMeasurementStatus();
    void resetScanMeasurementPeaks();
    void clearScanMeasurement();
    void exportScanMeasurementCsv();
    void updateSpectrumScience(const std::vector<float> &frequencies,
                               const std::vector<float> &levels);
    bool spectrumScienceAnalysisRequired() const;
    void feedZeroSpanFrame(const std::vector<float> &frequencies,
                           const std::vector<float> &levels,
                           bool fftShiftedStorage);
    void updateSpectrumScienceUi();
    void setSpectrumScienceMarker(double frequencyHz);
    void exportSpectrumScienceCsv();
    void openZeroSpanDialog();
    void openResearchAnalysis(int tabIndex = 0);
    void openWirelessLab();
    void openPropagationLab();
    void updateFpvHunter(const std::vector<float> &frequencies,
                         const std::vector<float> &magnitudes);
    void updateFpvHunterControls();
    void applyFpvHunterPresetToScan();
    void tuneFpvHunterCandidate();
    void selectFpvHunterCandidate(int direction);
    void tuneFpvHunterCandidateIndex(int index);
    void tuneFpvHunterCandidateValue(const FpvHunterCandidate &candidate, bool saveSettings);
    void rememberFpvHunterCandidate(const FpvHunterCandidate &candidate, bool startNewEvent, qint64 nowMs);
    void updateFpvHunterHistoryControls();
    void tuneFpvHunterHistorySelection();
    void clearFpvHunterHistory();
    void updateDigitalVideoHunter(const std::vector<float> &frequencies,
                                  const std::vector<float> &magnitudes);
    void updateDigitalVideoHunterControls();
    void applyDigitalVideoHunterPresetToScan();
    void tuneDigitalVideoHunterCandidate();
    void selectDigitalVideoHunterCandidate(int direction);
    void tuneDigitalVideoHunterCandidateIndex(int index);
    void tuneDigitalVideoHunterCandidateValue(const DigitalVideoHunterCandidate &candidate, bool saveSettings);
    void updateDmrHunter(const std::vector<float> &frequencies,
                         const std::vector<float> &magnitudes);
    void updateDmrHunterControls();
    void applyDmrHunterPresetToScan();
    void tuneDmrHunterCandidate();
    void selectDmrHunterCandidate(int direction);
    void tuneDmrHunterCandidateIndex(int index);
    void startSpurCalibration();
    void clearSpurMask();
    void updateSpurCalibration(const std::vector<float> &frequencies,
                               const std::vector<float> &magnitudes,
                               double centerFrequency);
    void finishSpurCalibration();
    void applySpurSuppression(const std::vector<float> &frequencies,
                              std::vector<float> &magnitudes,
                              double centerFrequency) const;
    void updateSpurSuppressionStatus();
    void updateGnssSpurWatch(const std::vector<float> &frequencies,
                             const std::vector<float> &magnitudes,
                             double centerFrequency);
    void ensureDefaultFrequencyPresets();
    void ensureDefaultBandMarkers();
    void updateFrequencyPresetControls();
    void updateGraphBandMarkers();
    void applyAlternativeInterfaceMode();
    void ensureDspFlowPanel();
    void setControlsPanelVisible(bool visible);
    QVector<QPair<QString, double>> presetMapToVector(const QMap<QString, double> &presets,
                                                      const QStringList &order) const;
    void openPresetManager();
    void openZoomSpectrum();
    void openZoomDensity();
    void openTransmitDialog();
    void openFt8StationDialog();
    void openApplicationSettings();
    bool isAnyVideoOutputVisible() const;
    void openDspBlockReference(const QString &type, const QString &id);
    void registerDspFilterBlock(const QString &type, const QString &id);
    void triggerDspControlBlock(const QString &controlType,
                                const QString &controlId,
                                const QString &targetType,
                                const QString &targetId);
    void refreshDspControlStates();
    void openApplicationHelp();
    void exportSettingsBackup();
    void importSettingsBackup();
    void updateQthControls();
    void applyQthPositionFromUi();
    void clearQthPosition();
    void pasteNmeaPositionFromClipboard();
    bool applyNmeaPositionText(const QString &text, QString *statusMessage = nullptr, bool persist = true);
    void toggleGnssSerial();
    void startGnssSerial();
    void stopGnssSerial();
    void handleGnssSerialReadyRead();
    void processGnssNmeaLine(const QString &line, bool fromReplay = false, bool fromSdr = false);
    void showGnssSdrPvt();
    void showLrptDecoder();
    void processGnssUbxFrame(quint8 messageClass, quint8 messageId, const QByteArray &payload);
    void sendGnssUbxConfiguration(bool enabled);
    void applyGnssUbxConstellationConfig();
    void saveGnssUbxConfigurationToModule();
    void handleGnssSerialError(QSerialPort::SerialPortError error);
    void updateGnssSerialStatus(const QString &message);
    void toggleGnssNmeaLogging();
    void toggleGnssRawSerialLogging();
    void replayGnssNmeaLog();
    void resetGnssNmeaSatellites();
    void updateGnssNmeaSatellitesFromSentence(const QString &line);
    void updateGnssSatelliteView(bool forceTableRefresh = false);
    void updateGnssNmeaSkyPlot();
    void updateQthMapSatelliteOverlay();
    QString formattedGnssUtc() const;
    void ensureGnssSatelliteTableRows(int requiredRows);
    void sortGnssSatelliteRows(QVector<GnssNmeaSatellite> &satellites) const;
    void setGnssSatelliteRowsEnabled(bool enabled);
    bool isGnssSatelliteEnabled(const QString &key) const;
    bool hasEnabledGnssNmeaFixSatellite() const;
    bool isGnssNmeaSystemEnabled(const QString &system) const;
    bool isGnssNmeaTalkerEnabled(const QString &talker) const;
    void openQthMapWindow();
    void copyQthLocator();
    void updateGnssSystemSelection();
    void applyGnssSystemPresetToReceiver(const QString &systemId);
    void applyQthMapSearch();
    void tuneGnssL1Preset();
    void applyGnssScanPreset();
    void logGnssRawContext();
    bool isGnssMonitorActive() const;
    void resetGnssMonitor();
    void processGnssIqSnapshot(const RadioSettings &settings);
    void processGnssPackedIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount);
    void updateGnssMonitorStatus(const GnssSignalReport &report, bool forceLog = false);
    void runGnssAcquisitionTest();
    void runGnssDeepAcquisitionTest();
    void runGnssOfflineReplayTest();
    void runGnssSyntheticSelfTest();
    void runGnssPositionSelfTest();
    void setGnssContinuousAcquisitionEnabled(bool enabled);
    void stopGnssSdrAcquisition(const QString &reason, bool persistSettings = false);
    void scheduleGnssContinuousAcquisition(int delayMs = -1);
    void handleGnssContinuousAcquisitionTick();
    void updateGnssAcquisitionStatus(const GnssAcquisitionResult &result);
    void updateGnssAcquisitionPlot(const GnssAcquisitionResult &result);
    void saveGnssAcquisitionArtifacts(const GnssAcquisitionResult &result, const QString &sourceLabel);
    void requestGnssNetworkTime();
    void handleGnssNetworkTimeResponse();
    void updateQthMapControls();
    void applyQthOnlineProviderPreset(const QString &providerId, bool applyTemplate);
    QString resolvedQthOnlineTileUrlTemplate() const;
    void selectQthTileDirectory();
    void openQthTileDirectory();
    double fineTuneRangeHz() const;
    double fineTuneStepHz() const;
    void updateFineTuneLabel();
    void updateFineTuneControlMode();
    void updateFineTuneScaleModeButton();
    void applyListeningFrequencyDelta(double deltaHz, int networkDelayMs = 80);
    void refreshSettingsFromUi();
    void publishSettingsToGlobals();
    bool isIdle() const;
    bool isRunningOrTransitioning() const;
    void updateUiForRunState();
    void updateSpectrumTimerInterval();
    void revertHardwareControlsToSettings();
    void finishFobosStop(bool forcedRecovery);
    void recreateDataProcessor();
    uint8_t currentGpoValue() const;
    bool isNetworkClientMode() const;
    bool isClientIqProcessingMode() const;
    bool isChannelIqProcessingMode() const;
    bool isFullIqProcessingMode() const;
    bool isClientIqProcessingMode(NetworkProcessingMode mode) const;
    void appendNetworkState(QJsonObject &command) const;
    void applyNetworkStateFromCommand(const QJsonObject &command);
    QJsonObject networkSettingsPatch(const QJsonObject &settings) const;
    bool isRemoteReceiverDeviceValue(int deviceIndex) const;
    QJsonObject networkClientSettingsForCommand(const QJsonObject &settings,
                                                const QString &action) const;
    void applyAuthoritativeNetworkState(const QJsonObject &command);
    void sendSettingsAckToPeer(const QString &peerId,
                               bool ok,
                               const QString &requestId = QString(),
                               const QString &reason = QString());
    void startNetworkSettingsAckWait();
    void handleNetworkSettingsAckTimeout();
    void buildLocalReceiverDeviceChoices(QStringList &labels, QVector<int> &values) const;
    void rebuildReceiverDeviceCombo();
    QJsonArray receiverDeviceListToJson() const;
    bool applyReceiverDeviceListFromJson(const QJsonArray &devices);
    void clearRemoteReceiverDeviceList();
    void sendServerStateToClients();
    bool sendRemoteControlCommand(const QString &action, const QJsonObject &extra = QJsonObject());
    void scheduleRemoteSettingsCommand(int delayMs = 120);
    void cancelPendingRemoteSettingsCommand();
    QJsonObject settingsToJson() const;
    void applySettingsFromJson(const QJsonObject &settingsJson, bool normalizeAfterApply = true);
    void updateUiFromPendingSettings();
    void loadUiTranslations();
    QString normalizedUiLanguage(const QString &language) const;
    void populateLanguageCombo(QComboBox *combo) const;
    QString uiText(const QString &key, const QString &fallback) const;
    QString localizedStatusText(const QString &status) const;
    void markTranslatable(QWidget *widget, const QString &key, const QString &fallback);
    void applyUiLanguage();
    void applySpectrumHunterTranslations();
    void setComboItemText(QComboBox *combo, const QVariant &data, const QString &key, const QString &fallback);
    void updateNetworkButtonText();
    void updateAudioFilterLabels();
    void updateHfNoiseCancelControls();
    void learnHfInterferenceBaseline();
    void clearHfInterferenceBaseline();
    void resetHfInterferenceDefaults();
    void loadHfInterferenceBaselineCurve();
    void saveHfInterferenceBaselineCurve() const;
    bool buildHfInterferenceBaselineVisual(const std::vector<float> &frequencies,
                                           const std::vector<float> &magnitudes,
                                           std::vector<float> &outputMagnitudes);
    void applyLiveRemoteSettings(const RadioSettings &previousSettings);
    void handleDataProcessorFailure(int errorCode, bool stoppedByRequest);
    void clearLiveSpectrumSnapshot(bool clearVisualHistory = false, uint64_t iqEpoch = 0);
    void connectDataProcessorSignals();
    void applyServerLocalOutputPolicy();
    bool applyCenterFrequencyToHardwareIfNeeded(const RadioSettings &previousSettings, const char *reason);
    bool applyLiveAgileCenterRetune(uint64_t generation, const QString &reason);
    void resetNetworkIqReceptionState(bool clearGraph, bool clearWaterfall, bool restartAudioPrebuffer);
    void loadPersistentSettings();
    void savePersistentSettings();
    void flushPendingPersistentSettingsSave();
    void startNetworkClientProcessing();
    void stopNetworkClientProcessing();
    void sendNetworkSpectrumFrame(const std::vector<float> &frequencies,
                                  const std::vector<float> &magnitudes,
                                  const std::vector<float> &referenceMagnitudes = {},
                                  double frameCenterFrequency = std::numeric_limits<double>::quiet_NaN(),
                                  double frameMinFrequency = std::numeric_limits<double>::quiet_NaN(),
                                  double frameMaxFrequency = std::numeric_limits<double>::quiet_NaN(),
                                  const QVector<ScanVisualSegment> &scanSegments = QVector<ScanVisualSegment>(),
                                  bool frameFresh = true,
                                  bool showScanSegmentMarkers = true);
    void displayNetworkSpectrumFrame(const QJsonObject &frame);
    void displayNetworkSpectrumFrameBinary(const QJsonObject &frame, const QByteArray &payload);
    void sendNetworkAudioFrame(const QByteArray &pcmData);
    void playNetworkAudioFrame(const QJsonObject &frame);
    void updateAudioRelaySocket();
    void sendAudioRelayFrame(const QByteArray &pcmData);
    void receiveAudioRelayDatagrams();
    void updateAudioHttpStreamServer();
    void acceptAudioHttpClient();
    void removeAudioHttpClient(QTcpSocket *client);
    void sendAudioHttpFrame(const QByteArray &pcmData);
    void sendNetworkIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount);
    void receiveNetworkIqFrame(const QJsonObject &frame);
    void receiveNetworkIqFrameBinary(const QJsonObject &frame, const QByteArray &iqData);
    void handleNetworkIqPayload(const QJsonObject &frame, QByteArray iqBytes);
    void processDigitalAudioFrame(const QByteArray &pcmData, int sampleRate = 48000);
    void processSstvAudioFrame(const QByteArray &pcmData);
    void processAptAudioFrame(const QByteArray &pcmData);
    void processWefaxAudioFrame(const QByteArray &pcmData);
    void updateDigitalDecoderMode();
    int selectedDmrBackend() const;
    QStringList dsdNeoProcessArguments() const;
    void updateDsdNeoBridgeSettings();
    void updateGopherTrunkBridgeSettings();
    void showDmrPrivacyKeyDialog();
    int selectedDmrPrivacyKeyId() const;
    void refreshDmrPrivacyKeyIdCombo();
    bool applyDmrPrivacyKeySelection();
    bool isVideoDecodeActive() const;
    void processVideoIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount);
    void processVideoSnapshotFrame();
    void updateVideoProcessorMode();
    RadioSettings audioProcessorSettings() const;
    RadioSettings spectrumProcessingSettings() const;
    RecordingManager::Mode selectedRecordingMode() const;
    bool isChannelIqRecordingActive() const;
    bool dmrAudioIqProducerNeeded() const;
    void handleLocalIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount,
                            bool channelized);
    void updateIqFrameProducerSettings();
    void updateRecordingStatus(const QString &status);
    void handlePlaybackAudioFrame(const QByteArray &pcmData);
    void handlePlaybackIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount);
    QString selectedPlaybackFilePath() const;
    QJsonObject recordingLabMetadata() const;
    QJsonObject recordingScientificSnapshot(const QString &phase) const;
    QJsonObject recordingScientificMetadata(const QString &captureKind) const;
    void showTuneContextMenu(double frequency, const QPoint &globalPos);
    void tuneSignalCenterAt(double frequency);
    void tuneSidebandEdgeAt(double frequency, int modulationType);
    void centerReceiverAt(double frequency);
    bool applyFftLengthChange(int newFftLength, bool notifyRemote);
    int fftLengthForBinWidth(double sampleRate, double hzPerPoint) const;
    int nearestStandardFftLength(int requestedLength) const;
    void applyFftBinWidthTarget(bool notifyRemote);
    void syncFftResolutionControls();
    bool stopAgileScanForNormalRf(const char *reason);
    
    QComboBox *clkBox = nullptr;
    QComboBox *comboBox = nullptr;
    QComboBox *modeBox = nullptr;
    QComboBox *sampleBox = nullptr;
    QComboBox *fftComboBox = nullptr;
    QCheckBox *fftBinWidthModeCheckbox = nullptr;
    QDoubleSpinBox *fftBinWidthSpin = nullptr;
    QComboBox *waterfallDisplayModeCombo = nullptr;
    QCheckBox *alternativeSpectrumGradientCheckbox = nullptr;
    QComboBox *waterfall3DResolutionCombo = nullptr;
    QComboBox *waterfall3DSurfaceStyleCombo = nullptr;
    QComboBox *waterfall3DSmoothingCombo = nullptr;
    QComboBox *waterfall3DLightingCombo = nullptr;
    QComboBox *audioDeviceComboBox = nullptr;
    QComboBox *recordingModeCombo = nullptr;
    QComboBox *playbackFileCombo = nullptr;
    QComboBox *languageComboBox = nullptr;
    QComboBox *agileScanPresetCombo = nullptr;
    QComboBox *scanVisualModeCombo = nullptr;
    QComboBox *standardScanPresetCombo = nullptr;
    QComboBox *listeningScanPresetCombo = nullptr;
    QComboBox *videoDemodCombo = nullptr;
    QComboBox *videoStandardCombo = nullptr;
    QComboBox *sstvDemodulationCombo = nullptr;
    QComboBox *dmrLabColorCodeCombo = nullptr;
    QComboBox *dmrLabSlotCombo = nullptr;
    QComboBox *dmrLabCallTypeCombo = nullptr;
    QComboBox *dmrBasebandRateCombo = nullptr;
    QComboBox *dmrChannelRateCombo = nullptr;
    QComboBox *dmrAmbeLayoutCombo = nullptr;
    QComboBox *dmrBackendCombo = nullptr;
    QComboBox *dmrPrivacyModeCombo = nullptr;
    QComboBox *dmrPrivacyFrameOffsetCombo = nullptr;
    QComboBox *dmrPrivacyDropCombo = nullptr;
    QComboBox *dmrPrivacyBitLayoutCombo = nullptr;
    QComboBox *cwDecoderAlphabetCombo = nullptr;
    QComboBox *spectrumEventModeCombo = nullptr;
    QLineEdit *dsdNeoProgramEdit = nullptr;
    QComboBox *qthSourceCombo = nullptr;
    QComboBox *gnssSystemCombo = nullptr;
    QComboBox *gnssPositionPolicyCombo = nullptr;
    QComboBox *gnssTimeZoneCombo = nullptr;
    QCheckBox *dmrManualTimingCheckbox = nullptr;
    QSpinBox *dmrTimingOffsetSpin = nullptr;
    QSpinBox *cwDecoderToneSpin = nullptr;
    QSpinBox *cwDecoderWpmSpin = nullptr;
    QSpinBox *cwDecoderSelectivitySpin = nullptr;
    QComboBox *dmrPrivacyKeyIdCombo = nullptr;
    QDoubleSpinBox *dmrSlicerRatioSpin = nullptr;
    QSpinBox *gnssDopplerSpanSpin = nullptr;
    QSpinBox *gnssDopplerStepSpin = nullptr;
    QSpinBox *waterfall3DSliceStepSpin = nullptr;
    QSpinBox *waterfall3DSliceWidthSpin = nullptr;
    QSpinBox *waterfall3DHistoryRowsSpin = nullptr;
    QSpinBox *waterfall3DSpectrumSliceStepSpin = nullptr;
    QSpinBox *waterfall3DSpectrumSliceRowsSpin = nullptr;
    QCheckBox *waterfall3DSpectrumSliceCaptureCheckbox = nullptr;
    QCheckBox *waterfall3DSpectrumSliceCaptureFixedCheckbox = nullptr;
    QCheckBox *waterfall3DFixedPlaneCheckbox = nullptr;
    QCheckBox *waterfall3DMonochromeCheckbox = nullptr;
    QCheckBox *waterfall3DAlternativeViewCheckbox = nullptr;
    QCheckBox *waterfall3DVncSliceInputCheckbox = nullptr;
    QCheckBox *dmrAdaptiveSlicerCheckbox = nullptr;
    QCheckBox *cwDecoderAdaptiveCheckbox = nullptr;
    QLabel *dmrPrivacyCapabilityLabel = nullptr;
    QCheckBox *dsdNeoAutoStartCheckbox = nullptr;
    QWidget *cwDecoderControlsWidget = nullptr;
    QWidget *pskDecoderControlsWidget = nullptr;
    QDoubleSpinBox *pskDecoderToneSpin = nullptr;
    QSpinBox *pskDecoderSquelchSpin = nullptr;
    QCheckBox *pskDecoderAfcCheckbox = nullptr;
    QComboBox *pskDecoderEncodingCombo = nullptr;
    QSpinBox *dsdNeoInputPortSpin = nullptr;
    QSpinBox *dsdNeoUdpOutputPortSpin = nullptr;
    QButtonGroup *modulationButtonGroup = nullptr;
    
    QPushButton *refreshButton = nullptr;
    QPushButton *fobosButton = nullptr;
    QPushButton *networkButton = nullptr;
    QPushButton *hackRfSettingsButton = nullptr;
    QPushButton *presetManagerButton = nullptr;
    QPushButton *zoomSpectrumButton = nullptr;
    QPushButton *zoomDensityButton = nullptr;
    QPushButton *transmitButton = nullptr;
    QPushButton *ft8StationButton = nullptr;
    QPushButton *appSettingsButton = nullptr;
    QPushButton *dspPathButton = nullptr;
    QPushButton *controlsToggleButton = nullptr;
    QPushButton *digitalToggleButton = nullptr;
    QPushButton *videoToggleButton = nullptr;
    QPushButton *dmrPrivacyKeysButton = nullptr;
    QPushButton *recordButton = nullptr;
    QPushButton *spectrumFrameRecordButton = nullptr;
    QPushButton *spectrumFrameReplayButton = nullptr;
    QPushButton *playbackRefreshButton = nullptr;
    QPushButton *playbackButton = nullptr;
    QPushButton *startButton = nullptr;
    QPushButton *stopButton = nullptr;
    QPushButton *qthMapButton = nullptr;
    QPushButton *qthCopyButton = nullptr;
    QPushButton *qthClearButton = nullptr;
    QPushButton *qthPasteNmeaButton = nullptr;
    QPushButton *gnssSerialButton = nullptr;
    QPushButton *gnssNmeaLogButton = nullptr;
    QPushButton *gnssNmeaReplayButton = nullptr;
    QPushButton *gnssSerialRawLogButton = nullptr;
    QPushButton *gnssUbxSystemsButton = nullptr;
    QPushButton *gnssUbxSaveButton = nullptr;
    QPushButton *gnssTuneButton = nullptr;
    QPushButton *gnssPvtButton = nullptr;
    QPushButton *lrptDecodeButton = nullptr;
    QPointer<GnssSdrPvtDialog> gnssSdrPvtDialog;
    QPointer<LrptDecodeDialog> lrptDecodeDialog;
    QPushButton *gnssScanButton = nullptr;
    QPushButton *gnssRawLogButton = nullptr;
    QPushButton *gnssAcquireButton = nullptr;
    QPushButton *gnssDeepAcquireButton = nullptr;
    QPushButton *gnssOfflineAcquireButton = nullptr;
    QPushButton *gnssSelfTestButton = nullptr;
    QPushButton *gnssPositionSelfTestButton = nullptr;
    QPushButton *gnssNetworkTimeButton = nullptr;
    QPushButton *gnssPlotButton = nullptr;
    QPushButton *gnssSatellitesButton = nullptr;
    QPushButton *gnssMonitorResetButton = nullptr;
    QPushButton *hfInterferenceBaselineLearnButton = nullptr;
    QPushButton *hfInterferenceBaselineClearButton = nullptr;
    QPushButton *hfInterferenceDefaultsButton = nullptr;
    
    QCheckBox *spectrumCheckbox = nullptr;
    QCheckBox *audioCheckbox = nullptr;
    QCheckBox *syncCheckbox = nullptr;
    QCheckBox *graphCheckbox = nullptr;
    QCheckBox *colorCheckbox = nullptr;
    QCheckBox *digitalDecodeCheckbox = nullptr;
    QCheckBox *dmrLabCaptureCheckbox = nullptr;
    QCheckBox *videoDecodeCheckbox = nullptr;
    QCheckBox *videoInvertCheckbox = nullptr;
    QCheckBox *videoHSyncCheckbox = nullptr;
    QCheckBox *videoVSyncCheckbox = nullptr;
    QCheckBox *videoTestPatternCheckbox = nullptr;
    QCheckBox *hfNoiseCancelFreezeCheckbox = nullptr;
    QCheckBox *hfAudioBlankerCheckbox = nullptr;
    QCheckBox *hfInterferenceBaselineCheckbox = nullptr;
    QCheckBox *hfInterferenceRawOverlayCheckbox = nullptr;
    QCheckBox *spectrumFrameBufferCheckbox = nullptr;
    QCheckBox *agileScanCheckbox = nullptr;
    QCheckBox *standardScanCheckbox = nullptr;
    QCheckBox *listeningScanCheckbox = nullptr;
    QCheckBox *scanListeningLockCheckbox = nullptr;
    QCheckBox *scanMeasurementCheckbox = nullptr;
    QCheckBox *waterfallAreaMeasurementCheckbox = nullptr;
    QCheckBox *spurSuppressionCheckbox = nullptr;
    QCheckBox *gnssMonitorCheckbox = nullptr;
    QCheckBox *gnssUseGpsCheckbox = nullptr;
    QCheckBox *gnssUseGlonassCheckbox = nullptr;
    QCheckBox *gnssUseGalileoCheckbox = nullptr;
    QCheckBox *gnssUseBeidouCheckbox = nullptr;
    QCheckBox *gnssUseQzssCheckbox = nullptr;
    QCheckBox *gnssUseSbasCheckbox = nullptr;
    QCheckBox *gnssUseOtherCheckbox = nullptr;
    QCheckBox *rtlAgcCheckbox = nullptr;
    QCheckBox *checkBoxes[8] = {};
    
    QSlider *scaleSlider = nullptr;
    QSlider *additionalScaleDivisorSlider = nullptr;
    QSlider *lnaGainSlider = nullptr;
    QSlider *vgaGainSlider = nullptr;
    QSlider *rtlGainSlider = nullptr;
    QSlider *contrastSlider = nullptr;
    QSlider *sensitivitySlider = nullptr;
    QSlider *levelMinSlider = nullptr;
    QSlider *levelMaxSlider = nullptr;
    QSlider *alternativeSpectrumGradientOpacitySlider = nullptr;
    QDial *fineTuneDial = nullptr;
    FineTuneScaleWidget *fineTuneScaleWidget = nullptr;
    QStackedWidget *fineTuneStack = nullptr;
    QToolButton *fineTuneScaleModeButton = nullptr;
    QSlider *volumeSlider = nullptr;
    QSlider *audioLowPassSlider = nullptr;
    QSlider *audioHighPassSlider = nullptr;
    QSlider *hfNoiseCancelDepthSlider = nullptr;
    QSlider *hfNoiseCancelRefGainSlider = nullptr;
    QSlider *hfNoiseCancelRefDelaySlider = nullptr;
    QSlider *hfNoiseCancelRefTiltSlider = nullptr;
    QSlider *hfAudioBlankerThresholdSlider = nullptr;
    QSlider *hfInterferenceBaselineDepthSlider = nullptr;
    QSlider *hfInterferenceBaselineSmoothSlider = nullptr;
    SpectrumHunterControls *dmrHunterControls = nullptr;
    SpectrumHunterControls *fpvHunterControls = nullptr;
    SpectrumHunterControls *digitalVideoHunterControls = nullptr;

    QLabel *volumeLabel = nullptr;
    QLabel *audioLowPassLabel = nullptr;
    QLabel *audioHighPassLabel = nullptr;
    QLabel *hfNoiseCancelDepthLabel = nullptr;
    QLabel *hfNoiseCancelRefGainLabel = nullptr;
    QLabel *hfNoiseCancelRefDelayLabel = nullptr;
    QLabel *hfNoiseCancelRefTiltLabel = nullptr;
    QLabel *hfAudioBlankerThresholdLabel = nullptr;
    QLabel *hfInterferenceBaselineDepthLabel = nullptr;
    QLabel *hfInterferenceBaselineSmoothLabel = nullptr;
    QLabel *hfInterferenceBaselineStatusLabel = nullptr;
    QLabel *lnaGainLabel = nullptr;
    QLabel *rtlGainLabel = nullptr;
    QLabel *centralFrequencyLabel = nullptr;
    QLabel *listeningFrequencyLabel = nullptr;
    QLabel *fftLabel = nullptr;
    QLabel *waterfallDisplayModeLabel = nullptr;
    QLabel *waterfall3DResolutionLabel = nullptr;
    QLabel *waterfall3DHistoryRowsLabel = nullptr;
    QLabel *waterfall3DSliceStepLabel = nullptr;
    QLabel *waterfall3DSliceWidthLabel = nullptr;
    QLabel *waterfall3DSpectrumSliceStepLabel = nullptr;
    QLabel *waterfall3DSpectrumSliceRowsLabel = nullptr;
    QLabel *alternativeSpectrumGradientOpacityValueLabel = nullptr;
    QLabel *contrastLabel = nullptr;
    QLabel *sensitivityLabel = nullptr;
    QLabel *levelMinLabel = nullptr;
    QLabel *levelMaxLabel = nullptr;
    QLabel *fineTuneLabel = nullptr;
    QLabel *vgaGainLabel = nullptr;
    QLabel *scaleLabel = nullptr;
    QLabel *additionalScaleDivisorLabel = nullptr;
    QLabel *digitalStatusLabel = nullptr;
    QLabel *videoStatusLabel = nullptr;
    QLabel *recordingStatusLabel = nullptr;
    QLabel *spectrumFrameRecordingStatusLabel = nullptr;
    QLabel *playbackStatusLabel = nullptr;

    QJsonObject uiTranslations;
    QMap<QString, QString> uiLanguageNames;
    QStringList uiLanguageOrder;
    QString uiLanguage = QStringLiteral("en");
       
    QLineEdit *dmrLabSourceIdEdit = nullptr;
    QLineEdit *dmrLabTargetIdEdit = nullptr;
    QLineEdit *dmrLabRadioEdit = nullptr;
    QLineEdit *dmrLabNotesEdit = nullptr;
    QVector<DmrPrivacyKeyEntry> dmrPrivacyKeys;
    QLineEdit *agileScanRangesEdit = nullptr;
    QLineEdit *standardScanCentersEdit = nullptr;
    QLineEdit *listeningScanTargetsEdit = nullptr;
    QLineEdit *standardScanRangeStartEdit = nullptr;
    QLineEdit *standardScanRangeEndEdit = nullptr;
    QLineEdit *qthTileDirectoryEdit = nullptr;
    QLineEdit *qthOnlineTileUrlEdit = nullptr;
    QLineEdit *qthOnlineAttributionEdit = nullptr;
    QLineEdit *qthOnlineApiKeyEdit = nullptr;
    QLineEdit *qthMapSearchEdit = nullptr;
    QComboBox *gnssSerialPortEdit = nullptr;
    QDoubleSpinBox *agileScanStepSpin = nullptr;
    QCheckBox *agileScanAutoStepCheckbox = nullptr;
    QDoubleSpinBox *qthLatitudeSpin = nullptr;
    QDoubleSpinBox *qthLongitudeSpin = nullptr;
    QDoubleSpinBox *gnssChannelFilterSpin = nullptr;
    QSpinBox *standardScanDwellSpin = nullptr;
    QSpinBox *standardScanSettleSpin = nullptr;
    QSpinBox *listeningScanDwellSpin = nullptr;
    QSpinBox *listeningScanSettleSpin = nullptr;
    QSpinBox *gnssIntegrationSpin = nullptr;
    QSpinBox *gnssSerialBaudSpin = nullptr;
    QComboBox *spectrumFrameBinsCombo = nullptr;
    QSpinBox *spectrumFramePrebufferSpin = nullptr;
    QDoubleSpinBox *scanMeasurementBinSpin = nullptr;
    QDoubleSpinBox *spectrumScienceAverageSpin = nullptr;
    QComboBox *spectrumScienceMarkerCombo = nullptr;
    QPushButton *agileScanSavePresetButton = nullptr;
    QPushButton *agileScanDeletePresetButton = nullptr;
    QPushButton *standardScanSavePresetButton = nullptr;
    QPushButton *standardScanDeletePresetButton = nullptr;
    QPushButton *listeningScanSavePresetButton = nullptr;
    QPushButton *listeningScanDeletePresetButton = nullptr;
    QPushButton *standardScanAddLowerButton = nullptr;
    QPushButton *standardScanAddUpperButton = nullptr;
    QPushButton *standardScanRemoveLowerButton = nullptr;
    QPushButton *standardScanRemoveUpperButton = nullptr;
    QPushButton *standardScanFillRangeButton = nullptr;
    QPushButton *scanMeasurementBaselineButton = nullptr;
    QPushButton *scanMeasurementResetPeakButton = nullptr;
    QPushButton *scanMeasurementExportButton = nullptr;
    QPushButton *spectrumScienceSetButton = nullptr;
    QPushButton *spectrumSciencePeakButton = nullptr;
    QPushButton *spectrumSciencePreviousButton = nullptr;
    QPushButton *spectrumScienceNextButton = nullptr;
    QPushButton *spectrumScienceClearButton = nullptr;
    QPushButton *spectrumScienceResetButton = nullptr;
    QPushButton *spectrumScienceExportButton = nullptr;
    QPushButton *zeroSpanButton = nullptr;
    QPushButton *researchToolsButton = nullptr;
    QPushButton *hfInterferenceAnalyzeButton = nullptr;
    QPushButton *spurCalibrateButton = nullptr;
    QPushButton *spurClearButton = nullptr;
    QPushButton *qthSelectTilesButton = nullptr;
    QPushButton *qthOpenTilesButton = nullptr;
    QPushButton *qthCenterMapButton = nullptr;
    QPushButton *qthUseOsmButton = nullptr;
    QPushButton *qthMapSearchButton = nullptr;
    QPushButton *fpvHunterHistoryTuneButton = nullptr;
    QPushButton *fpvHunterHistoryClearButton = nullptr;
    QComboBox *fpvHunterHistoryCombo = nullptr;
    QComboBox *qthMapLayerCombo = nullptr;
    QComboBox *qthOnlineProviderCombo = nullptr;
    QComboBox *qthGridPrecisionCombo = nullptr;
    QComboBox *qthMapOverlayCombo = nullptr;
    QCheckBox *qthOnlineNoDiskCacheCheckbox = nullptr;
    QSpinBox *qthMapZoomSpin = nullptr;
    QLabel *fpvHunterHistoryLabel = nullptr;
    QLabel *agileScanStatusLabel = nullptr;
    QLabel *standardScanStatusLabel = nullptr;
    QLabel *listeningScanStatusLabel = nullptr;
    QLabel *scanMeasurementStatusLabel = nullptr;
    QLabel *spectrumScienceMarkerStatusLabel = nullptr;
    QLabel *spectrumScienceMetricsLabel = nullptr;
    QLabel *spurSuppressionStatusLabel = nullptr;
    QLabel *qthLocatorLabel = nullptr;
    QLabel *qthStatusLabel = nullptr;
    QLabel *qthMapStatusLabel = nullptr;
    QLabel *gnssSerialStatusLabel = nullptr;
    QLabel *gnssMonitorStatusLabel = nullptr;
    QLabel *gnssAcquireStatusLabel = nullptr;
    QLabel *gnssAcquisitionPlotLabel = nullptr;
    QLabel *gnssSatelliteSkyLabel = nullptr;
    QLabel *gnssSatelliteStatusLabel = nullptr;
    QCheckBox *gnssSatelliteTableCheckbox = nullptr;
    QDialog *gnssSatelliteTableDialog = nullptr;
    ZoomSpectrumDialog *zoomSpectrumDialog = nullptr;
    ZoomDensityDialog *zoomDensityDialog = nullptr;
    TransmitDialog *transmitDialog = nullptr;
    Ft8StationDialog *ft8StationDialog = nullptr;
    QTableWidget *gnssSatelliteTable = nullptr;
    QLabel *dsdNeoStatusLabel = nullptr;
    QString lastDmrPrivacyCapabilityWarning;

    FrequencyControl *frequencyControl = nullptr;
    FrequencyControl *listeningFrequencyControl = nullptr;
    FrequencyControl *bandwidthControl = nullptr;
    
    QTimer *updateTimer = nullptr;
    QTimer *stopPollTimer = nullptr;
    QTimer *streamWatchdogTimer = nullptr;
    QTimer *sampleRateChangeTimer = nullptr;
    QTimer *networkSettingsDebounceTimer = nullptr;
    QTimer *networkSettingsAckTimer = nullptr;
    QTimer *standardScanAdvanceTimer = nullptr;
    QTimer *listeningScanAdvanceTimer = nullptr;
    QTimer *videoSnapshotTimer = nullptr;
    QTimer *agileLiveRetuneTimer = nullptr;
    QTimer *persistentSettingsSaveTimer = nullptr;
    QUdpSocket *gnssNtpSocket = nullptr;
    QSerialPort *gnssSerialPort = nullptr;

    DataProcessor *processor = nullptr;
    AudioProcessor *audioProcessor = nullptr;
    AudioFilterChainWidget *audioFilterChainWidget = nullptr;
    DspFlowPanel *dspFlowPanel = nullptr;
    MultiVfoWidget *multiVfoWidget = nullptr;
    QHash<QString, QPointer<QDialog>> dspBlockEditors;
    DigitalDecoder *digitalDecoder = nullptr;
    DsdNeoBridge *dsdNeoBridge = nullptr;
    QThread *dsdNeoThread = nullptr;
    GopherTrunkBridge *gopherTrunkBridge = nullptr;
    QThread *digitalDecoderThread = nullptr;
    VideoProcessor *videoProcessor = nullptr;
    QThread *videoProcessorThread = nullptr;
    PlaybackManager *playbackManager = nullptr;
    RecordingManager *recordingManager = nullptr;
    NetworkController *networkController = nullptr;
    RemoteAudioPlayer *remoteAudioPlayer = nullptr;
    MyGraphWidget *graphWidget = nullptr;
    MyWaterfallWidget *waterfallWidget = nullptr;
    ScaleWidget *scaleWidget = nullptr;
    QVBoxLayout *graphLayout = nullptr;
    QDockWidget *controlsDock = nullptr;
    QDockWidget *digitalDock = nullptr;
    QDockWidget *videoDock = nullptr;
    QDialog *qthMapDialog = nullptr;
    QthMapWidget *qthMapWidget = nullptr;
    QDialog *gnssSatelliteDialog = nullptr;
    QPlainTextEdit *digitalTextEdit = nullptr;
    VideoWidget *videoWidget = nullptr;
    
    bool deviceOpened;
    int openedDeviceIndex = -1;
    FobosApiKind openedDeviceApiKind = FobosApiKind::Standard;
    int openedNativeDeviceIndex = -1;
    double appliedSampleRate = 0.0;
    QElapsedTimer stopElapsedTimer;
    QElapsedTimer streamStartElapsedTimer;
    int stopCancelRetryCount = 0;
    int streamStartupRetryCount = 0;
    uint64_t streamStartCallbackCount = 0;
    bool restartAfterStartupWatchdog = false;
    bool automaticStreamRestart = false;
    bool pendingAudioStartAfterStreamReady = false;
    bool clearSpectrumAfterStop = false;
    bool closeFobosSessionAfterStop = false;
    bool pendingNetworkAudioStartAfterIqPrebuffer = false;
    bool sampleRateReopenRequired = false;
    bool fobosCloseKnownUnsafe = false;
    bool networkClientIqProcessingActive = false;
    bool networkClientReconnectPending = false;
    NetworkProcessingMode activeNetworkClientProcessingMode = NetworkProcessingMode::ServerSide;
    bool networkIqStreamMetadataValid = false;
    bool networkIqStreamWasChannelized = false;
    double networkIqStreamSampleRate = 0.0;
    double networkIqStreamCenterFrequency = 0.0;
    double networkIqStreamListeningFrequency = 0.0;
    int networkIqStreamInputMode = 0;
    bool networkSpectrumFrameMetadataValid = false;
    double networkSpectrumFrameMinFrequency = 0.0;
    double networkSpectrumFrameMaxFrequency = 0.0;
    int networkSpectrumFrameFftLength = 0;
    QVector<FobosDeviceInfo> availableFobosDevices;
    QTimer *usbDeviceRefreshTimer = nullptr;
    int usbDeviceRefreshPassesRemaining = 0;
    QStringList remoteReceiverDeviceLabels;
    QVector<int> remoteReceiverDeviceValues;
    bool remoteReceiverDeviceListValid = false;
    RadioSettings pendingSettings;
    RadioSettings appliedHardwareSettings;
    double spectrumDisplayCenterHz = std::numeric_limits<double>::quiet_NaN();
    bool hardwareSettingsApplied = false;
    int spectrumDebugFramesRemaining = 0;
    int spectrumTuningDebugFramesRemaining = 0;
    int scanVisualDebugFramesRemaining = 0;
    uint64_t scanVisualDebugSequence = 0;
    RadioRunState runState = RadioRunState::Idle;
    bool closeShutdownFinalized = false;
    NetworkMode networkMode = NetworkMode::Disabled;
    NetworkProcessingMode networkProcessingMode = NetworkProcessingMode::ServerSide;
    QString networkServerAddress = "127.0.0.1";
    QString networkBindAddress = "0.0.0.0";
    quint16 networkControlPort = 21090;
    bool networkFullResolutionSpectrumFrames = false;
    int fineTuneDialLastValue = 0;
    int fineTuneControlMode = 0;
    bool fineTuneScaleHoldMode = false;
    bool serverDisableLocalVisualAudio = true;
    bool digitalDecodeEnabled = false;
    QByteArray pendingDmrDecoderPcm;
    int pendingDmrDecoderSampleRate = 48000;
    int pendingDmrMetadataColorCode = -2;
    int pendingDmrMetadataTimeslot = -1;
    quint32 pendingDmrMetadataTargetId = 0;
    quint32 pendingDmrMetadataSourceId = 0;
    int pendingDmrMetadataFlco = -2;
    int pendingDmrMetadataStableHits = 0;
    std::atomic<uint64_t> digitalDecoderGeneration {0};
    std::atomic<int> pendingDigitalDecoderFrames {0};
    std::atomic<int> droppedDigitalDecoderFramesSinceLog {0};
    bool videoDecodeEnabled = false;
    bool closeShutdownInProgress = false;
    bool agileScanEnabled = false;
    bool agileScanRunning = false;
    bool standardScanEnabled = false;
    bool standardScanRunning = false;
    bool listeningScanEnabled = false;
    bool listeningScanRunning = false;
    bool scanListeningLockEnabled = true;
    bool agileScanAutoStepSampleRate = true;
    int scanVisualMode = static_cast<int>(ScanVisualMode::CompressedMosaic);
    QString agileScanRangesMhz = QStringLiteral("430-432");
    double agileScanStepMhz = 0.0125;
    QString standardScanCentersMhz = QStringLiteral("430, 480");
    int standardScanDwellMs = 650;
    int standardScanSettleMs = 60;
    QString standardScanRangeStartMhz;
    QString standardScanRangeEndMhz;
    QMap<QString, QString> standardScanPresets;
    QStringList standardScanPresetOrder;
    QString listeningScanTargetsMhz = QStringLiteral("1561.098, 1575.420, 1602.000");
    int listeningScanDwellMs = 3000;
    int listeningScanSettleMs = 100;
    QMap<QString, QString> listeningScanPresets;
    QStringList listeningScanPresetOrder;
    int spectrumUpdateIntervalMs = 0;
    QElapsedTimer spectrumAuxiliaryUiTimer;
    int fftBackendPreference = FFT_BACKEND_AUTO;
    bool fftBinWidthModeEnabled = false;
    double fftTargetBinWidthHz = 2.0;
    int additionalScaleDivisor = 1;
    double frequencyCalibrationOffsetHz = 0.0;
    double amplitudeCalibrationOffsetDb = 0.0;
    bool calibrationTableEnabled = false;
    ReceiverCalibrationTable receiverCalibrationTable;
    bool presetManagerOpen = false;
    int waterfallRowsPerFrame = 1;
    int waterfallDisplayMode = static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall2D);
    bool alternativeInterfaceMode = false;
    bool waterfall3DAlternativeView = false;
    bool alternativeSpectrumGradientFill = false;
    int alternativeSpectrumGradientOpacity = 70;
    int waterfall3DResolutionDivisor = 4;
    int waterfall3DHistoryRows = 128;
    int waterfall3DSurfaceStyle = 0;
    int waterfall3DSmoothing = 0;
    int waterfall3DLighting = 0;
    int waterfall3DSliceScrollStep = 1;
    int waterfall3DSliceWidth = 1;
    int waterfall3DSpectrumSliceScrollStep = 1;
    int waterfall3DSpectrumSliceRows = 1;
    bool waterfall3DSpectrumSliceCapture = false;
    bool waterfall3DSpectrumSliceCaptureFixed = false;
    bool waterfall3DFixedPlane = false;
    bool waterfall3DMonochrome = false;
    bool waterfall3DVncSliceInput = false;
    bool experimentalGpuWaterfall = false;
    bool showSpectrumFps = false;
    bool showWaterfallFps = false;
    bool showAudioQueueDropRows = false;
    bool showSpectrumPeakMeter = false;
    int spectrumPeakMeterStyle = 0;
    bool showExtendedSpectrumInfo = false;
    SpectrumFrameRecorder spectrumFrameRecorder;
    SpectrumIqEventRecorder spectrumIqEventRecorder;
    bool spectrumFrameBufferEnabled = false;
    int spectrumFramePrebufferSeconds = 10;
    int spectrumEventCaptureMode = 0;
    std::deque<SpectrumFrameRecord> spectrumFramePrebuffer;
    std::deque<SpectrumEventIqFrame> spectrumIqPrebuffer;
    QString spectrumEventBasePath;
    bool hfInterferenceBaselineEnabled = false;
    bool hfInterferenceRawOverlayEnabled = false;
    double hfInterferenceBaselineDepth = 1.0;
    int hfInterferenceBaselineSmoothBins = 121;
    std::vector<float> spectrumFrequencyScratch;
    std::vector<float> spectrumMagnitudeScratch;
    std::vector<float> spectrumReferenceScratch;
    std::uint64_t spectrumCalibrationFrameSerial = 0;
    std::vector<float> hfInterferenceLastFrequencies;
    std::vector<float> hfInterferenceLastMagnitudes;
    std::vector<float> hfInterferenceBaselineFrequencies;
    std::vector<float> hfInterferenceBaselineMagnitudes;
    std::vector<float> hfInterferenceVisualMagnitudes;
    bool scanMeasurementEnabled = true;
    bool waterfallAreaMeasurementEnabled = false;
    bool scanMeasurementBaselineRecording = false;
    double scanMeasurementBinMhz = 0.1;
    int scanMeasurementUpdateIntervalMs = SCAN_MEASUREMENT_DEFAULT_UPDATE_MS;
    SpectrumScienceAnalyzer spectrumScienceAnalyzer;
    bool spectrumScienceMaxHoldEnabled = false;
    bool spectrumScienceMinHoldEnabled = false;
    bool spectrumScienceAverageEnabled = false;
    double spectrumScienceAverageSeconds = 2.0;
    int spectrumDetectorMode = SPECTRUM_DETECTOR_SAMPLE;
    int spectrumDetectorFrames = 8;
    double spectrumVbwHz = 0.0;
    int spectrumFftOverlapPercent = 0;
    int spectrumAverageFrameCount = 0;
    bool spectrumPercentile50Enabled = false;
    bool spectrumPercentile90Enabled = false;
    bool spectrumPercentile99Enabled = false;
    int spectrumAmplitudeUnit = 0;
    int spectrumDisplayReductionMode = 0;
    std::uint64_t lastSpectrumFftEpoch = 0;
    std::uint64_t lastSpectrumFftEndFloats = 0;
    int lastSpectrumFftLength = 0;
    std::uint64_t lastIqDiscontinuityEpoch = 0;
    std::uint64_t lastIqDroppedQueuedBlocks = 0;
    std::uint64_t lastIqSkippedSnapshotBlocks = 0;
    int spectrumScienceActiveMarker = 0;
    enum SpectrumSciencePendingAction {
        SpectrumScienceActionNone = 0,
        SpectrumScienceActionPeak,
        SpectrumScienceActionPreviousPeak,
        SpectrumScienceActionNextPeak
    };
    SpectrumSciencePendingAction spectrumSciencePendingAction = SpectrumScienceActionNone;
    bool spectrumScienceExportPending = false;
    QCheckBox *spectrumScienceMaxHoldCheckbox = nullptr;
    QCheckBox *spectrumScienceMinHoldCheckbox = nullptr;
    QCheckBox *spectrumScienceAverageCheckbox = nullptr;
    ZeroSpanDialog *zeroSpanDialog = nullptr;
    ResearchAnalysisDialog *researchAnalysisDialog = nullptr;
    WirelessLabDialog *wirelessLabDialog = nullptr;
    PropagationLabDialog *propagationLabDialog = nullptr;
    DmrHunterSettings dmrHunterSettings;
    DmrHunterResult dmrHunterLastResult;
    std::vector<DmrHunterCandidate> dmrHunterCandidates;
    int dmrHunterCandidateIndex = -1;
    FpvHunterSettings fpvHunterSettings;
    FpvHunterResult fpvHunterLastResult;
    std::vector<FpvHunterCandidate> fpvHunterCandidates;
    int fpvHunterCandidateIndex = -1;
    bool fpvHunterFollowEnabled = false;
    double fpvHunterLastFollowCenterHz = std::numeric_limits<double>::quiet_NaN();
    double fpvHunterLastFollowBandwidthHz = std::numeric_limits<double>::quiet_NaN();
    struct FpvHunterTrack {
        bool valid = false;
        bool stable = false;
        double centerHz = 0.0;
        double widthHz = 0.0;
        float peakDb = -160.0f;
        float averageDb = -160.0f;
        float excessDb = 0.0f;
        float score = 0.0f;
        int hits = 0;
        int misses = 0;
        uint64_t lastSeenSequence = 0;
        qint64 firstSeenMsec = -1;
        qint64 lastSeenMsec = -1;
        QString type;
    };
    FpvHunterTrack fpvHunterTrack;
    uint64_t fpvHunterFrameSequence = 0;
    QElapsedTimer fpvHunterClock;
    struct FpvHunterEvent {
        bool valid = false;
        quint64 id = 0;
        double centerHz = 0.0;
        double widthHz = 0.0;
        float peakDb = -160.0f;
        float averageDb = -160.0f;
        float excessDb = 0.0f;
        float score = 0.0f;
        int hits = 0;
        qint64 firstSeenMsec = -1;
        qint64 lastSeenMsec = -1;
        QString type;
    };
    QVector<FpvHunterEvent> fpvHunterEvents;
    int fpvHunterActiveEventIndex = -1;
    quint64 fpvHunterNextEventId = 1;
    DigitalVideoHunterSettings digitalVideoHunterSettings;
    DigitalVideoHunterResult digitalVideoHunterLastResult;
    std::vector<DigitalVideoHunterCandidate> digitalVideoHunterCandidates;
    int digitalVideoHunterCandidateIndex = -1;
    QMap<QString, QString> agileScanPresets;
    QStringList agileScanPresetOrder;
    QVector<double> activeAgileScanFrequencies;
    QVector<double> activeStandardScanFrequencies;
    QVector<double> activeListeningScanFrequencies;
    int standardScanIndex = 0;
    int listeningScanIndex = 0;
    ScanVisualAssembler scanVisualAssembler;

    struct ScanMeasurementBin {
        double frequencyHz = 0.0;
        float currentDb = -160.0f;
        float peakDb = -160.0f;
        float baselineDb = -160.0f;
        int baselineCount = 0;
        int seenCount = 0;
        uint64_t lastSequence = 0;
    };
    QMap<qint64, ScanMeasurementBin> scanMeasurementBins;
    uint64_t scanMeasurementSequence = 0;
    QElapsedTimer scanMeasurementUpdateClock;
    QElapsedTimer scanMeasurementStatusClock;
    mutable QElapsedTimer scanMeasurementOverlayClock;
    mutable std::vector<float> scanMeasurementOverlayCache;
    mutable uint64_t scanMeasurementOverlayCacheSequence = 0;
    mutable int scanMeasurementOverlayCacheCount = 0;
    mutable double scanMeasurementOverlayCacheFirstHz = std::numeric_limits<double>::quiet_NaN();
    mutable double scanMeasurementOverlayCacheLastHz = std::numeric_limits<double>::quiet_NaN();

    struct SpurMaskEntry {
        double offsetHz = 0.0;
        double widthHz = 0.0;
        float prominenceDb = 0.0f;
        int hits = 0;
    };
    struct SpurCalibrationBin {
        double offsetWeightedSum = 0.0;
        double weightSum = 0.0;
        float maxProminenceDb = 0.0f;
        int hits = 0;
    };
    struct GnssSpurWatchBin {
        float averageDb = -160.0f;
        int hits = 0;
        qint64 lastSeenMs = 0;
    };
    QVector<SpurMaskEntry> spurMaskEntries;
    QMap<qint64, SpurCalibrationBin> spurCalibrationBins;
    QMap<qint64, GnssSpurWatchBin> gnssSpurWatchBins;
    bool spurSuppressionEnabled = false;
    bool spurCalibrationActive = false;
    int spurCalibrationFramesDone = 0;
    int spurCalibrationTargetFrames = 32;
    double spurCalibrationBinHz = 0.0;
    double spurCombStepHz = 0.0;
    int spurCombSpacingHits = 0;
    QMap<QString, double> centerFrequencyPresets;
    QMap<QString, double> listeningFrequencyPresets;
    QMap<QString, double> bandwidthValuePresets;
    QStringList centerFrequencyPresetOrder;
    QStringList listeningFrequencyPresetOrder;
    QStringList bandwidthPresetOrder;
    int centerFrequencyUnitIndex = 2;
    int listeningFrequencyUnitIndex = 2;
    int bandwidthUnitIndex = 1;
    QString centerFrequencyStepName = QStringLiteral("1 MHz");
    QString listeningFrequencyStepName = QStringLiteral("1 kHz");
    QString bandwidthStepName = QStringLiteral("1 kHz");
    QString centerFrequencyPresetName;
    QString listeningFrequencyPresetName;
    QString bandwidthPresetName;
    bool frequencyControlUiStateRestorePending = false;
    QVector<GraphBandMarker> bandMarkers;
    bool bandMarkersCustomized = false;
    bool showGeneralBandMarkers = true;
    bool showAmateurBandMarkers = true;
    bool compactBandMarkers = false;
    bool diagnosticVerboseLogging = false;
    bool extendedRecordingMetadataEnabled = false;
    bool gnssMonitorEnabled = false;
    bool gnssAcquisitionRunning = false;
    bool gnssContinuousAcquisitionEnabled = false;
    bool gnssContinuousAcquisitionTickActive = false;
    int gnssContinuousAcquisitionIntervalMs = 2500;
    std::uint64_t gnssLastContinuousAcquisitionSequence = 0;
    std::shared_ptr<std::atomic_bool> gnssAcquisitionCancelFlag;
    QVector<double> gnssPeakToSecondHistoryDb;
    QMap<QString, QVector<int>> gnssGpsLnavBitHistory;
    QMap<QString, QVector<int>> gnssGpsLnavSegmentHistory;
    QMap<QString, int> gnssGpsLnavNextSegmentId;
    QMap<QString, std::uint64_t> gnssGpsLnavLastSequence;
    QMap<QString, qint64> gnssGpsLnavLastUpdateMs;
    int gnssGpsFocusedPrn = 0;
    double gnssGpsFocusedDopplerHz = 0.0;
    int gnssGpsFocusedWeakCount = 0;
    int gnssGpsFocusedStableCount = 0;
    QString gnssSystemId = QStringLiteral("gps_l1_ca");
    QString gnssAcquisitionSource = QStringLiteral("live");
    QString gnssSerialPortName;
    int gnssSerialBaud = 9600;
    QString gnssPositionPolicy = QStringLiteral("auto");
    bool gnssUbxAutoEnable = false;
    QByteArray gnssSerialBuffer;
    quint64 gnssSerialSentenceCount = 0;
    quint64 gnssSerialUbxFrameCount = 0;
    quint64 gnssSerialFixCount = 0;
    bool gnssUbxOutputEnabled = false;
    bool gnssPendingCfgGnssApply = false;
    QByteArray gnssLastCfgGnssPayload;
    qint64 gnssLastContinuousArtifactSaveMs = 0;
    qint64 gnssLastUbxFixMs = 0;
    int gnssSerialFixQuality = -1;
    int gnssSerialFixMode = -1;
    int gnssSerialSatellitesUsed = -1;
    int gnssSerialGpsSatellites = -1;
    int gnssSerialGlonassSatellites = -1;
    int gnssSerialGalileoSatellites = -1;
    int gnssSerialBeidouSatellites = -1;
    int gnssSerialOtherSatellites = -1;
    QString gnssSerialUtc;
    int gnssTimeZoneOffsetMinutes = 0;
    double gnssSerialHdop = std::numeric_limits<double>::quiet_NaN();
    double gnssSerialVdop = std::numeric_limits<double>::quiet_NaN();
    double gnssSerialPdop = std::numeric_limits<double>::quiet_NaN();
    double gnssSerialAltitudeM = std::numeric_limits<double>::quiet_NaN();
    double gnssSerialGeoidSeparationM = std::numeric_limits<double>::quiet_NaN();
    double gnssSerialSpeedKmh = std::numeric_limits<double>::quiet_NaN();
    double gnssSerialCourseDeg = std::numeric_limits<double>::quiet_NaN();
    std::unique_ptr<QFile> gnssNmeaLogFile;
    QString gnssNmeaLogPath;
    std::unique_ptr<QFile> gnssRawSerialLogFile;
    QString gnssRawSerialLogPath;
    QMap<QString, GnssNmeaSatellite> gnssNmeaSatellites;
    QMap<QString, bool> gnssNmeaSatelliteEnabled;
    QSet<QString> gnssDisabledSatelliteKeys;
    QElapsedTimer gnssSatelliteTableUpdateTimer;
    QElapsedTimer gnssSatelliteOverlayUpdateTimer;
    bool gnssSatelliteTableDirty = false;
    bool gnssSatelliteTableVisible = false;
    int gnssSatelliteSortColumn = 6;
    bool gnssSatelliteSortAscending = false;
    bool gnssAcquisitionPlotShowsAcquisition = false;
    bool gnssUseGps = true;
    bool gnssUseGlonass = true;
    bool gnssUseGalileo = true;
    bool gnssUseBeidou = true;
    bool gnssUseQzss = true;
    bool gnssUseSbas = true;
    bool gnssUseOther = true;
    int gnssAcquisitionIntegrationMs = 24;
    double gnssChannelFilterCutoffHz = 1800000.0;
    int gnssDopplerSpanHz = 25000;
    int gnssDopplerStepHz = 1000;
    GnssSignalMonitor gnssSignalMonitor;
    QElapsedTimer gnssMonitorSnapshotTimer;
    QElapsedTimer gnssMonitorLogTimer;
    QElapsedTimer gnssMonitorUiTimer;
    QElapsedTimer gnssSpurWatchTimer;
    QElapsedTimer gnssSpurLogTimer;
    QDialog *gnssAcquisitionPlotDialog = nullptr;
    QTimer *gnssContinuousAcquireTimer = nullptr;
    double qthLatitude = 0.0;
    double qthLongitude = 0.0;
    bool qthPositionVisible = true;
    QString qthSource = QStringLiteral("manual");
    QString qthTileDirectory;
    QString qthOnlineProviderId = QStringLiteral("osm");
    QString qthOnlineTileUrlTemplate = QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png");
    QString qthOnlineAttribution = QString::fromUtf8("\xC2\xA9 OpenStreetMap contributors");
    QString qthOnlineApiKey;
    bool qthOnlineNoDiskCache = false;
    int qthMapLayer = 0;
    int qthMapZoom = 12;
    int qthGridPrecision = 6;
    int qthMapOverlayMode = 1;
    QVector<qth::UserMarker> qthUserMarkers;
    std::atomic_bool videoIqFramePending{false};
    bool momentaryRecordingActive = false;
    QJsonObject activeRecordingLabMetadata;
    bool offlineIqPlaybackActive = false;
    bool offlineIqPlaybackHasMetadata = false;
    double offlineIqPlaybackSampleRate = 0.0;
    bool pendingPlaybackAudioStartAfterIqPrebuffer = false;
    bool playbackSettingsSaved = false;
    RadioSettings settingsBeforePlayback;
    int volumePercent = 100;
    QElapsedTimer networkSpectrumFrameTimer;
    QElapsedTimer liveRetuneSettleTimer;
    QElapsedTimer standardScanDwellTimer;
    QElapsedTimer listeningScanDwellTimer;
    QElapsedTimer listeningScanSettleTimer;
    qint64 liveRetuneSettleDurationMs = 80;
    uint64_t liveCenterRetuneGeneration = 0;
    uint64_t queuedLiveCenterRetuneGeneration = 0;
    QString queuedLiveCenterRetuneReason;
    QElapsedTimer agileLiveRetuneCommandTimer;
    int agileLiveRetuneCommandIntervalMs = 120;
    QElapsedTimer persistentSettingsLastSaveTimer;
    bool persistentSettingsSaveDeferred = false;
    bool persistentSettingsSaveInProgress = false;
    uint64_t networkSpectrumFrameSequence = 0;
    uint64_t networkIqFrameSequence = 0;
    uint64_t networkIqFramesDropped = 0;
    QJsonObject networkClientConfirmedSettingsJson;
    bool networkClientConfirmedSettingsValid = false;
    bool networkClientAwaitingSettingsAck = false;
    quint64 networkClientSettingsRequestCounter = 0;
    bool audioRelayTransmitEnabled = false;
    QString audioRelayHost = "127.0.0.1";
    quint16 audioRelayPort = 21091;
    bool audioRelayReceiveEnabled = false;
    quint16 audioRelayListenPort = 21091;
    uint32_t audioRelaySequence = 0;
    QUdpSocket *audioRelaySocket = nullptr;
    bool audioHttpStreamEnabled = false;
    quint16 audioHttpStreamPort = 21092;
    QTcpServer *audioHttpServer = nullptr;
    QList<QTcpSocket *> audioHttpClients;
    float displayLevelMin = -140.0f;
    float displayLevelMax = -30.0f;
    bool persistentSettingsReady = false;
    int minScale = 1;
    int maxScale = 1000;
};

#endif // MAIN_H
