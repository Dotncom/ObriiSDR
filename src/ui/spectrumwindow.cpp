#include "spectrumwindow.h"
#include <QVBoxLayout>
#include <QDebug>
#include <cmath> 
#include <QChartView>
#include <QChart>
#include <QLineSeries>
#include <QValueAxis>
#include <QImage>
#include <QPainter>
#include <QVector>
#include <complex>
#include <QTimer>
#include <QCloseEvent>
#include <QColor>
#include <QSlider>
#include <QLabel>
#include "dataprocessor.h"
#include <QObject>
#include <fobos.h>
#include <main.h>
#include <QGraphicsRectItem>
#include <QGraphicsLineItem>
#include <QGraphicsView> 
#include <QGraphicsScene>

extern float* iqData;
extern fobos_dev_t *device;
extern double globalFrequency;
extern double globalSampleRate;
extern double listeningFrequency;
extern double globalBandwidth;
extern double minFrequency;
extern double maxFrequency;
extern std::vector<float> fftMagnitudes;
extern std::vector<float> fftFrequencies;
extern int globalMode;
extern int fftLength;
extern double currentScale;

SpectrumWindow::SpectrumWindow(QWidget *parent)
    : QWidget(parent), chart(new QChart), chartView(new QChartView(chart)), series(new QLineSeries),
      waterfallImage(1536, 512, QImage::Format_RGB32), showWaterfall(true),
      contrast(100), speed(50), markerLine(new QLineSeries()) {
    setFixedSize(1700, 900);
    chart->addSeries(markerLine);
    setupChart();

    contrastSlider = new QSlider(Qt::Horizontal, this);
    contrastSlider->setRange(0, 100);
    contrastSlider->setValue(contrast);
    contrastLabel = new QLabel(QString("Contrast: %1").arg(contrast), this);

    speedSlider = new QSlider(Qt::Horizontal, this);
    speedSlider->setRange(1, 100);
    speedSlider->setValue(speed);
    speedLabel = new QLabel(QString("Speed: %1").arg(speed), this);

    QVBoxLayout *layout = new QVBoxLayout;

    waterfallLabel = new QLabel(this);
    waterfallLabel->setGeometry(100, 300, 1536, 512);
    waterfallLabel->setAlignment(Qt::AlignCenter);
    waterfallLabel->setPixmap(QPixmap::fromImage(waterfallImage));
    waterfallCheckbox = new QCheckBox("Show Waterfall", this);
    waterfallCheckbox->setChecked(true);
    waterfallLabel->setVisible(showWaterfall);

    QHBoxLayout *controlLayout = new QHBoxLayout;
    controlLayout->addWidget(contrastLabel);
    controlLayout->addWidget(contrastSlider);
    controlLayout->addWidget(speedLabel);
    controlLayout->addWidget(speedSlider);

    layout->addWidget(chartView);
    layout->addWidget(waterfallLabel);
    layout->addWidget(waterfallCheckbox);
    layout->addLayout(controlLayout);
    setLayout(layout);

    updateTimer = new QTimer(this);
    updateTimer->start(50);
    toggleWaterfall(true);

    
    connect(updateTimer, &QTimer::timeout, this, &SpectrumWindow::updateSpectrum);
    connect(contrastSlider, &QSlider::valueChanged, this, &SpectrumWindow::onContrastChanged);
    connect(speedSlider, &QSlider::valueChanged, this, &SpectrumWindow::onSpeedChanged);    
    connect(waterfallCheckbox, &QCheckBox::toggled, this, &SpectrumWindow::toggleWaterfall); 

    qDebug() << "SpectrumWindow initialized.";
}

SpectrumWindow::~SpectrumWindow() {
    delete series;
    delete chart;
    delete chartView;
    waterfallImage = QImage();
}

void SpectrumWindow::setupChart() {
    chart->addSeries(series);
    chart->createDefaultAxes();
    chart->setTitle("Spectrum");
    
    QValueAxis *axisX = new QValueAxis;
    QValueAxis *axisY = new QValueAxis;
    chart->addAxis(axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignLeft);
    series->attachAxis(axisX);
    series->attachAxis(axisY);
    
    axisX->setTitleText("Frequency (Hz)");
    axisY->setTitleText("Magnitude");
    //axisX->setTickCount(10);
    //markerLine = new QLineSeries();
    markerLine->setName("Marker Line");
    markerLine->setColor(QColor(255, 0, 0));
    chart->addSeries(markerLine);
    
    for (auto axis : chart->axes()) {
        markerLine->detachAxis(axis);
    }
    QList<QAbstractAxis*> axesX = chart->axes(Qt::Horizontal);
    QList<QAbstractAxis*> axesY = chart->axes(Qt::Vertical);
    if (!axesX.isEmpty()) {
        markerLine->attachAxis(axesX.first());
    }
    if (!axesY.isEmpty()) {
        markerLine->attachAxis(axesY.first());
    qDebug() << "Chart setup complete.";
    }
}


void SpectrumWindow::updateSpectrum() {
    if (iqData) {
        series->clear();
        int fftLeng = fftLength;
        for (int ii = 0; ii < fftLeng; ++ii) {
            int shiftedIndex = (ii + fftLeng / 2) % fftLeng;
            double frequency = fftFrequencies[ii];
            if (isSinglePositiveHfInputMode(globalMode) && frequency < globalFrequency) {
                continue;
            }
            series->append(frequency, fftMagnitudes[shiftedIndex]);
        }
        updateMarker();
        if (chart->series().isEmpty()) {
            chart->addSeries(series);
        }
        chart->createDefaultAxes();
        QValueAxis* axisX = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).first());
        if (axisX) {
            axisX->setRange(minFrequency, maxFrequency);
            axisX->setTickCount(10);
        }
        chart->update();
        if (showWaterfall) {
            updateWaterfall();
        }
    }
}

void SpectrumWindow::updateWaterfall() {
    int leng = fftLength;
    int height = waterfallImage.height();
    int width = waterfallImage.width();
    QImage tempImage = waterfallImage.copy(0, 0, width, height - 1);
    waterfallImage.fill(Qt::black);
    {
        QPainter tempPainter(&waterfallImage);
        tempPainter.drawImage(0, 1, tempImage);
    }

    QPainter painter(&waterfallImage);
    if (painter.isActive()) {
             for (int i = 0; i < leng - 1; ++i) {
                int shiftedIndex1 = (i + leng / 2) % leng;
                float magnitude1 = fftMagnitudes[shiftedIndex1];
                float normMagnitude1 = qBound(0.0f, magnitude1 / 0.8f, 1.0f);
                QColor color1 = valueToColor(normMagnitude1);
                
                color1 = QColor(
                    qBound(0, static_cast<int>(color1.red() * contrast / 100.0), 255),
                    qBound(0, static_cast<int>(color1.green() * contrast / 100.0), 255),
                    qBound(0, static_cast<int>(color1.blue() * contrast / 100.0), 255)
                );
            double frequency1 = fftFrequencies[i];
            double frequency2 = fftFrequencies[i+1];
            int x1 = static_cast<int>((frequency1 - minFrequency) * width / (maxFrequency - minFrequency));
            int x2 = static_cast<int>((frequency2 - minFrequency) * width / (maxFrequency - minFrequency));
                if (x1 >= 0 && x1 < width && x2 >= 0 && x2 < width) {
                    painter.setPen(color1);
                    painter.drawLine(x1, 0, x2, 0); 
                }
            }
        } else {
        qDebug() << "Painter is not active.";
    }
    waterfallLabel->setPixmap(QPixmap::fromImage(waterfallImage));
}


void SpectrumWindow::onContrastChanged(int value) {
    contrast = value;
    contrastLabel->setText(QString("Contrast: %1").arg(value));
    updateWaterfall(); 
}

void SpectrumWindow::onSpeedChanged(int value) {
    speed = value;
    speedLabel->setText(QString("Speed: %1").arg(value));
    updateTimer->setInterval(1000 / value);  
}

void SpectrumWindow::toggleWaterfall(bool checked) {
    showWaterfall = checked;
    waterfallLabel->setVisible(showWaterfall);
    if (showWaterfall) {
        int width = waterfallImage.width();
        int height = waterfallImage.height();
        waterfallImage.fill(Qt::black); 
        QPainter painter(&waterfallImage);
        painter.fillRect(0, 0, width, height, Qt::black);
    waterfallLabel->setPixmap(QPixmap::fromImage(waterfallImage));
    }
    qDebug() << "Waterfall visibility toggled: " << showWaterfall;
}


void SpectrumWindow::updateMarker() {
    QList<QAbstractAxis*> axesY = chart->axes(Qt::Vertical);
    QList<QAbstractAxis*> axesX = chart->axes(Qt::Horizontal);
    if (!axesY.isEmpty() && !axesX.isEmpty()) {
        QValueAxis *axisY = qobject_cast<QValueAxis*>(axesY.first());
        QValueAxis *axisX = qobject_cast<QValueAxis*>(axesX.first());
        if (axisY && axisX) {
            double startFreq = listeningFrequency - globalBandwidth / 2;
            double endFreq = listeningFrequency + globalBandwidth / 2;
            markerLine->clear(); 
            markerLine->append(QPointF(listeningFrequency, axisY->min()));
            markerLine->append(QPointF(listeningFrequency, axisY->max()));
            QPen shadedPen(QColor(150, 150, 150, 100));
            QLineSeries *shadedArea = new QLineSeries();
            shadedArea->setColor(shadedPen.color());
            shadedArea->append(QPointF(startFreq, axisY->max()));
            shadedArea->append(QPointF(startFreq, axisY->min()));
            shadedArea->append(QPointF(endFreq, axisY->min()));
            shadedArea->append(QPointF(endFreq, axisY->max()));
            shadedArea->append(QPointF(startFreq, axisY->max()));
            chart->addSeries(shadedArea);
            shadedArea->attachAxis(axisX);
            shadedArea->attachAxis(axisY);
        }
    }
}

void SpectrumWindow::onMouseMove(QMouseEvent *event) {
    QPointF mousePos = chartView->mapToScene(event->pos());
    QValueAxis* axisX = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).first());
    if (axisX && event->buttons() & Qt::LeftButton) {
        double newFreq = axisX->min() + (axisX->max() - axisX->min()) * (mousePos.x() / chartView->width());
        listeningFrequency = newFreq;
        updateMarker();
        event->accept();  
    }
}

void SpectrumWindow::onMousePress(QMouseEvent *event) {
    QPointF mousePos = chartView->mapToScene(event->pos());
    QValueAxis* axisX = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).first());

    if (axisX && event->button() == Qt::LeftButton) {
        double newFreq = axisX->min() + (axisX->max() - axisX->min()) * (mousePos.x() / chartView->width());
        listeningFrequency = newFreq;
        updateMarker();
        event->accept();  
    }
}


QColor SpectrumWindow::valueToColor(float value) {
    value = qBound(0.0f, value, 1.0f);
    int r, g, b;
    if (value < 0.16f) {
        float ratio = value / 0.2f;
        r = 0;
        g = 0;
        b = static_cast<int>(255 * ratio);
    } else if (value < 0.33f) {
        float ratio = (value - 0.16f) / 0.2f;
        r = 0;
        g = static_cast<int>(255 * (1 - ratio));
        b = 255;
    } else if (value < 0.5f) {
        float ratio = (value - 0.33f) / 0.2f;
        r = 0;
        g = 255;
        b = static_cast<int>(255 * (1 - ratio));
    } else if (value < 0.66f) {
        float ratio = (value - 0.5f) / 0.2f;
        r = static_cast<int>(255 * ratio);
        g = 255;
        b = 0;
    } else if (value < 0.83f) {
        float ratio = (value - 0.66f) / 0.2f;
        r = 255;
        g = static_cast<int>(255 * (1 - 0.5f * ratio));
        b = 0;
    } else {
        float ratio = (value - 0.83f) / 0.2f;
        r = 255;
        g = static_cast<int>(128 * (1 - ratio));
        b = 0;
    }
    return QColor(r, g, b);
}
