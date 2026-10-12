#include "signallabconstellation3d.h"

#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QVector4D>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
constexpr double pi = 3.14159265358979323846;
float finiteSetting(const QJsonObject &state, const char *key, float fallback, float low, float high) {
    const double value = state.value(QLatin1String(key)).toDouble(fallback);
    return std::isfinite(value) ? float((std::clamp)(value, double(low), double(high))) : fallback;
}
}

SignalLabConstellation3DWidget::SignalLabConstellation3DWidget(QWidget *parent) : QWidget(parent) {
    setMinimumSize(430, 330);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setLanguage(QStringLiteral("en"));
}

void SignalLabConstellation3DWidget::setData(const SignalLabIqPreviewData &data) {
    data_ = data;
    sceneDirty_ = true;
    update();
}

void SignalLabConstellation3DWidget::clear() { setData({}); }

void SignalLabConstellation3DWidget::setSurfaceEnabled(bool enabled) {
    if (surfaceEnabled_ == enabled) return;
    surfaceEnabled_ = enabled;
    sceneDirty_ = true;
    update();
}

void SignalLabConstellation3DWidget::setLanguage(const QString &language) {
    ukrainian_ = language.toLower().startsWith(QStringLiteral("uk"));
    setToolTip(ukrainian_
        ? QString::fromUtf8(u8"Ctrl + ліва кнопка: обертання; Ctrl + права: переміщення; Ctrl + колесо: наближення; Ctrl + подвійний клік: скинути камеру.")
        : QStringLiteral("Ctrl + left drag: orbit; Ctrl + right drag: pan; Ctrl + wheel: zoom; Ctrl + double-click: reset camera."));
    update();
}

QJsonObject SignalLabConstellation3DWidget::cameraState() const {
    return {{"yaw", yaw_}, {"pitch", pitch_}, {"distance", distance_}, {"panX", panX_}, {"panY", panY_}};
}

void SignalLabConstellation3DWidget::setCameraState(const QJsonObject &state) {
    yaw_ = std::remainder(finiteSetting(state, "yaw", -35, -36000, 36000), 360.0f);
    pitch_ = finiteSetting(state, "pitch", 25, -85, 85);
    distance_ = finiteSetting(state, "distance", 6, 2.3f, 18);
    panX_ = finiteSetting(state, "panX", 0, -10, 10);
    panY_ = finiteSetting(state, "panY", 0, -10, 10);
    sceneDirty_ = true;
    update();
}

void SignalLabConstellation3DWidget::resetCamera() {
    dragButton_ = Qt::NoButton;
    unsetCursor();
    setCameraState({});
}

bool SignalLabConstellation3DWidget::project(const QVector3D &point, const QRectF &plot,
                                            const QMatrix4x4 &transform, QPointF &screen, float &depth) {
    const auto clip = transform * QVector4D(point, 1);
    if (!std::isfinite(clip.w()) || clip.w() <= 0.0001f) return false;
    const auto position = clip.toVector3DAffine();
    if (!std::isfinite(position.x()) || !std::isfinite(position.y()) ||
        !std::isfinite(position.z()) || position.z() < -1 || position.z() > 1) return false;
    screen = QPointF(plot.center().x() + position.x()*plot.width()*0.5,
                     plot.center().y() - position.y()*plot.height()*0.5);
    depth = position.z();
    return true;
}

void SignalLabConstellation3DWidget::rebuildScene(const QRectF &plot, const QMatrix4x4 &transform) {
    scene_.clear();
    const auto line = [&](const QVector3D &a, const QVector3D &b, QColor color, float width = 1, bool frame = true) {
        Primitive primitive;
        float secondDepth;
        if (!project(a, plot, transform, primitive.first, primitive.depth) ||
            !project(b, plot, transform, primitive.last, secondDepth)) return;
        primitive.depth = (primitive.depth + secondDepth)*0.5f;
        primitive.color = color;
        primitive.size = width;
        primitive.frame = frame;
        scene_.append(primitive);
    };
    const auto triangle = [&](const QVector3D &a, const QVector3D &b, const QVector3D &c, QColor color) {
        Primitive primitive;
        float secondDepth, thirdDepth;
        if (!project(a, plot, transform, primitive.first, primitive.depth) ||
            !project(b, plot, transform, primitive.last, secondDepth) ||
            !project(c, plot, transform, primitive.third, thirdDepth)) return;
        const auto normal = QVector3D::crossProduct(b-a, c-a).normalized();
        const float light = 0.65f+0.35f*std::abs(QVector3D::dotProduct(normal, QVector3D(0.3f, 0.8f, 0.5f).normalized()));
        color.setRgbF(color.redF()*light, color.greenF()*light, color.blueF()*light);
        primitive.depth = (primitive.depth+secondDepth+thirdDepth)/3;
        primitive.color = color;
        primitive.surface = true;
        scene_.append(primitive);
    };
    for (int tick = 0; tick <= 4; ++tick) {
        const float value = -1 + tick*0.5f;
        const QColor grid(50, 59, 68);
        line({value, -1, -1}, {value, -1, 1}, grid);
        line({-1, -1, value}, {1, -1, value}, grid);
        line({value, -1, 1}, {value, 1, 1}, grid);
        line({-1, value, 1}, {1, value, 1}, grid);
    }
    line({-1,-1,1}, {1,-1,1}, QColor(83,171,255), 1.7f);
    line({-1,-1,1}, {-1,1,1}, QColor(65,220,155), 1.7f);
    line({-1,-1,1}, {-1,-1,-1}, QColor(255,188,78), 1.7f);
    const auto world = [&](const std::complex<float> &sample, double time) {
        const double ratio = data_.durationSeconds > 0 ? time/data_.durationSeconds : 0;
        const double scale = std::isfinite(data_.amplitudeScale) && data_.amplitudeScale > 0
                                 ? data_.amplitudeScale : 1.0;
        return QVector3D(float(sample.real()/scale), float(sample.imag()/scale), float(1-2*ratio));
    };
    const int count = (std::min)(data_.samples.size(), data_.sampleTimes.size());
    for (int index = 0; index < count; ++index) {
        const double ratio = data_.durationSeconds > 0 ? data_.sampleTimes[index]/data_.durationSeconds : 0;
        const QColor color = QColor::fromHsvF(0.50 - 0.38*std::clamp(ratio, 0.0, 1.0), 0.75, 0.95);
        Primitive primitive;
        if (!project(world(data_.samples[index], data_.sampleTimes[index]), plot, transform,
                     primitive.first, primitive.depth)) continue;
        primitive.point = true;
        primitive.size = 1;
        primitive.color = color;
        scene_.append(primitive);
        if (surfaceEnabled_ && index > 0) {
            const auto previous = world(data_.samples[index-1], data_.sampleTimes[index-1]);
            const auto current = world(data_.samples[index], data_.sampleTimes[index]);
            const QVector3D previousCenter(0, 0, previous.z()), currentCenter(0, 0, current.z());
            // Filled ribbons interpolate displayed points only; they do not reconstruct IQ.
            triangle(previousCenter, previous, current, color);
            triangle(previousCenter, current, currentCenter, color);
        }
        // Only join genuinely adjacent samples, never decimated long-record points.
        if (data_.continuousTrajectory && index > 0)
            line(world(data_.samples[index-1], data_.sampleTimes[index-1]),
                 world(data_.samples[index], data_.sampleTimes[index]), color, 1, false);
    }
    const int symbols = (std::min)(data_.symbols.size(), data_.symbolTimes.size());
    for (int index = 0; index < symbols; ++index) {
        Primitive primitive;
        if (!project(world(data_.symbols[index], data_.symbolTimes[index]), plot, transform,
                     primitive.first, primitive.depth)) continue;
        primitive.point = true;
        primitive.size = 2.5f;
        primitive.color = QColor(240,244,248);
        scene_.append(primitive);
    }
    std::stable_sort(scene_.begin(), scene_.end(), [this](const Primitive &a, const Primitive &b) {
        // Keep the reference grid behind opaque ribbons, not sliced through them.
        if (surfaceEnabled_ && a.frame != b.frame) return a.frame;
        return a.depth > b.depth;
    });
    sceneDirty_ = false;
    sceneSize_ = size();
}

void SignalLabConstellation3DWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(12,15,19));
    const QRectF plot = QRectF(rect()).adjusted(38, 36, -38, -35);
    if (plot.width() <= 0 || plot.height() <= 0) return;
    QMatrix4x4 projection, view, pan;
    projection.perspective(35, float(plot.width()/plot.height()), 0.03f, 100);
    const double yaw = yaw_*pi/180, pitch = pitch_*pi/180;
    view.lookAt(QVector3D(float(distance_*std::cos(pitch)*std::sin(yaw)), float(distance_*std::sin(pitch)),
                          float(distance_*std::cos(pitch)*std::cos(yaw))), {}, {0,1,0});
    pan.translate(panX_, panY_, 0);
    const auto transform = projection * pan * view;
    if (sceneDirty_ || sceneSize_ != size()) rebuildScene(plot, transform);
    painter.save();
    painter.setClipRect(plot);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const auto &primitive : scene_) {
        if (primitive.surface) {
            painter.setPen(QPen(primitive.color, 0.5));
            painter.setBrush(primitive.color);
            QPolygonF polygon;
            polygon << primitive.first << primitive.last << primitive.third;
            painter.drawPolygon(polygon);
        } else if (primitive.point) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(primitive.color);
            painter.drawEllipse(primitive.first, primitive.size, primitive.size);
        } else {
            painter.setPen(QPen(primitive.color, primitive.size));
            painter.drawLine(primitive.first, primitive.last);
        }
    }
    painter.restore();
    QVector<QRectF> labels;
    const auto label = [&](const QString &text, QPointF position, QColor color) {
        const auto bounds = painter.fontMetrics().boundingRect(text);
        QRectF target(position.x()-bounds.width()*0.5-3, position.y()-bounds.height()*0.5-2,
                      bounds.width()+6, bounds.height()+4);
        for (int attempt = 0; attempt < 6; ++attempt) {
            target.moveLeft((std::clamp)(target.left(), 2.0, (std::max)(2.0, width()-target.width()-2)));
            target.moveTop((std::clamp)(target.top(), 32.0, (std::max)(32.0, height()-target.height()-2)));
            bool collision = false;
            for (const auto &previous : labels) if (previous.intersects(target)) collision = true;
            if (!collision) {
                painter.fillRect(target, QColor(12,15,19,215));
                painter.setPen(color);
                painter.drawText(target, Qt::AlignCenter, text);
                labels.append(target);
                return;
            }
            target.translate(0, target.height()+1);
        }
    };
    const double timeScale = data_.durationSeconds >= 1 ? 1 : data_.durationSeconds >= 0.001 ? 1000 :
                             data_.durationSeconds >= 0.000001 ? 1000000 : 1000000000;
    const QString unit = timeScale == 1 ? QStringLiteral("s") : timeScale == 1000 ? QStringLiteral("ms") :
                         timeScale == 1000000 ? QStringLiteral("us") : QStringLiteral("ns");
    QPointF origin;
    float originDepth;
    const double amplitudeScale = std::isfinite(data_.amplitudeScale) && data_.amplitudeScale > 0
                                      ? data_.amplitudeScale : 1.0;
    const auto amplitudeText = [](double value) { return QString::number(value, 'g', 3); };
    if (project({-1,-1,1}, plot, transform, origin, originDepth))
        label(QStringLiteral("I=%1, Q=%1, T=0").arg(amplitudeText(-amplitudeScale)),
              origin+QPointF(0, 19), QColor(205,214,223));
    const int tickStep = plot.width() < 520 || plot.height() < 400 ? 2 : 1;
    const auto axis = [&](const QVector3D &end, const QString &name, QColor color, int dimension) {
        const QVector3D start(-1,-1,1);
        QPointF first, last;
        float depth;
        if (!project(start, plot, transform, first, depth) || !project(end, plot, transform, last, depth)) return;
        QPointF direction = last-first;
        const double length = std::hypot(direction.x(), direction.y());
        if (length < 1) return;
        direction /= length;
        QPointF normal(-direction.y(), direction.x());
        const auto outside = (first+last)*0.5 - plot.center();
        if (normal.x()*outside.x()+normal.y()*outside.y() < 0) normal = -normal;
        const int axisStep = length < 180 ? 4 : length < 300 ? (std::max)(2, tickStep) : tickStep;
        for (int tick = axisStep; length >= 40 && tick <= 4; tick += axisStep) {
            const double ratio = tick/4.0;
            QPointF screen;
            if (!project(start*(1-ratio)+end*ratio, plot, transform, screen, depth)) continue;
            painter.setPen(color);
            painter.drawLine(screen, screen+normal*4);
            const double value = dimension == 2 ? data_.durationSeconds*ratio*timeScale : (-1+2*ratio)*amplitudeScale;
            label(dimension == 2 ? QString::number(value, 'f', 2) : amplitudeText(value), screen+normal*14, color);
        }
        label(name, last+direction*24+normal*10, color);
    };
    axis({1,-1,1}, QStringLiteral("I"), QColor(83,171,255), 0);
    axis({-1,1,1}, QStringLiteral("Q"), QColor(65,220,155), 1);
    axis({-1,-1,-1}, QStringLiteral("T (%1)").arg(unit), QColor(255,188,78), 2);
    painter.setPen(QColor(205,214,223));
    painter.drawText(rect().adjusted(10,5,-10,-5), Qt::AlignLeft | Qt::AlignTop,
                     ukrainian_ ? QString::fromUtf8(u8"3D-сузір'я: I / Q / час") : QStringLiteral("3D constellation: I / Q / time"));
    if (data_.samples.isEmpty()) {
        painter.drawText(rect(), Qt::AlignCenter, ukrainian_ ? QString::fromUtf8(u8"Немає IQ") : QStringLiteral("No IQ"));
    }
}

void SignalLabConstellation3DWidget::mousePressEvent(QMouseEvent *event) {
    if ((event->modifiers() & Qt::ControlModifier) &&
        (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
        dragButton_ = event->button();
        lastMousePosition_ = event->pos();
        setCursor(dragButton_ == Qt::RightButton ? Qt::SizeAllCursor : Qt::ClosedHandCursor);
        event->accept();
    } else if (event->button() == Qt::RightButton) {
        QMenu menu(this);
        auto *reset = menu.addAction(ukrainian_ ? QString::fromUtf8(u8"Скинути камеру") : QStringLiteral("Reset camera"));
        if (menu.exec(event->globalPos()) == reset) resetCamera();
        event->accept();
    } else QWidget::mousePressEvent(event);
}

void SignalLabConstellation3DWidget::mouseMoveEvent(QMouseEvent *event) {
    if (dragButton_ == Qt::NoButton || !(event->buttons() & dragButton_)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const auto delta = event->pos()-lastMousePosition_;
    lastMousePosition_ = event->pos();
    if (dragButton_ == Qt::LeftButton) {
        yaw_ = std::remainder(yaw_-delta.x()*0.45f, 360.0f);
        pitch_ = (std::clamp)(pitch_+delta.y()*0.45f, -85.0f, 85.0f);
    } else {
        const float scale = float(2*distance_*std::tan(17.5*pi/180)/(std::max)(1, height()-71));
        panX_ = (std::clamp)(panX_+delta.x()*scale, -10.0f, 10.0f);
        panY_ = (std::clamp)(panY_-delta.y()*scale, -10.0f, 10.0f);
    }
    sceneDirty_ = true;
    update();
    event->accept();
}

void SignalLabConstellation3DWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == dragButton_) {
        dragButton_ = Qt::NoButton;
        unsetCursor();
        event->accept();
    } else QWidget::mouseReleaseEvent(event);
}

void SignalLabConstellation3DWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)) {
        dragButton_ = Qt::NoButton;
        unsetCursor();
        resetCamera();
        event->accept();
    } else QWidget::mouseDoubleClickEvent(event);
}

void SignalLabConstellation3DWidget::wheelEvent(QWheelEvent *event) {
    if (!(event->modifiers() & Qt::ControlModifier)) {
        QWidget::wheelEvent(event);
        return;
    }
    const auto angle = event->angleDelta(), pixel = event->pixelDelta();
    const int delta = angle.y() ? angle.y() : angle.x() ? angle.x() : pixel.y() ? pixel.y() : pixel.x();
    if (delta) {
        distance_ = (std::clamp)(float(distance_*std::exp(-delta/120.0*0.12)), 2.3f, 18.0f);
        sceneDirty_ = true;
        update();
    }
    event->accept();
}
