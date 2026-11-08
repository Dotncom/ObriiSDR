#ifndef SIGNALLABCONSTELLATION3D_H
#define SIGNALLABCONSTELLATION3D_H

#include "signallabpreview.h"

#include <QColor>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QPoint>
#include <QVector3D>

class SignalLabConstellation3DWidget final : public QWidget {
public:
    explicit SignalLabConstellation3DWidget(QWidget *parent = nullptr);
    void setData(const SignalLabIqPreviewData &data);
    void clear();
    void setLanguage(const QString &language);
    void setSurfaceEnabled(bool enabled);
    QJsonObject cameraState() const;
    void setCameraState(const QJsonObject &state);
    void resetCamera();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    friend struct SignalLabConstellation3DTestAccess;
    struct Primitive {
        QPointF first;
        QPointF last;
        QPointF third;
        QColor color;
        float depth = 0;
        float size = 1;
        bool point = false;
        bool surface = false;
        bool frame = false;
    };
    void rebuildScene(const QRectF &plot, const QMatrix4x4 &transform);
    static bool project(const QVector3D &point, const QRectF &plot,
                        const QMatrix4x4 &transform, QPointF &screen, float &depth);
    SignalLabIqPreviewData data_;
    QVector<Primitive> scene_;
    QSize sceneSize_;
    bool sceneDirty_ = true;
    bool ukrainian_ = false;
    bool surfaceEnabled_ = false;
    float yaw_ = -35;
    float pitch_ = 25;
    float distance_ = 6;
    float panX_ = 0;
    float panY_ = 0;
    Qt::MouseButton dragButton_ = Qt::NoButton;
    QPoint lastMousePosition_;
};

#endif
