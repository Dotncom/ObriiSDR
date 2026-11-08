#ifndef ZEROSPANDIALOG_H
#define ZEROSPANDIALOG_H

#include "spectrumoverlaytypes.h"
#include "spectrumscienceanalyzer.h"

#include <QDialog>
#include <QElapsedTimer>
#include <deque>
#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class ZeroSpanPlotWidget;

class ZeroSpanDialog : public QDialog {
public:
    using Translator = std::function<QString(const QString &, const QString &)>;

    explicit ZeroSpanDialog(Translator translator, QWidget *parent = nullptr);
    ~ZeroSpanDialog() override;

    void appendSpectrumFrame(const std::vector<float> &frequencies,
                             const std::vector<float> &levels,
                             double listeningFrequencyHz,
                             const SpectrumScienceMarker &markerA,
                             const SpectrumScienceMarker &markerB,
                             const SpectrumScienceMetrics &metrics,
                             bool fftShiftedStorage = false);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum class Source {
        Listening = 0,
        MarkerA = 1,
        MarkerB = 2,
        Peak = 3
    };
    enum class TriggerMode {
        Off = 0,
        Auto = 1,
        Normal = 2
    };
    enum class TriggerEdge {
        Rising = 0,
        Falling = 1
    };

    struct Sample {
        qint64 utcMs = 0;
        double frequencyHz = 0.0;
        float levelDb = -160.0f;
    };

    QString text(const QString &key, const QString &fallback) const;
    void loadSettings();
    void saveSettings() const;
    void clearTrace();
    void armTrigger();
    void updateControlState();
    void updateStatus();
    void exportCsv();
    void trimRollingSamples(qint64 newestMs);
    double selectedFrequency(double listeningFrequencyHz,
                             const SpectrumScienceMarker &markerA,
                             const SpectrumScienceMarker &markerB,
                             const SpectrumScienceMetrics &metrics) const;
    float measuredLevel(const std::vector<float> &frequencies,
                        const std::vector<float> &levels,
                        double targetFrequencyHz,
                        bool fftShiftedStorage) const;
    bool triggerCrossed(float previous, float current) const;

    Translator translator;
    ZeroSpanPlotWidget *plot = nullptr;
    QComboBox *sourceCombo = nullptr;
    QDoubleSpinBox *bandwidthSpin = nullptr;
    QDoubleSpinBox *timeSpanSpin = nullptr;
    QComboBox *triggerModeCombo = nullptr;
    QComboBox *triggerEdgeCombo = nullptr;
    QDoubleSpinBox *triggerLevelSpin = nullptr;
    QSpinBox *pretriggerSpin = nullptr;
    QCheckBox *autoScaleCheckbox = nullptr;
    QDoubleSpinBox *levelMinSpin = nullptr;
    QDoubleSpinBox *levelMaxSpin = nullptr;
    QPushButton *runButton = nullptr;
    QPushButton *armButton = nullptr;
    QPushButton *clearButton = nullptr;
    QPushButton *exportButton = nullptr;
    QLabel *frequencyLabel = nullptr;
    QLabel *statusLabel = nullptr;

    std::deque<Sample> samples;
    bool running = true;
    bool armed = false;
    bool triggered = false;
    qint64 triggerUtcMs = -1;
    float previousLevelDb = -160.0f;
    bool havePreviousLevel = false;
    double currentFrequencyHz = 0.0;
    QElapsedTimer displayUpdateClock;
};

#endif // ZEROSPANDIALOG_H
