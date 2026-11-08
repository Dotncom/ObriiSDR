#include "analyzerspectrumwidget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int kLeft = 66;
constexpr int kTop = 20;
constexpr int kRight = 18;
constexpr int kBottom = 42;
constexpr int kHorizontalDivisions = 10;
constexpr int kVerticalDivisions = 10;
constexpr int kHorizontalMinorDivisions = 5;
constexpr int kVerticalMinorDivisions = 2;

QString levelText(double db) {
    return QStringLiteral("%1%2").arg(db > 0.0 ? QStringLiteral("+") : QString(),
                                      QString::number(db, 'f', 1));
}
}

AnalyzerSpectrumWidget::AnalyzerSpectrumWidget(QWidget *parent) : QWidget(parent) {
    setMinimumSize(640, 400);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void AnalyzerSpectrumWidget::setFrame(const std::vector<float> &newFrequencies,
                                      const std::vector<float> &newLevels) {
    if (newFrequencies.size() != newLevels.size()) return;
    frequencies = newFrequencies;
    levels = newLevels;
    update();
}

void AnalyzerSpectrumWidget::setTraces(const std::vector<float> &newMax,
                                       const std::vector<float> &newMin,
                                       const std::vector<float> &newAverage,
                                       const std::vector<float> &newP50,
                                       const std::vector<float> &newP90,
                                       const std::vector<float> &newP99,
                                       bool showMax,
                                       bool showMin,
                                       bool showAverage,
                                       bool showP50,
                                       bool showP90,
                                       bool showP99,
                                       bool showCurrent) {
    if (showMax) maxHold = newMax; else maxHold.clear();
    if (showMin) minHold = newMin; else minHold.clear();
    if (showAverage) average = newAverage; else average.clear();
    if (showP50) percentile50 = newP50; else percentile50.clear();
    if (showP90) percentile90 = newP90; else percentile90.clear();
    if (showP99) percentile99 = newP99; else percentile99.clear();
    maxVisible = showMax;
    minVisible = showMin;
    averageVisible = showAverage;
    p50Visible = showP50;
    p90Visible = showP90;
    p99Visible = showP99;
    currentVisible = showCurrent;
    update();
}

void AnalyzerSpectrumWidget::setMarkers(const QVector<SpectrumScienceMarker> &newMarkers) {
    markers = newMarkers;
    update();
}

void AnalyzerSpectrumWidget::setAmplitudeRange(double referenceLevelDb, double dbPerDivision) {
    referenceLevel = std::clamp(referenceLevelDb, -200.0, 100.0);
    divisionDb = std::clamp(dbPerDivision, 0.1, 50.0);
    update();
}

void AnalyzerSpectrumWidget::setAmplitudeBounds(double topDb, double bottomDb) {
    const double top = std::clamp(std::max(topDb, bottomDb + 0.1), -199.9, 100.0);
    const double bottom = std::clamp(std::min(bottomDb, top - 0.1), -200.0, 99.9);
    setAmplitudeRange(top, (top - bottom) / kVerticalDivisions);
}

void AnalyzerSpectrumWidget::clear() {
    frequencies.clear();
    levels.clear();
    maxHold.clear();
    minHold.clear();
    average.clear();
    percentile50.clear();
    percentile90.clear();
    percentile99.clear();
    update();
}

void AnalyzerSpectrumWidget::setMeasurementDetails(const QString &details) {
    if (measurementDetails == details) return;
    measurementDetails = details;
    update();
}

QRect AnalyzerSpectrumWidget::plotRect() const {
    return rect().adjusted(kLeft, kTop, -kRight, -kBottom);
}

double AnalyzerSpectrumWidget::frequencyAtX(int x) const {
    if (frequencies.empty()) return 0.0;
    const QRect plot = plotRect();
    const double ratio = std::clamp((x - plot.left()) / static_cast<double>(std::max(1, plot.width())), 0.0, 1.0);
    return frequencies.front() + ratio * (frequencies.back() - frequencies.front());
}

double AnalyzerSpectrumWidget::levelAtY(int y) const {
    const QRect plot = plotRect();
    const double ratio = std::clamp((y - plot.top()) / static_cast<double>(std::max(1, plot.height())), 0.0, 1.0);
    return referenceLevel - ratio * divisionDb * kVerticalDivisions;
}

int AnalyzerSpectrumWidget::xForFrequency(double hz) const {
    if (frequencies.empty() || frequencies.back() == frequencies.front()) return plotRect().left();
    const double ratio = (hz - frequencies.front()) / (frequencies.back() - frequencies.front());
    return plotRect().left() + qRound(ratio * plotRect().width());
}

int AnalyzerSpectrumWidget::yForLevel(double db) const {
    const QRect plot = plotRect();
    if (!std::isfinite(db)) return plot.bottom();
    const double ratio = std::clamp(
        (referenceLevel - db) / (divisionDb * kVerticalDivisions), 0.0, 1.0);
    return plot.top() + qRound(ratio * plot.height());
}

QString AnalyzerSpectrumWidget::frequencyText(double hz) const {
    if (std::abs(hz) >= 1.0e9) return QStringLiteral("%1 GHz").arg(hz / 1.0e9, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e6) return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e3) return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 1);
}

void AnalyzerSpectrumWidget::drawTrace(QPainter &painter,
                                       const std::vector<float> &trace,
                                       const QColor &color,
                                       qreal width,
                                       bool preserveMinimum) const {
    if (trace.size() != frequencies.size() || trace.size() < 2) return;
    const QRect plot = plotRect();
    QPainterPath path;
    bool started = false;
    const int pixelCount = std::max(1, plot.width());
    for (int pixel = 0; pixel <= pixelCount; ++pixel) {
        const std::size_t first = std::min(trace.size() - 1,
            static_cast<std::size_t>((static_cast<quint64>(pixel) * trace.size()) /
                                     static_cast<quint64>(pixelCount + 1)));
        const std::size_t last = std::max(first + 1, std::min(trace.size(),
            static_cast<std::size_t>((static_cast<quint64>(pixel + 1) * trace.size()) /
                                     static_cast<quint64>(pixelCount + 1))));
        float selected = preserveMinimum ? std::numeric_limits<float>::infinity()
                                         : -std::numeric_limits<float>::infinity();
        for (std::size_t index = first; index < last; ++index) {
            if (!std::isfinite(trace[index])) continue;
            selected = preserveMinimum ? std::min(selected, trace[index])
                                       : std::max(selected, trace[index]);
        }
        if (!std::isfinite(selected)) {
            started = false;
            continue;
        }
        const QPointF point(plot.left() + pixel, yForLevel(selected));
        if (!started) { path.moveTo(point); started = true; }
        else path.lineTo(point);
    }
    painter.setPen(QPen(color, width));
    painter.drawPath(path);
}

void AnalyzerSpectrumWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(8, 11, 14));
    const QRect plot = plotRect();
    painter.fillRect(plot, QColor(2, 5, 7));

    const int horizontalLines = kHorizontalDivisions * kHorizontalMinorDivisions;
    for (int i = 0; i <= horizontalLines; ++i) {
        const bool major = (i % kHorizontalMinorDivisions) == 0;
        QPen pen(major ? QColor(45, 78, 81) : QColor(20, 38, 41));
        pen.setWidthF(major ? 1.25 : 0.45);
        pen.setCosmetic(true);
        painter.setPen(pen);
        const int x = plot.left() + qRound(i * plot.width() / static_cast<double>(horizontalLines));
        painter.drawLine(x, plot.top(), x, plot.bottom());
    }
    const int verticalLines = kVerticalDivisions * kVerticalMinorDivisions;
    for (int i = 0; i <= verticalLines; ++i) {
        const bool major = (i % kVerticalMinorDivisions) == 0;
        QPen pen(major ? QColor(45, 78, 81) : QColor(20, 38, 41));
        pen.setWidthF(major ? 1.25 : 0.45);
        pen.setCosmetic(true);
        painter.setPen(pen);
        const int y = plot.top() + qRound(i * plot.height() / static_cast<double>(verticalLines));
        painter.drawLine(plot.left(), y, plot.right(), y);
    }

    painter.setPen(QColor(154, 178, 177));
    const QFontMetrics metrics(font());
    for (int i = 0; i <= kVerticalDivisions; ++i) {
        const double db = referenceLevel - i * divisionDb;
        const int y = plot.top() + qRound(i * plot.height() / static_cast<double>(kVerticalDivisions));
        painter.drawText(QRect(2, y - metrics.height() / 2, kLeft - 8, metrics.height()),
                         Qt::AlignRight | Qt::AlignVCenter, levelText(db));
    }
    if (!frequencies.empty()) {
        for (int i = 0; i <= kHorizontalDivisions; i += 2) {
            const double hz = frequencies.front() + i * (frequencies.back() - frequencies.front()) / kHorizontalDivisions;
            const int x = plot.left() + qRound(i * plot.width() / static_cast<double>(kHorizontalDivisions));
            painter.drawText(QRect(x - 70, plot.bottom() + 7, 140, metrics.height()),
                             Qt::AlignHCenter | Qt::AlignTop, frequencyText(hz));
        }
    }

    painter.save();
    painter.setClipRect(plot.adjusted(0, 0, 1, 1));
    if (minVisible) drawTrace(painter, minHold, QColor(90, 150, 255, 180), 1.0, true);
    if (p50Visible) drawTrace(painter, percentile50, QColor(185, 130, 255, 150), 1.0);
    if (p90Visible) drawTrace(painter, percentile90, QColor(245, 120, 220, 170), 1.0);
    if (p99Visible) drawTrace(painter, percentile99, QColor(255, 145, 120, 190), 1.0);
    if (averageVisible) drawTrace(painter, average, QColor(255, 207, 70, 210), 1.2);
    if (maxVisible) drawTrace(painter, maxHold, QColor(255, 90, 72, 210), 1.0);
    if (currentVisible) drawTrace(painter, levels, QColor(78, 244, 158), 1.6);

    if (measuring) {
        const int left = std::clamp(std::min(dragStartPosition.x(), dragLastPosition.x()),
                                    plot.left(), plot.right());
        const int right = std::clamp(std::max(dragStartPosition.x(), dragLastPosition.x()),
                                     plot.left(), plot.right());
        const QRect selection(left, plot.top(), std::max(1, right - left), plot.height());
        painter.fillRect(selection, QColor(255, 210, 70, 35));
        painter.setPen(QPen(QColor(255, 215, 80), 1, Qt::DashLine));
        painter.drawLine(left, plot.top(), left, plot.bottom());
        painter.drawLine(right, plot.top(), right, plot.bottom());
    }

    for (int markerIndex = 0; markerIndex < markers.size(); ++markerIndex) {
        const auto &marker = markers.at(markerIndex);
        if (!marker.enabled) continue;
        const int x = xForFrequency(marker.frequencyHz);
        painter.setPen(QPen(QColor(255, 210, 70), 1, Qt::DashLine));
        painter.drawLine(x, plot.top(), x, plot.bottom());
        const QString label = QStringLiteral("%1  %2  %3 dB")
            .arg(marker.label, frequencyText(marker.frequencyHz))
            .arg(marker.levelDb, 0, 'f', 2);
        const QRect textRect(x + 5, plot.top() + 5 + 20 * markerIndex,
                             std::min(260, plot.right() - x - 8), 20);
        painter.fillRect(textRect, QColor(20, 22, 17, 220));
        painter.setPen(QColor(255, 220, 90));
        painter.drawText(textRect.adjusted(4, 0, -2, 0), Qt::AlignVCenter, label);
    }

    if (hoverVisible && plot.contains(hoverPosition)) {
        painter.setPen(QPen(QColor(210, 220, 225, 150), 1, Qt::DashLine));
        painter.drawLine(hoverPosition.x(), plot.top(), hoverPosition.x(), plot.bottom());
        painter.drawLine(plot.left(), hoverPosition.y(), plot.right(), hoverPosition.y());
        const QString text = QStringLiteral("%1   %2 dB")
            .arg(frequencyText(frequencyAtX(hoverPosition.x())))
            .arg(levelAtY(hoverPosition.y()), 0, 'f', 2);
        const QRect box(plot.left() + 8, plot.top() + 8, 260, 24);
        painter.fillRect(box, QColor(9, 18, 20, 225));
        painter.setPen(QColor(210, 230, 230));
        painter.drawText(box.adjusted(6, 0, -4, 0), Qt::AlignVCenter, text);
    }
    if (!measurementDetails.isEmpty()) {
        const int panelWidth = std::min(340, std::max(220, plot.width() / 3));
        const QRect textBounds = painter.fontMetrics().boundingRect(
            QRect(0, 0, panelWidth - 16, plot.height() - 16),
            Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, measurementDetails);
        const QRect panel(plot.right() - panelWidth - 8, plot.top() + 8,
                          panelWidth, std::min(plot.height() - 16, textBounds.height() + 12));
        painter.fillRect(panel, QColor(9, 18, 20, 225));
        painter.setPen(QColor(235, 221, 142));
        painter.drawText(panel.adjusted(7, 5, -7, -5),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         measurementDetails);
    }
    painter.restore();

    painter.setPen(QColor(85, 125, 126));
    painter.drawRect(plot);
    painter.setPen(QColor(154, 178, 177));
    painter.drawText(6, 15, QStringLiteral("dBFS"));
}

void AnalyzerSpectrumWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && plotRect().contains(event->pos()) && !frequencies.empty()) {
        dragging = true;
        dragMoved = false;
        measuring = event->modifiers().testFlag(Qt::ShiftModifier);
        dragStartPosition = event->pos();
        dragLastPosition = event->pos();
        setCursor(measuring ? Qt::CrossCursor : Qt::ClosedHandCursor);
    }
}

void AnalyzerSpectrumWidget::mouseMoveEvent(QMouseEvent *event) {
    if (dragging && !frequencies.empty()) {
        const QRect plot = plotRect();
        const int deltaPixels = event->pos().x() - dragLastPosition.x();
        if (std::abs(event->pos().x() - dragStartPosition.x()) >= QApplication::startDragDistance())
            dragMoved = true;
        if (measuring) {
            dragLastPosition.setX(std::clamp(event->pos().x(), plot.left(), plot.right()));
        } else if (dragMoved && deltaPixels != 0 && plot.width() > 1) {
            const double spanHz = frequencies.back() - frequencies.front();
            emit frequencyPanRequested(-deltaPixels * spanHz / static_cast<double>(plot.width()));
            dragLastPosition = event->pos();
        }
    }
    hoverPosition = event->pos();
    hoverVisible = plotRect().contains(hoverPosition);
    update();
}

void AnalyzerSpectrumWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && dragging) {
        if (measuring && dragMoved) {
            emit frequencyRangeSelected(frequencyAtX(dragStartPosition.x()),
                                        frequencyAtX(dragLastPosition.x()));
        } else if (!dragMoved && plotRect().contains(event->pos()) && !frequencies.empty()) {
            emit frequencyClicked(frequencyAtX(event->pos().x()));
        }
        dragging = false;
        dragMoved = false;
        measuring = false;
        unsetCursor();
        update();
    }
}

void AnalyzerSpectrumWidget::leaveEvent(QEvent *) {
    hoverVisible = false;
    update();
}
