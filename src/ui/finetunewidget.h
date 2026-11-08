#ifndef FINETUNEWIDGET_H
#define FINETUNEWIDGET_H

#include <QMouseEvent>
#include <QString>
#include <QWidget>
#include <QWheelEvent>

class FineTuneScaleWidget : public QWidget {
    Q_OBJECT

public:
    explicit FineTuneScaleWidget(QWidget *parent = nullptr);

    void setRangeHz(double rangeHz);
    double rangeHz() const;
    void setHoldOffsetMode(bool enabled);
    bool holdOffsetMode() const;

signals:
    void fineTuneDelta(double deltaHz);
    void holdOffsetModeChanged(bool enabled);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    double hzPerPixel() const;
    QString tickLabel(double hz) const;
    QString offsetLabel() const;
    void resetVisualOffset();

    double tuningRangeHz = 10000.0;
    double visualOffsetHz = 0.0;
    bool holdMode = false;
    bool dragging = false;
    int lastMouseX = 0;
};

#endif // FINETUNEWIDGET_H
