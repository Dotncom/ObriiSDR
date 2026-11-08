#ifndef SPECTRUMFRAMEREPLAYDIALOG_H
#define SPECTRUMFRAMEREPLAYDIALOG_H

#include <QDialog>
#include <QByteArray>
#include <QColor>
#include <QCheckBox>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QPoint>
#include <QElapsedTimer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QTimer>
#include <atomic>
#include <thread>
#include <memory>
#include <vector>

#include "spectrumframerecorder.h"

class MyGraphWidget;
class AudioProcessor;
class FrequencyControl;
class QResizeEvent;
class QScrollArea;
class QShowEvent;
class QToolButton;
class ScaleWidget;
class Waterfall3DView;
class DspBranchAudioOutput;
template <typename T> class QFutureWatcher;

class SpectrumFrameReplayDialog : public QDialog {
    Q_OBJECT

public:
    explicit SpectrumFrameReplayDialog(QWidget *parent = nullptr, bool isolatedExperiment = false);
    ~SpectrumFrameReplayDialog() override;

    bool loadRecording(const QString &path, QString *errorMessage = nullptr);
    // The caller decodes a bounded recording off the UI thread after bundle verification.
    bool loadExperimentRecording(const QString &path, SpectrumFrameRecording loaded,
                                 const QJsonObject &context, QString *errorMessage = nullptr,
                                 const QStringList &verifiedAssetPaths = {});
    bool applyExperimentContext(const QJsonObject &context, QString *errorMessage = nullptr);
    QJsonObject experimentReplayState() const;

signals:
    void replayAudioFrameReady(const QByteArray &pcmData);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    enum class ReplayIqSource {
        ChannelIq,
        FullIq
    };

    void rebuildWaterfallImage();
    void rebuildExperimentWaterfall();
    void adoptRecording(const QString &path, SpectrumFrameRecording loaded);
    void startExperimentIqPlayback(const QString &path, quint64 dataOffset, quint64 byteOffset);
    void rebuild3DWindow(bool force = false);
    void updateReplayDisplayMode();
    void renderWaterfallPixmap();
    void renderExperimentWaterfallViewport();
    void updateFrameSelection(int index);
    void updateLabels();
    void setPlaybackEnabled(bool enabled);
    double replaySpeedMultiplier() const;
    void scheduleNextPlaybackFrame();
    void updateLinkedIqControls();
    void updateReplayMarker();
    void updateScaleWidget();
    void scheduleDeferredRender();
    void setReplayListenFrequency(double frequencyHz, bool fromUser);
    void openRecordingFromDialog();
    void loadPersistentUiSettings();
    void savePersistentUiSettings() const;
    void schedulePersistentUiSettingsSave();
    void updateSliderValueLabels();
    void startLocalIqPlayback();
    void stopLocalIqPlayback();
    void resetLocalIqPlaybackState(bool updateButton);
    void stopLocalIqWorker();
    RadioSettings replaySettingsForSource(double sampleRate) const;
    ReplayIqSource selectedReplayIqSource() const;
    QString replayIqPathForSource(ReplayIqSource source) const;
    QString replayIqSidecarPathForSource(ReplayIqSource source) const;
    double replayIqEffectiveSampleRate(ReplayIqSource source,
                                       double fallbackSampleRate,
                                       quint64 dataSize,
                                       int bytesPerSample) const;
    double replayIqNominalSampleRate(ReplayIqSource source, double fallbackSampleRate) const;
    double replayIqTimelineOffsetSeconds(ReplayIqSource source, double iqDurationSeconds) const;
    bool replayIqSourceAvailable(ReplayIqSource source) const;
    double frequencyForLocalX(int x, int width) const;
    static QColor colorForLevel(float value,
                                float minLevel,
                                float maxLevel,
                                float contrastValue,
                                float sensitivityValue);
    static QString formatFrequency(double hz);

    MyGraphWidget *graph = nullptr;
    ScaleWidget *scaleWidget = nullptr;
    QLabel *waterfallLabel = nullptr;
    QLabel *experimentWaterfallTile = nullptr;
    QScrollArea *graphScroll = nullptr;
    QScrollArea *scaleScroll = nullptr;
    QScrollArea *waterfallScroll = nullptr;
    Waterfall3DView *waterfall3DView = nullptr;
    QLabel *infoLabel = nullptr;
    QSlider *timelineSlider = nullptr;
    QPushButton *openButton = nullptr;
    QPushButton *playButton = nullptr;
    QToolButton *waterfallPauseButton = nullptr;
    QCheckBox *replayAudioCheckbox = nullptr;
    QComboBox *speedCombo = nullptr;
    QComboBox *replayIqSourceCombo = nullptr;
    QComboBox *waterfallDisplayModeCombo = nullptr;
    QComboBox *waterfall3DResolutionCombo = nullptr;
    QSpinBox *waterfall3DSliceStepSpin = nullptr;
    QSpinBox *waterfall3DSliceWidthSpin = nullptr;
    QSpinBox *waterfall3DSpectrumSliceStepSpin = nullptr;
    QSpinBox *waterfall3DSpectrumSliceRowsSpin = nullptr;
    QCheckBox *waterfall3DSpectrumSliceCaptureCheckbox = nullptr;
    QCheckBox *waterfall3DSpectrumSliceCaptureFixedCheckbox = nullptr;
    QCheckBox *waterfall3DVncSliceInputCheckbox = nullptr;
    QCheckBox *waterfallAreaRulerCheckbox = nullptr;
    QSlider *zoomSlider = nullptr;
    QSlider *rowHeightSlider = nullptr;
    QSlider *contrastSlider = nullptr;
    QSlider *sensitivitySlider = nullptr;
    QLabel *zoomValueLabel = nullptr;
    QLabel *rowHeightValueLabel = nullptr;
    QLabel *contrastValueLabel = nullptr;
    QLabel *sensitivityValueLabel = nullptr;
    FrequencyControl *listenFrequencyControl = nullptr;
    QSpinBox *levelMinSpin = nullptr;
    QSpinBox *levelMaxSpin = nullptr;
    QLabel *linkedIqLabel = nullptr;
    QTimer playbackTimer;
    QTimer saveSettingsTimer;
    QElapsedTimer playbackClock;

    std::shared_ptr<SpectrumFrameRecording> recording = std::make_shared<SpectrumFrameRecording>();
    QString recordingPath;
    QString linkedChannelIqPath;
    QString linkedFullIqPath;
    AudioProcessor *localAudioProcessor = nullptr;
    QImage waterfallImage;
    QImage scaledWaterfallCache;
    int scaledWaterfallCacheWidth = 0;
    bool scaledWaterfallDirty = true;
    std::vector<float> frequencyScratch;
    std::vector<float> replay3DLevels;
    std::vector<unsigned char> replay3DColors;
    std::thread localIqWorkerThread;
    std::atomic<bool> localIqWorkerStop{false};
    int selectedFrame = 0;
    int playbackStartFrame = 0;
    qint64 playbackStartElapsedMs = 0;
    float levelMin = -140.0f;
    float levelMax = -30.0f;
    int waterfallZoomPercent = 100;
    int waterfallRowHeight = 4;
    int replayWaterfallDisplayMode = 0;
    int replay3DResolutionDivisor = 4;
    int replay3DSliceStep = 1;
    int replay3DSliceWidth = 1;
    int replay3DSpectrumSliceStep = 1;
    int replay3DSpectrumSliceRows = 1;
    bool replay3DSpectrumSliceCapture = false;
    bool replay3DSpectrumSliceCaptureFixed = false;
    bool replay3DVncSliceInput = false;
    bool replayAreaRulerEnabled = false;
    bool replayAreaRulerDragging = false;
    QPoint replayAreaRulerStart;
    QPoint replayAreaRulerEnd;
    int replay3DWindowStart = -1;
    int replay3DWindowEnd = -1;
    float replayContrast = 10.0f;
    float replaySensitivity = 10.0f;
    double replayListenFrequencyHz = 0.0;
    double localIqPlaybackSampleRate = 0.0;
    double localIqPlaybackAudioSampleRate = 0.0;
    double localIqPlaybackSeekSampleRate = 0.0;
    double localIqPlaybackFeedSampleRate = 0.0;
    quint64 localIqPlaybackDataSize = 0;
    quint64 localIqPlaybackBytesRead = 0;
    quint64 localIqPlaybackStartByteOffset = 0;
    int localIqPlaybackBytesPerSample = 0;
    bool localIqPlaybackS16 = false;
    double localIqPlaybackDurationSeconds = 0.0;
    double localIqPlaybackTimelineOffsetSeconds = 0.0;
    double localIqPlaybackStartDelaySeconds = 0.0;
    bool localIqPlaybackActive = false;
    bool syncingHorizontalScroll = false;
    bool deferredRenderQueued = false;
    bool replayWaterfallPaused = false;
    bool isolatedExperiment = false;
    bool replayColoredSpectrum = true;
    QJsonObject experimentSettings, experimentCalibration;
    QString experimentContextSource;
    bool experimentDisplayAdjusted = false;
    std::shared_ptr<const SpectrumFrameRecording> experimentFrames;
    std::shared_ptr<std::atomic_bool> imageCancel;
    QFutureWatcher<QImage> *imageWatcher = nullptr;
    bool experimentImageDirty = false;
    std::unique_ptr<DspBranchAudioOutput> experimentAudio;
    std::uint64_t experimentAudioGeneration = 0;
    QSet<QString> verifiedReplayAssets;
    int experimentViewportPixels = 0;
    bool renderingExperimentViewport = false;
};

#endif // SPECTRUMFRAMEREPLAYDIALOG_H
