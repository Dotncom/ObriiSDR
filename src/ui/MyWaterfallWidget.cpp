#include "MyWaterfallWidget.h"
#include "radiosettings.h"
#include "waterfall3drenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <QLabel>
#include <QDateTime>
#include <QLinearGradient>
#include <QPainterPath>
#include <QStyle>
#include <QToolButton>

bool changebit=false;

namespace {
constexpr double AUTO_TUNE_WINDOW_FRACTION = 1.0 / 80.0;
constexpr double AUTO_TUNE_MIN_WINDOW_HZ = 300.0;
constexpr double AUTO_TUNE_MAX_WINDOW_HZ = 500000.0;

struct SignalSample {
    double frequency = 0.0;
    float level = 0.0f;
};

const std::array<QColor, 17> &legacyWaterfallPalette() {
    static const std::array<QColor, 17> palette = {{
        QColor("#000020"),
        QColor("#000050"),
        QColor("#000090"),
        QColor("#0000F0"),
        QColor("#0000FF"),
        QColor("#50F030"),
        QColor("#1E90FF"),
        QColor("#FFFFFF"),
        QColor("#FFFF00"),
        QColor("#FE6D16"),
        QColor("#FE6D16"),
        QColor("#FF0000"),
        QColor("#FF0000"),
        QColor("#C60000"),
        QColor("#9F0000"),
        QColor("#750000"),
        QColor("#4A0000"),
    }};
    return palette;
}

QColor legacyWaterfallColor(float normalizedValue, float contrastFactor, float sensitivityFactor) {
    if (!std::isfinite(normalizedValue)) {
        normalizedValue = 0.0f;
    }
    const float value = qBound(0.0f, normalizedValue * sensitivityFactor, 1.0f);
    const auto &palette = legacyWaterfallPalette();
    int index = static_cast<int>(value * static_cast<float>(palette.size() - 1));
    index = std::clamp(index, 0, static_cast<int>(palette.size() - 1));

    const QColor baseColor = palette[static_cast<std::size_t>(index)];
    const int blue = (baseColor.green() + baseColor.blue()) / 3;
    const int r = static_cast<int>(baseColor.red() * contrastFactor + blue * (1.0f - contrastFactor));
    const int g = static_cast<int>(baseColor.green() * contrastFactor + blue * (1.0f - contrastFactor));
    const int b = static_cast<int>(baseColor.blue() * contrastFactor + blue * (1.0f - contrastFactor));
    return QColor(r, g, b);
}

void writeWaterfallColor(float normalizedValue,
                         float contrastFactor,
                         float sensitivityFactor,
                         unsigned char *dest) {
    if (!dest) {
        return;
    }
    const QColor color = legacyWaterfallColor(normalizedValue, contrastFactor, sensitivityFactor);
    dest[0] = static_cast<unsigned char>(color.red());
    dest[1] = static_cast<unsigned char>(color.green());
    dest[2] = static_cast<unsigned char>(color.blue());
}
}

MyWaterfallWidget::MyWaterfallWidget(QWidget *parent)
    : QOpenGLWidget(parent), xMin(60000000), xMax(140000000), yMin(-120), yMax(0), contrast(10), sensitivity(10),
      levelMin(-120), levelMax(0), fftLength(32768), initialized(false), secondGraph(false),
      waterfall3DRenderer(std::make_unique<Waterfall3DRenderer>()) {
		waterfallTexture = 0;
    setMouseTracking(true);
    pauseControl = new QToolButton(this);
    pauseControl->setAutoRaise(true);
    pauseControl->setCheckable(true);
    pauseControl->setFixedSize(24, 24);
    pauseControl->hide();
    connect(pauseControl, &QToolButton::toggled, this, &MyWaterfallWidget::setDisplayPaused);
    updatePauseControl();
    fpsOverlayLabel = new QLabel(QStringLiteral("FPS --"), this);
    fpsOverlayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    fpsOverlayLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: rgb(120, 255, 150); background-color: rgba(0, 0, 0, 180); "
        "padding: 3px 6px; border: 1px solid rgba(120, 255, 150, 100); }"));
    fpsOverlayLabel->adjustSize();
    fpsOverlayLabel->hide();
    sliceOverlayLabel = new QLabel(this);
    sliceOverlayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    sliceOverlayLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: white; background-color: rgba(12, 16, 22, 215); "
        "padding: 4px 7px; border: 1px solid rgba(120, 210, 255, 180); }"));
    sliceOverlayLabel->hide();
    sliceDetailsOverlayLabel = new QLabel(this);
    sliceDetailsOverlayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    sliceDetailsOverlayLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: rgb(225, 240, 255); background-color: rgba(0, 0, 0, 190); "
        "padding: 5px 7px; border: 1px solid rgba(120, 210, 255, 120); }"));
    sliceDetailsOverlayLabel->hide();
    areaMeasurementOverlayLabel = new QLabel(this);
    areaMeasurementOverlayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    areaMeasurementOverlayLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    areaMeasurementOverlayLabel->setTextFormat(Qt::PlainText);
    areaMeasurementOverlayLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: rgb(225, 245, 255); background-color: rgba(4, 12, 24, 224); "
        "padding: 5px 7px; border: 1px solid rgba(120, 210, 255, 210); }"));
    areaMeasurementOverlayLabel->hide();
    for (QLabel *&label : alternativeDbLabels) {
        label = new QLabel(this);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setStyleSheet(QStringLiteral(
            "QLabel { color: rgb(220, 238, 242); background-color: rgba(0, 0, 0, 190); "
            "padding-left: 3px; }") );
        label->hide();
    }
    alternativeMeasurementLabel = new QLabel(this);
    alternativeMeasurementLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    alternativeMeasurementLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    alternativeMeasurementLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: rgb(220, 238, 255); background-color: rgba(4, 12, 24, 224); "
        "padding: 0px 7px; border: 1px solid rgba(120, 190, 255, 210); }"));
    alternativeMeasurementLabel->hide();
    alternativeHoverLabel = new QLabel(this);
    alternativeHoverLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    alternativeHoverLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    alternativeHoverLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: rgb(225, 245, 255); background-color: rgba(4, 12, 24, 218); "
        "padding: 0px 6px; border: 1px solid rgba(120, 210, 255, 170); }"));
    alternativeHoverLabel->hide();
}

MyWaterfallWidget::~MyWaterfallWidget() {
    makeCurrent();
    waterfall3DRenderer->releaseGpuResources();
    if (waterfallTexture != 0) {
        glDeleteTextures(1, &waterfallTexture);
        waterfallTexture = 0;
    }
    waterfallVbo.release();
    doneCurrent();
}


void MyWaterfallWidget::wheelEvent(QWheelEvent *event) {
    if ((frequencySliceMouseActive || spectrumFrameSliceMouseActive) &&
        activeDisplayMode != DisplayMode::Waterfall2D) {
        const QPoint angleDelta = event->angleDelta();
        const QPoint pixelDelta = event->pixelDelta();
        const int signedDelta = angleDelta.y() != 0
                                    ? angleDelta.y()
                                    : (angleDelta.x() != 0
                                           ? angleDelta.x()
                                           : (pixelDelta.y() != 0 ? pixelDelta.y() : pixelDelta.x()));
        if (signedDelta == 0) {
            event->accept();
            return;
        }
        {
            QMutexLocker locker(&mutex);
            if (frequencySliceMouseActive) {
                waterfall3DRenderer->stepFrequencySlice(signedDelta > 0 ? 1 : -1);
            } else {
                waterfall3DRenderer->stepSpectrumSlice(signedDelta > 0 ? 1 : -1);
            }
        }
        updateSliceOverlay(sliceOverlayAnchor);
        update();
        event->accept();
        return;
    }
    if (!alternativeInterfaceMode && !fixed3DPlane &&
        activeDisplayMode != DisplayMode::Waterfall2D &&
        event->modifiers().testFlag(Qt::ControlModifier)) {
        {
            QMutexLocker locker(&mutex);
            waterfall3DRenderer->zoomCamera(event->angleDelta().y());
        }
        update();
        event->accept();
        return;
    }
    emit scaleChanged(event->angleDelta().y() > 0 ? 1 : -1);
    event->accept();
}

void MyWaterfallWidget::mousePressEvent(QMouseEvent *event) {
    const bool sliceModifier = event->modifiers().testFlag(Qt::AltModifier) ||
                               event->modifiers().testFlag(Qt::ShiftModifier);
    const bool modifierFreeSlice = modifierFreeSliceInput &&
                                   !event->modifiers().testFlag(Qt::ControlModifier);
    if (areaMeasurementEnabled &&
        activeDisplayMode == DisplayMode::Waterfall2D &&
        !secondGraph &&
        event->button() == Qt::LeftButton) {
        areaMeasurementActive = true;
        areaMeasurementVisible = true;
        areaMeasurementStartPos = event->pos();
        areaMeasurementEndPos = event->pos();
        spectrumPanActive = false;
        spectrumPanButton = Qt::NoButton;
        update();
        event->accept();
        return;
    }
    if (activeDisplayMode != DisplayMode::Waterfall2D &&
        event->button() == Qt::LeftButton &&
        (sliceModifier || modifierFreeSlice)) {
        bool selected = false;
        {
            QMutexLocker locker(&mutex);
            selected = waterfall3DRenderer->beginFrequencySlice(event->x(),
                                                                event->y(),
                                                                width(),
                                                                height());
        }
        if (selected) {
            spectrumFrameSliceMouseActive = false;
            frequencySliceMouseActive = true;
            updateSliceOverlay(event->pos());
            update();
        }
        event->accept();
        return;
    }
    if (activeDisplayMode != DisplayMode::Waterfall2D &&
        event->button() == Qt::RightButton &&
        (sliceModifier || modifierFreeSlice)) {
        bool selected = false;
        {
            QMutexLocker locker(&mutex);
            selected = waterfall3DRenderer->beginSpectrumSlice(event->x(),
                                                               event->y(),
                                                               width(),
                                                               height());
        }
        if (selected) {
            frequencySliceMouseActive = false;
            spectrumFrameSliceMouseActive = true;
            updateSliceOverlay(event->pos());
            update();
        }
        event->accept();
        return;
    }
    if (!alternativeInterfaceMode && !fixed3DPlane &&
        activeDisplayMode != DisplayMode::Waterfall2D &&
        event->button() == Qt::LeftButton &&
        event->modifiers().testFlag(Qt::ControlModifier)) {
        cameraOrbitActive = true;
        cameraOrbitLastPos = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (!alternativeInterfaceMode && !fixed3DPlane &&
        activeDisplayMode != DisplayMode::Waterfall2D &&
        event->button() == Qt::RightButton &&
        event->modifiers().testFlag(Qt::ControlModifier)) {
        cameraPanActive = true;
        cameraPanLastPos = event->pos();
        setCursor(Qt::SizeAllCursor);
        event->accept();
        return;
    }
    if (alternativeInterfaceMode &&
        event->button() == Qt::LeftButton &&
        !sliceModifier &&
        !event->modifiers().testFlag(Qt::ControlModifier) &&
        alternativeSpectrumPlotRect().contains(event->pos())) {
        alternativeSpectrumMeasurementActive = true;
        alternativeSpectrumMeasurementVisible = true;
        alternativeSpectrumMeasureStartPos = event->pos();
        alternativeSpectrumMeasureEndPos = event->pos();
        alternativeSpectrumHoverVisible = true;
        alternativeSpectrumHoverPos = event->pos();
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        spectrumPanActive = true;
        spectrumPanMoved = false;
        spectrumPanButton = event->button();
        spectrumPanLastPos = event->pos();
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        if (tuneContextEnabled)
            emit tuneContextRequested(frequencyAtX(event->x()), event->globalPos());
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        emit autoTuneRequested(signalCenterNearFrequency(frequencyAtX(event->x())));
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void MyWaterfallWidget::mouseMoveEvent(QMouseEvent *event) {
    if (alternativeInterfaceMode) {
        alternativeSpectrumHoverVisible = alternativeSpectrumPlotRect().contains(event->pos());
        alternativeSpectrumHoverPos = event->pos();
    }
    if (alternativeSpectrumMeasurementActive) {
        alternativeSpectrumMeasurementVisible = true;
        alternativeSpectrumMeasureEndPos = event->pos();
        update();
        event->accept();
        return;
    }
    if (areaMeasurementActive) {
        areaMeasurementEndPos = event->pos();
        update();
        event->accept();
        return;
    }
    if (cameraPanActive) {
        const QPoint delta = event->pos() - cameraPanLastPos;
        cameraPanLastPos = event->pos();
        if (!delta.isNull()) {
            {
                QMutexLocker locker(&mutex);
                waterfall3DRenderer->panCamera(static_cast<float>(delta.x()),
                                               static_cast<float>(delta.y()),
                                               width(),
                                               height());
            }
            update();
        }
        event->accept();
        return;
    }
    if (cameraOrbitActive) {
        const QPoint delta = event->pos() - cameraOrbitLastPos;
        cameraOrbitLastPos = event->pos();
        if (!delta.isNull()) {
            {
                QMutexLocker locker(&mutex);
                waterfall3DRenderer->orbitCamera(static_cast<float>(delta.x()),
                                                 static_cast<float>(delta.y()));
            }
            update();
        }
        event->accept();
        return;
    }
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
    if (alternativeInterfaceMode) {
        update();
    }
    QOpenGLWidget::mouseMoveEvent(event);
}

void MyWaterfallWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && alternativeSpectrumMeasurementActive) {
        alternativeSpectrumMeasureEndPos = event->pos();
        alternativeSpectrumMeasurementActive = false;
        if (std::abs(alternativeSpectrumMeasureEndPos.x() -
                     alternativeSpectrumMeasureStartPos.x()) < 4) {
            alternativeSpectrumMeasurementVisible = false;
        }
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && areaMeasurementActive) {
        areaMeasurementEndPos = event->pos();
        areaMeasurementActive = false;
        if (std::abs(areaMeasurementEndPos.x() - areaMeasurementStartPos.x()) < 3 ||
            std::abs(areaMeasurementEndPos.y() - areaMeasurementStartPos.y()) < 3) {
            areaMeasurementVisible = false;
        }
        update();
        event->accept();
        return;
    }
    if (spectrumFrameSliceMouseActive && event->button() == Qt::RightButton) {
        {
            QMutexLocker locker(&mutex);
            waterfall3DRenderer->endSpectrumSlice();
        }
        spectrumFrameSliceMouseActive = false;
        hideSliceOverlay();
        update();
        event->accept();
        return;
    }
    if (frequencySliceMouseActive && event->button() == Qt::LeftButton) {
        {
            QMutexLocker locker(&mutex);
            waterfall3DRenderer->endFrequencySlice();
        }
        frequencySliceMouseActive = false;
        hideSliceOverlay();
        update();
        event->accept();
        return;
    }
    if (cameraOrbitActive && event->button() == Qt::LeftButton) {
        cameraOrbitActive = false;
        unsetCursor();
        event->accept();
        return;
    }
    if (cameraPanActive && event->button() == Qt::RightButton) {
        cameraPanActive = false;
        unsetCursor();
        event->accept();
        return;
    }
    if (spectrumPanActive && event->button() == spectrumPanButton) {
        const bool middleClickAutoTune = spectrumPanButton == Qt::MiddleButton && !spectrumPanMoved;
        spectrumPanActive = false;
        spectrumPanButton = Qt::NoButton;
        if (middleClickAutoTune) {
            emit autoTuneRequested(signalCenterNearFrequency(frequencyAtX(event->x())));
        }
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void MyWaterfallWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        alternativeSpectrumMeasurementActive = false;
        alternativeSpectrumMeasurementVisible = false;
        spectrumPanActive = false;
        spectrumPanButton = Qt::NoButton;
        emit autoTuneRequested(signalCenterNearFrequency(frequencyAtX(event->x())));
        event->accept();
        return;
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void MyWaterfallWidget::leaveEvent(QEvent *event) {
    alternativeSpectrumHoverVisible = false;
    update();
    QOpenGLWidget::leaveEvent(event);
}

double MyWaterfallWidget::displayFrequencyAtX(int x) const {
    if (width() <= 0 || qFuzzyCompare(xMin, xMax)) {
        return xMin;
    }
    const double normalized = std::clamp(static_cast<double>(x) / (std::max)(1, width()), 0.0, 1.0);
    return xMin + normalized * (xMax - xMin);
}

double MyWaterfallWidget::actualFrequencyForDisplayFrequency(double displayFrequency) const {
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

double MyWaterfallWidget::displayFrequencyForActualFrequency(double actualFrequency) const {
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

double MyWaterfallWidget::frequencyAtX(int x) const {
    return actualFrequencyForDisplayFrequency(displayFrequencyAtX(x));
}

double MyWaterfallWidget::signalCenterNearFrequency(double frequency) {
    QMutexLocker locker(&mutex);
    if (!std::isfinite(frequency) || qFuzzyCompare(xMin, xMax) || pixelFrequencyData.empty() || pixelLevelData.empty()) {
        return frequency;
    }

    const int dataCount = std::min(static_cast<int>(pixelFrequencyData.size()),
                                   static_cast<int>(pixelLevelData.size()));
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
        const double sampleFrequency = pixelFrequencyData[static_cast<std::size_t>(i)];
        if (!std::isfinite(sampleFrequency) ||
            std::abs(sampleFrequency - targetDisplayFrequency) > halfWindow) {
            continue;
        }
        const float level = pixelLevelData[static_cast<std::size_t>(i)];
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

void MyWaterfallWidget::ensureLineBuffer() {
    const int lineWidth = std::max(1, width());
    const size_t requiredSize = static_cast<size_t>(lineWidth) * 3;
    if (lineData.size() != requiredSize) {
        lineData.assign(requiredSize, 0);
    }
    if (pixelMaxData.size() != static_cast<std::size_t>(lineWidth)) {
        pixelMaxData.resize(static_cast<std::size_t>(lineWidth));
    }
}

void MyWaterfallWidget::initializeGL() {
qDebug() << "MyWaterfallWidget::initializeGL start";
initializeOpenGLFunctions();


    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); 
    ensureLineBuffer();
    glGenTextures(1, &waterfallTexture);
    resetWaterfallTexture(width(), height());
    waterfallVbo.create();
    waterfallVbo.bind();
    waterfallVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    waterfallVbo.allocate(static_cast<int>(16 * sizeof(GLfloat)));
    waterfallVbo.release();
	initialized = true;
qDebug() << "MyWaterfallWidget::initializeGL done";
}

void MyWaterfallWidget::resizeGL(int w, int h) {
        glViewport(0, 0, w, h);
        if (pauseControl) {
            pauseControl->move((std::max)(2, w - pauseControl->width() - 6), 6);
            pauseControl->raise();
        }
        ensureLineBuffer();
        resizeWaterfallTexturePreserve(w, h);
        if (waterfallVbo.isCreated()) {
            waterfallVbo.bind();
            waterfallVbo.allocate(static_cast<int>(16 * sizeof(GLfloat)));
            waterfallVbo.release();
        }
        positionInfoOverlays();
        updateAlternativeDbLabels();
}

void MyWaterfallWidget::resetWaterfallTexture(int w, int h) {
    textureWidth = std::max(1, w);
    textureHeight = std::max(1, h);
    waterfallWriteRow = 0;

    waterfallTexturePixels.assign(static_cast<size_t>(textureWidth) * textureHeight * 3, 0);
    glBindTexture(GL_TEXTURE_2D, waterfallTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, textureWidth, textureHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, waterfallTexturePixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void MyWaterfallWidget::resizeWaterfallTexturePreserve(int w, int h) {
    const int newWidth = std::max(1, w);
    const int newHeight = std::max(1, h);
    if (newWidth == textureWidth && newHeight == textureHeight) {
        return;
    }

    if (waterfallTexture == 0 || textureWidth <= 0 || textureHeight <= 0) {
        resetWaterfallTexture(newWidth, newHeight);
        return;
    }

    const int oldWidth = textureWidth;
    const int oldHeight = textureHeight;
    const int oldWriteRow = waterfallWriteRow;
    const std::deque<qint64> oldRowTimes = waterfallRowTimesMs;
    const size_t oldPixelBytes = static_cast<size_t>(oldWidth) * oldHeight * 3;
    if (waterfallTexturePixels.size() != oldPixelBytes) {
        resetWaterfallTexture(newWidth, newHeight);
        return;
    }

    std::vector<unsigned char> resized(static_cast<size_t>(newWidth) * newHeight * 3, 0);
    for (int y = 0; y < newHeight; ++y) {
        const int sourceVisualY = std::min(oldHeight - 1, static_cast<int>((static_cast<long long>(y) * oldHeight) / newHeight));
        const int sourceRow = (oldWriteRow + sourceVisualY) % oldHeight;
        for (int x = 0; x < newWidth; ++x) {
            const int sourceX = std::min(oldWidth - 1, static_cast<int>((static_cast<long long>(x) * oldWidth) / newWidth));
            const size_t sourceIndex = (static_cast<size_t>(sourceRow) * oldWidth + sourceX) * 3;
            const size_t destIndex = (static_cast<size_t>(y) * newWidth + x) * 3;
            resized[destIndex + 0] = waterfallTexturePixels[sourceIndex + 0];
            resized[destIndex + 1] = waterfallTexturePixels[sourceIndex + 1];
            resized[destIndex + 2] = waterfallTexturePixels[sourceIndex + 2];
        }
    }

    textureWidth = newWidth;
    textureHeight = newHeight;
    waterfallWriteRow = 0;
    waterfallTexturePixels = std::move(resized);
    waterfallRowTimesMs.clear();
    if (!oldRowTimes.empty()) {
        const int preservedRows = (std::clamp)(
            static_cast<int>(std::lround(static_cast<double>(oldRowTimes.size()) *
                                         static_cast<double>(newHeight) /
                                         static_cast<double>((std::max)(1, oldHeight)))),
            1,
            newHeight);
        for (int row = 0; row < preservedRows; ++row) {
            const std::size_t sourceIndex = (std::min)(
                oldRowTimes.size() - 1,
                static_cast<std::size_t>(
                    static_cast<long long>(row) * static_cast<long long>(oldRowTimes.size()) /
                    (std::max)(1, preservedRows)));
            waterfallRowTimesMs.push_back(oldRowTimes[sourceIndex]);
        }
    }
    glBindTexture(GL_TEXTURE_2D, waterfallTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, textureWidth, textureHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, waterfallTexturePixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void MyWaterfallWidget::setData(const std::vector<float> &sourceXData,
                                const std::vector<float> &sourceYData,
                                double xMin,
                                double xMax,
                                int fftLength,
                                bool secondGraph,
                                bool colorSpectrum,
                                float contrast,
                                float sensitivity,
                                float levelMin,
                                 float levelMax,
                                 bool displayOrdered) {
    if (displayPaused) return;
    bool shouldScheduleUpdate = false;
    bool shouldRefreshSliceOverlay = false;
    {
        QMutexLocker locker(&mutex);
        this->xMin = xMin;
        this->xMax = xMax;
        this->fftLength = std::max(0, fftLength);
        this->secondGraph = secondGraph;
        waterfall3DRenderer->setFixedFrontExpanded(
            (alternativeInterfaceMode || fixed3DPlane) && !secondGraph);
        this->colorSpectrum = colorSpectrum;
        this->contrast = contrast;
        this->sensitivity = sensitivity;
        if (std::isfinite(levelMin) && std::isfinite(levelMax) && levelMax > levelMin) {
            this->levelMin = levelMin;
            this->levelMax = levelMax;
            yMin = levelMin;
            yMax = levelMax;
        }

        ensureLineBuffer();

        const int lineWidth = std::max(1, width());
        const int dataCount = std::min({this->fftLength,
                                        static_cast<int>(sourceXData.size()),
                                        static_cast<int>(sourceYData.size())});
        const bool validFrame =
            !lineData.empty() &&
            dataCount > 0 &&
            !qFuzzyCompare(this->xMin, this->xMax);

        if (!validFrame) {
            pixelFrequencyData.clear();
            pixelLevelData.clear();
            pendingTextureLine = !pendingTextureLines.empty();
        } else {
            if (pixelMaxData.size() != static_cast<std::size_t>(lineWidth)) {
                pixelMaxData.resize(static_cast<std::size_t>(lineWidth));
            }
            if (pixelFrequencyData.size() != static_cast<std::size_t>(lineWidth)) {
                pixelFrequencyData.resize(static_cast<std::size_t>(lineWidth));
            }
            if (pixelLevelData.size() != static_cast<std::size_t>(lineWidth)) {
                pixelLevelData.resize(static_cast<std::size_t>(lineWidth));
            }

            std::fill(pixelMaxData.begin(),
                      pixelMaxData.end(),
                      std::numeric_limits<float>::quiet_NaN());

            for (int id = 0; id < dataCount; ++id) {
                if (!displayOrdered && !std::isfinite(sourceXData[static_cast<std::size_t>(id)])) {
                    continue;
                }
                const int x1 = displayOrdered
                                   ? std::clamp(static_cast<int>(
                                                    static_cast<long long>(id) * lineWidth /
                                                    (std::max)(1, dataCount)),
                                                0,
                                                lineWidth - 1)
                                   : static_cast<int>((sourceXData[static_cast<std::size_t>(id)] - this->xMin) *
                                                      lineWidth /
                                                      (this->xMax - this->xMin));
                if (x1 < 0 || x1 >= lineWidth) {
                    continue;
                }
                const int shiftedIndex = displayOrdered ? id : (id + dataCount / 2) % dataCount;
                const float value = sourceYData[static_cast<std::size_t>(shiftedIndex)];
                if (std::isfinite(value)) {
                    float &pixelValue = pixelMaxData[static_cast<std::size_t>(x1)];
                    pixelValue = std::isfinite(pixelValue) ? (std::max)(pixelValue, value) : value;
                }
            }

            int previousFilled = -1;
            float previousValue = this->levelMin;
            for (int x = 0; x < lineWidth; ++x) {
                const float value = pixelMaxData[static_cast<std::size_t>(x)];
                if (!std::isfinite(value)) {
                    continue;
                }

                if (previousFilled < 0) {
                    for (int fill = 0; fill < x; ++fill) {
                        pixelMaxData[static_cast<std::size_t>(fill)] = value;
                    }
                } else if (x - previousFilled > 1) {
                    const int gap = x - previousFilled;
                    for (int fill = previousFilled + 1; fill < x; ++fill) {
                        const float t = static_cast<float>(fill - previousFilled) / static_cast<float>(gap);
                        pixelMaxData[static_cast<std::size_t>(fill)] =
                            previousValue + (value - previousValue) * t;
                    }
                }

                previousFilled = x;
                previousValue = value;
            }

            if (previousFilled < 0) {
                std::fill(pixelMaxData.begin(), pixelMaxData.end(), this->levelMin);
            } else {
                for (int fill = previousFilled + 1; fill < lineWidth; ++fill) {
                    pixelMaxData[static_cast<std::size_t>(fill)] = previousValue;
                }
            }

            const float contrastFactor = this->contrast / 10.0f;
            const float sensitivityFactor = this->sensitivity / 10.0f;
            const double span = this->xMax - this->xMin;
            for (int x = 0; x < lineWidth; ++x) {
                const std::size_t index = static_cast<std::size_t>(x);
                pixelFrequencyData[index] = static_cast<float>(
                    this->xMin + (static_cast<double>(x) + 0.5) * span / static_cast<double>(lineWidth));
                pixelLevelData[index] = pixelMaxData[index];
                writeWaterfallColor(normalizedLevel(pixelMaxData[index]),
                                    contrastFactor,
                                    sensitivityFactor,
                                    &lineData[index * 3]);
            }
            if (activeDisplayMode != DisplayMode::Waterfall2D) {
                waterfall3DRenderer->appendRow(pixelLevelData,
                                               lineData,
                                               this->levelMin,
                                               this->levelMax,
                                               rowsPerFrame);
            }
            queueCurrentTextureLine();
            ++waterfallFftFramesSinceRateUpdate;
            waterfallRowsSinceRateUpdate += static_cast<quint64>((std::max)(1, rowsPerFrame));
        }

        if (!updateQueued) {
            updateQueued = true;
            shouldScheduleUpdate = true;
        }
        if ((frequencySliceMouseActive || spectrumFrameSliceMouseActive) &&
            (!sliceOverlayUpdateTimer.isValid() || sliceOverlayUpdateTimer.elapsed() >= 200)) {
            sliceOverlayUpdateTimer.restart();
            shouldRefreshSliceOverlay = true;
        }
    }

    if (shouldRefreshSliceOverlay) {
        updateSliceOverlay(sliceOverlayAnchor);
    }
    if (shouldScheduleUpdate) {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
}

void MyWaterfallWidget::queueDiscontinuityRow(DiscontinuityKind kind) {
    if (displayPaused) return;
    bool shouldScheduleUpdate = false;
    {
        QMutexLocker locker(&mutex);
        ensureLineBuffer();
        if (lineData.empty()) return;

        const unsigned char red = kind == DiscontinuityKind::ProcessingQueueDrop ? 238 : 72;
        const unsigned char green = kind == DiscontinuityKind::ProcessingQueueDrop ? 42 : 72;
        const unsigned char blue = kind == DiscontinuityKind::ProcessingQueueDrop ? 42 : 72;
        for (std::size_t index = 0; index + 2 < lineData.size(); index += 3) {
            lineData[index] = red;
            lineData[index + 1] = green;
            lineData[index + 2] = blue;
        }

        if (activeDisplayMode != DisplayMode::Waterfall2D) {
            const int width = (std::max)(1, static_cast<int>(lineData.size() / 3));
            pixelLevelData.assign(static_cast<std::size_t>(width), levelMin);
            waterfall3DRenderer->appendRow(pixelLevelData,
                                           lineData,
                                           levelMin,
                                           levelMax,
                                           rowsPerFrame);
        }
        queueCurrentTextureLine();
        waterfallRowsSinceRateUpdate += static_cast<quint64>((std::max)(1, rowsPerFrame));
        if (!updateQueued) {
            updateQueued = true;
            shouldScheduleUpdate = true;
        }
    }
    if (shouldScheduleUpdate) {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
}

void MyWaterfallWidget::setPauseControlVisible(bool visible) {
    if (!pauseControl) return;
    pauseControl->setVisible(visible);
    if (visible) pauseControl->raise();
}

void MyWaterfallWidget::setDisplayPaused(bool paused) {
    if (displayPaused == paused) {
        updatePauseControl();
        return;
    }
    displayPaused = paused;
    if (pauseControl && pauseControl->isChecked() != paused) {
        pauseControl->blockSignals(true);
        pauseControl->setChecked(paused);
        pauseControl->blockSignals(false);
    }
    updatePauseControl();
    emit displayPausedChanged(paused);
}

void MyWaterfallWidget::updatePauseControl() {
    if (!pauseControl) return;
    pauseControl->setIcon(style()->standardIcon(displayPaused
                                                    ? QStyle::SP_MediaPlay
                                                    : QStyle::SP_MediaPause));
    pauseControl->setToolTip(displayPaused
                                 ? QString::fromUtf8(u8"Відновити оновлення водоспаду")
                                 : QString::fromUtf8(u8"Призупинити лише відображення водоспаду"));
}

void MyWaterfallWidget::setRowsPerFrame(int rows) {
    const int clampedRows = (std::clamp)(rows, 1, 8);
    QMutexLocker locker(&mutex);
    rowsPerFrame = clampedRows;
}

void MyWaterfallWidget::setSpectrumMetadata(double centerFrequencyHz,
                                            double listeningFrequencyHz,
                                            double sampleRateHz,
                                            int sourceFftLength,
                                            int fftWindowType) {
    QMutexLocker locker(&mutex);
    metadataCenterFrequencyHz = centerFrequencyHz;
    metadataListeningFrequencyHz = listeningFrequencyHz;
    metadataSampleRateHz = sampleRateHz;
    metadataFftLength = (std::max)(0, sourceFftLength);
    metadataFftWindowType = normalizedFftWindowType(fftWindowType);
}

void MyWaterfallWidget::setFpsOverlayEnabled(bool enabled) {
    if (fpsOverlayEnabled == enabled) {
        return;
    }
    fpsOverlayEnabled = enabled;
    fpsElapsedTimer.invalidate();
    waterfallRowsElapsedTimer.invalidate();
    fpsFrameCount = 0;
    displayedFps = 0.0;
    waterfallFftFramesSinceRateUpdate = 0;
    displayedWaterfallFftFramesPerSecond = 0.0;
    waterfallRowsSinceRateUpdate = 0;
    displayedWaterfallRowsPerSecond = 0.0;
    if (fpsOverlayLabel) {
        fpsOverlayLabel->setText(QStringLiteral("FPS --"));
        fpsOverlayLabel->adjustSize();
        positionInfoOverlays();
        fpsOverlayLabel->setVisible(enabled);
        fpsOverlayLabel->raise();
    }
    update();
}

void MyWaterfallWidget::setAreaMeasurementEnabled(bool enabled) {
    if (areaMeasurementEnabled == enabled) {
        return;
    }
    areaMeasurementEnabled = enabled;
    areaMeasurementActive = false;
    if (!enabled) {
        areaMeasurementVisible = false;
        if (areaMeasurementOverlayLabel) {
            areaMeasurementOverlayLabel->hide();
        }
    }
    update();
}

void MyWaterfallWidget::setExtendedInfoOverlayEnabled(bool enabled) {
    if (extendedInfoOverlayEnabled == enabled) {
        return;
    }
    extendedInfoOverlayEnabled = enabled;
    if (!enabled && sliceDetailsOverlayLabel) {
        sliceDetailsOverlayLabel->hide();
    } else if (frequencySliceMouseActive || spectrumFrameSliceMouseActive) {
        updateSliceOverlay(sliceOverlayAnchor);
    }
    positionInfoOverlays();
    update();
}

void MyWaterfallWidget::setScienceAnalysisData(const std::vector<float> &maxHold,
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
    QMutexLocker locker(&mutex);
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
    locker.unlock();
    update();
}

void MyWaterfallWidget::setRenderBackend(RenderBackend backend) {
    bool changed = false;
    {
        QMutexLocker locker(&mutex);
        changed = activeRenderBackend != backend;
        activeRenderBackend = backend;
    }
    if (changed) {
        qDebug() << "[Waterfall] render backend"
                 << (backend == RenderBackend::GpuPrepared ? "gpu-prepared" : "cpu-texture");
        update();
    }
}

MyWaterfallWidget::RenderBackend MyWaterfallWidget::renderBackend() const {
    QMutexLocker locker(&mutex);
    return activeRenderBackend;
}

void MyWaterfallWidget::setDisplayMode(DisplayMode mode) {
    {
        QMutexLocker locker(&mutex);
        activeDisplayMode = mode;
        waterfall3DRenderer->endFrequencySlice();
        waterfall3DRenderer->endSpectrumSlice();
        frequencySliceMouseActive = false;
        spectrumFrameSliceMouseActive = false;
    }
    if (mode == DisplayMode::Waterfall2D) {
        hideSliceOverlay();
    }
    update();
}

MyWaterfallWidget::DisplayMode MyWaterfallWidget::displayMode() const {
    QMutexLocker locker(&mutex);
    return activeDisplayMode;
}

void MyWaterfallWidget::set3DResolutionDivisor(int divisor) {
    {
        QMutexLocker locker(&mutex);
        waterfall3DRenderer->setResolutionDivisor(divisor);
    }
    update();
}

void MyWaterfallWidget::set3DHistoryRows(int rows) {
    {
        QMutexLocker locker(&mutex);
        waterfall3DRenderer->setHistoryCapacity(rows);
    }
    update();
}

void MyWaterfallWidget::set3DSurfaceStyle(int style) {
    {
        QMutexLocker locker(&mutex);
        waterfall3DRenderer->setSurfaceStyle(style);
    }
    update();
}

void MyWaterfallWidget::set3DSurfaceSmoothing(int smoothing) {
    {
        QMutexLocker locker(&mutex);
        waterfall3DRenderer->setSurfaceSmoothing(smoothing);
    }
    update();
}

void MyWaterfallWidget::set3DSurfaceLighting(int lighting) {
    {
        QMutexLocker locker(&mutex);
        waterfall3DRenderer->setSurfaceLighting(lighting);
    }
    update();
}

void MyWaterfallWidget::set3DSliceScrollStep(int points) {
    QMutexLocker locker(&mutex);
    waterfall3DRenderer->setSliceScrollStep(points);
}

void MyWaterfallWidget::set3DSliceWidth(int points) {
    QMutexLocker locker(&mutex);
    waterfall3DRenderer->setSliceWidth(points);
}

void MyWaterfallWidget::set3DSpectrumSliceScrollStep(int rows) {
    QMutexLocker locker(&mutex);
    waterfall3DRenderer->setSpectrumSliceScrollStep(rows);
}

void MyWaterfallWidget::set3DSpectrumSliceWidth(int rows) {
    QMutexLocker locker(&mutex);
    waterfall3DRenderer->setSpectrumSliceWidth(rows);
}

void MyWaterfallWidget::set3DSpectrumSliceCapture(bool enabled) {
    QMutexLocker locker(&mutex);
    waterfall3DRenderer->setSpectrumSliceCapture(enabled);
}

void MyWaterfallWidget::set3DSpectrumSliceCaptureFixed(bool enabled) {
    QMutexLocker locker(&mutex);
    waterfall3DRenderer->setSpectrumSliceCaptureFixed(enabled);
}

void MyWaterfallWidget::set3DModifierFreeSliceInput(bool enabled) {
    QMutexLocker locker(&mutex);
    modifierFreeSliceInput = enabled;
}

void MyWaterfallWidget::set3DFixedPlane(bool enabled) {
    {
        QMutexLocker locker(&mutex);
        fixed3DPlane = enabled;
        cameraOrbitActive = false;
        cameraPanActive = false;
        waterfall3DRenderer->setFixedFrontPresentation(alternativeInterfaceMode || fixed3DPlane);
        waterfall3DRenderer->setFixedFrontExpanded(
            (alternativeInterfaceMode || fixed3DPlane) && !secondGraph);
        waterfall3DRenderer->setFrontFaceGradient(
            fixed3DPlane && !alternativeInterfaceMode && alternativeSpectrumGradientFill,
            alternativeSpectrumGradientOpacity);
    }
    unsetCursor();
    update();
}

void MyWaterfallWidget::set3DMonochrome(bool enabled) {
    {
        QMutexLocker locker(&mutex);
        waterfall3DRenderer->setMonochrome(enabled);
    }
    update();
}
void MyWaterfallWidget::setAlternativeInterfaceMode(bool enabled) {
    {
        QMutexLocker locker(&mutex);
        alternativeInterfaceMode = enabled;
        cameraOrbitActive = false;
        cameraPanActive = false;
        waterfall3DRenderer->setFixedFrontPresentation(enabled || fixed3DPlane);
        waterfall3DRenderer->setFixedFrontExpanded((enabled || fixed3DPlane) && !secondGraph);
        waterfall3DRenderer->setFrontFaceGradient(
            fixed3DPlane && !enabled && alternativeSpectrumGradientFill,
            alternativeSpectrumGradientOpacity);
    }
    unsetCursor();
    update();
}

void MyWaterfallWidget::setAlternativeSpectrumGradientFill(bool enabled) {
    QMutexLocker locker(&mutex);
    alternativeSpectrumGradientFill = enabled;
    waterfall3DRenderer->setFrontFaceGradient(
        fixed3DPlane && !alternativeInterfaceMode && enabled,
        alternativeSpectrumGradientOpacity);
    locker.unlock();
    update();
}

void MyWaterfallWidget::updateAlternativeDbLabels() {
    const bool visible = alternativeInterfaceMode &&
                         activeDisplayMode != DisplayMode::Waterfall2D &&
                         !secondGraph && width() >= 120 && height() >= 120;
    if (!visible) {
        for (QLabel *label : alternativeDbLabels) {
            if (label) {
                label->hide();
            }
        }
        return;
    }

    const QRect plotRect = alternativeSpectrumPlotRect();
    constexpr int LabelWidth = 56;
    constexpr int LabelHeight = 18;
    const int divisions = static_cast<int>(alternativeDbLabels.size()) - 1;
    for (int division = 0; division <= divisions; ++division) {
        QLabel *label = alternativeDbLabels[static_cast<std::size_t>(division)];
        if (!label) {
            continue;
        }
        const double ratio = 1.0 - static_cast<double>(division) / divisions;
        const double level = levelMin + ratio * (levelMax - levelMin);
        const int y = plotRect.top() + division * plotRect.height() / divisions;
        label->setText(QStringLiteral("%1 dB").arg(level, 0, 'f', 0));
        label->setGeometry(3, y - LabelHeight / 2, LabelWidth, LabelHeight);
        label->show();
        label->raise();
    }
}

void MyWaterfallWidget::updateAlternativeBandLabels() {
    const bool visible = alternativeInterfaceMode &&
                         activeDisplayMode != DisplayMode::Waterfall2D &&
                         !secondGraph && !bandMarkers.isEmpty() &&
                         (generalBandMarkersEnabled || amateurBandMarkersEnabled) &&
                         !qFuzzyCompare(xMin, xMax);
    if (!visible) {
        for (QLabel *label : alternativeBandLabels) {
            if (label) label->hide();
        }
        return;
    }

    while (alternativeBandLabels.size() < static_cast<std::size_t>(bandMarkers.size())) {
        auto *label = new QLabel(this);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setStyleSheet(QStringLiteral(
            "QLabel { background-color: rgba(0, 0, 0, 170); padding: 0px 3px; }"));
        label->hide();
        alternativeBandLabels.push_back(label);
    }

    const QRect plotRect = alternativeSpectrumPlotRect();
    const int markerTop = compactBandMarkersEnabled
                              ? (std::max)(plotRect.top(), plotRect.bottom() - 22)
                              : plotRect.top();
    const double viewStart = (std::min)(xMin, xMax);
    const double viewEnd = (std::max)(xMin, xMax);
    const double span = xMax - xMin;
    auto frequencyToX = [&](double frequency) {
        return plotRect.left() +
               (frequency - xMin) / span * static_cast<double>(plotRect.width());
    };

    std::size_t labelIndex = 0;
    for (const GraphBandMarker &marker : bandMarkers) {
        if ((marker.amateur && !amateurBandMarkersEnabled) ||
            (!marker.amateur && !generalBandMarkersEnabled) ||
            !std::isfinite(marker.startHz) || !std::isfinite(marker.endHz) ||
            marker.endHz <= marker.startHz || marker.endHz < viewStart ||
            marker.startHz > viewEnd || marker.label.trimmed().isEmpty()) {
            continue;
        }

        int x1 = static_cast<int>(std::floor(frequencyToX((std::max)(marker.startHz, viewStart))));
        int x2 = static_cast<int>(std::ceil(frequencyToX((std::min)(marker.endHz, viewEnd))));
        x1 = std::clamp(x1, plotRect.left(), plotRect.right());
        x2 = std::clamp(x2, plotRect.left(), plotRect.right());
        const int markerWidth = (std::max)(1, x2 - x1);
        if (markerWidth < 44) {
            continue;
        }

        QLabel *label = alternativeBandLabels[labelIndex++];
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText,
                         marker.amateur ? QColor(255, 238, 178, 235)
                                        : QColor(205, 235, 255, 225));
        label->setPalette(palette);
        const int labelWidth = markerWidth - 6;
        label->setText(label->fontMetrics().elidedText(marker.label.trimmed(),
                                                       Qt::ElideRight,
                                                       labelWidth - 6));
        label->setGeometry(x1 + 3, markerTop + 2, labelWidth, 16);
        label->show();
        label->raise();
    }

    while (labelIndex < alternativeBandLabels.size()) {
        alternativeBandLabels[labelIndex++]->hide();
    }
}

void MyWaterfallWidget::updateAlternativeInteractionLabels(
    const std::vector<float> &normalizedLevels) {
    if (!alternativeMeasurementLabel || !alternativeHoverLabel ||
        !alternativeInterfaceMode || normalizedLevels.size() < 2) {
        if (alternativeMeasurementLabel) alternativeMeasurementLabel->hide();
        if (alternativeHoverLabel) alternativeHoverLabel->hide();
        return;
    }

    const QRect plotRect = alternativeSpectrumPlotRect();
    const int left = plotRect.left();
    const int right = plotRect.right();
    const int top = plotRect.top();
    const int bottom = plotRect.bottom();
    const int plotWidth = (std::max)(1, right - left);
    const int plotHeight = (std::max)(1, bottom - top);

    if (alternativeSpectrumMeasurementVisible) {
        int x1 = std::clamp(alternativeSpectrumMeasureStartPos.x(), left, right);
        int x2 = std::clamp(alternativeSpectrumMeasureEndPos.x(), left, right);
        if (std::abs(x2 - x1) >= 4) {
            if (x2 < x1) std::swap(x1, x2);
            const double f1 = frequencyAtX(x1);
            const double f2 = frequencyAtX(x2);
            alternativeMeasurementLabel->setText(
                QStringLiteral("BW %1  %2 - %3")
                    .arg(formatFrequencySpanLabel(std::abs(f2 - f1)))
                    .arg(formatFrequencyLabel((std::min)(f1, f2)))
                    .arg(formatFrequencyLabel((std::max)(f1, f2))));
            alternativeMeasurementLabel->adjustSize();
            const int labelWidth = alternativeMeasurementLabel->width();
            const int labelX = std::clamp(x1 + (x2 - x1 - labelWidth) / 2,
                                          left + 2,
                                          (std::max)(left + 2, right - labelWidth - 2));
            alternativeMeasurementLabel->move(labelX, top + 5);
            alternativeMeasurementLabel->show();
            alternativeMeasurementLabel->raise();
        } else {
            alternativeMeasurementLabel->hide();
        }
    } else {
        alternativeMeasurementLabel->hide();
    }

    if (alternativeSpectrumHoverVisible && plotRect.contains(alternativeSpectrumHoverPos)) {
        const int x = std::clamp(alternativeSpectrumHoverPos.x(), left, right);
        const int pointCount = static_cast<int>(normalizedLevels.size());
        const int dataIndex = std::clamp((x - left) * (pointCount - 1) / plotWidth,
                                         0,
                                         pointCount - 1);
        const float normalized = std::clamp(normalizedLevels[static_cast<std::size_t>(dataIndex)],
                                            0.0f,
                                            1.0f);
        const float level = levelMin + normalized * (levelMax - levelMin);
        const int y = bottom - static_cast<int>(std::lround(normalized * plotHeight));
        alternativeHoverLabel->setText(
            QStringLiteral("%1   %2 dB")
                .arg(formatFrequencyLabel(frequencyAtX(x)))
                .arg(level, 0, 'f', 1));
        alternativeHoverLabel->adjustSize();
        const int labelWidth = alternativeHoverLabel->width();
        const int labelHeight = alternativeHoverLabel->height();
        int labelX = x + 8;
        if (labelX + labelWidth > right) labelX = x - labelWidth - 8;
        labelX = std::clamp(labelX, left + 2, (std::max)(left + 2, right - labelWidth));
        const int labelY = std::clamp(y - labelHeight - 4,
                                      top + 2,
                                      (std::max)(top + 2, bottom - labelHeight - 2));
        alternativeHoverLabel->move(labelX, labelY);
        alternativeHoverLabel->show();
        alternativeHoverLabel->raise();
    } else {
        alternativeHoverLabel->hide();
    }
}

void MyWaterfallWidget::setAlternativeSpectrumGradientOpacity(int percent) {
    QMutexLocker locker(&mutex);
    alternativeSpectrumGradientOpacity = std::clamp(percent, 0, 100);
    waterfall3DRenderer->setFrontFaceGradient(
        fixed3DPlane && !alternativeInterfaceMode && alternativeSpectrumGradientFill,
        alternativeSpectrumGradientOpacity);
    locker.unlock();
    update();
}

void MyWaterfallWidget::setBandMarkersEnabled(bool generalEnabled, bool amateurEnabled) {
    generalBandMarkersEnabled = generalEnabled;
    amateurBandMarkersEnabled = amateurEnabled;
    update();
}

void MyWaterfallWidget::setBandMarkersCompact(bool compact) {
    compactBandMarkersEnabled = compact;
    update();
}

void MyWaterfallWidget::setBandMarkers(const QVector<GraphBandMarker> &markers) {
    bandMarkers = markers;
    update();
}

bool MyWaterfallWidget::ensureGpuWaterfallProgram() {
    if (waterfallProgramReady) {
        return true;
    }
    if (waterfallProgramTried) {
        return false;
    }
    waterfallProgramTried = true;

    static const char *vertexSource =
        "attribute vec2 position;\n"
        "attribute vec2 texcoord;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "    gl_Position = vec4(position, 0.0, 1.0);\n"
        "    vTexCoord = texcoord;\n"
        "}\n";
    static const char *fragmentSource =
        "uniform sampler2D waterfallTextureSampler;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(waterfallTextureSampler, vTexCoord);\n"
        "}\n";

    if (!waterfallProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexSource) ||
        !waterfallProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentSource) ||
        !waterfallProgram.link()) {
        qDebug() << "[Waterfall] GPU program unavailable; CPU texture fallback"
                 << waterfallProgram.log();
        waterfallProgram.removeAllShaders();
        return false;
    }

    waterfallProgramReady = true;
    qDebug() << "[Waterfall] GPU program ready";
    return true;
}

bool MyWaterfallWidget::drawGpuPreparedWaterfall(float vStart) {
    if (!ensureGpuWaterfallProgram()) {
        return false;
    }

    const GLfloat vertices[] = {
        -1.0f, -1.0f,  0.0f, vStart + 1.0f,
         1.0f, -1.0f,  1.0f, vStart + 1.0f,
         1.0f,  1.0f,  1.0f, vStart,
        -1.0f,  1.0f,  0.0f, vStart
    };
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    if (!waterfallVbo.isCreated()) {
        waterfallVbo.create();
        waterfallVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    }
    waterfallVbo.bind();
    waterfallVbo.allocate(vertices, static_cast<int>(sizeof(vertices)));
    waterfallProgram.bind();
    waterfallProgram.setUniformValue("waterfallTextureSampler", 0);
    const int positionLocation = waterfallProgram.attributeLocation("position");
    const int texcoordLocation = waterfallProgram.attributeLocation("texcoord");
    if (positionLocation < 0 || texcoordLocation < 0) {
        waterfallProgram.release();
        waterfallVbo.release();
        return false;
    }
    waterfallProgram.enableAttributeArray(positionLocation);
    waterfallProgram.enableAttributeArray(texcoordLocation);
    waterfallProgram.setAttributeBuffer(positionLocation, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
    waterfallProgram.setAttributeBuffer(texcoordLocation,
                                        GL_FLOAT,
                                        2 * static_cast<int>(sizeof(GLfloat)),
                                        2,
                                        4 * static_cast<int>(sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    waterfallProgram.disableAttributeArray(texcoordLocation);
    waterfallProgram.disableAttributeArray(positionLocation);
    waterfallProgram.release();
    waterfallVbo.release();
    return true;
}

void MyWaterfallWidget::drawMiniWaterfallOverlay(float vStart) {
    if (waterfallTexture == 0 || width() <= 0 || height() <= 0) {
        return;
    }

    const float margin = 12.0f;
    const float overlayWidth = std::clamp(width() * 0.34f, 220.0f, 520.0f);
    const float overlayHeight = std::clamp(height() * 0.30f, 90.0f, 260.0f);
    const float left = margin;
    const float right = margin + overlayWidth;
    const float top = static_cast<float>(height()) - margin;
    const float bottom = top - overlayHeight;
    const float border = 3.0f;

    glViewport(0, 0, width(), height());
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, width(), 0.0, height(), -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glDisable(GL_TEXTURE_2D);
    glColor4f(0.02f, 0.025f, 0.035f, 0.88f);
    glBegin(GL_QUADS);
    glVertex2f(left - border, bottom - border);
    glVertex2f(right + border, bottom - border);
    glVertex2f(right + border, top + border);
    glVertex2f(left - border, top + border);
    glEnd();

    glColor4f(0.72f, 0.76f, 0.82f, 0.85f);
    glLineWidth(1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(left - border, bottom - border);
    glVertex2f(right + border, bottom - border);
    glVertex2f(right + border, top + border);
    glVertex2f(left - border, top + border);
    glEnd();

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, waterfallTexture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, vStart + 1.0f); glVertex2f(left, bottom);
    glTexCoord2f(1.0f, vStart + 1.0f); glVertex2f(right, bottom);
    glTexCoord2f(1.0f, vStart); glVertex2f(right, top);
    glTexCoord2f(0.0f, vStart); glVertex2f(left, top);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

void MyWaterfallWidget::setLevelRange(float minLevel, float maxLevel) {
    bool shouldScheduleUpdate = false;
    {
        QMutexLocker locker(&mutex);
        if (!std::isfinite(minLevel) || !std::isfinite(maxLevel) || maxLevel <= minLevel) {
            return;
        }
        levelMin = minLevel;
        levelMax = maxLevel;
        yMin = minLevel;
        yMax = maxLevel;
        if (!updateQueued) {
            updateQueued = true;
            shouldScheduleUpdate = true;
        }
    }
    if (shouldScheduleUpdate) {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
}

void MyWaterfallWidget::setScanSegments(const QVector<ScanVisualSegment> &segments) {
    {
        QMutexLocker locker(&mutex);
        scanSegments = segments;
    }
    update();
}

void MyWaterfallWidget::setScanSegmentMarkersVisible(bool visible) {
    {
        QMutexLocker locker(&mutex);
        if (scanSegmentMarkersVisible == visible) {
            return;
        }
        scanSegmentMarkersVisible = visible;
    }
    update();
}

void MyWaterfallWidget::clearData() {
    {
        QMutexLocker locker(&mutex);
        pixelFrequencyData.clear();
        pixelLevelData.clear();
        fftLength = 0;
        std::fill(lineData.begin(), lineData.end(), 0);
        pendingTextureLines.clear();
        pendingTextureLineTimesMs.clear();
        waterfallRowTimesMs.clear();
        pendingTextureLine = false;
        textureClearRequested = true;
        waterfall3DRenderer->clear();
    }
    update();
}

void MyWaterfallWidget::queueCurrentTextureLine() {
    if (lineData.empty()) {
        return;
    }
    const int effectiveHeight = (std::max)(1, textureHeight > 0 ? textureHeight : height());
    const int effectiveRows = (std::clamp)(rowsPerFrame, 1, effectiveHeight);
    const std::size_t maximumPendingLines = static_cast<std::size_t>(
        (std::clamp)(effectiveHeight / effectiveRows, 1, 512));
    while (pendingTextureLines.size() >= maximumPendingLines) {
        pendingTextureLines.pop_front();
        if (!pendingTextureLineTimesMs.empty()) {
            pendingTextureLineTimesMs.pop_front();
        }
    }
    pendingTextureLines.push_back(lineData);
    pendingTextureLineTimesMs.push_back(QDateTime::currentMSecsSinceEpoch());
    pendingTextureLine = true;
}

void MyWaterfallWidget::uploadPendingTextureLine() {
    ensureLineBuffer();
    const int texWidth = std::max(1, width());
    const int texHeight = std::max(1, height());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (texWidth != textureWidth || texHeight != textureHeight) {
        resizeWaterfallTexturePreserve(texWidth, texHeight);
    }
    if (!pendingTextureLine || pendingTextureLines.empty()) {
        pendingTextureLine = false;
        return;
    }

    glBindTexture(GL_TEXTURE_2D, waterfallTexture);
    const int rowsToWrite = (std::clamp)(rowsPerFrame, 1, (std::max)(1, textureHeight));
    const std::size_t maximumLines = static_cast<std::size_t>((std::max)(1, textureHeight / rowsToWrite));
    while (pendingTextureLines.size() > maximumLines) {
        pendingTextureLines.pop_front();
        if (!pendingTextureLineTimesMs.empty()) {
            pendingTextureLineTimesMs.pop_front();
        }
    }

    const std::size_t rowBytes = static_cast<std::size_t>(textureWidth) * 3U;
    std::vector<unsigned char> resizedLine;
    while (!pendingTextureLines.empty()) {
        std::vector<unsigned char> queuedLine = std::move(pendingTextureLines.front());
        pendingTextureLines.pop_front();
        const qint64 queuedTimeMs = pendingTextureLineTimesMs.empty()
                                          ? QDateTime::currentMSecsSinceEpoch()
                                          : pendingTextureLineTimesMs.front();
        if (!pendingTextureLineTimesMs.empty()) {
            pendingTextureLineTimesMs.pop_front();
        }
        const std::vector<unsigned char> *sourceLine = &queuedLine;
        if (queuedLine.size() != rowBytes) {
            const int sourceWidth = static_cast<int>(queuedLine.size() / 3U);
            resizedLine.assign(rowBytes, 0);
            if (sourceWidth > 0) {
                for (int x = 0; x < textureWidth; ++x) {
                    const int sourceX = (std::min)(sourceWidth - 1,
                        static_cast<int>((static_cast<long long>(x) * sourceWidth) / textureWidth));
                    const std::size_t sourceOffset = static_cast<std::size_t>(sourceX) * 3U;
                    const std::size_t destinationOffset = static_cast<std::size_t>(x) * 3U;
                    resizedLine[destinationOffset] = queuedLine[sourceOffset];
                    resizedLine[destinationOffset + 1] = queuedLine[sourceOffset + 1];
                    resizedLine[destinationOffset + 2] = queuedLine[sourceOffset + 2];
                }
            }
            sourceLine = &resizedLine;
        }

        if (rowsToWrite <= 1) {
            waterfallWriteRow = (waterfallWriteRow + textureHeight - 1) % textureHeight;
            const std::size_t destinationOffset = static_cast<std::size_t>(waterfallWriteRow) * rowBytes;
            std::copy(sourceLine->begin(), sourceLine->end(),
                      waterfallTexturePixels.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
            glTexSubImage2D(GL_TEXTURE_2D,
                            0,
                            0,
                            waterfallWriteRow,
                            textureWidth,
                            1,
                            GL_RGB,
                            GL_UNSIGNED_BYTE,
                            sourceLine->data());
            waterfallRowTimesMs.push_front(queuedTimeMs);
            while (waterfallRowTimesMs.size() > static_cast<std::size_t>(textureHeight)) {
                waterfallRowTimesMs.pop_back();
            }
            continue;
        }

        const std::size_t uploadBytes = rowBytes * static_cast<std::size_t>(rowsToWrite);
        if (textureUploadRows.size() != uploadBytes) {
            textureUploadRows.resize(uploadBytes);
        }
        for (int row = 0; row < rowsToWrite; ++row) {
            auto rowBegin = textureUploadRows.begin() + static_cast<std::ptrdiff_t>(rowBytes * row);
            std::copy(sourceLine->begin(), sourceLine->end(), rowBegin);
        }

        const int startRow = (waterfallWriteRow + textureHeight - rowsToWrite) % textureHeight;
        waterfallWriteRow = startRow;
        const int firstRunRows = (std::min)(rowsToWrite, textureHeight - startRow);
        std::copy_n(textureUploadRows.begin(),
                    static_cast<std::ptrdiff_t>(rowBytes * static_cast<std::size_t>(firstRunRows)),
                    waterfallTexturePixels.begin() + static_cast<std::ptrdiff_t>(rowBytes * static_cast<std::size_t>(startRow)));
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        startRow,
                        textureWidth,
                        firstRunRows,
                        GL_RGB,
                        GL_UNSIGNED_BYTE,
                        textureUploadRows.data());
        const int wrappedRows = rowsToWrite - firstRunRows;
        if (wrappedRows > 0) {
            std::copy_n(textureUploadRows.begin() + static_cast<std::ptrdiff_t>(rowBytes * static_cast<std::size_t>(firstRunRows)),
                        static_cast<std::ptrdiff_t>(rowBytes * static_cast<std::size_t>(wrappedRows)),
                        waterfallTexturePixels.begin());
            glTexSubImage2D(GL_TEXTURE_2D,
                            0,
                            0,
                            0,
                            textureWidth,
                            wrappedRows,
                            GL_RGB,
                            GL_UNSIGNED_BYTE,
                            textureUploadRows.data() + rowBytes * static_cast<std::size_t>(firstRunRows));
        }
        for (int row = 0; row < rowsToWrite; ++row) {
            waterfallRowTimesMs.push_front(queuedTimeMs);
        }
        while (waterfallRowTimesMs.size() > static_cast<std::size_t>(textureHeight)) {
            waterfallRowTimesMs.pop_back();
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    pendingTextureLine = false;
}

void MyWaterfallWidget::computeLineData() {
    bool shouldScheduleUpdate = false;
	QMutexLocker locker(&mutex);
    ensureLineBuffer();
    if (lineData.empty() || pixelLevelData.empty()) {
        if (!updateQueued) {
            updateQueued = true;
            shouldScheduleUpdate = true;
        }
        locker.unlock();
        if (shouldScheduleUpdate) {
            QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
        }
        return;
    }

	float contrastFactor = contrast/10;
	float sensitivityFactor = sensitivity/10;
    const int lineWidth = std::max(1, width());
    const int dataCount = std::min(lineWidth, static_cast<int>(pixelLevelData.size()));
    if (dataCount < lineWidth) {
        std::fill(lineData.begin(), lineData.end(), 0);
    }
    for (int x = 0; x < dataCount; ++x) {
        writeWaterfallColor(normalizedLevel(pixelLevelData[static_cast<std::size_t>(x)]),
                            contrastFactor,
                            sensitivityFactor,
                            &lineData[static_cast<std::size_t>(x) * 3]);
    }
    queueCurrentTextureLine();
    if (!updateQueued) {
        updateQueued = true;
        shouldScheduleUpdate = true;
    }
    locker.unlock();
    if (shouldScheduleUpdate) {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
}

void MyWaterfallWidget::paintGL() {
	updateFpsCounter();
	updateAlternativeDbLabels();
	if (width() <= 0 || height() <= 0) {
		return;
	}
	QPainter painter(this);
	painter.beginNativePainting();
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    bool drawSegmentOverlay = true;
    std::vector<float> secondGraphLevels;
    std::vector<float> alternativeSpectrumLevels;
    {
        QMutexLocker locker(&mutex);
        updateQueued = false;
        if (textureClearRequested) {
            ensureLineBuffer();
            std::fill(lineData.begin(), lineData.end(), 0);
            pendingTextureLines.clear();
            pendingTextureLineTimesMs.clear();
            waterfallRowTimesMs.clear();
            resetWaterfallTexture(width(), height());
            pendingTextureLine = false;
            textureClearRequested = false;
        }

        uploadPendingTextureLine();

        if (activeDisplayMode != DisplayMode::Waterfall2D) {
            drawSegmentOverlay = false;
            waterfall3DRenderer->render(width(), height());
            if (alternativeInterfaceMode) {
                alternativeSpectrumLevels.reserve(pixelLevelData.size());
                for (const float level : pixelLevelData) {
                    alternativeSpectrumLevels.push_back(normalizedLevel(level));
                }
            }
            if (activeDisplayMode == DisplayMode::Waterfall3DWithMini) {
                const float vStart = static_cast<float>(waterfallWriteRow) /
                                     static_cast<float>((std::max)(1, textureHeight));
                drawMiniWaterfallOverlay(vStart);
            }
        } else if (secondGraph == true) {
	        // Draw the secondary spectrum with QPainter below. The old fixed-function
	        // OpenGL path was not portable to Raspberry Pi and inherited stale state
	        // after switching back from the 3D renderer.
            secondGraphLevels.reserve(pixelLevelData.size());
            for (const float level : pixelLevelData) {
                secondGraphLevels.push_back(normalizedLevel(level));
            }
	    } else {
        //waterfall

        glViewport(0, 0, width(), height());
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, width(), 0, height(), -1, 1);

	    glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, waterfallTexture);
        glColor3f(1.0f, 1.0f, 1.0f);

        const float vStart = static_cast<float>(waterfallWriteRow) / static_cast<float>(textureHeight);
        const bool useGpuPrepared = activeRenderBackend == RenderBackend::GpuPrepared;
        if (!useGpuPrepared || !drawGpuPreparedWaterfall(vStart)) {
            glBegin(GL_QUADS);
            glTexCoord2f(0.0f, vStart + 1.0f); glVertex2f(0, 0);
            glTexCoord2f(1.0f, vStart + 1.0f); glVertex2f(width(), 0);
            glTexCoord2f(1.0f, vStart); glVertex2f(width(), height());
            glTexCoord2f(0.0f, vStart); glVertex2f(0, height());
            glEnd();
        }
            glBindTexture(GL_TEXTURE_2D, 0);
	    }
    }
    painter.endNativePainting();
    updateAlternativeBandLabels();
    updateAlternativeInteractionLabels(alternativeSpectrumLevels);
    if (!secondGraphLevels.empty() || !alternativeSpectrumLevels.empty() || drawSegmentOverlay) {
        painter.resetTransform();
        if (!alternativeSpectrumLevels.empty()) {
            drawAlternativeSpectrumOverlay(painter, alternativeSpectrumLevels);
        }
        if (!secondGraphLevels.empty()) {
            painter.setRenderHint(QPainter::Antialiasing, false);
            const int pointCount = std::min(width(), static_cast<int>(secondGraphLevels.size()));
            const float graphHeight = static_cast<float>(height()) * 0.75f;
            for (int x = 1; x < pointCount; ++x) {
                const float previousLevel = secondGraphLevels[static_cast<std::size_t>(x - 1)];
                const float currentLevel = secondGraphLevels[static_cast<std::size_t>(x)];
                painter.setPen(valueToColors(currentLevel));
                painter.drawLine(x - 1,
                                 height() - 1 - qRound(previousLevel * graphHeight),
                                 x,
                                 height() - 1 - qRound(currentLevel * graphHeight));
            }
        }
        if (drawSegmentOverlay) {
            drawScanSegments(painter);
        }
    }
    drawWaterfallAreaMeasurement(painter);
}

qint64 MyWaterfallWidget::waterfallTimeAtY(int y) const {
    if (waterfallRowTimesMs.empty() || height() <= 0) {
        return 0;
    }
    const int clampedY = (std::clamp)(y, 0, (std::max)(0, height() - 1));
    const std::size_t index = (std::min)(
        waterfallRowTimesMs.size() - 1,
        static_cast<std::size_t>(
            static_cast<long long>(clampedY) * static_cast<long long>(waterfallRowTimesMs.size()) /
            (std::max)(1, height())));
    return waterfallRowTimesMs[index];
}

void MyWaterfallWidget::drawWaterfallAreaMeasurement(QPainter &painter) const {
    if (!areaMeasurementEnabled || !areaMeasurementVisible ||
        activeDisplayMode != DisplayMode::Waterfall2D || secondGraph ||
        width() <= 1 || height() <= 1) {
        if (areaMeasurementOverlayLabel) {
            areaMeasurementOverlayLabel->hide();
        }
        return;
    }

    QMutexLocker locker(&mutex);
    QRect selection(areaMeasurementStartPos, areaMeasurementEndPos);
    selection = selection.normalized().intersected(rect().adjusted(0, 0, -1, -1));
    if (selection.width() < 3 || selection.height() < 3) {
        if (areaMeasurementOverlayLabel) {
            areaMeasurementOverlayLabel->hide();
        }
        return;
    }

    const double f1 = frequencyAtX(selection.left());
    const double f2 = frequencyAtX(selection.right());
    const qint64 t1 = waterfallTimeAtY(selection.top());
    const qint64 t2 = waterfallTimeAtY(selection.bottom());
    const qint64 olderTime = (std::min)(t1, t2);
    const qint64 newerTime = (std::max)(t1, t2);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(selection, QColor(50, 155, 255, 34));
    painter.setPen(QPen(QColor(120, 215, 255, 235), 1, Qt::DashLine));
    painter.drawRect(selection.adjusted(0, 0, -1, -1));
    painter.drawLine(selection.center().x(), selection.top(),
                     selection.center().x(), selection.bottom());
    painter.drawLine(selection.left(), selection.center().y(),
                     selection.right(), selection.center().y());

    QStringList lines;
    lines << QStringLiteral("%1 - %2   df %3")
                 .arg(formatFrequencyLabel((std::min)(f1, f2)))
                 .arg(formatFrequencyLabel((std::max)(f1, f2)))
                 .arg(formatFrequencySpanLabel(std::abs(f2 - f1)));
    if (olderTime > 0 && newerTime > 0) {
        lines << QStringLiteral("%1 - %2   dt %3 s")
                     .arg(QDateTime::fromMSecsSinceEpoch(olderTime).toString(QStringLiteral("HH:mm:ss.zzz")))
                     .arg(QDateTime::fromMSecsSinceEpoch(newerTime).toString(QStringLiteral("HH:mm:ss.zzz")))
                     .arg(static_cast<double>(newerTime - olderTime) / 1000.0, 0, 'f', 3);
    } else {
        lines << QStringLiteral("t: --");
    }

    painter.restore();

    if (areaMeasurementOverlayLabel) {
        areaMeasurementOverlayLabel->setText(lines.join(QLatin1Char('\n')));
        areaMeasurementOverlayLabel->adjustSize();
        int labelX = selection.left() + 5;
        int labelY = selection.top() + 5;
        if (labelY + areaMeasurementOverlayLabel->height() > height() - 4) {
            labelY = selection.top() - areaMeasurementOverlayLabel->height() - 5;
        }
        labelX = (std::clamp)(labelX,
                              4,
                              (std::max)(4, width() - areaMeasurementOverlayLabel->width() - 4));
        labelY = (std::clamp)(labelY,
                              4,
                              (std::max)(4, height() - areaMeasurementOverlayLabel->height() - 4));
        areaMeasurementOverlayLabel->move(labelX, labelY);
        areaMeasurementOverlayLabel->show();
        areaMeasurementOverlayLabel->raise();
    }
}

void MyWaterfallWidget::drawAlternativeSpectrumOverlay(
    QPainter &painter,
    const std::vector<float> &normalizedLevels) const {
    if (normalizedLevels.size() < 2 || width() < 120 || height() < 120) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QRect plotRect = alternativeSpectrumPlotRect();
    const int left = plotRect.left();
    const int right = plotRect.right();
    const int top = plotRect.top();
    const int bottom = plotRect.bottom();
    const int plotWidth = (std::max)(1, right - left);
    const int plotHeight = (std::max)(1, bottom - top);
    const bool drawLowerGrid = !secondGraph;

    if (drawLowerGrid) {
        drawAlternativeBandMarkers(painter, plotRect);
    }

    QPen gridPen(QColor(115, 185, 205, 74));
    gridPen.setWidth(1);
    painter.setPen(gridPen);
    constexpr int HorizontalDivisions = 5;
    constexpr int VerticalDivisions = 10;
    if (drawLowerGrid) {
        for (int division = 0; division <= HorizontalDivisions; ++division) {
            const int y = top + division * plotHeight / HorizontalDivisions;
            painter.drawLine(left, y, right, y);
        }
        for (int division = 0; division <= VerticalDivisions; ++division) {
            const int x = left + division * plotWidth / VerticalDivisions;
            painter.drawLine(x, top, x, bottom);
        }
    }

    const int pointCount = static_cast<int>(normalizedLevels.size());
    const float first = std::clamp(normalizedLevels.front(), 0.0f, 1.0f);
    const int firstY = bottom - static_cast<int>(std::lround(first * plotHeight));
    QPainterPath spectrumPath;
    spectrumPath.moveTo(left, firstY);
    QPainterPath fillPath;
    fillPath.moveTo(left, bottom);
    fillPath.lineTo(left, firstY);
    for (int point = 1; point < pointCount; ++point) {
        const float current = std::clamp(normalizedLevels[static_cast<std::size_t>(point)],
                                         0.0f,
                                         1.0f);
        const int x2 = left + point * plotWidth / (pointCount - 1);
        const int y2 = bottom - static_cast<int>(std::lround(current * plotHeight));
        spectrumPath.lineTo(x2, y2);
        fillPath.lineTo(x2, y2);
    }
    if (alternativeSpectrumGradientFill) {
        fillPath.lineTo(right, bottom);
        fillPath.closeSubpath();
        QLinearGradient fillGradient(0, bottom, 0, top);
        const float opacity = static_cast<float>(alternativeSpectrumGradientOpacity) / 100.0f;
        const int alpha = qRound(255.0f * opacity);
        if (colorSpectrum) {
            constexpr int PaletteStops = 16;
            for (int stop = 0; stop <= PaletteStops; ++stop) {
                const float normalized = static_cast<float>(stop) /
                                         static_cast<float>(PaletteStops);
                QColor color = valueToColors(normalized);
                color.setAlpha(alpha);
                fillGradient.setColorAt(normalized, color);
            }
        } else {
            fillGradient.setColorAt(0.0, QColor(0, 70, 0, alpha));
            fillGradient.setColorAt(0.45, QColor(0, 175, 0, alpha));
            fillGradient.setColorAt(1.0, QColor(70, 255, 100, alpha));
        }
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.fillPath(fillPath, fillGradient);
    }
    if (colorSpectrum) {
        for (int point = 1; point < pointCount; ++point) {
            const float previous = std::clamp(normalizedLevels[static_cast<std::size_t>(point - 1)],
                                              0.0f,
                                              1.0f);
            const float current = std::clamp(normalizedLevels[static_cast<std::size_t>(point)],
                                             0.0f,
                                             1.0f);
            const int x1 = left + (point - 1) * plotWidth / (pointCount - 1);
            const int x2 = left + point * plotWidth / (pointCount - 1);
            const int y1 = bottom - static_cast<int>(std::lround(previous * plotHeight));
            const int y2 = bottom - static_cast<int>(std::lround(current * plotHeight));
            painter.setPen(QPen(valueToColors(current), 1));
            painter.drawLine(x1, y1, x2, y2);
        }
    } else {
        painter.setPen(QPen(QColor(0, 255, 0, 235), 1));
        painter.drawPath(spectrumPath);
    }

    drawAlternativeScienceOverlays(painter, plotRect);

    if (drawLowerGrid) {
        painter.setPen(QPen(QColor(220, 235, 240, 190), 1));
        painter.drawLine(left, top, left, bottom);
        painter.drawLine(left, bottom, right, bottom);
    }

    if (alternativeSpectrumMeasurementVisible) {
        int x1 = std::clamp(alternativeSpectrumMeasureStartPos.x(), left, right);
        int x2 = std::clamp(alternativeSpectrumMeasureEndPos.x(), left, right);
        if (std::abs(x2 - x1) >= 4) {
            if (x2 < x1) {
                std::swap(x1, x2);
            }
            const QRect selectionRect(x1, top, x2 - x1, plotHeight);
            painter.fillRect(selectionRect, QColor(70, 135, 255, 48));
            painter.setPen(QPen(QColor(120, 190, 255, 230), 1, Qt::DashLine));
            painter.drawRect(selectionRect.adjusted(0, 0, -1, -1));
        }
    }

    if (alternativeSpectrumHoverVisible && plotRect.contains(alternativeSpectrumHoverPos)) {
        const int x = std::clamp(alternativeSpectrumHoverPos.x(), left, right);
        const int dataIndex = std::clamp((x - left) * (pointCount - 1) / plotWidth,
                                         0,
                                         pointCount - 1);
        const float normalized = std::clamp(normalizedLevels[static_cast<std::size_t>(dataIndex)],
                                            0.0f,
                                            1.0f);
        const int y = bottom - static_cast<int>(std::lround(normalized * plotHeight));
        painter.setPen(QPen(QColor(210, 240, 255, 180), 1, Qt::DashLine));
        painter.drawLine(x, top, x, bottom);
        painter.setBrush(QColor(120, 240, 255));
        painter.drawEllipse(QPoint(x, y), 3, 3);
    }
    painter.restore();
}

void MyWaterfallWidget::drawAlternativeScienceOverlays(QPainter &painter,
                                                        const QRect &plotRect) const {
    const int left = plotRect.left();
    const int right = plotRect.right();
    const int top = plotRect.top();
    const int bottom = plotRect.bottom();
    const int plotWidth = (std::max)(1, right - left);
    const int plotHeight = (std::max)(1, bottom - top);
    auto drawTrace = [&](const std::vector<float> &trace, const QColor &color) {
        if (trace.size() < 2) return;
        QPainterPath path;
        bool started = false;
        for (int i = 0; i < static_cast<int>(trace.size()); ++i) {
            if (!std::isfinite(trace[static_cast<std::size_t>(i)])) continue;
            const int x = left + i * plotWidth / (static_cast<int>(trace.size()) - 1);
            const int y = bottom - static_cast<int>(std::lround(normalizedLevel(trace[static_cast<std::size_t>(i)]) * plotHeight));
            if (!started) { path.moveTo(x, y); started = true; }
            else path.lineTo(x, y);
        }
        if (started) {
            painter.setPen(QPen(color, 1));
            painter.drawPath(path);
        }
    };
    painter.save();
    painter.setClipRect(plotRect);
    if (scienceMinHoldVisible) drawTrace(scienceMinHoldData, QColor(85, 155, 255, 210));
    if (sciencePercentile50Visible) drawTrace(sciencePercentile50Data, QColor(75, 220, 145, 220));
    if (sciencePercentile90Visible) drawTrace(sciencePercentile90Data, QColor(215, 105, 245, 220));
    if (sciencePercentile99Visible) drawTrace(sciencePercentile99Data, QColor(255, 80, 95, 230));
    if (scienceAverageVisible) drawTrace(scienceAverageData, QColor(235, 235, 235, 220));
    if (scienceMaxHoldVisible) drawTrace(scienceMaxHoldData, QColor(255, 170, 35, 235));
    const QColor colors[] = {QColor(255, 225, 60), QColor(80, 220, 255)};
    for (int index = 0; index < scienceMarkers.size(); ++index) {
        const SpectrumScienceMarker &marker = scienceMarkers.at(index);
        if (!marker.enabled || !std::isfinite(marker.frequencyHz)) continue;
        const double displayFrequency = displayFrequencyForActualFrequency(marker.frequencyHz);
        if (displayFrequency < (std::min)(xMin, xMax) || displayFrequency > (std::max)(xMin, xMax)) continue;
        const int x = left + static_cast<int>(std::lround((displayFrequency - xMin) /
                                                          (xMax - xMin) * plotWidth));
        painter.setPen(QPen(colors[index % 2], 1, Qt::DashLine));
        painter.drawLine(x, top, x, bottom);
        painter.drawText(QRect(x + 3, top + 3 + index * 17, 22, 15),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         marker.label);
    }
    painter.restore();
}

QRect MyWaterfallWidget::alternativeSpectrumPlotRect() const {
    const int top = static_cast<int>(std::lround(height() * 0.67));
    return QRect(0, top, (std::max)(1, width()), (std::max)(1, height() - top - 4));
}

QString MyWaterfallWidget::formatFrequencyLabel(double frequencyHz) const {
    const double absolute = std::abs(frequencyHz);
    if (absolute >= 1.0e9) return QStringLiteral("%1 GHz").arg(frequencyHz / 1.0e9, 0, 'f', 6);
    if (absolute >= 1.0e6) return QStringLiteral("%1 MHz").arg(frequencyHz / 1.0e6, 0, 'f', 6);
    if (absolute >= 1.0e3) return QStringLiteral("%1 kHz").arg(frequencyHz / 1.0e3, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(frequencyHz, 0, 'f', 0);
}

QString MyWaterfallWidget::formatFrequencySpanLabel(double spanHz) const {
    const double absolute = std::abs(spanHz);
    if (absolute >= 1.0e9) return QStringLiteral("%1 GHz").arg(absolute / 1.0e9, 0, 'f', 6);
    if (absolute >= 1.0e6) return QStringLiteral("%1 MHz").arg(absolute / 1.0e6, 0, 'f', 3);
    if (absolute >= 1.0e3) return QStringLiteral("%1 kHz").arg(absolute / 1.0e3, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(absolute, 0, 'f', 0);
}

void MyWaterfallWidget::drawAlternativeBandMarkers(QPainter &painter,
                                                    const QRect &plotRect) const {
    if (bandMarkers.isEmpty() ||
        (!generalBandMarkersEnabled && !amateurBandMarkersEnabled) ||
        qFuzzyCompare(xMin, xMax)) {
        return;
    }

    const int markerTop = compactBandMarkersEnabled
                              ? (std::max)(plotRect.top(), plotRect.bottom() - 22)
                              : plotRect.top();
    const int markerBottom = plotRect.bottom();
    const double viewStart = (std::min)(xMin, xMax);
    const double viewEnd = (std::max)(xMin, xMax);
    const double span = xMax - xMin;
    auto frequencyToX = [&](double frequency) {
        return plotRect.left() +
               (frequency - xMin) / span * static_cast<double>(plotRect.width());
    };

    painter.save();
    painter.setClipRect(plotRect);
    if (compactBandMarkersEnabled) {
        painter.fillRect(QRect(plotRect.left(), markerTop, plotRect.width(), markerBottom - markerTop),
                         QColor(0, 0, 0, 190));
    }
    for (const GraphBandMarker &marker : bandMarkers) {
        if ((marker.amateur && !amateurBandMarkersEnabled) ||
            (!marker.amateur && !generalBandMarkersEnabled) ||
            !std::isfinite(marker.startHz) || !std::isfinite(marker.endHz) ||
            marker.endHz <= marker.startHz || marker.endHz < viewStart || marker.startHz > viewEnd) {
            continue;
        }
        int x1 = static_cast<int>(std::floor(frequencyToX((std::max)(marker.startHz, viewStart))));
        int x2 = static_cast<int>(std::ceil(frequencyToX((std::min)(marker.endHz, viewEnd))));
        x1 = std::clamp(x1, plotRect.left(), plotRect.right());
        x2 = std::clamp(x2, plotRect.left(), plotRect.right());
        const int markerWidth = (std::max)(1, x2 - x1);
        const QColor fill = marker.amateur
                                ? QColor(255, 198, 66, compactBandMarkersEnabled ? 118 : 34)
                                : QColor(76, 162, 255, compactBandMarkersEnabled ? 105 : 28);
        const QColor edge = marker.amateur ? QColor(255, 220, 96, 170)
                                           : QColor(112, 196, 255, 155);
        painter.fillRect(QRect(x1, markerTop, markerWidth, markerBottom - markerTop), fill);
        painter.setPen(edge);
        painter.drawLine(x1, markerTop, x1, markerBottom);
        painter.drawLine(x2, markerTop, x2, markerBottom);
    }
    painter.restore();
}

void MyWaterfallWidget::updateFpsCounter() {
    if (!fpsOverlayEnabled) {
        return;
    }
    if (!fpsElapsedTimer.isValid()) {
        fpsElapsedTimer.start();
        fpsFrameCount = 0;
    }
    if (!waterfallRowsElapsedTimer.isValid()) {
        waterfallRowsElapsedTimer.start();
        waterfallRowsSinceRateUpdate = 0;
    }
    ++fpsFrameCount;
    const qint64 elapsedMs = fpsElapsedTimer.elapsed();
    if (elapsedMs >= 500) {
        displayedFps = static_cast<double>(fpsFrameCount) * 1000.0 /
                       static_cast<double>(elapsedMs);
        fpsFrameCount = 0;
        fpsElapsedTimer.restart();
        const qint64 rowElapsedMs = waterfallRowsElapsedTimer.elapsed();
        if (rowElapsedMs > 0) {
            displayedWaterfallFftFramesPerSecond =
                static_cast<double>(waterfallFftFramesSinceRateUpdate) * 1000.0 /
                static_cast<double>(rowElapsedMs);
            displayedWaterfallRowsPerSecond =
                static_cast<double>(waterfallRowsSinceRateUpdate) * 1000.0 /
                static_cast<double>(rowElapsedMs);
        }
        waterfallFftFramesSinceRateUpdate = 0;
        waterfallRowsSinceRateUpdate = 0;
        waterfallRowsElapsedTimer.restart();
        if (fpsOverlayLabel) {
            fpsOverlayLabel->setText(QStringLiteral("FPS %1 | FFT/s %2 | rows/s %3")
                                         .arg(displayedFps, 0, 'f', 1)
                                         .arg(displayedWaterfallFftFramesPerSecond, 0, 'f', 0)
                                         .arg(displayedWaterfallRowsPerSecond, 0, 'f', 0));
            fpsOverlayLabel->adjustSize();
            positionInfoOverlays();
            fpsOverlayLabel->raise();
        }
    }
}

void MyWaterfallWidget::positionInfoOverlays() {
    int nextY = pauseControl && pauseControl->isVisible() ? 36 : 8;
    if (fpsOverlayLabel) {
        fpsOverlayLabel->move((std::max)(8, width() - fpsOverlayLabel->width() - 8), nextY);
        if (fpsOverlayLabel->isVisible()) {
            nextY += fpsOverlayLabel->height() + 4;
        }
    }
    if (sliceDetailsOverlayLabel) {
        sliceDetailsOverlayLabel->move(
            (std::max)(8, width() - sliceDetailsOverlayLabel->width() - 8),
            nextY);
    }
}

void MyWaterfallWidget::updateSliceOverlay(const QPoint &anchor) {
    if (!sliceOverlayLabel) {
        return;
    }

    QString text;
    QString detailsText;
    {
        QMutexLocker locker(&mutex);
        if (frequencySliceMouseActive) {
            double centerRatio = 0.0;
            double firstRatio = 0.0;
            double lastRatio = 0.0;
            Waterfall3DRenderer::SliceStatistics statistics;
            if (waterfall3DRenderer->frequencySliceRange(centerRatio, firstRatio, lastRatio)) {
                const double span = xMax - xMin;
                const double centerHz = xMin + centerRatio * span;
                const double firstHz = xMin + firstRatio * span;
                const double lastHz = xMin + lastRatio * span;
                const bool hasStatistics =
                    waterfall3DRenderer->frequencySliceStatistics(statistics);
                const QString maximumText = hasStatistics
                                                ? QStringLiteral("\nmax  %1 dBFS")
                                                      .arg(statistics.maximumLevelDb, 0, 'f', 1)
                                                : QString();
                text = qFuzzyCompare(firstHz, lastHz)
                           ? QStringLiteral("f  %1 MHz%2")
                                 .arg(centerHz / 1.0e6, 0, 'f', 6)
                                 .arg(maximumText)
                           : QStringLiteral("f  %1 MHz\n%2 - %3 MHz%4")
                                 .arg(centerHz / 1.0e6, 0, 'f', 6)
                                 .arg(firstHz / 1.0e6, 0, 'f', 6)
                                 .arg(lastHz / 1.0e6, 0, 'f', 6)
                                 .arg(maximumText);
                if (extendedInfoOverlayEnabled && hasStatistics) {
                    const int selectedColumns = statistics.lastColumn -
                                                statistics.firstColumn + 1;
                    const double peakRatio = statistics.columnCount > 1
                                                 ? static_cast<double>(statistics.peakColumn) /
                                                       static_cast<double>(statistics.columnCount - 1)
                                                 : 0.0;
                    const double peakHz = xMin + peakRatio * span;
                    const double rawBinWidthHz = metadataFftLength > 0 && metadataSampleRateHz > 0.0
                                                     ? metadataSampleRateHz / metadataFftLength
                                                     : 0.0;
                    const double resolutionBandwidthHz =
                        rawBinWidthHz * fftWindowEnbwBins(metadataFftWindowType);
                    const double displayPointWidthHz = statistics.columnCount > 1
                                                           ? span / (statistics.columnCount - 1)
                                                           : span;
                    detailsText = QStringLiteral(
                                      "FREQUENCY SLICE\n"
                                      "f       %1 MHz\n"
                                      "range   %2 - %3 MHz\n"
                                      "width   %4 kHz / %5 pt\n"
                                      "level   %6 / %7 / %8 dBFS\n"
                                      "peak    %9 MHz\n"
                                      "center  %10 MHz\n"
                                      "listen  %11 MHz\n"
                                      "SR      %12 MHz\n"
                                      "FFT     %13\n"
                                      "window  %14\n"
                                      "RBW     %15 Hz\n"
                                      "bin     %16 Hz\n"
                                      "display %17 Hz / pt\n"
                                      "history %18 rows")
                                      .arg(centerHz / 1.0e6, 0, 'f', 6)
                                      .arg(firstHz / 1.0e6, 0, 'f', 6)
                                      .arg(lastHz / 1.0e6, 0, 'f', 6)
                                      .arg(std::abs(lastHz - firstHz) / 1.0e3, 0, 'f', 3)
                                      .arg(selectedColumns)
                                      .arg(statistics.minimumLevelDb, 0, 'f', 1)
                                      .arg(statistics.averageLevelDb, 0, 'f', 1)
                                      .arg(statistics.maximumLevelDb, 0, 'f', 1)
                                      .arg(peakHz / 1.0e6, 0, 'f', 6)
                                      .arg(metadataCenterFrequencyHz / 1.0e6, 0, 'f', 6)
                                      .arg(metadataListeningFrequencyHz / 1.0e6, 0, 'f', 6)
                                      .arg(metadataSampleRateHz / 1.0e6, 0, 'f', 3)
                                      .arg(metadataFftLength)
                                      .arg(QString::fromLatin1(fftWindowTypeName(metadataFftWindowType)))
                                      .arg(resolutionBandwidthHz, 0, 'f', 3)
                                      .arg(rawBinWidthHz, 0, 'f', 3)
                                      .arg(displayPointWidthHz, 0, 'f', 3)
                                      .arg(statistics.rowCount);
                }
            }
        } else if (spectrumFrameSliceMouseActive) {
            int firstRow = 0;
            int lastRow = 0;
            int rowCount = 0;
            Waterfall3DRenderer::SliceStatistics statistics;
            if (waterfall3DRenderer->spectrumSliceRange(firstRow, lastRow, rowCount)) {
                const bool hasStatistics =
                    waterfall3DRenderer->spectrumSliceStatistics(statistics);
                const QString maximumText = hasStatistics
                                                ? QStringLiteral("\nmax  %1 dBFS")
                                                      .arg(statistics.maximumLevelDb, 0, 'f', 1)
                                                : QString();
                text = QStringLiteral("rows  %1 - %2 / %3\n%4 - %5 MHz%6")
                           .arg(firstRow + 1)
                           .arg(lastRow + 1)
                           .arg(rowCount)
                           .arg(xMin / 1.0e6, 0, 'f', 6)
                           .arg(xMax / 1.0e6, 0, 'f', 6)
                           .arg(maximumText);
                if (extendedInfoOverlayEnabled && hasStatistics) {
                    const double peakRatio = statistics.columnCount > 1
                                                 ? static_cast<double>(statistics.peakColumn) /
                                                       static_cast<double>(statistics.columnCount - 1)
                                                 : 0.0;
                    const double peakHz = xMin + peakRatio * (xMax - xMin);
                    const double rawBinWidthHz = metadataFftLength > 0 && metadataSampleRateHz > 0.0
                                                     ? metadataSampleRateHz / metadataFftLength
                                                     : 0.0;
                    const double resolutionBandwidthHz =
                        rawBinWidthHz * fftWindowEnbwBins(metadataFftWindowType);
                    const double displayPointWidthHz = statistics.columnCount > 1
                                                           ? (xMax - xMin) / (statistics.columnCount - 1)
                                                           : (xMax - xMin);
                    detailsText = QStringLiteral(
                                      "SPECTRUM SLICE\n"
                                      "rows    %1 - %2 / %3\n"
                                      "count   %4 rows / %5 pt\n"
                                      "f       %6 - %7 MHz\n"
                                      "level   %8 / %9 / %10 dBFS\n"
                                      "peak    %11 MHz\n"
                                      "peak row %12\n"
                                      "center  %13 MHz\n"
                                      "listen  %14 MHz\n"
                                      "SR      %15 MHz\n"
                                      "FFT     %16\n"
                                      "window  %17\n"
                                      "RBW     %18 Hz\n"
                                      "bin     %19 Hz\n"
                                      "display %20 Hz / pt")
                                      .arg(statistics.firstRow + 1)
                                      .arg(statistics.lastRow + 1)
                                      .arg(statistics.rowCount)
                                      .arg(statistics.lastRow - statistics.firstRow + 1)
                                      .arg(statistics.columnCount)
                                      .arg(xMin / 1.0e6, 0, 'f', 6)
                                      .arg(xMax / 1.0e6, 0, 'f', 6)
                                      .arg(statistics.minimumLevelDb, 0, 'f', 1)
                                      .arg(statistics.averageLevelDb, 0, 'f', 1)
                                      .arg(statistics.maximumLevelDb, 0, 'f', 1)
                                      .arg(peakHz / 1.0e6, 0, 'f', 6)
                                      .arg(statistics.peakRow + 1)
                                      .arg(metadataCenterFrequencyHz / 1.0e6, 0, 'f', 6)
                                      .arg(metadataListeningFrequencyHz / 1.0e6, 0, 'f', 6)
                                      .arg(metadataSampleRateHz / 1.0e6, 0, 'f', 3)
                                      .arg(metadataFftLength)
                                      .arg(QString::fromLatin1(fftWindowTypeName(metadataFftWindowType)))
                                      .arg(resolutionBandwidthHz, 0, 'f', 3)
                                      .arg(rawBinWidthHz, 0, 'f', 3)
                                      .arg(displayPointWidthHz, 0, 'f', 3);
                }
            }
        }
    }

    if (text.isEmpty()) {
        hideSliceOverlay();
        return;
    }

    sliceOverlayAnchor = anchor;
    sliceOverlayLabel->setText(text);
    sliceOverlayLabel->adjustSize();
    int overlayX = anchor.x() + 14;
    int overlayY = anchor.y() + 14;
    if (overlayX + sliceOverlayLabel->width() > width() - 6) {
        overlayX = anchor.x() - sliceOverlayLabel->width() - 14;
    }
    if (overlayY + sliceOverlayLabel->height() > height() - 6) {
        overlayY = anchor.y() - sliceOverlayLabel->height() - 14;
    }
    sliceOverlayLabel->move((std::clamp)(overlayX, 6, (std::max)(6, width() - sliceOverlayLabel->width() - 6)),
                            (std::clamp)(overlayY, 6, (std::max)(6, height() - sliceOverlayLabel->height() - 6)));
    sliceOverlayLabel->show();
    sliceOverlayLabel->raise();
    if (sliceDetailsOverlayLabel) {
        if (extendedInfoOverlayEnabled && !detailsText.isEmpty()) {
            sliceDetailsOverlayLabel->setText(detailsText);
            sliceDetailsOverlayLabel->adjustSize();
            sliceDetailsOverlayLabel->show();
            sliceDetailsOverlayLabel->raise();
            positionInfoOverlays();
        } else {
            sliceDetailsOverlayLabel->hide();
        }
    }
}

void MyWaterfallWidget::hideSliceOverlay() {
    if (sliceOverlayLabel) {
        sliceOverlayLabel->hide();
    }
    if (sliceDetailsOverlayLabel) {
        sliceDetailsOverlayLabel->hide();
    }
}

void MyWaterfallWidget::drawScanSegments(QPainter &painter) const {
    QMutexLocker locker(&mutex);
    if (!scanSegmentMarkersVisible ||
        scanSegments.size() < 2 ||
        width() <= 0 ||
        height() <= 0 ||
        qFuzzyCompare(xMin, xMax)) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    QPen edgePen(QColor(255, 255, 255, 42));
    edgePen.setWidth(1);
    painter.setPen(edgePen);
    for (const ScanVisualSegment &segment : scanSegments) {
        if (!std::isfinite(segment.startHz) || !std::isfinite(segment.endHz)) {
            continue;
        }
        const int left = static_cast<int>(std::round((segment.startHz - xMin) * width() / (xMax - xMin)));
        const int right = static_cast<int>(std::round((segment.endHz - xMin) * width() / (xMax - xMin)));
        if (right < 0 || left > width() || right <= left) {
            continue;
        }
        painter.drawLine(left, 0, left, height());
        painter.drawLine(right, 0, right, height());
    }
    painter.restore();
}

QColor MyWaterfallWidget::valueToColor(float value, float contrastFactor, float sensitivityFactor) {
    if (!std::isfinite(value)) {
        value = 0.0f;
    }
    value = qBound(0.0f, value * sensitivityFactor, 1.0f);
    QVector<QColor> colorMap = {
        QColor("#000020"),
        QColor("#000050"),
        QColor("#000090"),
        QColor("#0000F0"),
        QColor("#0000FF"),
        QColor("#50F030"),
        QColor("#1E90FF"),
        QColor("#FFFFFF"),
        QColor("#FFFF00"),
        QColor("#FE6D16"),
        QColor("#FE6D16"),
        QColor("#FF0000"),
        QColor("#FF0000"),
        QColor("#C60000"),
        QColor("#9F0000"),
        QColor("#750000"),
        QColor("#4A0000"),
        //QColor("#F000F0")
    };
    int index = static_cast<int>(value * (colorMap.size() - 1));
    index = std::clamp(index, 0, colorMap.size() - 1);
    QColor baseColor = colorMap[index];
    int blue = (baseColor.green() + baseColor.blue())/3;
    int r = static_cast<int>(baseColor.red() * contrastFactor + blue * (1.0f - contrastFactor));
    int g = static_cast<int>(baseColor.green() * contrastFactor + blue * (1.0f - contrastFactor));
    int b = static_cast<int>(baseColor.blue() * contrastFactor + blue * (1.0f - contrastFactor));
    return QColor(r, g, b);
}

float MyWaterfallWidget::normalizedLevel(float value) const {
    if (!std::isfinite(value) || qFuzzyCompare(levelMin, levelMax)) {
        return 0.0f;
    }
    return qBound(0.0f, (value - levelMin) / (levelMax - levelMin), 1.0f);
}


 QColor MyWaterfallWidget::valueToColors(float value) const {
    if (!std::isfinite(value)) {
        value = 0.0f;
    }
    value = qBound(0.0f, value, 1.0f);
    int r, g, b;
    if (value < 0.12f) {
        float ratio = value / 0.2f;
        r = 0;
        g = 0;
        b = static_cast<int>(255 * ratio);
    } else if (value < 0.26f) {
        float ratio = (value - 0.12f) / 0.2f;
        r = 0;
        g = static_cast<int>(255 * (1 - ratio));
        b = 255;
    } else if (value < 0.40f) {
        float ratio = (value - 0.28f) / 0.2f;
        r = 0;
        g = 255;
        b = static_cast<int>(255 * (1 - ratio));
    } else if (value < 0.54f) {
        float ratio = (value - 0.40f) / 0.2f;
        r = static_cast<int>(255 * ratio);
        g = 255;
        b = 0;
    } else if (value < 0.68f) {
        float ratio = (value - 0.54f) / 0.2f;
        r = 255;
        g = static_cast<int>(255 * (1 - 0.5f * ratio));
        b = 0;    
    } else if (value < 0.86f) {
        float ratio = (value - 0.68f) / 0.2f;
        r = 255;
        g = static_cast<int>(128 * (1 - 0.5f * ratio));
        b = 0;
    } else {
        float ratio = (value - 0.86f) / 0.2f;
        r = 255;
        g = 0;
        b = static_cast<int>(255 * (1 - ratio));
    }
    return QColor(r, g, b);
}
