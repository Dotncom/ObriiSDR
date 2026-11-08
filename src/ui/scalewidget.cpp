#include "scalewidget.h"

#include <algorithm>
#include <cmath>

ScaleWidget::ScaleWidget(QWidget *parent)
    : QWidget(parent),
      minValue(0),
      maxValue(100),
      markerPosition(0.5),
      listeningValue(50),
      centerValue(50),
      bandwidthValue(1),
      modulationTypeValue(MOD_AM) {}

void ScaleWidget::setRange(double min, double max) {
    minValue = min;
    maxValue = max;
    if (maxValue > minValue) {
        const double displayListening = displayFrequencyForActualFrequency(listeningValue);
        setMarkerPosition((displayListening - minValue) / (maxValue - minValue));
    }
    update();  
}

void ScaleWidget::setScanSegments(const QVector<ScanVisualSegment> &segments) {
    scanSegments = segments;
    if (maxValue > minValue) {
        const double displayListening = displayFrequencyForActualFrequency(listeningValue);
        setMarkerPosition((displayListening - minValue) / (maxValue - minValue));
    } else {
        update();
    }
}

void ScaleWidget::setScanSegmentMarkersVisible(bool visible) {
    if (scanSegmentMarkersVisible == visible) {
        return;
    }
    scanSegmentMarkersVisible = visible;
    update();
}

void ScaleWidget::setMarkerPosition(double position) {
    markerPosition = qBound(0.0, position, 1.0);
    update();
}

void ScaleWidget::setTuning(double listeningFrequency, double centerFrequency, double bandwidth) {
    setTuning(listeningFrequency, centerFrequency, bandwidth, modulationTypeValue);
}

void ScaleWidget::setTuning(double listeningFrequency, double centerFrequency, double bandwidth, int modulationType) {
    listeningValue = listeningFrequency;
    centerValue = centerFrequency;
    bandwidthValue = bandwidth;
    modulationTypeValue = modulationType;
    if (maxValue > minValue) {
        const double displayListening = displayFrequencyForActualFrequency(listeningValue);
        setMarkerPosition((displayListening - minValue) / (maxValue - minValue));
    } else {
        update();
    }
}

void ScaleWidget::setModulationType(int modulationType) {
    modulationTypeValue = modulationType;
    update();
}

double ScaleWidget::currentListeningFrequency() const {
    return listeningValue;
}

double ScaleWidget::currentCenterFrequency() const {
    return centerValue;
}

void ScaleWidget::mousePressEvent(QMouseEvent *event) {
    if (width() <= 0 || maxValue <= minValue) {
        return;
    }

    if (event->button() == Qt::LeftButton) {
        double newMarkerPos = static_cast<double>(event->x()) / width();
        setMarkerPosition(newMarkerPos);
        listeningValue = actualFrequencyForDisplayFrequency(displayFrequencyAtPosition(markerPosition));
        emit frequencyChanged(); 
    } else if (event->button() == Qt::RightButton) {
		double newMPos = static_cast<double>(event->x()) / width();
        centerValue = actualFrequencyForDisplayFrequency(displayFrequencyAtPosition(qBound(0.0, newMPos, 1.0)));
        emit centralFrequencyChanged();
        dragging = true;
        lastMouseX = event->x();
    }
    
}

void ScaleWidget::mouseMoveEvent(QMouseEvent *event) {
    if (dragging && width() > 0 && maxValue > minValue) {
        int deltaX = event->x() - lastMouseX;
        double frequencyShift = deltaX * (maxValue - minValue) / width();
        centerValue -= frequencyShift;
        lastMouseX = event->x();
        emit centralFrequencyChanged(); 
        update();
    }
}

void ScaleWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::RightButton) {
        dragging = false;
    }
}

void ScaleWidget::wheelEvent(QWheelEvent *event) {
    if (maxValue <= minValue) {
        return;
    }

    int numSteps = event->angleDelta().y() / 120; 
    if (numSteps == 0) {
        return;
    }

    double stepSize = (maxValue - minValue) / 100; 
    const double proposedListening = listeningValue + numSteps * stepSize;
    bool shiftedCenter = false;

    if (proposedListening < minValue) {
        centerValue += proposedListening - minValue;
        shiftedCenter = true;
    } else if (proposedListening > maxValue) {
        centerValue += proposedListening - maxValue;
        shiftedCenter = true;
    }

    listeningValue = proposedListening;
    const double displayListening = displayFrequencyForActualFrequency(listeningValue);
    setMarkerPosition((displayListening - minValue) / (maxValue - minValue));
    if (shiftedCenter) {
        emit tuningChanged(listeningValue, centerValue);
    } else {
        emit frequencyChanged();
    }
}


double ScaleWidget::displayFrequencyAtPosition(double position) const {
    return minValue + qBound(0.0, position, 1.0) * (maxValue - minValue);
}

double ScaleWidget::actualFrequencyForDisplayFrequency(double displayFrequency) const {
    for (const ScanVisualSegment &segment : scanSegments) {
        if (!std::isfinite(segment.startHz) ||
            !std::isfinite(segment.endHz) ||
            !std::isfinite(segment.actualStartHz) ||
            !std::isfinite(segment.actualEndHz) ||
            segment.endHz <= segment.startHz ||
            displayFrequency < segment.startHz ||
            displayFrequency > segment.endHz) {
            continue;
        }
        const double ratio = (displayFrequency - segment.startHz) / (segment.endHz - segment.startHz);
        return segment.actualStartHz +
               std::clamp(ratio, 0.0, 1.0) * (segment.actualEndHz - segment.actualStartHz);
    }
    return displayFrequency;
}

double ScaleWidget::displayFrequencyForActualFrequency(double actualFrequency) const {
    for (const ScanVisualSegment &segment : scanSegments) {
        if (!std::isfinite(segment.startHz) ||
            !std::isfinite(segment.endHz) ||
            !std::isfinite(segment.actualStartHz) ||
            !std::isfinite(segment.actualEndHz) ||
            segment.endHz <= segment.startHz ||
            segment.actualEndHz <= segment.actualStartHz ||
            actualFrequency < segment.actualStartHz ||
            actualFrequency > segment.actualEndHz) {
            continue;
        }
        const double ratio = (actualFrequency - segment.actualStartHz) /
                             (segment.actualEndHz - segment.actualStartHz);
        return segment.startHz + std::clamp(ratio, 0.0, 1.0) * (segment.endHz - segment.startHz);
    }
    return actualFrequency;
}

void ScaleWidget::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    int width = this->width();
    int height = this->height();
    painter.drawLine(0, height / 2, width, height / 2);
    int numTicks = 10; 
    int numMinorTicks = 9;
    for (int i = 0; i <= numTicks; ++i) {
        double value = minValue + i * (maxValue - minValue) / numTicks;
        double pos = static_cast<double>(i) / numTicks * width;
        const double labelFrequency = actualFrequencyForDisplayFrequency(value);
        QString label = labelFrequency >= 1000000000.0
                            ? QString::number(labelFrequency / 1e9, 'f', 3) + " GHz"
                            : QString::number(labelFrequency / 1e6, 'f', 3) + " MHz";
        painter.drawText(pos - 20, height - 5, label);
        painter.drawLine(pos, height / 2 - 10, pos, height / 2 + 10);
            for (int j = 1; j <= numMinorTicks; ++j) {
            double minorPos = pos + j * width / (numTicks * (numMinorTicks + 1));
            painter.drawLine(minorPos, height / 2 - 3, minorPos, height / 2 + 3);
        }
    }
    if (scanSegmentMarkersVisible && !scanSegments.isEmpty() && maxValue > minValue) {
        QPen segmentPen(QColor(120, 175, 235, 170));
        painter.setPen(segmentPen);
        for (const ScanVisualSegment &segment : scanSegments) {
            if (!std::isfinite(segment.startHz) || !std::isfinite(segment.endHz)) {
                continue;
            }
            const int left = static_cast<int>(std::round((segment.startHz - minValue) * width / (maxValue - minValue)));
            const int right = static_cast<int>(std::round((segment.endHz - minValue) * width / (maxValue - minValue)));
            if (right < 0 || left > width || right <= left) {
                continue;
            }
            painter.drawLine(left, 0, left, height);
            painter.drawLine(right, 0, right, height);
        }
    }
    int markerX = static_cast<int>(markerPosition * width);
    double bandwidthFraction = maxValue > minValue ? bandwidthValue / (maxValue - minValue) : 0.0;
    int bandwidthPixels = qMax(1, static_cast<int>(bandwidthFraction * width));

    QColor green(0, 255, 0, 100);
    painter.setBrush(green);
    painter.setPen(Qt::NoPen);

    int bandwidthStart = markerX - bandwidthPixels / 2;
    int bandwidthEnd = markerX + bandwidthPixels / 2;
    if (isUpperSidebandMode(modulationTypeValue)) {
        bandwidthStart = markerX;
        bandwidthEnd = markerX + bandwidthPixels;
    } else if (isLowerSidebandMode(modulationTypeValue)) {
        bandwidthStart = markerX - bandwidthPixels;
        bandwidthEnd = markerX;
    }
    bandwidthStart = qBound(0, bandwidthStart, width);
    bandwidthEnd = qBound(0, bandwidthEnd, width);
    if (bandwidthEnd > bandwidthStart) {
        painter.drawRect(bandwidthStart, 0, bandwidthEnd - bandwidthStart, height);
    }
    painter.setPen(Qt::red);
    painter.drawLine(markerX, 0, markerX, height);
}
