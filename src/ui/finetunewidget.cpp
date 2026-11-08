#include "finetunewidget.h"

#include <QPainter>
#include <QSizePolicy>
#include <QTimer>
#include <algorithm>
#include <cmath>

FineTuneScaleWidget::FineTuneScaleWidget(QWidget *parent)
    : QWidget(parent) {
    setMinimumSize(180, 54);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    setFocusPolicy(Qt::WheelFocus);
    setToolTip(QStringLiteral("Green: temporary fine tune. Double-click to switch to held offset mode."));
}

void FineTuneScaleWidget::setRangeHz(double rangeHz) {
    if (!std::isfinite(rangeHz) || rangeHz <= 0.0) {
        return;
    }
    tuningRangeHz = rangeHz;
    visualOffsetHz = (std::clamp)(visualOffsetHz, -tuningRangeHz, tuningRangeHz);
    update();
}

double FineTuneScaleWidget::rangeHz() const {
    return tuningRangeHz;
}

void FineTuneScaleWidget::setHoldOffsetMode(bool enabled) {
    if (holdMode == enabled) {
        return;
    }
    holdMode = enabled;
    setToolTip(holdMode
                   ? QStringLiteral("Red: held fine tune offset. Double-click to switch back to temporary mode.")
                   : QStringLiteral("Green: temporary fine tune. Double-click to switch to held offset mode."));
    if (!holdMode) {
        resetVisualOffset();
    } else {
        update();
    }
    emit holdOffsetModeChanged(holdMode);
}

bool FineTuneScaleWidget::holdOffsetMode() const {
    return holdMode;
}

void FineTuneScaleWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging = true;
    lastMouseX = event->x();
    event->accept();
}

void FineTuneScaleWidget::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const int dx = event->x() - lastMouseX;
    lastMouseX = event->x();
    if (dx == 0) {
        return;
    }

    const double deltaHz = dx * hzPerPixel();
    visualOffsetHz = (std::clamp)(visualOffsetHz + deltaHz, -tuningRangeHz, tuningRangeHz);
    emit fineTuneDelta(deltaHz);
    update();
    event->accept();
}

void FineTuneScaleWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        dragging = false;
        if (!holdMode) {
            resetVisualOffset();
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void FineTuneScaleWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        setHoldOffsetMode(!holdMode);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void FineTuneScaleWidget::wheelEvent(QWheelEvent *event) {
    double wheelUnits = event->angleDelta().y() / 120.0;
    if (!event->pixelDelta().isNull()) {
        wheelUnits = event->pixelDelta().y() / 18.0;
    }
    if (std::abs(wheelUnits) < 0.01) {
        return;
    }

    const double deltaHz = wheelUnits * tuningRangeHz / 80.0;
    visualOffsetHz = (std::clamp)(visualOffsetHz + deltaHz, -tuningRangeHz, tuningRangeHz);
    emit fineTuneDelta(deltaHz);
    update();
    if (!holdMode) {
        QTimer::singleShot(120, this, [this]() {
            if (!holdMode) {
                resetVisualOffset();
            }
        });
    }
    event->accept();
}

void FineTuneScaleWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF area = rect().adjusted(1, 1, -1, -1);
    painter.fillRect(area, QColor(15, 21, 27));

    painter.setPen(QPen(QColor(48, 64, 72), 1));
    painter.drawRoundedRect(area, 4, 4);

    const int w = width();
    const int h = height();
    const double pxPerHz = 1.0 / hzPerPixel();
    const double majorStepHz = tuningRangeHz / 4.0;
    const double minorStepHz = majorStepHz / 5.0;
    const double centerX = w * 0.5;
    const QColor accent = holdMode ? QColor(242, 91, 91) : QColor(92, 220, 128);

    painter.setFont(QFont(QStringLiteral("Consolas"), 8));
    for (int i = -20; i <= 20; ++i) {
        const double hz = i * minorStepHz + visualOffsetHz;
        const double x = centerX + hz * pxPerHz;
        if (x < -20.0 || x > w + 20.0) {
            continue;
        }

        const bool major = (i % 5) == 0;
        painter.setPen(QPen(major ? QColor(105, 130, 140) : QColor(58, 78, 86),
                            major ? 1.2 : 1.0));
        const int tickTop = major ? 8 : 16;
        const int tickBottom = major ? h - 13 : h - 18;
        painter.drawLine(QPointF(x, tickTop), QPointF(x, tickBottom));

        if (major) {
            const QString label = tickLabel(hz);
            const QRectF labelRect(x - 42, h - 14, 84, 12);
            painter.setPen(QColor(182, 198, 207));
            painter.drawText(labelRect, Qt::AlignHCenter | Qt::AlignVCenter, label);
        }
    }

    painter.setPen(QPen(accent, 2));
    painter.drawLine(QPointF(centerX, 3), QPointF(centerX, h - 3));
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawEllipse(QPointF(centerX, h * 0.5), 3, 3);

    if (holdMode || std::abs(visualOffsetHz) >= 1.0) {
        painter.setFont(QFont(QStringLiteral("Consolas"), 8, holdMode ? QFont::Bold : QFont::Normal));
        painter.setPen(accent);
        painter.drawText(QRectF(w - 76, 4, 70, 14),
                         Qt::AlignRight | Qt::AlignVCenter,
                         offsetLabel());
    }
}

double FineTuneScaleWidget::hzPerPixel() const {
    return tuningRangeHz / std::max(64, width());
}

QString FineTuneScaleWidget::tickLabel(double hz) const {
    if (std::abs(hz) >= 1000000.0) {
        return QStringLiteral("%1M").arg(hz / 1000000.0, 0, 'f', 1);
    }
    if (std::abs(hz) >= 1000.0) {
        return QStringLiteral("%1k").arg(hz / 1000.0, 0, 'f', 0);
    }
    return QStringLiteral("%1").arg(static_cast<int>(std::lround(hz)));
}

QString FineTuneScaleWidget::offsetLabel() const {
    const double hz = visualOffsetHz;
    const QString sign = hz > 0.0 ? QStringLiteral("+") : QString();
    if (std::abs(hz) >= 1000000.0) {
        return QStringLiteral("%1%2M").arg(sign).arg(hz / 1000000.0, 0, 'f', 2);
    }
    if (std::abs(hz) >= 1000.0) {
        return QStringLiteral("%1%2k").arg(sign).arg(hz / 1000.0, 0, 'f', 1);
    }
    return QStringLiteral("%1%2").arg(sign).arg(static_cast<int>(std::lround(hz)));
}

void FineTuneScaleWidget::resetVisualOffset() {
    visualOffsetHz = 0.0;
    update();
}
