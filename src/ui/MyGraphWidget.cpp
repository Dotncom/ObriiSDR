#include "MyGraphWidget.h"
#include "radiosettings.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <QPainterPath>
#include <QStyle>
#include <QStringList>
#include <QToolButton>

namespace {
constexpr int GRAPH_LEFT_MARGIN = 0;
constexpr int GRAPH_RIGHT_MARGIN = 0;
constexpr int GRAPH_TOP_MARGIN = 8;
constexpr int GRAPH_BOTTOM_MARGIN = 20;
constexpr int GRAPH_COMPACT_BAND_BOTTOM_MARGIN = 30;
constexpr int GRAPH_SCAN_SEGMENT_BOTTOM_MARGIN = 34;
constexpr int GRAPH_Y_TICKS = 6;
constexpr double AUTO_TUNE_WINDOW_FRACTION = 1.0 / 80.0;
constexpr double AUTO_TUNE_MIN_WINDOW_HZ = 300.0;
constexpr double AUTO_TUNE_MAX_WINDOW_HZ = 500000.0;

struct SignalSample {
    double frequency = 0.0;
    float level = 0.0f;
};
}

MyGraphWidget::MyGraphWidget(QWidget *parent)
    : QOpenGLWidget(parent), xMin(60e6), xMax(140e6), yMin(-120), yMax(0), fftLength(32768), initialized(false) {
    setMouseTracking(true);
    pauseControl = new QToolButton(this);
    pauseControl->setAutoRaise(true);
    pauseControl->setCheckable(true);
    pauseControl->setFixedSize(24, 24);
    pauseControl->hide();
    connect(pauseControl, &QToolButton::toggled, this, &MyGraphWidget::setDisplayPaused);
    updatePauseControl();
}

MyGraphWidget::~MyGraphWidget() {
}

bool MyGraphWidget::bandwidthSelection(double &lowHz, double &highHz) const {
    if (!bandwidthMeasurementVisible ||
        std::abs(bandwidthMeasureEndPos.x() - bandwidthMeasureStartPos.x()) < 4) {
        return false;
    }
    lowHz = frequencyAtX(bandwidthMeasureStartPos.x());
    highHz = frequencyAtX(bandwidthMeasureEndPos.x());
    if (highHz < lowHz) {
        std::swap(lowHz, highHz);
    }
    return std::isfinite(lowHz) && std::isfinite(highHz) && highHz > lowHz;
}

void MyGraphWidget::initializeGL() {
    initializeOpenGLFunctions();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width(), height(), 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    initialized = true;
}

void MyGraphWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    if (pauseControl) {
        pauseControl->move((std::max)(2, w - pauseControl->width() - 6), 6);
        pauseControl->raise();
    }
}

void MyGraphWidget::setData(const std::vector<float> &xData, const std::vector<float> &yData, double xMin, double xMax, int fftLength, bool colorf, bool displayOrdered) {
    if (displayPaused) return;
    this->xData = xData;
    this->yData = yData;
    this->xMin = xMin;
    this->xMax = xMax;
    this->fftLength = std::max(0, fftLength);
    this->colorf = colorf;
    this->dataDisplayOrdered = displayOrdered;
    update();
}

void MyGraphWidget::setPauseControlVisible(bool visible) {
    if (!pauseControl) return;
    pauseControl->setVisible(visible);
    if (visible) pauseControl->raise();
}

void MyGraphWidget::setDisplayPaused(bool paused) {
    displayPaused = paused;
    if (pauseControl && pauseControl->isChecked() != paused) {
        pauseControl->blockSignals(true);
        pauseControl->setChecked(paused);
        pauseControl->blockSignals(false);
    }
    updatePauseControl();
}

void MyGraphWidget::updatePauseControl() {
    if (!pauseControl) return;
    pauseControl->setIcon(style()->standardIcon(displayPaused
                                                    ? QStyle::SP_MediaPlay
                                                    : QStyle::SP_MediaPause));
    pauseControl->setToolTip(displayPaused
                                 ? QString::fromUtf8(u8"Відновити оновлення спектра")
                                 : QString::fromUtf8(u8"Призупинити лише відображення спектра"));
}

void MyGraphWidget::setOverlayData(const std::vector<float> &yData, bool enabled, bool displayOrdered) {
    overlayYData = enabled ? yData : std::vector<float>();
    overlayEnabled = enabled && !overlayYData.empty();
    overlayDisplayOrdered = overlayEnabled && displayOrdered;
    update();
}

void MyGraphWidget::setLevelRange(float minLevel, float maxLevel) {
    if (!std::isfinite(minLevel) || !std::isfinite(maxLevel) || maxLevel <= minLevel) {
        return;
    }
    yMin = minLevel;
    yMax = maxLevel;
    update();
}

void MyGraphWidget::setBandMarkersEnabled(bool generalEnabled, bool amateurEnabled) {
    if (generalBandMarkersEnabled == generalEnabled && amateurBandMarkersEnabled == amateurEnabled) {
        return;
    }
    generalBandMarkersEnabled = generalEnabled;
    amateurBandMarkersEnabled = amateurEnabled;
    update();
}

void MyGraphWidget::setBandMarkersCompact(bool compact) {
    if (compactBandMarkersEnabled == compact) {
        return;
    }
    compactBandMarkersEnabled = compact;
    update();
}

void MyGraphWidget::setBandMarkers(const QVector<GraphBandMarker> &markers) {
    bandMarkers = markers;
    update();
}

void MyGraphWidget::setScanSegments(const QVector<ScanVisualSegment> &segments) {
    scanSegments = segments;
    update();
}

void MyGraphWidget::setScanSegmentMarkersVisible(bool visible) {
    if (scanSegmentMarkersVisible == visible) {
        return;
    }
    scanSegmentMarkersVisible = visible;
    update();
}

void MyGraphWidget::setTuningMarker(double frequencyHz, bool visible) {
    tuningMarkerFrequencyHz = frequencyHz;
    tuningMarkerVisible = visible && std::isfinite(frequencyHz);
    update();
}

void MyGraphWidget::setSpectrumMetadata(double centerFrequencyHz,
                                        double listeningFrequencyHz,
                                        double sampleRateHz,
                                        int sourceFftLength,
                                        int fftWindowType) {
    metadataCenterFrequencyHz = centerFrequencyHz;
    metadataListeningFrequencyHz = listeningFrequencyHz;
    metadataSampleRateHz = sampleRateHz;
    metadataFftLength = (std::max)(0, sourceFftLength);
    metadataFftWindowType = normalizedFftWindowType(fftWindowType);
}

void MyGraphWidget::setFpsOverlayEnabled(bool enabled) {
    if (fpsOverlayEnabled == enabled) {
        return;
    }
    fpsOverlayEnabled = enabled;
    fpsElapsedTimer.invalidate();
    fpsFrameCount = 0;
    displayedFps = 0.0;
    update();
}

void MyGraphWidget::setExtendedInfoOverlayEnabled(bool enabled) {
    if (extendedInfoOverlayEnabled == enabled) {
        return;
    }
    extendedInfoOverlayEnabled = enabled;
    update();
}

void MyGraphWidget::setAnalogPeakMeterEnabled(bool enabled) {
    if (analogPeakMeterEnabled == enabled) return;
    analogPeakMeterEnabled = enabled;
    update();
}

void MyGraphWidget::setAnalogPeakMeterStyle(int style) {
    const int normalized = std::clamp(style, 0, 1);
    if (analogPeakMeterStyle == normalized) return;
    analogPeakMeterStyle = normalized;
    update();
}

void MyGraphWidget::setAnalogPeakMeterTarget(double frequencyHz, bool valid) {
    const bool targetValid = valid && std::isfinite(frequencyHz);
    if (analogPeakMeterTargetValid == targetValid &&
        (!targetValid || qFuzzyCompare(analogPeakMeterTargetHz + 1.0, frequencyHz + 1.0))) return;
    analogPeakMeterTargetValid = targetValid;
    analogPeakMeterTargetHz = targetValid ? frequencyHz : 0.0;
    update();
}

void MyGraphWidget::setFrequencyAxisLabelsVisible(bool visible) {
    if (frequencyAxisLabelsVisible == visible) {
        return;
    }
    frequencyAxisLabelsVisible = visible;
    update();
}

void MyGraphWidget::setScienceAnalysisData(const std::vector<float> &maxHold,
                                           const std::vector<float> &minHold,
                                           const std::vector<float> &average,
                                           const std::vector<float> &percentile50,
                                           const std::vector<float> &percentile90,
                                           const std::vector<float> &percentile99,
                                           bool showMaxHold,
                                           bool showMinHold,
                                           bool showAverage,
                                           bool showPercentile50,
                                           bool showPercentile90,
                                           bool showPercentile99,
                                           const QVector<SpectrumScienceMarker> &markers) {
    scienceMaxHoldData = maxHold;
    scienceMinHoldData = minHold;
    scienceAverageData = average;
    sciencePercentile50Data = percentile50;
    sciencePercentile90Data = percentile90;
    sciencePercentile99Data = percentile99;
    scienceMaxHoldVisible = showMaxHold && !scienceMaxHoldData.empty();
    scienceMinHoldVisible = showMinHold && !scienceMinHoldData.empty();
    scienceAverageVisible = showAverage && !scienceAverageData.empty();
    sciencePercentile50Visible = showPercentile50 && !sciencePercentile50Data.empty();
    sciencePercentile90Visible = showPercentile90 && !sciencePercentile90Data.empty();
    sciencePercentile99Visible = showPercentile99 && !sciencePercentile99Data.empty();
    scienceMarkers = markers;
    update();
}

void MyGraphWidget::clearData() {
    xData.clear();
    yData.clear();
    overlayYData.clear();
    overlayEnabled = false;
    dataDisplayOrdered = false;
    overlayDisplayOrdered = false;
    fftLength = 0;
    update();
}

void MyGraphWidget::paintGL() {
    updateFpsCounter();
    QPainter painter(this);
    painter.beginNativePainting();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const bool canDrawData = !xData.empty() && !yData.empty() && fftLength > 0 &&
                             !qFuzzyCompare(xMin, xMax) && !qFuzzyCompare(yMin, yMax);
    if (canDrawData) {
        glViewport(0, 0, width(), height());
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, width(), 0, height(), -1, 1);
        glColor3f(1.0f, 1.0f, 1.0f);

        const float plotLeft = static_cast<float>(GRAPH_LEFT_MARGIN);
        const float plotBottom = static_cast<float>(bottomMargin());
        const float plotWidth = static_cast<float>((std::max)(1, width() - GRAPH_LEFT_MARGIN - GRAPH_RIGHT_MARGIN));
        const float plotHeight = static_cast<float>((std::max)(1, height() - GRAPH_TOP_MARGIN - bottomMargin()));
        const int dataCount = std::min({fftLength, static_cast<int>(xData.size()), static_cast<int>(yData.size())});
        const int pixelCount = (std::max)(1, static_cast<int>(std::ceil(plotWidth)));
        const bool compressForRender = dataCount > pixelCount * 2;
        if (compressForRender) {
            renderFrequencyScratch.assign(static_cast<std::size_t>(pixelCount),
                                          std::numeric_limits<float>::quiet_NaN());
            renderLevelScratch.assign(static_cast<std::size_t>(pixelCount),
                                      -std::numeric_limits<float>::infinity());
            for (int i = 0; i < dataCount; ++i) {
                const double frequency = displayFrequencyAt(i, dataCount);
                if (!std::isfinite(frequency)) {
                    continue;
                }
                const float xNorm = static_cast<float>((frequency - xMin) / (xMax - xMin));
                if (xNorm < 0.0f || xNorm > 1.0f) {
                    continue;
                }
                float intensity = displayLevelAt(yData, i, dataCount, dataDisplayOrdered);
                if (!std::isfinite(intensity)) {
                    intensity = yMin;
                }
                const int pixel = (std::clamp)(static_cast<int>(std::lround(xNorm * (pixelCount - 1))),
                                               0,
                                               pixelCount - 1);
                float &level = renderLevelScratch[static_cast<std::size_t>(pixel)];
                if (!std::isfinite(level) || intensity > level) {
                    level = intensity;
                    renderFrequencyScratch[static_cast<std::size_t>(pixel)] = frequency;
                }
            }

            bool drawing = false;
            for (int pixel = 0; pixel < pixelCount; ++pixel) {
                const std::size_t index = static_cast<std::size_t>(pixel);
                const float frequency = renderFrequencyScratch[index];
                const float intensity = renderLevelScratch[index];
                if (!std::isfinite(frequency) || !std::isfinite(intensity)) {
                    continue;
                }
                if (!drawing) {
                    glBegin(GL_LINE_STRIP);
                    drawing = true;
                }
                const float level = normalizedLevel(intensity);
                if (colorf) {
                    QColor color = valueToColor(level);
                    glColor3f(color.redF(), color.greenF(), color.blueF());
                } else {
                    glColor3f(0.0f, 1.0f, 0.0f);
                }
                const float xPos = plotLeft + static_cast<float>(pixel) * plotWidth /
                                               static_cast<float>((std::max)(1, pixelCount - 1));
                const float yPos = plotBottom + level * plotHeight;
                glVertex2f(xPos, yPos);
            }
            if (drawing) {
                glEnd();
            }
        } else {
            glBegin(GL_LINE_STRIP);
            for (int i = 0; i < dataCount; ++i) {
                const double frequency = displayFrequencyAt(i, dataCount);
                if (!std::isfinite(frequency)) {
                    continue;
                }
                const float xNorm = static_cast<float>((frequency - xMin) / (xMax - xMin));
                if (xNorm < 0.0f || xNorm > 1.0f) {
                    continue;
                }
                float intensity = displayLevelAt(yData, i, dataCount, dataDisplayOrdered);
                if (!std::isfinite(intensity)) {
                    intensity = 0.0f;
                }
                const float level = normalizedLevel(intensity);
                if (colorf == true) {
                    QColor color = valueToColor(level);
                    glColor3f(color.redF(), color.greenF(), color.blueF());
                } else {
                    glColor3f(0.0f, 1.0f, 0.0f);
                }
                const float xPos = plotLeft + xNorm * plotWidth;
                const float yPos = plotBottom + level * plotHeight;
                glVertex2f(xPos, yPos);
            }
            glEnd();
        }

        if (overlayEnabled && !overlayYData.empty()) {
            glColor3f(1.0f, 0.22f, 0.16f);
            const int overlayCount = std::min({dataCount, static_cast<int>(overlayYData.size())});
            if (compressForRender) {
                renderOverlayLevelScratch.assign(static_cast<std::size_t>(pixelCount),
                                                 -std::numeric_limits<float>::infinity());
                for (int i = 0; i < overlayCount; ++i) {
                    const double frequency = displayFrequencyAt(i, dataCount);
                    if (!std::isfinite(frequency)) {
                        continue;
                    }
                    const float xNorm = static_cast<float>((frequency - xMin) / (xMax - xMin));
                    if (xNorm < 0.0f || xNorm > 1.0f) {
                        continue;
                    }
                    float intensity = displayLevelAt(overlayYData, i, overlayCount, overlayDisplayOrdered);
                    if (!std::isfinite(intensity)) {
                        intensity = yMin;
                    }
                    const int pixel = (std::clamp)(static_cast<int>(std::lround(xNorm * (pixelCount - 1))),
                                                   0,
                                                   pixelCount - 1);
                    float &level = renderOverlayLevelScratch[static_cast<std::size_t>(pixel)];
                    level = std::isfinite(level) ? (std::max)(level, intensity) : intensity;
                }
                bool drawing = false;
                for (int pixel = 0; pixel < pixelCount; ++pixel) {
                    const float intensity = renderOverlayLevelScratch[static_cast<std::size_t>(pixel)];
                    if (!std::isfinite(intensity)) {
                        continue;
                    }
                    if (!drawing) {
                        glBegin(GL_LINE_STRIP);
                        drawing = true;
                    }
                    const float level = normalizedLevel(intensity);
                    const float xPos = plotLeft + static_cast<float>(pixel) * plotWidth /
                                                   static_cast<float>((std::max)(1, pixelCount - 1));
                    const float yPos = plotBottom + level * plotHeight;
                    glVertex2f(xPos, yPos);
                }
                if (drawing) {
                    glEnd();
                }
            } else {
                glBegin(GL_LINE_STRIP);
                for (int i = 0; i < overlayCount; ++i) {
                    const double frequency = displayFrequencyAt(i, dataCount);
                    if (!std::isfinite(frequency)) {
                        continue;
                    }
                    const float xNorm = static_cast<float>((frequency - xMin) / (xMax - xMin));
                    if (xNorm < 0.0f || xNorm > 1.0f) {
                        continue;
                    }
                    float intensity = displayLevelAt(overlayYData, i, overlayCount, overlayDisplayOrdered);
                    if (!std::isfinite(intensity)) {
                        intensity = yMin;
                    }
                    const float level = normalizedLevel(intensity);
                    const float xPos = plotLeft + xNorm * plotWidth;
                    const float yPos = plotBottom + level * plotHeight;
                    glVertex2f(xPos, yPos);
                }
                glEnd();
            }
        }
    }
    painter.endNativePainting();
    drawScanSegments(painter);
    drawBandMarkers(painter);
    drawYAxis(painter);
    drawXAxis(painter);
    drawScienceTraces(painter);
    drawTuningMarker(painter);
    drawScienceMarkers(painter);
    drawBandwidthMeasurement(painter);
    drawHoverCursor(painter);
    drawAnalogPeakMeter(painter);
    drawFpsOverlay(painter);
    drawExtendedInfoOverlay(painter);
}

void MyGraphWidget::drawScienceTraces(QPainter &painter) const {
    const int dataCount = std::min({fftLength,
                                    static_cast<int>(xData.size()),
                                    static_cast<int>(yData.size())});
    if (dataCount < 2) return;
    const int top = GRAPH_TOP_MARGIN;
    const int bottom = (std::max)(top + 1, height() - bottomMargin());
    const int plotHeight = bottom - top;
    auto drawTrace = [&](const std::vector<float> &trace, const QColor &color) {
        const int count = std::min(dataCount, static_cast<int>(trace.size()));
        if (count < 2) return;
        QPainterPath path;
        bool started = false;
        for (int i = 0; i < count; ++i) {
            const float level = trace[static_cast<std::size_t>(i)];
            if (!std::isfinite(level)) continue;
            const double frequency = displayFrequencyAt(i, dataCount);
            if (!std::isfinite(frequency)) continue;
            const int x = xForFrequency(frequency);
            const int y = bottom - static_cast<int>(std::lround(normalizedLevel(level) * plotHeight));
            if (!started) {
                path.moveTo(x, y);
                started = true;
            } else {
                path.lineTo(x, y);
            }
        }
        if (started) {
            painter.setPen(QPen(color, 1));
            painter.drawPath(path);
        }
    };
    painter.save();
    painter.setClipRect(QRect(GRAPH_LEFT_MARGIN, top,
                              (std::max)(1, width() - GRAPH_LEFT_MARGIN - GRAPH_RIGHT_MARGIN),
                              plotHeight));
    painter.setRenderHint(QPainter::Antialiasing, false);
    if (scienceMinHoldVisible) drawTrace(scienceMinHoldData, QColor(85, 155, 255, 210));
    if (sciencePercentile50Visible) drawTrace(sciencePercentile50Data, QColor(75, 220, 145, 220));
    if (sciencePercentile90Visible) drawTrace(sciencePercentile90Data, QColor(215, 105, 245, 220));
    if (sciencePercentile99Visible) drawTrace(sciencePercentile99Data, QColor(255, 80, 95, 230));
    if (scienceAverageVisible) drawTrace(scienceAverageData, QColor(235, 235, 235, 220));
    if (scienceMaxHoldVisible) drawTrace(scienceMaxHoldData, QColor(255, 170, 35, 235));
    painter.restore();
}

void MyGraphWidget::drawScienceMarkers(QPainter &painter) const {
    if (scienceMarkers.isEmpty() || qFuzzyCompare(xMin, xMax)) return;
    const int top = GRAPH_TOP_MARGIN;
    const int bottom = (std::max)(top + 1, height() - bottomMargin());
    const QColor colors[] = {QColor(255, 225, 60), QColor(80, 220, 255)};
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (int index = 0; index < scienceMarkers.size(); ++index) {
        const SpectrumScienceMarker &marker = scienceMarkers.at(index);
        if (!marker.enabled || !std::isfinite(marker.frequencyHz)) continue;
        const double displayFrequency = displayFrequencyForActualFrequency(marker.frequencyHz);
        if (displayFrequency < (std::min)(xMin, xMax) || displayFrequency > (std::max)(xMin, xMax)) continue;
        const int x = xForFrequency(displayFrequency);
        const QColor color = colors[index % 2];
        painter.setPen(QPen(color, 1, Qt::DashLine));
        painter.drawLine(x, top, x, bottom);
        const QString text = QStringLiteral("%1  %2 MHz  %3 dB")
                                 .arg(marker.label)
                                 .arg(marker.frequencyHz / 1.0e6, 0, 'f', 6)
                                 .arg(marker.levelDb, 0, 'f', 1);
        const QFontMetrics metrics(painter.font());
        QRect labelRect = metrics.boundingRect(text).adjusted(-4, -2, 4, 2);
        labelRect.moveTop(top + 3 + index * (labelRect.height() + 2));
        labelRect.moveLeft((std::clamp)(x + 4, 2, (std::max)(2, width() - labelRect.width() - 2)));
        painter.fillRect(labelRect, QColor(0, 0, 0, 190));
        painter.setPen(color);
        painter.drawText(labelRect, Qt::AlignCenter, text);
    }
    painter.restore();
}

void MyGraphWidget::updateFpsCounter() {
    if (!fpsOverlayEnabled) {
        return;
    }
    if (!fpsElapsedTimer.isValid()) {
        fpsElapsedTimer.start();
        fpsFrameCount = 0;
    }
    ++fpsFrameCount;
    const qint64 elapsedMs = fpsElapsedTimer.elapsed();
    if (elapsedMs >= 500) {
        displayedFps = static_cast<double>(fpsFrameCount) * 1000.0 /
                       static_cast<double>(elapsedMs);
        fpsFrameCount = 0;
        fpsElapsedTimer.restart();
    }
}

void MyGraphWidget::drawAnalogPeakMeter(QPainter &painter) const {
    if (!analogPeakMeterEnabled || yData.empty() || fftLength <= 0 || qFuzzyCompare(yMin, yMax)) return;
    const int count = std::min({fftLength, static_cast<int>(xData.size()), static_cast<int>(yData.size())});
    if (count <= 0) return;

    CursorPeak peak;
    if (analogPeakMeterTargetValid) {
        const double displayFrequency = displayFrequencyForActualFrequency(analogPeakMeterTargetHz);
        peak = cursorPeakAtX(xForFrequency(displayFrequency));
    }
    if (!peak.valid) {
        for (int index = 0; index < count; ++index) {
            const float level = displayLevelAt(yData, index, count, dataDisplayOrdered);
            const double displayFrequency = displayFrequencyAt(index, count);
            if (!std::isfinite(level) || !std::isfinite(displayFrequency)) continue;
            if (!peak.valid || level > peak.level) {
                peak.valid = true;
                peak.level = level;
                peak.displayFrequency = displayFrequency;
                peak.frequency = actualFrequencyForDisplayFrequency(displayFrequency);
                peak.dataIndex = index;
            }
        }
    }
    if (!peak.valid) return;

    const int meterWidth = (std::clamp)(width() / 4, 145, 210);
    const int meterHeight = (std::clamp)(meterWidth * 54 / 100, 78, 112);
    const QRect meterRect(8, 8, meterWidth, meterHeight);
    const QPointF center(meterRect.center().x(), meterRect.bottom() - 14.0);
    const qreal radius = (std::min)(meterRect.width() * 0.43, meterRect.height() * 0.80);
    // Keep the instrument scale independent from a narrowly zoomed display range.
    // dBFS cannot exceed 0 in an unclipped stream, while the lower bound follows
    // the visible floor when the user needs more than the usual 120 dB span.
    const double meterMinimum = (std::min)(double(yMin), -120.0);
    const double meterMaximum = (std::max)(double(yMax), 0.0);
    const double normalized = std::clamp((double(peak.level) - meterMinimum) /
                                             (meterMaximum - meterMinimum), 0.0, 1.0);
    constexpr double startDegrees = 210.0;
    constexpr double sweepDegrees = 120.0;
    constexpr double pi = 3.14159265358979323846;
    const auto pointAt = [&](double degrees, double scale) {
        const double radians = degrees * pi / 180.0;
        return QPointF(center.x() + std::cos(radians) * radius * scale,
                       center.y() + std::sin(radians) * radius * scale);
    };

    const bool retro = analogPeakMeterStyle == 1;
    const QColor scaleColor = retro ? QColor(60, 45, 18) : QColor(183, 197, 210);
    const QColor textColor = retro ? QColor(52, 37, 12) : QColor(226, 234, 241);
    const QColor secondaryText = retro ? QColor(91, 62, 18) : QColor(146, 161, 177);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (retro) {
        painter.setPen(QPen(QColor(116, 79, 20), 1.2));
        painter.setBrush(QColor(242, 190, 72, 232));
        painter.drawRoundedRect(meterRect, 4.0, 4.0);
    } else {
        painter.setPen(QPen(QColor(140, 154, 169, 165), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(meterRect, 4.0, 4.0);
    }
    for (int tick = 0; tick <= 10; ++tick) {
        const double degrees = startDegrees + sweepDegrees * tick / 10.0;
        painter.setPen(QPen(tick >= 8 ? QColor(210, 51, 38) : scaleColor,
                            tick % 5 == 0 ? 1.8 : 1.0));
        painter.drawLine(pointAt(degrees, tick % 5 == 0 ? 0.76 : 0.82), pointAt(degrees, 0.96));
        if (tick % 5 == 0) {
            painter.save();
            QFont scaleFont = painter.font();
            scaleFont.setPointSizeF((std::max)(6.0, scaleFont.pointSizeF() - 2.0));
            painter.setFont(scaleFont);
            painter.setPen(scaleColor);
            const QString label = QString::number(
                meterMinimum + (meterMaximum - meterMinimum) * tick / 10.0, 'f', 0);
            const QPointF labelCenter = pointAt(degrees, 0.60);
            painter.drawText(QRectF(labelCenter.x() - 24.0, labelCenter.y() - 8.0, 48.0, 16.0),
                             Qt::AlignCenter, label);
            painter.restore();
        }
    }
    const double needleDegrees = startDegrees + sweepDegrees * normalized;
    painter.setPen(QPen(QColor(255, 91, 72), 2.1, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(center, pointAt(needleDegrees, 0.83));
    painter.setPen(Qt::NoPen);
    painter.setBrush(retro ? QColor(55, 39, 12) : QColor(226, 232, 238));
    painter.drawEllipse(center, 3.5, 3.5);

    QFont font = painter.font();
    font.setPointSizeF((std::max)(7.0, font.pointSizeF() - 1.0));
    painter.setFont(font);
    painter.setPen(textColor);
    const QString source = analogPeakMeterTargetValid ? QStringLiteral("MARKER") : QStringLiteral("PEAK");
    painter.drawText(meterRect.adjusted(7, 3, -7, -3), Qt::AlignTop | Qt::AlignLeft,
                     QStringLiteral("%1  %2 dBFS").arg(source).arg(peak.level, 0, 'f', 1));
    painter.setPen(secondaryText);
    painter.drawText(meterRect.adjusted(7, 3, -7, -5), Qt::AlignBottom | Qt::AlignCenter,
                     formatFrequencyLabel(peak.frequency));
    painter.restore();
}

void MyGraphWidget::drawFpsOverlay(QPainter &painter) const {
    if (!fpsOverlayEnabled) {
        return;
    }
    const QString text = displayedFps > 0.0
                             ? QStringLiteral("FPS %1").arg(displayedFps, 0, 'f', 1)
                             : QStringLiteral("FPS --");
    const QFontMetrics metrics(painter.font());
    const QRect textRect = metrics.boundingRect(text).adjusted(-6, -3, 6, 3);
    const int overlayY = pauseControl && pauseControl->isVisible() ? 36 : 8;
    const QRect overlayRect(width() - textRect.width() - 8,
                            overlayY,
                            textRect.width(),
                            textRect.height());
    painter.fillRect(overlayRect, QColor(0, 0, 0, 180));
    painter.setPen(QColor(120, 255, 150));
    painter.drawText(overlayRect, Qt::AlignCenter, text);
}

void MyGraphWidget::drawExtendedInfoOverlay(QPainter &painter) const {
    if (!extendedInfoOverlayEnabled || !hoverCursorVisible) {
        return;
    }

    const CursorPeak peak = cursorPeakAtX(hoverCursorPos.x());
    const int dataCount = std::min({fftLength,
                                    static_cast<int>(xData.size()),
                                    static_cast<int>(yData.size())});
    if (!peak.valid || peak.dataIndex < 0 || peak.dataIndex >= dataCount) {
        return;
    }

    const double pointResolutionHz = std::abs(xMax - xMin) /
                                     static_cast<double>((std::max)(1, dataCount));
    const int localRadius = 5;
    const int localFirst = (std::max)(0, peak.dataIndex - localRadius);
    const int localLast = (std::min)(dataCount - 1, peak.dataIndex + localRadius);
    float localMinimum = std::numeric_limits<float>::infinity();
    float localMaximum = -std::numeric_limits<float>::infinity();
    double localSum = 0.0;
    int localCount = 0;
    for (int index = localFirst; index <= localLast; ++index) {
        const float level = displayLevelAt(yData, index, dataCount, dataDisplayOrdered);
        if (!std::isfinite(level)) {
            continue;
        }
        localMinimum = (std::min)(localMinimum, level);
        localMaximum = (std::max)(localMaximum, level);
        localSum += level;
        ++localCount;
    }

    QStringList lines;
    lines << QStringLiteral("f       %1").arg(formatFrequencyLabel(peak.frequency));
    lines << QStringLiteral("level   %1 dBFS%2")
                 .arg(peak.level, 0, 'f', 1)
                 .arg(peak.fromOverlay ? QStringLiteral("  overlay") : QString());
    if (std::isfinite(metadataCenterFrequencyHz)) {
        lines << QStringLiteral("center  %1")
                     .arg(formatFrequencyLabel(metadataCenterFrequencyHz));
    }
    if (std::isfinite(metadataListeningFrequencyHz)) {
        lines << QStringLiteral("listen  %1")
                     .arg(formatFrequencyLabel(metadataListeningFrequencyHz));
    }
    if (std::isfinite(metadataSampleRateHz) && metadataSampleRateHz > 0.0) {
        lines << QStringLiteral("SR      %1")
                     .arg(formatFrequencySpanLabel(metadataSampleRateHz));
    }
    if (metadataFftLength > 0) {
        const double rawBinWidthHz = metadataSampleRateHz > 0.0
                                         ? metadataSampleRateHz / metadataFftLength
                                         : 0.0;
        const double resolutionBandwidthHz =
            rawBinWidthHz * fftWindowEnbwBins(metadataFftWindowType);
        lines << QStringLiteral("FFT     %1").arg(metadataFftLength);
        lines << QStringLiteral("window  %1")
                     .arg(QString::fromLatin1(fftWindowTypeName(metadataFftWindowType)));
        if (rawBinWidthHz > 0.0 && std::isfinite(rawBinWidthHz)) {
            lines << QStringLiteral("RBW     %1")
                         .arg(formatFrequencySpanLabel(resolutionBandwidthHz));
            lines << QStringLiteral("bin     %1")
                         .arg(formatFrequencySpanLabel(rawBinWidthHz));
        }
    }
    if (tuningMarkerVisible && std::isfinite(tuningMarkerFrequencyHz)) {
        lines << QStringLiteral("dTune   %1")
                     .arg(formatFrequencySpanLabel(peak.frequency - tuningMarkerFrequencyHz));
    }
    lines << QStringLiteral("point   %1 / %2")
                 .arg(peak.dataIndex + 1)
                 .arg(dataCount);
    lines << QStringLiteral("display %1 / pt")
                 .arg(formatFrequencySpanLabel(pointResolutionHz));
    if (localCount > 0) {
        lines << QStringLiteral("local   %1 / %2 / %3 dBFS")
                     .arg(localMinimum, 0, 'f', 1)
                     .arg(localSum / localCount, 0, 'f', 1)
                     .arg(localMaximum, 0, 'f', 1);
    }
    if (overlayEnabled && static_cast<int>(overlayYData.size()) >= dataCount) {
        const float overlayLevel = displayLevelAt(overlayYData,
                                                  peak.dataIndex,
                                                  dataCount,
                                                  overlayDisplayOrdered);
        if (std::isfinite(overlayLevel)) {
            lines << QStringLiteral("overlay %1 dBFS").arg(overlayLevel, 0, 'f', 1);
        }
    }

    const QString text = lines.join(QLatin1Char('\n'));
    const QFontMetrics metrics(painter.font());
    const QRect textBounds = metrics.boundingRect(QRect(0, 0, width(), height()),
                                                   Qt::AlignLeft | Qt::AlignTop,
                                                   text);
    const int panelWidth = textBounds.width() + 14;
    const int panelHeight = textBounds.height() + 10;
    int panelY = 8;
    if (fpsOverlayEnabled) {
        const QString fpsText = displayedFps > 0.0
                                    ? QStringLiteral("FPS %1").arg(displayedFps, 0, 'f', 1)
                                    : QStringLiteral("FPS --");
        panelY += metrics.boundingRect(fpsText).adjusted(-6, -3, 6, 3).height() + 4;
    }
    const QRect panelRect((std::max)(8, width() - panelWidth - 8),
                          panelY,
                          panelWidth,
                          panelHeight);
    painter.fillRect(panelRect, QColor(0, 0, 0, 190));
    painter.setPen(QColor(255, 224, 140, 180));
    painter.drawRect(panelRect.adjusted(0, 0, -1, -1));
    painter.setPen(QColor(235, 245, 235));
    painter.drawText(panelRect.adjusted(7, 5, -7, -5),
                     Qt::AlignLeft | Qt::AlignTop,
                     text);
}

void MyGraphWidget::wheelEvent(QWheelEvent *event) {
    emit scaleChanged(event->angleDelta().y() > 0 ? 1 : -1);
    event->accept();
}

void MyGraphWidget::mouseMoveEvent(QMouseEvent *event) {
    hoverCursorVisible = true;
    hoverCursorPos = event->pos();
    if (spectrumPanActive) {
        const int deltaPixels = event->pos().x() - spectrumPanLastPos.x();
        if (deltaPixels != 0) {
            spectrumPanMoved = true;
            spectrumPanLastPos = event->pos();
            emit panRequested(deltaPixels, width());
        }
        event->accept();
        return;
    }
    if (bandwidthMeasurementActive) {
        bandwidthMeasurementVisible = true;
        bandwidthMeasureEndPos = event->pos();
        update();
        event->accept();
        return;
    }
    update();
    QOpenGLWidget::mouseMoveEvent(event);
}

void MyGraphWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            emit scienceMarkerRequested(frequencyAtX(event->x()));
            event->accept();
            return;
        }
        multiVfoSelectionActive = event->modifiers().testFlag(Qt::ControlModifier);
        bandwidthMeasurementActive = true;
        bandwidthMeasurementVisible = true;
        bandwidthMeasureStartPos = event->pos();
        bandwidthMeasureEndPos = event->pos();
        hoverCursorVisible = true;
        hoverCursorPos = event->pos();
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        emit tuneContextRequested(frequencyAtX(event->x()), event->globalPos());
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        spectrumPanActive = true;
        spectrumPanMoved = false;
        spectrumPanLastPos = event->pos();
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void MyGraphWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && bandwidthMeasurementActive) {
        bandwidthMeasureEndPos = event->pos();
        bandwidthMeasurementActive = false;
        if (std::abs(bandwidthMeasureEndPos.x() - bandwidthMeasureStartPos.x()) < 4) {
            bandwidthMeasurementVisible = false;
            if (multiVfoSelectionActive) {
                const double frequency = signalCenterNearFrequency(frequencyAtX(event->x()));
                emit multiVfoSelectionRequested(frequency, frequency);
            }
        } else {
            double lowHz = 0.0;
            double highHz = 0.0;
            if (bandwidthSelection(lowHz, highHz)) {
                if (multiVfoSelectionActive) emit multiVfoSelectionRequested(lowHz, highHz);
                else emit bandwidthSelectionChanged(lowHz, highHz);
            }
        }
        multiVfoSelectionActive = false;
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton && spectrumPanActive) {
        spectrumPanActive = false;
        if (!spectrumPanMoved) {
            emit autoTuneRequested(signalCenterNearFrequency(frequencyAtX(event->x())));
        }
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void MyGraphWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        bandwidthMeasurementActive = false;
        bandwidthMeasurementVisible = false;
        emit autoTuneRequested(signalCenterNearFrequency(frequencyAtX(event->x())));
        update();
        event->accept();
        return;
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void MyGraphWidget::leaveEvent(QEvent *event) {
    hoverCursorVisible = false;
    update();
    QOpenGLWidget::leaveEvent(event);
}

double MyGraphWidget::displayFrequencyAtX(int x) const {
    if (width() <= 0 || qFuzzyCompare(xMin, xMax)) {
        return xMin;
    }
    const double plotLeft = GRAPH_LEFT_MARGIN;
    const double plotWidth = (std::max)(1, width() - GRAPH_LEFT_MARGIN - GRAPH_RIGHT_MARGIN);
    const double normalized = std::clamp((static_cast<double>(x) - plotLeft) / plotWidth, 0.0, 1.0);
    return xMin + normalized * (xMax - xMin);
}

double MyGraphWidget::actualFrequencyForDisplayFrequency(double displayFrequency) const {
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

double MyGraphWidget::displayFrequencyForActualFrequency(double actualFrequency) const {
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

double MyGraphWidget::frequencyAtX(int x) const {
    return actualFrequencyForDisplayFrequency(displayFrequencyAtX(x));
}

int MyGraphWidget::xForFrequency(double frequency) const {
    if (!std::isfinite(frequency) || width() <= 0 || qFuzzyCompare(xMin, xMax)) {
        return GRAPH_LEFT_MARGIN;
    }
    const double plotLeft = GRAPH_LEFT_MARGIN;
    const double plotWidth = (std::max)(1, width() - GRAPH_LEFT_MARGIN - GRAPH_RIGHT_MARGIN);
    const double normalized = std::clamp((frequency - xMin) / (xMax - xMin), 0.0, 1.0);
    return static_cast<int>(std::round(plotLeft + normalized * plotWidth));
}

MyGraphWidget::CursorPeak MyGraphWidget::cursorPeakAtX(int x) const {
    CursorPeak peak;
    if (width() <= 0 || height() <= 0 || qFuzzyCompare(xMin, xMax) ||
        xData.empty() || yData.empty() || fftLength <= 0) {
        return peak;
    }

    const int dataCount = std::min({fftLength, static_cast<int>(xData.size()), static_cast<int>(yData.size())});
    if (dataCount <= 0) {
        return peak;
    }

    const double cursorDisplayFrequency = displayFrequencyAtX(x);
    const double plotWidth = (std::max)(1, width() - GRAPH_LEFT_MARGIN - GRAPH_RIGHT_MARGIN);
    const double hzPerPixel = std::abs(xMax - xMin) / plotWidth;
    const double binSpanHz = std::abs(xMax - xMin) / (std::max)(1, dataCount);
    const double halfWindowHz = (std::max)(hzPerPixel * 5.0, binSpanHz * 2.0);

    auto considerLevel = [&](int index,
                             double displayFrequency,
                             float level,
                             bool fromOverlay) {
        if (!std::isfinite(displayFrequency) || !std::isfinite(level) ||
            std::abs(displayFrequency - cursorDisplayFrequency) > halfWindowHz) {
            return;
        }
        if (!peak.valid || level > peak.level) {
            peak.valid = true;
            peak.displayFrequency = displayFrequency;
            peak.frequency = actualFrequencyForDisplayFrequency(displayFrequency);
            peak.level = level;
            peak.dataIndex = index;
            peak.fromOverlay = fromOverlay;
        }
    };

    for (int i = 0; i < dataCount; ++i) {
        const double frequency = displayFrequencyAt(i, dataCount);
        const float level = displayLevelAt(yData, i, dataCount, dataDisplayOrdered);
        considerLevel(i, frequency, level, false);
        if (overlayEnabled && static_cast<int>(overlayYData.size()) >= dataCount) {
            const float overlayLevel = displayLevelAt(overlayYData, i, dataCount, overlayDisplayOrdered);
            if (overlayLevel > yMin + 0.5f) {
                considerLevel(i, frequency, overlayLevel, true);
            }
        }
    }

    if (!peak.valid) {
        double bestDistance = std::numeric_limits<double>::infinity();
        for (int i = 0; i < dataCount; ++i) {
            const double frequency = displayFrequencyAt(i, dataCount);
            const float level = displayLevelAt(yData, i, dataCount, dataDisplayOrdered);
            if (!std::isfinite(frequency) || !std::isfinite(level)) {
                continue;
            }
            const double distance = std::abs(frequency - cursorDisplayFrequency);
            if (distance < bestDistance) {
                bestDistance = distance;
                peak.valid = true;
                peak.displayFrequency = frequency;
                peak.frequency = actualFrequencyForDisplayFrequency(frequency);
                peak.level = level;
                peak.dataIndex = i;
                peak.fromOverlay = false;
            }
        }
    }

    if (!peak.valid) {
        return peak;
    }

    const int plotTop = GRAPH_TOP_MARGIN;
    const int plotBottom = (std::max)(plotTop + 1, height() - bottomMargin());
    const int plotHeight = plotBottom - plotTop;
    peak.x = xForFrequency(peak.displayFrequency);
    peak.y = plotBottom - static_cast<int>(std::round(normalizedLevel(peak.level) * plotHeight));
    peak.y = (std::clamp)(peak.y, plotTop, plotBottom);
    return peak;
}

QString MyGraphWidget::formatFrequencyLabel(double frequencyHz) const {
    const double absFrequency = std::abs(frequencyHz);
    if (absFrequency >= 1000000000.0) {
        return QStringLiteral("%1 GHz").arg(frequencyHz / 1000000000.0, 0, 'f', 6);
    }
    if (absFrequency >= 1000000.0) {
        return QStringLiteral("%1 MHz").arg(frequencyHz / 1000000.0, 0, 'f', 6);
    }
    if (absFrequency >= 1000.0) {
        return QStringLiteral("%1 kHz").arg(frequencyHz / 1000.0, 0, 'f', 3);
    }
    return QStringLiteral("%1 Hz").arg(frequencyHz, 0, 'f', 0);
}

QString MyGraphWidget::formatFrequencySpanLabel(double spanHz) const {
    const double absSpan = std::abs(spanHz);
    if (absSpan >= 1000000000.0) {
        return QStringLiteral("%1 GHz").arg(absSpan / 1000000000.0, 0, 'f', 6);
    }
    if (absSpan >= 1000000.0) {
        return QStringLiteral("%1 MHz").arg(absSpan / 1000000.0, 0, 'f', 3);
    }
    if (absSpan >= 1000.0) {
        return QStringLiteral("%1 kHz").arg(absSpan / 1000.0, 0, 'f', 3);
    }
    return QStringLiteral("%1 Hz").arg(absSpan, 0, 'f', 0);
}

double MyGraphWidget::signalCenterNearFrequency(double frequency) const {
    if (!std::isfinite(frequency) || qFuzzyCompare(xMin, xMax) || xData.empty() || yData.empty() || fftLength <= 0) {
        return frequency;
    }

    const int dataCount = std::min({fftLength, static_cast<int>(xData.size()), static_cast<int>(yData.size())});
    if (dataCount <= 0) {
        return frequency;
    }

    const double targetDisplayFrequency = displayFrequencyForActualFrequency(frequency);
    const double visibleSpan = std::abs(xMax - xMin);
    const double halfWindow = (std::clamp)(visibleSpan * AUTO_TUNE_WINDOW_FRACTION,
                                           AUTO_TUNE_MIN_WINDOW_HZ,
                                           AUTO_TUNE_MAX_WINDOW_HZ);
    std::vector<SignalSample> samples;
    samples.reserve(256);
    for (int i = 0; i < dataCount; ++i) {
        const double sampleFrequency = displayFrequencyAt(i, dataCount);
        if (!std::isfinite(sampleFrequency) ||
            std::abs(sampleFrequency - targetDisplayFrequency) > halfWindow) {
            continue;
        }
        const float level = displayLevelAt(yData, i, dataCount, dataDisplayOrdered);
        if (!std::isfinite(level)) {
            continue;
        }
        samples.push_back({sampleFrequency, level});
    }
    if (samples.empty()) {
        return frequency;
    }

    std::sort(samples.begin(), samples.end(), [](const SignalSample &a, const SignalSample &b) {
        return a.frequency < b.frequency;
    });

    const int sampleCount = static_cast<int>(samples.size());
    const int smoothRadius = (std::clamp)(sampleCount / 160, 1, 6);
    std::vector<float> smoothedLevels(static_cast<std::size_t>(sampleCount), -160.0f);
    std::vector<float> sortedSmoothedLevels;
    sortedSmoothedLevels.reserve(static_cast<std::size_t>(sampleCount));
    for (int i = 0; i < sampleCount; ++i) {
        double sum = 0.0;
        int count = 0;
        for (int j = (std::max)(0, i - smoothRadius);
             j <= (std::min)(sampleCount - 1, i + smoothRadius);
             ++j) {
            sum += samples[j].level;
            ++count;
        }
        smoothedLevels[static_cast<std::size_t>(i)] =
            count > 0 ? static_cast<float>(sum / count) : samples[i].level;
        sortedSmoothedLevels.push_back(smoothedLevels[static_cast<std::size_t>(i)]);
    }
    std::sort(sortedSmoothedLevels.begin(), sortedSmoothedLevels.end());
    const float baseline = sortedSmoothedLevels[sortedSmoothedLevels.size() / 4];

    int peakIndex = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < sampleCount; ++i) {
        const double distanceRatio = std::abs(samples[i].frequency - targetDisplayFrequency) /
                                     (std::max)(1.0, halfWindow);
        const double score = smoothedLevels[static_cast<std::size_t>(i)] - distanceRatio * 8.0;
        if (score > bestScore) {
            bestScore = score;
            peakIndex = i;
        }
    }
    const float peakLevel = smoothedLevels[static_cast<std::size_t>(peakIndex)];
    const double dynamicRange = (std::max)(0.0, static_cast<double>(peakLevel - baseline));
    if (dynamicRange < 2.0) {
        return samples[peakIndex].frequency;
    }

    const float threshold = (std::max)(baseline + 3.0f,
                                       peakLevel - static_cast<float>((std::clamp)(dynamicRange * 0.42, 4.0, 10.0)));
    int left = peakIndex;
    while (left > 0 && smoothedLevels[static_cast<std::size_t>(left - 1)] >= threshold) {
        --left;
    }
    int right = peakIndex;
    while (right + 1 < sampleCount && smoothedLevels[static_cast<std::size_t>(right + 1)] >= threshold) {
        ++right;
    }

    if (right <= left) {
        return samples[peakIndex].frequency;
    }

    auto interpolatedEdge = [&](int inside, int outside) {
        if (outside < 0 || outside >= sampleCount) {
            return samples[inside].frequency;
        }
        const double insideLevel = smoothedLevels[static_cast<std::size_t>(inside)];
        const double outsideLevel = smoothedLevels[static_cast<std::size_t>(outside)];
        const double denom = insideLevel - outsideLevel;
        if (std::abs(denom) < 0.0001) {
            return samples[inside].frequency;
        }
        const double ratio = (insideLevel - threshold) / denom;
        return samples[inside].frequency +
               (samples[outside].frequency - samples[inside].frequency) *
                   (std::clamp)(ratio, 0.0, 1.0);
    };

    const double leftFrequency = interpolatedEdge(left, left - 1);
    const double rightFrequency = interpolatedEdge(right, right + 1);
    const double centerFrequency = (leftFrequency + rightFrequency) * 0.5;
    const double actualCenterFrequency = actualFrequencyForDisplayFrequency(centerFrequency);
    return std::isfinite(actualCenterFrequency)
               ? actualCenterFrequency
               : actualFrequencyForDisplayFrequency(samples[peakIndex].frequency);
}

QColor MyGraphWidget::valueToColor(float value) {
    if (!std::isfinite(value)) {
        value = 0.0f;
    }
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

float MyGraphWidget::normalizedLevel(float value) const {
    if (!std::isfinite(value) || qFuzzyCompare(yMin, yMax)) {
        return 0.0f;
    }
    return qBound(0.0f, static_cast<float>((value - yMin) / (yMax - yMin)), 1.0f);
}

float MyGraphWidget::displayLevelAt(const std::vector<float> &levels,
                                    int index,
                                    int count,
                                    bool ordered) const {
    if (index < 0 || count <= 0 || levels.empty()) {
        return yMin;
    }
    const int sourceIndex = ordered ? index : (index + count / 2) % count;
    if (sourceIndex < 0 || sourceIndex >= static_cast<int>(levels.size())) {
        return yMin;
    }
    return levels[static_cast<std::size_t>(sourceIndex)];
}

double MyGraphWidget::displayFrequencyAt(int index, int count) const {
    if (index < 0 || count <= 0 || index >= count) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (dataDisplayOrdered && std::isfinite(xMin) && std::isfinite(xMax) && xMax > xMin) {
        return xMin + (static_cast<double>(index) + 0.5) *
                          (xMax - xMin) / static_cast<double>(count);
    }
    if (index >= static_cast<int>(xData.size())) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return static_cast<double>(xData[static_cast<std::size_t>(index)]);
}

int MyGraphWidget::bottomMargin() const {
    int margin = frequencyAxisLabelsVisible ? 34 : GRAPH_BOTTOM_MARGIN;
    if (compactBandMarkersEnabled && (generalBandMarkersEnabled || amateurBandMarkersEnabled)) {
        margin = (std::max)(margin, GRAPH_COMPACT_BAND_BOTTOM_MARGIN);
    }
    if (scanSegmentMarkersVisible && !scanSegments.isEmpty()) {
        margin = (std::max)(margin, GRAPH_SCAN_SEGMENT_BOTTOM_MARGIN);
    }
    return margin;
}

void MyGraphWidget::drawXAxis(QPainter &painter) const {
    if (!frequencyAxisLabelsVisible || width() <= 0 || height() <= 0 ||
        !std::isfinite(xMin) || !std::isfinite(xMax) || qFuzzyCompare(xMin, xMax)) {
        return;
    }

    const int plotLeft = GRAPH_LEFT_MARGIN;
    const int plotRight = (std::max)(plotLeft + 1, width() - GRAPH_RIGHT_MARGIN);
    const int plotTop = GRAPH_TOP_MARGIN;
    const int plotBottom = (std::max)(GRAPH_TOP_MARGIN + 1, height() - bottomMargin());
    const int divisions = width() >= 1600 ? 18 : (width() >= 1100 ? 14 : 10);
    const double maximumAbsoluteFrequency = (std::max)(std::abs(xMin), std::abs(xMax));
    double unitScale = 1.0;
    QString unitSuffix = QStringLiteral(" Hz");
    if (maximumAbsoluteFrequency >= 1.0e9) {
        unitScale = 1.0e9;
        unitSuffix = QStringLiteral("G");
    } else if (maximumAbsoluteFrequency >= 1.0e6) {
        unitScale = 1.0e6;
        unitSuffix = QStringLiteral("M");
    } else if (maximumAbsoluteFrequency >= 1.0e3) {
        unitScale = 1.0e3;
        unitSuffix = QStringLiteral("k");
    }
    const double stepInUnits = std::abs(xMax - xMin) /
                               static_cast<double>(divisions) / unitScale;
    const int decimals = stepInUnits >= 10.0 ? 1 : (stepInUnits >= 1.0 ? 2 : 3);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QFontMetrics metrics = painter.fontMetrics();
    for (int tick = 0; tick <= divisions; ++tick) {
        const double ratio = static_cast<double>(tick) / divisions;
        const int x = plotLeft + static_cast<int>(std::lround(
                                     ratio * static_cast<double>(plotRight - plotLeft)));
        const double displayFrequency = xMin + ratio * (xMax - xMin);
        const double actualFrequency = actualFrequencyForDisplayFrequency(displayFrequency);
        const QString label = QStringLiteral("%1%2")
                                  .arg(actualFrequency / unitScale, 0, 'f', decimals)
                                  .arg(unitSuffix);
        const int textWidth = metrics.horizontalAdvance(label);
        int textX = x - textWidth / 2;
        if (tick == 0) {
            textX = plotLeft + 3;
        } else if (tick == divisions) {
            textX = plotRight - textWidth - 3;
        }
        painter.setPen(QColor(80, 110, 98, 190));
        painter.drawLine(x, plotTop, x, plotBottom + 5);
        painter.setPen(QColor(195, 220, 205));
        painter.drawText(textX,
                         plotBottom + 7,
                         textWidth + 2,
                         metrics.height() + 2,
                         Qt::AlignHCenter | Qt::AlignTop,
                         label);
    }
    painter.restore();
}

void MyGraphWidget::drawScanSegments(QPainter &painter) const {
    if (!scanSegmentMarkersVisible ||
        scanSegments.size() < 2 ||
        width() <= 0 ||
        height() <= 0 ||
        qFuzzyCompare(xMin, xMax)) {
        return;
    }

    const int markerTop = (std::max)(GRAPH_TOP_MARGIN + 1, height() - bottomMargin());
    const int markerBottom = height();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    QPen edgePen(QColor(180, 215, 255, 190));
    edgePen.setWidth(1);
    painter.setPen(edgePen);
    for (const ScanVisualSegment &segment : scanSegments) {
        if (!std::isfinite(segment.startHz) || !std::isfinite(segment.endHz)) {
            continue;
        }
        const int left = xForFrequency(segment.startHz);
        const int right = xForFrequency(segment.endHz);
        if (right < 0 || left > width() || right <= left) {
            continue;
        }
        const QRect segmentRect(left,
                                markerTop,
                                right - left,
                                (std::max)(1, markerBottom - markerTop));
        painter.fillRect(segmentRect, QColor(50, 130, 220, 72));
        painter.drawLine(left, markerTop, left, markerBottom);
        painter.drawLine(right, markerTop, right, markerBottom);
        if (right - left >= 52) {
            const QRect labelRect(left + 3, markerTop, right - left - 6, markerBottom - markerTop);
            painter.setPen(QColor(235, 246, 255, 230));
            painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, segment.label);
            painter.setPen(edgePen);
        }
    }
    painter.restore();
}

void MyGraphWidget::drawBandMarkers(QPainter &painter) const {
    if (bandMarkers.isEmpty() ||
        (!generalBandMarkersEnabled && !amateurBandMarkersEnabled) ||
        width() <= 0 ||
        qFuzzyCompare(xMin, xMax)) {
        return;
    }

    const int plotLeft = GRAPH_LEFT_MARGIN;
    const int plotRight = (std::max)(plotLeft + 1, width() - GRAPH_RIGHT_MARGIN);
    const int plotTop = GRAPH_TOP_MARGIN;
    const int plotBottom = (std::max)(plotTop + 1, height() - bottomMargin());
    const int markerTop = compactBandMarkersEnabled ? plotBottom : plotTop;
    const int markerBottom = compactBandMarkersEnabled ? height() : plotBottom;
    const int markerHeight = (std::max)(1, markerBottom - markerTop);
    const double viewStart = (std::min)(xMin, xMax);
    const double viewEnd = (std::max)(xMin, xMax);
    const double span = xMax - xMin;
    const double plotWidth = static_cast<double>((std::max)(1, plotRight - plotLeft));

    auto frequencyToX = [&](double frequency) {
        return plotLeft + ((frequency - xMin) / span) * plotWidth;
    };

    auto drawLayer = [&](bool amateurLayer) {
        if (amateurLayer && !amateurBandMarkersEnabled) {
            return;
        }
        if (!amateurLayer && !generalBandMarkersEnabled) {
            return;
        }

        const QColor fillColor = compactBandMarkersEnabled
                                     ? (amateurLayer ? QColor(255, 198, 66, 128)
                                                     : QColor(76, 162, 255, 112))
                                     : (amateurLayer ? QColor(255, 198, 66, 42)
                                                     : QColor(76, 162, 255, 32));
        const QColor edgeColor = compactBandMarkersEnabled
                                     ? (amateurLayer ? QColor(255, 220, 96, 220)
                                                     : QColor(112, 196, 255, 200))
                                     : (amateurLayer ? QColor(255, 220, 96, 110)
                                                     : QColor(112, 196, 255, 90));
        const QColor textColor = compactBandMarkersEnabled
                                     ? (amateurLayer ? QColor(255, 236, 170, 245)
                                                     : QColor(210, 236, 255, 235))
                                     : (amateurLayer ? QColor(255, 232, 150, 210)
                                                     : QColor(176, 224, 255, 190));

        for (const GraphBandMarker &marker : bandMarkers) {
            if (marker.amateur != amateurLayer ||
                !std::isfinite(marker.startHz) ||
                !std::isfinite(marker.endHz) ||
                marker.endHz <= marker.startHz ||
                marker.endHz < viewStart ||
                marker.startHz > viewEnd) {
                continue;
            }

            const double clippedStart = (std::max)(marker.startHz, viewStart);
            const double clippedEnd = (std::min)(marker.endHz, viewEnd);
            int x1 = static_cast<int>(std::floor(frequencyToX(clippedStart)));
            int x2 = static_cast<int>(std::ceil(frequencyToX(clippedEnd)));
            if (x2 < x1) {
                std::swap(x1, x2);
            }
            x1 = (std::clamp)(x1, plotLeft, plotRight);
            x2 = (std::clamp)(x2, plotLeft, plotRight);
            const int markerWidth = (std::max)(1, x2 - x1);

            painter.fillRect(QRect(x1, markerTop, markerWidth, markerHeight), fillColor);
            painter.setPen(edgeColor);
            painter.drawLine(x1, markerTop, x1, markerBottom);
            if (markerWidth > 2) {
                painter.drawLine(x1 + markerWidth, markerTop, x1 + markerWidth, markerBottom);
            }

            if (markerWidth >= 44 && !marker.label.trimmed().isEmpty()) {
                const int textY = compactBandMarkersEnabled
                                      ? markerTop + (std::max)(2, (markerHeight - 15) / 2)
                                      : markerTop + 2;
                const QRect textRect(x1 + 3, textY, markerWidth - 6, 15);
                const QString label = painter.fontMetrics().elidedText(marker.label.trimmed(),
                                                                       Qt::ElideRight,
                                                                       textRect.width());
                painter.setPen(textColor);
                painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, label);
            }
        }
    };

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    if (compactBandMarkersEnabled) {
        painter.fillRect(QRect(plotLeft, markerTop, plotRight - plotLeft, markerHeight), QColor(0, 0, 0, 210));
        painter.setPen(QColor(80, 96, 92, 160));
        painter.drawLine(plotLeft, markerTop, plotRight, markerTop);
    }
    painter.setClipRect(QRect(plotLeft, markerTop, plotRight - plotLeft, markerHeight));
    drawLayer(false);
    drawLayer(true);
    painter.restore();
}

void MyGraphWidget::drawBandwidthMeasurement(QPainter &painter) const {
    if (!bandwidthMeasurementVisible || qFuzzyCompare(xMin, xMax)) {
        return;
    }

    const int plotLeft = GRAPH_LEFT_MARGIN;
    const int plotRight = (std::max)(plotLeft + 1, width() - GRAPH_RIGHT_MARGIN);
    const int plotTop = GRAPH_TOP_MARGIN;
    const int plotBottom = (std::max)(plotTop + 1, height() - bottomMargin());
    int x1 = (std::clamp)(bandwidthMeasureStartPos.x(), plotLeft, plotRight);
    int x2 = (std::clamp)(bandwidthMeasureEndPos.x(), plotLeft, plotRight);
    if (std::abs(x2 - x1) < 4) {
        return;
    }
    if (x2 < x1) {
        std::swap(x1, x2);
    }

    const double f1 = frequencyAtX(x1);
    const double f2 = frequencyAtX(x2);
    const double lowFrequency = (std::min)(f1, f2);
    const double highFrequency = (std::max)(f1, f2);
    const double spanHz = highFrequency - lowFrequency;
    const QRect selectionRect(x1, plotTop, x2 - x1, plotBottom - plotTop);
    const QString label = QStringLiteral("BW %1  %2 - %3")
                              .arg(formatFrequencySpanLabel(spanHz))
                              .arg(formatFrequencyLabel(lowFrequency))
                              .arg(formatFrequencyLabel(highFrequency));

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(selectionRect, QColor(70, 135, 255, 48));
    painter.setPen(QPen(QColor(105, 175, 255, 210), 1, Qt::SolidLine));
    painter.drawRect(selectionRect.adjusted(0, 0, -1, -1));
    painter.setPen(QPen(QColor(180, 225, 255, 230), 1, Qt::DashLine));
    painter.drawLine(x1, plotTop, x1, plotBottom);
    painter.drawLine(x2, plotTop, x2, plotBottom);

    painter.setRenderHint(QPainter::Antialiasing, true);
    const QFontMetrics metrics = painter.fontMetrics();
    const int paddingX = 8;
    const int labelWidth = metrics.horizontalAdvance(label) + paddingX * 2;
    const int labelHeight = metrics.height() + 8;
    int labelX = x1 + ((x2 - x1) - labelWidth) / 2;
    labelX = (std::clamp)(labelX, plotLeft + 2, (std::max)(plotLeft + 2, plotRight - labelWidth - 2));
    int labelY = plotTop + 6;
    if (bandwidthMeasureStartPos.y() < plotTop + labelHeight + 16 ||
        bandwidthMeasureEndPos.y() < plotTop + labelHeight + 16) {
        labelY = plotBottom - labelHeight - 6;
    }
    labelY = (std::clamp)(labelY, plotTop + 2, (std::max)(plotTop + 2, plotBottom - labelHeight - 2));
    const QRect labelRect(labelX, labelY, labelWidth, labelHeight);
    painter.setPen(QColor(125, 190, 255, 190));
    painter.setBrush(QColor(4, 12, 24, 224));
    painter.drawRoundedRect(labelRect, 4, 4);
    painter.setPen(QColor(220, 238, 255, 245));
    painter.drawText(labelRect.adjusted(paddingX, 0, -paddingX, 0),
                     Qt::AlignVCenter | Qt::AlignLeft,
                     label);
    painter.restore();
}

void MyGraphWidget::drawHoverCursor(QPainter &painter) const {
    if (!hoverCursorVisible) {
        return;
    }

    const int plotLeft = GRAPH_LEFT_MARGIN;
    const int plotRight = (std::max)(plotLeft + 1, width() - GRAPH_RIGHT_MARGIN);
    const int plotTop = GRAPH_TOP_MARGIN;
    const int plotBottom = (std::max)(plotTop + 1, height() - bottomMargin());
    if (hoverCursorPos.x() < plotLeft ||
        hoverCursorPos.x() > plotRight ||
        hoverCursorPos.y() < plotTop ||
        hoverCursorPos.y() > plotBottom) {
        return;
    }

    const CursorPeak peak = cursorPeakAtX(hoverCursorPos.x());
    if (!peak.valid) {
        return;
    }

    const QColor lineColor(255, 236, 170, 210);
    const QColor markerColor(255, 214, 92, 240);
    const QColor textColor(238, 246, 230, 245);
    const QColor fillColor(5, 12, 10, 222);
    const QColor borderColor(255, 236, 170, 170);
    const QString label = QStringLiteral("%1  %2 dB")
                              .arg(formatFrequencyLabel(peak.frequency))
                              .arg(peak.level, 0, 'f', 1);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(lineColor, 1, Qt::SolidLine));
    painter.drawLine(peak.x, plotTop, peak.x, plotBottom);
    painter.drawLine((std::max)(plotLeft, peak.x - 4), peak.y,
                     (std::min)(plotRight, peak.x + 4), peak.y);

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(markerColor);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPoint(peak.x, peak.y), 3, 3);

    const QFontMetrics metrics = painter.fontMetrics();
    const int paddingX = 7;
    const int labelWidth = metrics.horizontalAdvance(label) + paddingX * 2;
    const int labelHeight = metrics.height() + 7;
    int labelX = peak.x + 8;
    if (labelX + labelWidth > plotRight - 2) {
        labelX = peak.x - labelWidth - 8;
    }
    labelX = (std::clamp)(labelX, plotLeft + 2, (std::max)(plotLeft + 2, plotRight - labelWidth - 2));
    int labelY = peak.y - labelHeight - 8;
    if (labelY < plotTop + 2) {
        labelY = peak.y + 8;
    }
    labelY = (std::clamp)(labelY, plotTop + 2, (std::max)(plotTop + 2, plotBottom - labelHeight - 2));
    const QRect labelRect(labelX, labelY, labelWidth, labelHeight);
    painter.setPen(borderColor);
    painter.setBrush(fillColor);
    painter.drawRoundedRect(labelRect, 4, 4);
    painter.setPen(textColor);
    painter.drawText(labelRect.adjusted(paddingX, 0, -paddingX, 0),
                     Qt::AlignVCenter | Qt::AlignLeft,
                     label);
    painter.restore();
}

void MyGraphWidget::drawYAxis(QPainter &painter) const {
    const int plotLeft = GRAPH_LEFT_MARGIN;
    const int plotRight = (std::max)(plotLeft + 1, width() - GRAPH_RIGHT_MARGIN);
    const int plotTop = GRAPH_TOP_MARGIN;
    const int plotBottom = (std::max)(plotTop + 1, height() - bottomMargin());
    const int plotHeight = plotBottom - plotTop;

    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(55, 80, 70, 170));
    painter.drawRect(plotLeft, plotTop, plotRight - plotLeft, plotHeight);

    for (int tick = 0; tick <= GRAPH_Y_TICKS; ++tick) {
        const double fraction = static_cast<double>(tick) / GRAPH_Y_TICKS;
        const double level = yMin + (yMax - yMin) * fraction;
        const int y = plotBottom - static_cast<int>(std::round(fraction * plotHeight));
        painter.setPen(QColor(45, 65, 58, 150));
        painter.drawLine(plotLeft, y, plotRight, y);
        painter.setPen(QColor(185, 210, 195));
        const QString label = QString::number(level, 'f', 0);
        painter.drawText(4, y + 4, label);
    }

    painter.setPen(QColor(185, 210, 195));
    if (frequencyAxisLabelsVisible) {
        painter.drawText(4, plotTop + painter.fontMetrics().height(), "dBFS");
    } else {
        painter.drawText(4, height() - 4, "dBFS");
    }
}

void MyGraphWidget::drawTuningMarker(QPainter &painter) const {
    if (!tuningMarkerVisible ||
        !std::isfinite(tuningMarkerFrequencyHz) ||
        width() <= 0 ||
        height() <= 0 ||
        qFuzzyCompare(xMin, xMax)) {
        return;
    }

    const int plotLeft = GRAPH_LEFT_MARGIN;
    const int plotRight = (std::max)(plotLeft + 1, width() - GRAPH_RIGHT_MARGIN);
    const int plotTop = GRAPH_TOP_MARGIN;
    const int plotBottom = (std::max)(plotTop + 1, height() - bottomMargin());
    const int x = (std::clamp)(xForFrequency(tuningMarkerFrequencyHz), plotLeft, plotRight);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(QColor(255, 72, 72, 230), 2));
    painter.drawLine(x, plotTop, x, plotBottom);
    painter.setPen(QColor(255, 220, 220, 240));
    const QString label = formatFrequencyLabel(tuningMarkerFrequencyHz);
    const QFontMetrics metrics = painter.fontMetrics();
    const int labelWidth = metrics.horizontalAdvance(label) + 10;
    const QRect labelRect((std::clamp)(x + 5, plotLeft + 2, (std::max)(plotLeft + 2, plotRight - labelWidth - 2)),
                          plotTop + 4,
                          labelWidth,
                          metrics.height() + 5);
    painter.fillRect(labelRect, QColor(30, 0, 0, 190));
    painter.drawRect(labelRect);
    painter.drawText(labelRect.adjusted(5, 0, -5, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
    painter.restore();
}
