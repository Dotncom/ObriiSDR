#include "videowidget.h"

#include <QPainter>
#include <QPaintEvent>

VideoWidget::VideoWidget(QWidget *parent)
    : QWidget(parent) {
    setMinimumSize(360, 260);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

const QImage &VideoWidget::frame() const {
    return currentFrame;
}

void VideoWidget::setFrame(const QImage &frame) {
    currentFrame = frame;
    update();
}

void VideoWidget::clearFrame() {
    currentFrame = QImage();
    update();
}

void VideoWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor(8, 10, 12));

    if (currentFrame.isNull()) {
        painter.setPen(QColor(115, 125, 135));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No video frame"));
        return;
    }

    const QSize targetSize = currentFrame.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect targetRect(QPoint((width() - targetSize.width()) / 2,
                                  (height() - targetSize.height()) / 2),
                           targetSize);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(targetRect, currentFrame);
}
