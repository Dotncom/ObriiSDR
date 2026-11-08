#ifndef VIDEOWIDGET_H
#define VIDEOWIDGET_H

#include <QImage>
#include <QWidget>

class VideoWidget : public QWidget {
    Q_OBJECT

public:
    explicit VideoWidget(QWidget *parent = nullptr);
    const QImage &frame() const;

public slots:
    void setFrame(const QImage &frame);
    void clearFrame();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage currentFrame;
};

#endif // VIDEOWIDGET_H
