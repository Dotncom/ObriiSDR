#ifndef ZOOMSPECTRUMDIALOG_H
#define ZOOMSPECTRUMDIALOG_H

#include <QDialog>
#include <QElapsedTimer>
#include <memory>

class QLabel;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QTimer;
class QComboBox;
class QStackedWidget;
class MyGraphWidget;
class MyWaterfallWidget;
class ScaleWidget;
class Waterfall3DView;
class ZoomSpectrumProcessor;

class ZoomSpectrumDialog : public QDialog {
    Q_OBJECT
public:
    explicit ZoomSpectrumDialog(QWidget *parent = nullptr);
    ~ZoomSpectrumDialog() override;

    void setSource(const std::shared_ptr<ZoomSpectrumProcessor> &processor,
                   double lowHz,
                   double highHz);
    double selectedLowHz() const noexcept { return lowHz; }
    double selectedHighHz() const noexcept { return highHz; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void applyConfiguration();
    void pollFrames();
    void updateStatus(int fftLength,
                      double sampleRateHz,
                      double binWidthHz,
                      int decimation,
                      int deliveredFrames);
    QString formatFrequency(double hz) const;

    std::shared_ptr<ZoomSpectrumProcessor> processor;
    double lowHz = 0.0;
    double highHz = 0.0;

    MyGraphWidget *graph = nullptr;
    ScaleWidget *scale = nullptr;
    MyWaterfallWidget *waterfall = nullptr;
    Waterfall3DView *waterfall3D = nullptr;
    QStackedWidget *waterfallStack = nullptr;
    QComboBox *displayModeCombo = nullptr;
    std::vector<unsigned char> colorScratch;
    QCheckBox *enabledCheck = nullptr;
    QDoubleSpinBox *binWidthSpin = nullptr;
    QSpinBox *updateIntervalSpin = nullptr;
    QSpinBox *levelMinSpin = nullptr;
    QSpinBox *levelMaxSpin = nullptr;
    QLabel *rangeLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QTimer *pollTimer = nullptr;
    QElapsedTimer rateTimer;
    int rateFrameCount = 0;
    double measuredRowsPerSecond = 0.0;
};

#endif
