#include "dspresearchwidget.h"

#include "iqbuffer.h"
#include "iqconstellationpreview.h"
#include "signallabconstellation3d.h"

#include <QPainter>
#include <QPaintEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsView>

#include <algorithm>
#include <cmath>

namespace {

constexpr QColor kBackground(5, 8, 13);
constexpr QColor kGrid(36, 46, 58);
constexpr QColor kPrimary(75, 222, 150);
constexpr QColor kSecondary(74, 159, 255);
constexpr QColor kMuted(151, 163, 177);

int boundedSamples(const QJsonObject &settings) {
    return std::clamp(settings.value(QStringLiteral("samples")).toInt(4096), 256, 262144);
}

double centeredMaximum(const std::vector<float> &first, const std::vector<float> &second = {}) {
    double maximum = 1.0e-6;
    for (float value : first) if (std::isfinite(value)) maximum = std::max(maximum, std::abs(double(value)));
    for (float value : second) if (std::isfinite(value)) maximum = std::max(maximum, std::abs(double(value)));
    return maximum;
}

} // namespace

DspResearchWidget::DspResearchWidget(Mode mode, QWidget *parent)
    : QWidget(parent), mode_(mode), timer_(new QTimer(this)) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 110);
    if (mode_ == Mode::Constellation) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        constellation3D_ = new SignalLabConstellation3DWidget(this);
        constellation3D_->setObjectName(QStringLiteral("dspResearchConstellation3D"));
        constellation3D_->setMinimumSize(200, 110);
        layout->addWidget(constellation3D_);
        constellation3D_->hide();
    }
    timer_->setInterval(40);
    connect(timer_, &QTimer::timeout, this, [this]() { refreshSnapshot(); });
    timer_->start();
}

void DspResearchWidget::setSettings(const QJsonObject &settings) {
    if (settings_ == settings) return;
    settings_ = settings;
    timer_->setInterval(std::clamp(settings_.value(QStringLiteral("refreshMs")).toInt(40), 10, 1000));
    updateConstellationView();
    update();
}

void DspResearchWidget::setConnected(bool connected, bool ukrainian) {
    if (connected_ == connected && ukrainian_ == ukrainian) return;
    connected_ = connected;
    ukrainian_ = ukrainian;
    if (constellation3D_) constellation3D_->setLanguage(ukrainian ? QStringLiteral("uk") : QStringLiteral("en"));
    updateConstellationView();
    update();
}

void DspResearchWidget::refreshSnapshot() {
    if (externalInput_ || !isVisible() || window()->isMinimized() || !connected_) return;
    // Embedded widgets can stay QWidget-visible while their board is hidden.
    if (const auto *proxy = graphicsProxyWidget()) {
        if (!proxy->isVisible() || !proxy->scene()) return;
        bool presented = false;
        for (const auto *view : proxy->scene()->views()) {
            if (view->isVisible() && !view->window()->isMinimized() &&
                view->viewport()->rect().intersects(view->mapFromScene(proxy->sceneBoundingRect()).boundingRect())) {
                presented = true;
                break;
            }
        }
        if (!presented) return;
    }
    std::vector<float> raw;
    std::uint64_t sequence = 0;
    const std::size_t requestedFloats = static_cast<std::size_t>(boundedSamples(settings_)) * 2U;
    if (!IqBuffer::snapshotRecent(raw, requestedFloats, &sequence) || raw.size() < 8U || sequence == sequence_) return;
    ingestIq(raw, IqBuffer::sampleRateEstimate(), sequence);
}

void DspResearchWidget::setExternalInput(bool enabled) {
    if (externalInput_ == enabled) return;
    externalInput_ = enabled;
    iSamples_.clear(); qSamples_.clear(); sequence_ = 0;
    updateConstellationView(); update();
}

void DspResearchWidget::setExternalFrame(const std::vector<float> &iq, double sampleRate, std::uint64_t sequence) {
    if (!externalInput_ || !connected_) return;
    const std::size_t count = std::min(iq.size(), std::size_t(boundedSamples(settings_))*2U);
    const std::vector<float> tail(iq.end()-static_cast<std::ptrdiff_t>(count), iq.end());
    ingestIq(tail, sampleRate, sequence);
}

void DspResearchWidget::ingestIq(const std::vector<float> &raw, double sampleRate, std::uint64_t sequence) {
    sequence_ = sequence;
    const std::size_t pairs = raw.size() / 2U;
    if (pairs < 2) {
        iSamples_.clear(); qSamples_.clear(); updateConstellationView(); update(); return;
    }
    iSamples_.resize(pairs);
    qSamples_.resize(pairs);
    double meanI = 0.0;
    double meanQ = 0.0;
    for (std::size_t index = 0; index < pairs; ++index) {
        const float i = raw[index * 2U];
        const float q = raw[index * 2U + 1U];
        iSamples_[index] = std::isfinite(i) ? i : 0.0f;
        qSamples_[index] = std::isfinite(q) ? q : 0.0f;
        meanI += iSamples_[index];
        meanQ += qSamples_[index];
    }
    meanI /= static_cast<double>(pairs);
    meanQ /= static_cast<double>(pairs);
    for (std::size_t index = 0; index < pairs; ++index) {
        iSamples_[index] -= static_cast<float>(meanI);
        qSamples_[index] -= static_cast<float>(meanQ);
    }
    sampleRate_ = sampleRate;
    updateConstellationView();
    update();
}

void DspResearchWidget::updateConstellationView() {
    if (!constellation3D_) return;
    const bool threeD = settings_.value(QStringLiteral("constellation3D")).toBool(false);
    constellation3D_->setSurfaceEnabled(settings_.value(QStringLiteral("constellationSurface")).toBool(false));
    constellation3D_->setVisible(threeD);
    setAttribute(Qt::WA_TransparentForMouseEvents, !threeD);
    if (!threeD) return;
    if (!connected_) {
        constellation3D_->clear();
        return;
    }
    auto data = buildResearchIqConstellation(iSamples_, qSamples_, sampleRate_,
        settings_.value(QStringLiteral("symbolRate")).toDouble(4800.0),
        settings_.value(QStringLiteral("phasePercent")).toDouble(50.0)/100.0);
    data.amplitudeScale /= std::clamp(settings_.value(QStringLiteral("gain")).toDouble(1.0), 0.1, 20.0);
    constellation3D_->setData(data);
}

void DspResearchWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);
    const QRectF plot = QRectF(rect()).adjusted(8.0, 24.0, -8.0, -18.0);
    drawGrid(painter, plot);

    if (!connected_) {
        painter.setPen(kMuted);
        painter.drawText(plot, Qt::AlignCenter | Qt::TextWordWrap,
                         ukrainian_ ? QStringLiteral("Під'єднайте блок налаштувань")
                                    : QStringLiteral("Connect a settings block"));
        return;
    }
    if (iSamples_.empty()) {
        painter.setPen(kMuted);
        painter.drawText(plot, Qt::AlignCenter,
                         ukrainian_ ? QStringLiteral("Очікування IQ") : QStringLiteral("Waiting for IQ"));
        return;
    }

    switch (mode_) {
    case Mode::Oscilloscope: drawOscilloscope(painter, plot); break;
    case Mode::Constellation: drawConstellation(painter, plot); break;
    case Mode::EyeDiagram: drawEye(painter, plot); break;
    case Mode::Synchronization: drawSynchronization(painter, plot); break;
    }

    painter.setPen(QColor(214, 222, 232));
    painter.drawText(QRectF(8.0, 3.0, width() - 16.0, 17.0), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("%1 samples | %2 MS/s | seq %3")
                         .arg(iSamples_.size())
                         .arg(sampleRate_ / 1.0e6, 0, 'f', 3)
                         .arg(sequence_));
}

void DspResearchWidget::drawGrid(QPainter &painter, const QRectF &plot) const {
    painter.setPen(QPen(kGrid, 1.0));
    for (int index = 0; index <= 4; ++index) {
        const qreal x = plot.left() + plot.width() * index / 4.0;
        const qreal y = plot.top() + plot.height() * index / 4.0;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
}

void DspResearchWidget::drawOscilloscope(QPainter &painter, const QRectF &plot) const {
    const double scale = centeredMaximum(iSamples_, qSamples_) /
                         std::clamp(settings_.value(QStringLiteral("gain")).toDouble(1.0), 0.1, 20.0);
    auto drawTrace = [&](const std::vector<float> &samples, const QColor &color) {
        QPolygonF line;
        const int points = std::max(2, std::min<int>(plot.width() * 2.0, static_cast<int>(samples.size())));
        line.reserve(points);
        for (int point = 0; point < points; ++point) {
            const std::size_t index = static_cast<std::size_t>(point) * (samples.size() - 1U) /
                                      static_cast<std::size_t>(points - 1);
            const qreal x = plot.left() + plot.width() * point / qreal(points - 1);
            const qreal y = plot.center().y() - std::clamp(double(samples[index]) / scale, -1.0, 1.0) * plot.height() * 0.46;
            line << QPointF(x, y);
        }
        painter.setPen(QPen(color, 1.2));
        painter.drawPolyline(line);
    };
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawTrace(iSamples_, kPrimary);
    drawTrace(qSamples_, kSecondary);
}

void DspResearchWidget::drawConstellation(QPainter &painter, const QRectF &plot) const {
    const double scale = centeredMaximum(iSamples_, qSamples_) /
                         std::clamp(settings_.value(QStringLiteral("gain")).toDouble(1.0), 0.1, 20.0);
    const std::size_t stride = std::max<std::size_t>(1U, iSamples_.size() / 2500U);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(QColor(79, 221, 159, 175), 1.0));
    for (std::size_t index = 0; index < iSamples_.size(); index += stride) {
        const qreal x = plot.center().x() + std::clamp(double(iSamples_[index]) / scale, -1.0, 1.0) * plot.width() * 0.47;
        const qreal y = plot.center().y() - std::clamp(double(qSamples_[index]) / scale, -1.0, 1.0) * plot.height() * 0.47;
        painter.drawPoint(QPointF(x, y));
    }
}

float DspResearchWidget::interpolated(const std::vector<float> &values, double position) const {
    if (values.empty()) return 0.0f;
    position = std::clamp(position, 0.0, static_cast<double>(values.size() - 1U));
    const std::size_t left = static_cast<std::size_t>(position);
    if (left + 1U >= values.size()) return values.back();
    const float fraction = static_cast<float>(position - static_cast<double>(left));
    return values[left] + (values[left + 1U] - values[left]) * fraction;
}

void DspResearchWidget::drawEye(QPainter &painter, const QRectF &plot) const {
    const double symbolRate = std::max(1.0, settings_.value(QStringLiteral("symbolRate")).toDouble(4800.0));
    const double samplesPerSymbol = sampleRate_ / symbolRate;
    if (samplesPerSymbol < 2.0) {
        painter.setPen(kMuted);
        painter.drawText(plot, Qt::AlignCenter, ukrainian_ ? QStringLiteral("Замала кількість відліків на символ")
                                                          : QStringLiteral("Too few samples per symbol"));
        return;
    }
    const int traces = std::clamp(settings_.value(QStringLiteral("traces")).toInt(32), 1, 256);
    const double phase = std::clamp(settings_.value(QStringLiteral("phasePercent")).toDouble(50.0), 0.0, 100.0) / 100.0;
    const double scale = centeredMaximum(iSamples_, qSamples_) /
                         std::clamp(settings_.value(QStringLiteral("gain")).toDouble(1.0), 0.1, 20.0);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int trace = 0; trace < traces; ++trace) {
        const double start = (phase + trace) * samplesPerSymbol;
        if (start + 2.0 * samplesPerSymbol + 1.0 >= iSamples_.size()) break;
        QPolygonF line;
        constexpr int points = 128;
        line.reserve(points);
        for (int point = 0; point < points; ++point) {
            const double position = start + 2.0 * samplesPerSymbol * point / double(points - 1);
            const qreal x = plot.left() + plot.width() * point / qreal(points - 1);
            const qreal y = plot.center().y() - std::clamp(double(interpolated(iSamples_, position)) / scale, -1.0, 1.0) * plot.height() * 0.46;
            line << QPointF(x, y);
        }
        painter.setPen(QPen(QColor(75, 222, 150, 45 + 150 / std::max(1, traces)), 1.0));
        painter.drawPolyline(line);
    }
}

void DspResearchWidget::drawSynchronization(QPainter &painter, const QRectF &plot) const {
    if (iSamples_.size() < 2U) return;
    std::vector<float> phaseError(iSamples_.size() - 1U);
    for (std::size_t index = 1; index < iSamples_.size(); ++index) {
        const double cross = double(iSamples_[index - 1U]) * qSamples_[index] -
                             double(qSamples_[index - 1U]) * iSamples_[index];
        const double dot = double(iSamples_[index - 1U]) * iSamples_[index] +
                           double(qSamples_[index - 1U]) * qSamples_[index];
        phaseError[index - 1U] = static_cast<float>(std::atan2(cross, dot));
    }
    QPolygonF line;
    const int points = std::max(2, std::min<int>(plot.width() * 2.0, static_cast<int>(phaseError.size())));
    line.reserve(points);
    for (int point = 0; point < points; ++point) {
        const std::size_t index = static_cast<std::size_t>(point) * (phaseError.size() - 1U) /
                                  static_cast<std::size_t>(points - 1);
        line << QPointF(plot.left() + plot.width() * point / qreal(points - 1),
                        plot.center().y() - phaseError[index] / 3.14159265358979323846 * plot.height() * 0.46);
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kPrimary, 1.2));
    painter.drawPolyline(line);
}
