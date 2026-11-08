#include "waterfall3dview.h"

#include "waterfall3drenderer.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
bool hasSliceModifier(Qt::KeyboardModifiers modifiers) {
    return modifiers.testFlag(Qt::AltModifier) ||
           modifiers.testFlag(Qt::ShiftModifier);
}
}

Waterfall3DView::Waterfall3DView(QWidget *parent)
    : QOpenGLWidget(parent),
      renderer(std::make_unique<Waterfall3DRenderer>()) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumHeight(300);
}

Waterfall3DView::~Waterfall3DView() = default;

void Waterfall3DView::clearHistory() {
    renderer->clear();
    update();
}

void Waterfall3DView::appendHistoryRow(const std::vector<float> &levels,
                                       const std::vector<unsigned char> &rgb,
                                       float levelMin,
                                       float levelMax) {
    renderer->appendRow(levels, rgb, levelMin, levelMax, 1);
}

void Waterfall3DView::setHistoryCapacity(int rows) {
    renderer->setHistoryCapacity(rows);
}

void Waterfall3DView::setHighlightedHistoryRow(int row) {
    renderer->setHighlightedHistoryRow(row);
    update();
}

void Waterfall3DView::setResolutionDivisor(int divisor) {
    renderer->setResolutionDivisor(divisor);
}

void Waterfall3DView::setSliceScrollStep(int points) {
    renderer->setSliceScrollStep(points);
}

void Waterfall3DView::setSliceWidth(int points) {
    renderer->setSliceWidth(points);
}

void Waterfall3DView::setSpectrumSliceScrollStep(int rows) {
    renderer->setSpectrumSliceScrollStep(rows);
}

void Waterfall3DView::setSpectrumSliceWidth(int rows) {
    renderer->setSpectrumSliceWidth(rows);
}

void Waterfall3DView::setSpectrumSliceCapture(bool enabled) {
    renderer->setSpectrumSliceCapture(enabled);
}

void Waterfall3DView::setSpectrumSliceCaptureFixed(bool enabled) {
    renderer->setSpectrumSliceCaptureFixed(enabled);
}

void Waterfall3DView::setModifierFreeSliceInput(bool enabled) {
    modifierFreeSliceInput = enabled;
}

void Waterfall3DView::setDensityAxes(bool enabled) {
    densityAxes = enabled;
    renderer->setDensityAxes(enabled);
    update();
}

void Waterfall3DView::setDensityAxisMapping(int xDimension, int yDimension) {
    renderer->setDensityAxisMapping(xDimension, yDimension);
    update();
}

void Waterfall3DView::setDensityFrontProfile(
    const std::vector<float> &normalizedLevels) {
    renderer->setDensityFrontProfile(normalizedLevels);
    update();
}

void Waterfall3DView::setDensityFrontProfileStyle(int style) {
    renderer->setDensityFrontProfileStyle(style);
    update();
}

void Waterfall3DView::setSurfaceStyle(int style) {
    renderer->setSurfaceStyle(style);
    update();
}

void Waterfall3DView::setSurfaceSmoothing(int smoothing) {
    renderer->setSurfaceSmoothing(smoothing);
    update();
}

void Waterfall3DView::setSurfaceLighting(int lighting) {
    renderer->setSurfaceLighting(lighting);
    update();
}

void Waterfall3DView::setDensityAxisLabels(const QString &frequency,
                                           const QString &density,
                                           const QString &level) {
    densityFrequencyLabel = frequency;
    densityAccumulationLabel = density;
    densityLevelLabel = level;
    update();
}

void Waterfall3DView::setDensityAxisRanges(double firstFrequencyHz,
                                           double lastFrequencyHz,
                                           double minimumDb,
                                           double maximumDb,
                                           double maximumDensity) {
    densityFirstFrequencyHz = firstFrequencyHz;
    densityLastFrequencyHz = lastFrequencyHz;
    densityMinimumDb = minimumDb;
    densityMaximumDb = maximumDb;
    densityMaximumValue = (std::max)(0.0, maximumDensity);
}

void Waterfall3DView::setOverview(const QImage &image,
                                  int windowStart,
                                  int windowEnd,
                                  int selectedRow,
                                  int totalRows) {
    if (overviewImage.cacheKey() != image.cacheKey()) {
        overviewImage = image;
        overviewThumbnail = QImage();
    }
    overviewWindowStart = windowStart;
    overviewWindowEnd = windowEnd;
    overviewSelectedRow = selectedRow;
    overviewTotalRows = totalRows;
    update();
}

void Waterfall3DView::setOverviewVisible(bool visible) {
    overviewVisible = visible;
    update();
}

void Waterfall3DView::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.025f, 0.03f, 0.04f, 1.0f);
}

void Waterfall3DView::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderer->render(width(), height());
    if (overviewVisible) {
        drawOverview();
    }
    if (densityAxes) {
        drawDensityAxes();
    }
}

void Waterfall3DView::resizeGL(int width, int height) {
    glViewport(0, 0, width, height);
}

void Waterfall3DView::wheelEvent(QWheelEvent *event) {
    const QPoint angle = event->angleDelta();
    const QPoint pixel = event->pixelDelta();
    const int wheelDelta = angle.y() != 0 ? angle.y()
                           : angle.x() != 0 ? angle.x()
                           : pixel.y() != 0 ? pixel.y()
                           : pixel.x();
    if (frequencySliceActive &&
        (modifierFreeSliceInput || hasSliceModifier(event->modifiers()))) {
        renderer->stepFrequencySlice(wheelDelta >= 0 ? 1 : -1);
        emitSelectedFrequency();
        update();
        event->accept();
        return;
    }
    if (spectrumSliceActive &&
        (modifierFreeSliceInput || hasSliceModifier(event->modifiers()))) {
        renderer->stepSpectrumSlice(wheelDelta >= 0 ? 1 : -1);
        update();
        event->accept();
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        renderer->zoomCamera(wheelDelta);
        update();
        event->accept();
        return;
    }
    QOpenGLWidget::wheelEvent(event);
}

void Waterfall3DView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton &&
        (hasSliceModifier(event->modifiers()) ||
         (modifierFreeSliceInput && !(event->modifiers() & Qt::ControlModifier))) &&
        renderer->beginFrequencySlice(event->pos().x(),
                                      event->pos().y(),
                                      width(),
                                      height())) {
        frequencySliceActive = true;
        spectrumSliceActive = false;
        emitSelectedFrequency();
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton &&
        (hasSliceModifier(event->modifiers()) ||
         (modifierFreeSliceInput && !(event->modifiers() & Qt::ControlModifier))) &&
        renderer->beginSpectrumSlice(event->pos().x(),
                                     event->pos().y(),
                                     width(),
                                     height())) {
        spectrumSliceActive = true;
        frequencySliceActive = false;
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton &&
        (event->modifiers() & Qt::ControlModifier)) {
        cameraDragActive = true;
        lastMousePosition = event->pos();
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton &&
        (event->modifiers() & Qt::ControlModifier)) {
        cameraPanActive = true;
        lastMousePosition = event->pos();
        setCursor(Qt::SizeAllCursor);
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void Waterfall3DView::mouseMoveEvent(QMouseEvent *event) {
    if (cameraPanActive && (event->buttons() & Qt::RightButton)) {
        const QPoint delta = event->pos() - lastMousePosition;
        lastMousePosition = event->pos();
        renderer->panCamera(static_cast<float>(delta.x()),
                            static_cast<float>(delta.y()),
                            width(),
                            height());
        update();
        event->accept();
        return;
    }
    if (cameraDragActive && (event->buttons() & Qt::LeftButton)) {
        const QPoint delta = event->pos() - lastMousePosition;
        lastMousePosition = event->pos();
        renderer->orbitCamera(static_cast<float>(delta.x()),
                              static_cast<float>(-delta.y()));
        update();
        event->accept();
        return;
    }
    QOpenGLWidget::mouseMoveEvent(event);
}

void Waterfall3DView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::RightButton && spectrumSliceActive) {
        spectrumSliceActive = false;
        renderer->endSpectrumSlice();
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton && cameraPanActive) {
        cameraPanActive = false;
        unsetCursor();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (cameraDragActive) {
            cameraDragActive = false;
            event->accept();
            return;
        }
        if (frequencySliceActive) {
            frequencySliceActive = false;
            renderer->endFrequencySlice();
            update();
            event->accept();
            return;
        }
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void Waterfall3DView::drawOverview() {
    if (overviewImage.isNull() || overviewTotalRows <= 0) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    const int previewWidth = std::clamp(width() / 4, 180, 360);
    const int previewHeight = std::clamp(height() / 4, 90, 180);
    const QRect target(width() - previewWidth - 12,
                       height() - previewHeight - 12,
                       previewWidth,
                       previewHeight);
    painter.fillRect(target.adjusted(-3, -3, 3, 3), QColor(5, 7, 10, 220));
    if (overviewThumbnail.size() != target.size()) {
        overviewThumbnail = overviewImage.scaled(target.size(),
                                                 Qt::IgnoreAspectRatio,
                                                 Qt::FastTransformation);
    }
    painter.drawImage(target, overviewThumbnail);

    const double rowScale = static_cast<double>(target.height()) /
                            static_cast<double>(overviewTotalRows);
    const int windowTop = target.top() + static_cast<int>(overviewWindowStart * rowScale);
    const int windowBottom = target.top() +
                             static_cast<int>((overviewWindowEnd + 1) * rowScale);
    painter.setPen(QPen(QColor(245, 245, 245, 230), 2));
    painter.drawRect(target.left(),
                     std::clamp(windowTop, target.top(), target.bottom()),
                     target.width() - 1,
                     std::max(2,
                              std::clamp(windowBottom, target.top(), target.bottom() + 1) -
                                  std::clamp(windowTop, target.top(), target.bottom())));

    if (overviewSelectedRow >= 0) {
        const int selectedY = target.top() +
                              static_cast<int>((overviewSelectedRow + 0.5) * rowScale);
        painter.setPen(QPen(QColor(255, 72, 72, 245), 2));
        painter.drawLine(target.left(),
                         std::clamp(selectedY, target.top(), target.bottom()),
                         target.right(),
                         std::clamp(selectedY, target.top(), target.bottom()));
    }
}

void Waterfall3DView::drawDensityAxes() {
    QPointF origin;
    QPointF frequencyEnd;
    QPointF densityEnd;
    QPointF levelEnd;
    if (!renderer->densityAxisScreenPoints(width(),
                                           height(),
                                           origin,
                                           frequencyEnd,
                                           densityEnd,
                                           levelEnd)) {
        return;
    }

    QImage overlay((std::max)(1, width()),
                   (std::max)(1, height()),
                   QImage::Format_ARGB32_Premultiplied);
    overlay.fill(Qt::transparent);
    QPainter painter(&overlay);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(QFont(painter.font().family(), 9, QFont::DemiBold));

    const auto drawLabel = [&](const QPointF &anchor,
                               const QString &text,
                               const QColor &color,
                               int horizontalOffset,
                               int verticalOffset) {
        const QFontMetrics metrics(painter.font());
        QRect rect = metrics.boundingRect(text).adjusted(-4, -2, 4, 2);
        QPoint topLeft(static_cast<int>(std::lround(anchor.x())) + horizontalOffset,
                       static_cast<int>(std::lround(anchor.y())) + verticalOffset);
        if (horizontalOffset < 0) topLeft.rx() -= rect.width();
        if (verticalOffset < 0) topLeft.ry() -= rect.height();
        topLeft.setX(std::clamp(topLeft.x(), 2, (std::max)(2, width() - rect.width() - 2)));
        topLeft.setY(std::clamp(topLeft.y(), 2, (std::max)(2, height() - rect.height() - 2)));
        rect.moveTopLeft(topLeft);
        painter.fillRect(rect, QColor(0, 0, 0, 220));
        painter.setPen(color.lighter(125));
        painter.drawText(rect, Qt::AlignCenter, text);
    };

    const auto drawAxis = [&](const QPointF &end, const QColor &color, const QString &label) {
        painter.setPen(QPen(color, 2.0));
        painter.drawLine(origin, end);
        const QLineF axis(origin, end);
        if (axis.length() > 8.0) {
            QLineF left = axis;
            left.setLength(8.0);
            left.setAngle(axis.angle() + 155.0);
            left.translate(end - left.p1());
            QLineF right = axis;
            right.setLength(8.0);
            right.setAngle(axis.angle() - 155.0);
            right.translate(end - right.p1());
            painter.drawLine(left);
            painter.drawLine(right);
        }
        drawLabel(end, label, color, 6, -4);
    };

    drawAxis(frequencyEnd, QColor(70, 205, 255), densityFrequencyLabel);
    drawAxis(densityEnd, QColor(255, 177, 70), densityAccumulationLabel);
    drawAxis(levelEnd, QColor(105, 235, 125), densityLevelLabel);

    const auto frequencyText = [](double hz) {
        const double magnitude = std::abs(hz);
        if (magnitude >= 1.0e9) return QString::number(hz / 1.0e9, 'f', 3) + QStringLiteral(" GHz");
        if (magnitude >= 1.0e6) return QString::number(hz / 1.0e6, 'f', 3) + QStringLiteral(" MHz");
        if (magnitude >= 1.0e3) return QString::number(hz / 1.0e3, 'f', 2) + QStringLiteral(" kHz");
        return QString::number(hz, 'f', 1) + QStringLiteral(" Hz");
    };
    constexpr int TickCount = 5;
    for (int tick = 0; tick < TickCount; ++tick) {
        const float ratio = static_cast<float>(tick) / static_cast<float>(TickCount - 1);
        QPointF point;
        if (renderer->densityPointToScreen(0.0f, ratio, 0.0f, width(), height(), point)) {
            painter.setPen(QPen(QColor(70, 205, 255), 2.0));
            painter.drawEllipse(point, 2.5, 2.5);
            const double value = densityFirstFrequencyHz +
                                 (densityLastFrequencyHz - densityFirstFrequencyHz) * ratio;
            drawLabel(point, frequencyText(value), QColor(70, 205, 255), 5, -7);
        }
        if (renderer->densityPointToScreen(ratio, 0.0f, 0.0f, width(), height(), point)) {
            painter.setPen(QPen(QColor(255, 177, 70), 2.0));
            painter.drawEllipse(point, 2.5, 2.5);
            drawLabel(point,
                      QString::number(densityMaximumValue * ratio,
                                      'f',
                                      densityMaximumValue < 10.0 ? 2 : 0),
                      QColor(255, 177, 70),
                      5,
                      5);
        }
        if (renderer->densityPointToScreen(0.0f, 0.0f, ratio, width(), height(), point)) {
            painter.setPen(QPen(QColor(105, 235, 125), 2.0));
            painter.drawEllipse(point, 2.5, 2.5);
            const double value = densityMinimumDb + (densityMaximumDb - densityMinimumDb) * ratio;
            drawLabel(point,
                      QString::number(value, 'f', 1) + QStringLiteral(" dBFS"),
                      QColor(105, 235, 125),
                      -6,
                      -4);
        }
    }
    painter.end();

    QPainter widgetPainter(this);
    widgetPainter.drawImage(0, 0, overlay);
}

void Waterfall3DView::emitSelectedFrequency() {
    const double ratio = renderer->selectedFrequencyRatio();
    if (ratio >= 0.0) {
        emit frequencySliceSelected(ratio);
    }
}
