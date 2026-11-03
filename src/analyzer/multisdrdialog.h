#ifndef MULTISDRDIALOG_H
#define MULTISDRDIALOG_H

#include "analyzerreceiver.h"
#include "coherentiqaligner.h"
#include "spectrummosaicengine.h"

#include <QDialog>
#include <QElapsedTimer>

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <vector>

class AnalyzerSpectrumWidget;
class AnalyzerWaterfallPreview;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class MultiSdrDialog final : public QDialog {
    Q_OBJECT
public:
    explicit MultiSdrDialog(bool ukrainianUi, QWidget *parent = nullptr,
                            bool primaryFromHost = false);
    ~MultiSdrDialog() override;
    void consumePrimaryFrame(const AnalyzerSpectrumFrame &frame);
    void setPrimaryRunning(bool running);
    void setPrimaryConfiguration(double centerHz, double sampleRateHz, int fftLength);
    bool mainMosaicFrame(AnalyzerSpectrumFrame &frame) const;
    void setMainViewMode(int mode);
    void focusAdjacentMainReceiver(int direction);
    void panMainDisplay(double deltaHz);
    int selectAudioSourceForFrequency(double frequencyHz);
    int currentAudioSourceIndex() const {
        return audioSourceIndex.load(std::memory_order_acquire);
    }
    int mainIncludedReceiverCount() const;
    bool anyReceiverRunning() const;
    int focusedMainReceiverNumber() const;

signals:
    void primaryRetuneRequested(double deltaHz);
    void primaryStartRequested();
    void primaryStopRequested();
    void audioSourceChanged(int receiverIndex);
    void auxiliaryIqReady(int receiverIndex, const float *samples, quint64 floatCount,
                          double sampleRateHz, double centerHz);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct SourceUi;

    void buildUi();
    void refreshDevices();
    void populateDeviceCombo(int sourceIndex);
    void populateInputs(int sourceIndex);
    void populateSampleRates(int sourceIndex);
    AnalyzerReceiverSettings receiverSettings(int sourceIndex) const;
    SpectrumMosaicSourceSettings previewMosaicSettings(int sourceIndex) const;
    SpectrumMosaicSourceSettings mainMosaicSettings(int sourceIndex) const;
    bool startSource(int sourceIndex);
    void stopSource(int sourceIndex);
    void startAll();
    void stopAll();
    void addReceiverSlot();
    void removeReceiverSlot();
    void removeReceiverSlot(int sourceIndex);
    void consumeFrame(int sourceIndex, const AnalyzerSpectrumFrame &frame);
    void refreshMosaic(bool force = false);
    void alignEnabledSources(int anchorSourceIndex = 0);
    double sourceUsefulBandwidthHz(int sourceIndex) const;
    void panMosaic(double deltaHz);
    void retuneSource(int sourceIndex, double centerHz);
    void saveSettings() const;
    void restoreSettings();
    DspSourceIdentity sourceIdentity(int sourceIndex) const;
    void refreshCoherentReferenceChoices();
    void updateCoherentSubscriptions();
    void invalidateCoherentCalibration(const QString &reason = {});
    void calibrateCoherentGroup();
    void saveCoherentSnapshot();
    QString text(const char *english, const char *ukrainian) const;
    QString frequencyText(double hz) const;

    bool ukrainian = false;
    bool hostPrimary = false;
    bool primaryRunning = false;
    bool restoringSettings = false;
    std::atomic<int> audioSourceIndex{0};
    double pendingSynchronizedPrimaryCenterHz = std::numeric_limits<double>::quiet_NaN();
    int mainViewMode = 0;
    int focusedMainSource = 0;
    QVector<AnalyzerReceiverDevice> devices;
    std::array<std::unique_ptr<SourceUi>, 8> sources;
    int activeSourceCount = 2;
    SpectrumMosaicEngine previewMosaicEngine;
    SpectrumMosaicEngine mainMosaicEngine;
    AnalyzerSpectrumFrame latestMainMosaicFrame;
    bool latestMainMosaicValid = false;
    AnalyzerSpectrumWidget *spectrum = nullptr;
    AnalyzerWaterfallPreview *previewWaterfall = nullptr;
    QComboBox *panTargetCombo = nullptr;
    QCheckBox *synchronizedPanCheckbox = nullptr;
    QCheckBox *autoAlignCheckbox = nullptr;
    QDoubleSpinBox *alignmentOverlapSpin = nullptr;
    QSpinBox *frameAgeSpin = nullptr;
    QDoubleSpinBox *topDbSpin = nullptr;
    QDoubleSpinBox *bottomDbSpin = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *refreshButton = nullptr;
    QPushButton *startAllButton = nullptr;
    QPushButton *stopAllButton = nullptr;
    QPushButton *addReceiverButton = nullptr;
    QPushButton *removeReceiverButton = nullptr;
    QCheckBox *coherentCaptureCheckbox = nullptr;
    QComboBox *coherentReferenceCombo = nullptr;
    QSpinBox *coherentMaxDelaySpin = nullptr;
    QPushButton *coherentCalibrateButton = nullptr;
    QPushButton *coherentSaveButton = nullptr;
    QLabel *coherentStatusLabel = nullptr;
    QVector<CoherentIqCalibration> coherentCalibrations;
    std::vector<float> mosaicFrequencies;
    std::vector<float> mosaicLevels;
    QElapsedTimer mosaicRefreshClock;
};

#endif
