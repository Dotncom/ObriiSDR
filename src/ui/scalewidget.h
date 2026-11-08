#ifndef SCALEWIDGET_H
#define SCALEWIDGET_H

#include <QObject>
#include <QWidget>
#include <QPainter>
#include <QPen>
#include <QMouseEvent>
#include <QWheelEvent>
#include "radiosettings.h"
#include "scanvisualassembler.h"

class ScaleWidget : public QWidget {
    Q_OBJECT

public:
    explicit ScaleWidget(QWidget *parent = nullptr);
	
    void setRange(double min, double max);
    void setScanSegments(const QVector<ScanVisualSegment> &segments);
    void setScanSegmentMarkersVisible(bool visible);
    void setMarkerPosition(double position);
    void setTuning(double listeningFrequency, double centerFrequency, double bandwidth);
    void setTuning(double listeningFrequency, double centerFrequency, double bandwidth, int modulationType);
    void setModulationType(int modulationType);
    double currentListeningFrequency() const;
    double currentCenterFrequency() const;

    void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
signals:
    void frequencyChanged();
    void centralFrequencyChanged();
    void tuningChanged(double listeningFrequency, double centerFrequency);
private:
    double displayFrequencyAtPosition(double position) const;
    double actualFrequencyForDisplayFrequency(double displayFrequency) const;
    double displayFrequencyForActualFrequency(double actualFrequency) const;
    double minValue;
    double maxValue;
    double markerPosition;
    double listeningValue;
    double centerValue;
    double bandwidthValue;
    int modulationTypeValue;
    QVector<ScanVisualSegment> scanSegments;
    bool scanSegmentMarkersVisible = true;
    
    bool dragging = false;
    int lastMouseX = 0;
};

#endif // SCALEWIDGET_H
