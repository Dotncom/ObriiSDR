#ifndef SIGNALLABDIALOG_H
#define SIGNALLABDIALOG_H

#include "signallabgenerator.h"

#include <QDialog>

#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class SignalLabPreviewWidget;
class SignalLabWaterfall2DWidget;
class SignalLabConstellation3DWidget;
class Waterfall3DView;

class SignalLabDialog final : public QDialog {
public:
    using TransmitHandler = std::function<void(const SignalLabGenerationResult &)>;
    using StopHandler = std::function<void()>;

    explicit SignalLabDialog(const QString &language,
                             TransmitHandler transmitHandler,
                             StopHandler stopHandler,
                             QWidget *parent = nullptr);

    void setLanguage(const QString &language);
    void setInitialSampleRate(int sampleRate);

protected:
    void hideEvent(QHideEvent *event) override;

private:
    QString text(const QString &english, const QString &ukrainian) const;
    void buildUi();
    void updateTexts();
    void updateModeUi();
    void updateSampleRateLimits();
    void rebuildSpectralPreview();
    void updatePreviewMode();
    SignalLabConfiguration configurationFromUi() const;
    bool generateCurrent();
    void exportCurrent();
    void loadSettings();
    void saveSettings() const;

    QString language;
    TransmitHandler transmitHandler;
    StopHandler stopHandler;
    SignalLabGenerationResult lastResult;
    SignalLabConfiguration lastConfiguration;

    QComboBox *waveformCombo = nullptr;
    QComboBox *sampleRateCombo = nullptr;
    QDoubleSpinBox *durationSpin = nullptr;
    QSpinBox *amplitudeSpin = nullptr;
    QDoubleSpinBox *centerOffsetSpin = nullptr;
    QGroupBox *basicBox = nullptr;
    QDoubleSpinBox *toneSpin = nullptr;
    QDoubleSpinBox *secondToneSpin = nullptr;
    QDoubleSpinBox *dutySpin = nullptr;
    QLineEdit *multiToneEdit = nullptr;
    QGroupBox *sweepBox = nullptr;
    QDoubleSpinBox *sweepStartSpin = nullptr;
    QDoubleSpinBox *sweepStopSpin = nullptr;
    QDoubleSpinBox *sweepBandwidthSpin = nullptr;
    QDoubleSpinBox *sweepPeriodSpin = nullptr;
    QComboBox *sweepModeCombo = nullptr;
    QGroupBox *digitalBox = nullptr;
    QDoubleSpinBox *symbolRateSpin = nullptr;
    QDoubleSpinBox *rolloffSpin = nullptr;
    QDoubleSpinBox *fskDeviationSpin = nullptr;
    QSpinBox *seedSpin = nullptr;
    QGroupBox *ofdmBox = nullptr;
    QComboBox *ofdmFftCombo = nullptr;
    QSpinBox *ofdmActiveSpin = nullptr;
    QSpinBox *ofdmCpSpin = nullptr;
    QSpinBox *ofdmSymbolsSpin = nullptr;
    QSpinBox *ofdmPilotSpin = nullptr;
    QComboBox *ofdmConstellationCombo = nullptr;
    QCheckBox *ofdmDcNullCheck = nullptr;
    QDoubleSpinBox *ofdmWindowSpin = nullptr;
    QComboBox *previewModeCombo = nullptr;
    QCheckBox *constellationSurfaceCheckbox = nullptr;
    QComboBox *previewFftCombo = nullptr;
    QSpinBox *previewRowsSpin = nullptr;
    QStackedWidget *previewStack = nullptr;
    SignalLabPreviewWidget *preview = nullptr;
    SignalLabWaterfall2DWidget *waterfall2D = nullptr;
    SignalLabConstellation3DWidget *constellation3D = nullptr;
    Waterfall3DView *waterfall3D = nullptr;
    QLabel *summaryLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *generateButton = nullptr;
    QPushButton *transmitButton = nullptr;
    QPushButton *stopButton = nullptr;
    QPushButton *exportButton = nullptr;
    QPushButton *closeButton = nullptr;
};

#endif // SIGNALLABDIALOG_H
