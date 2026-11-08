#ifndef SPECTRUMWINDOW_H
#define SPECTRUMWINDOW_H

#include <QCloseEvent>
#include <QVBoxLayout>
#include <QDebug>
#include <cmath>
#include <QChartView>
#include <QChart>
#include <QLineSeries>
#include <QValueAxis>
#include <QWidget>
#include <QCheckBox>
#include <QImage>
#include <QLabel>

#include <QTimer>
#include <QColor>
#include <QVector>
#include <QSlider>
#include <QObject>
#include <QMouseEvent>
#include <fobos.h>
#include "dataprocessor.h"

QT_CHARTS_USE_NAMESPACE

extern float* iqData;
extern double globalFrequency;
extern double globalSampleRate;
extern fobos_dev_t *device;
extern double listeningFrequency;
extern double globalBandwidth;
extern std::vector<float> fftMagnitudes;
extern std::vector<float> fftFrequencies;
extern int globalMode;
extern int fftLength;
extern double currentScale;
extern double minFrequency;
extern double maxFrequency;
class SpectrumWindow : public QWidget {
    Q_OBJECT

public:
    explicit SpectrumWindow(QWidget *parent = nullptr);
    ~SpectrumWindow();

public slots:
    void toggleWaterfall(bool checked);
    void onContrastChanged(int value);
    void onSpeedChanged(int value);

    void onMouseMove(QMouseEvent *event);
    void onMousePress(QMouseEvent *event);
    void updateSpectrum();
    void updateMarker();
    void updateWaterfall();
    void setupChart();

private:
    QColor valueToColor(float value);

    QChart *chart;
    QChartView *chartView;
    QLineSeries *series;
    QTimer *updateTimer;
    QImage waterfallImage;
    QLabel *waterfallLabel;
    QLabel *speedLabel;
    QLabel *contrastLabel;

    QCheckBox *waterfallCheckbox;
    QSlider *contrastSlider;
    QSlider *speedSlider;

    QVBoxLayout *layout;
    QHBoxLayout *controlLayout;
    bool showWaterfall;
    int contrast;
    int speed;
    QtCharts::QLineSeries *markerLine;
};

#endif // SPECTRUMWINDOW_H
