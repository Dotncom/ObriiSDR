#include "signallabpreview.h"
#include "fft.h"

#include <fftw3.h>

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace {
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr qint64 kMaximumPreviewPoints = 2 * 1024 * 1024;

QColor spectralColor(float level, float minimumDb, float maximumDb) {
    const float span = (std::max)(1.0f, maximumDb - minimumDb);
    const float value = (std::clamp)((level - minimumDb) / span, 0.0f, 1.0f);
    struct Stop {
        float position;
        int red;
        int green;
        int blue;
    };
    static const Stop stops[] = {
        {0.00f, 0, 0, 0},
        {0.18f, 12, 28, 92},
        {0.38f, 0, 135, 210},
        {0.56f, 28, 205, 142},
        {0.72f, 238, 222, 45},
        {0.87f, 246, 96, 28},
        {1.00f, 255, 247, 230}};
    for (std::size_t index = 1; index < std::size(stops); ++index) {
        if (value > stops[index].position) {
            continue;
        }
        const Stop &left = stops[index - 1];
        const Stop &right = stops[index];
        const float denominator = (std::max)(0.0001f, right.position - left.position);
        const float ratio = (value - left.position) / denominator;
        return QColor(static_cast<int>(std::lround(left.red + (right.red - left.red) * ratio)),
                      static_cast<int>(std::lround(left.green + (right.green - left.green) * ratio)),
                      static_cast<int>(std::lround(left.blue + (right.blue - left.blue) * ratio)));
    }
    return QColor(255, 247, 230);
}

bool isPowerOfTwo(int value) {
    return value >= 2 && (value & (value - 1)) == 0;
}
} // namespace

SignalLabSpectralPreviewData buildSignalLabSpectralPreview(
    const SignalLabGenerationResult &result,
    int fftSize,
    int requestedRows) {
    SignalLabSpectralPreviewData preview;
    preview.sampleRate = result.sampleRate;
    preview.durationSeconds = result.durationSeconds;
    if (!result.ok() || result.sampleRate <= 0) {
        preview.error = QStringLiteral("No generated IQ data is available");
        return preview;
    }
    if (!isPowerOfTwo(fftSize) || fftSize < 64 || fftSize > 16384) {
        preview.error = QStringLiteral("Preview FFT must be a power of two from 64 to 16384");
        return preview;
    }

    requestedRows = (std::clamp)(requestedRows, 8, 512);
    requestedRows = (std::min)(requestedRows,
                               static_cast<int>((std::max<qint64>)(
                                   8, kMaximumPreviewPoints / fftSize)));
    const int maximumStart = (std::max)(0, result.iq.size() - fftSize);
    int rows = 1;
    if (maximumStart > 0) {
        const int approximateFrames = 1 + maximumStart / (std::max)(1, fftSize / 8);
        rows = (std::min)(requestedRows, (std::max)(2, approximateFrames));
    }

    auto *input = static_cast<fftwf_complex *>(
        fftwf_malloc(sizeof(fftwf_complex) * static_cast<std::size_t>(fftSize)));
    auto *output = static_cast<fftwf_complex *>(
        fftwf_malloc(sizeof(fftwf_complex) * static_cast<std::size_t>(fftSize)));
    if (!input || !output) {
        if (input) fftwf_free(input);
        if (output) fftwf_free(output);
        preview.error = QStringLiteral("Unable to allocate FFT preview buffers");
        return preview;
    }
    fftwf_plan plan = nullptr;
    {
        std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
        plan = fftwf_plan_dft_1d(fftSize, input, output, FFTW_FORWARD, FFTW_ESTIMATE);
    }
    if (!plan) {
        fftwf_free(input);
        fftwf_free(output);
        preview.error = QStringLiteral("Unable to create FFT preview plan");
        return preview;
    }

    std::vector<float> window(static_cast<std::size_t>(fftSize));
    double windowSum = 0.0;
    double windowPower = 0.0;
    preview.windowSamples = (std::min)(fftSize, result.iq.size());
    for (int index = 0; index < preview.windowSamples; ++index) {
        const float value = preview.windowSamples > 2
                                ? static_cast<float>(0.5 - 0.5 * std::cos(
                                      2.0 * kPi * index / (preview.windowSamples - 1)))
                                : 1.0f;
        window[static_cast<std::size_t>(index)] = value;
        windowSum += value;
        windowPower += value * value;
    }
    const double normalization = 1.0 / (std::max)(1.0, windowSum);
    preview.rbwHz = result.sampleRate * windowPower * normalization * normalization;

    preview.fftSize = fftSize;
    preview.rowCount = rows;
    preview.levels.reserve(static_cast<std::size_t>(rows));
    preview.colors.reserve(static_cast<std::size_t>(rows));
    preview.image = QImage(fftSize, rows, QImage::Format_ARGB32);
    for (int row = 0; row < rows; ++row) {
        const int start = rows > 1
                              ? static_cast<int>((static_cast<qint64>(row) * maximumStart) /
                                                 (rows - 1))
                              : 0;
        for (int index = 0; index < fftSize; ++index) {
            const int sourceIndex = start + index;
            const std::complex<float> sample = sourceIndex < result.iq.size()
                                                   ? result.iq.at(sourceIndex)
                                                   : std::complex<float>(0.0f, 0.0f);
            const float scale = window[static_cast<std::size_t>(index)];
            input[index][0] = sample.real() * scale;
            input[index][1] = sample.imag() * scale;
        }
        fftwf_execute(plan);

        std::vector<float> levels(static_cast<std::size_t>(fftSize));
        std::vector<unsigned char> colors(static_cast<std::size_t>(fftSize) * 3U);
        auto *pixels = reinterpret_cast<QRgb *>(preview.image.scanLine(row));
        for (int bin = 0; bin < fftSize; ++bin) {
            const int shifted = (bin + fftSize / 2) % fftSize;
            const double real = output[shifted][0];
            const double imag = output[shifted][1];
            const double magnitude = std::sqrt(real * real + imag * imag) * normalization;
            const float db = static_cast<float>((std::max)(
                static_cast<double>(preview.minimumDb),
                20.0 * std::log10((std::max)(magnitude, 1.0e-12))));
            levels[static_cast<std::size_t>(bin)] = db;
            const QColor color = spectralColor(db, preview.minimumDb, preview.maximumDb);
            const std::size_t colorIndex = static_cast<std::size_t>(bin) * 3U;
            colors[colorIndex] = static_cast<unsigned char>(color.red());
            colors[colorIndex + 1U] = static_cast<unsigned char>(color.green());
            colors[colorIndex + 2U] = static_cast<unsigned char>(color.blue());
            pixels[bin] = color.rgb();
        }
        preview.levels.emplace_back(std::move(levels));
        preview.colors.emplace_back(std::move(colors));
    }

    {
        std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
        fftwf_destroy_plan(plan);
    }
    fftwf_free(input);
    fftwf_free(output);
    return preview;
}

SignalLabPreviewWidget::SignalLabPreviewWidget(QWidget *parent) : QWidget(parent) {
    setMinimumSize(430, 330);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

SignalLabIqPreviewData buildSignalLabIqPreview(const SignalLabGenerationResult &result) {
    SignalLabIqPreviewData data;
    if (result.ok() && result.sampleRate > 0) {
        QVector<int> symbolIndices;
        data.symbols = SignalLabGenerator::matchedSymbols(result, 2500, &symbolIndices);
        data.symbolTimes.reserve(symbolIndices.size());
        for (int index : symbolIndices) data.symbolTimes.append(double(index) / result.sampleRate);
        data.durationSeconds = double(result.iq.size()) / result.sampleRate;
        const int timeCount = (std::min)(result.iq.size(), 1600);
        data.timeSpanSeconds = double(timeCount - 1) / result.sampleRate;
        data.timePoints.reserve(timeCount);
        for (int x = 0; x < timeCount; ++x) {
            data.timePoints.append(result.iq[x]);
        }
        const int constellationCount = (std::min)(result.iq.size(), 2500);
        data.continuousTrajectory = constellationCount == result.iq.size();
        data.samples.reserve(constellationCount);
        data.sampleTimes.reserve(constellationCount);
        for (int point = 0; point < constellationCount; ++point) {
            const qint64 first = qint64(point) * result.iq.size() / constellationCount;
            const qint64 end = qint64(point + 1) * result.iq.size() / constellationCount;
            const quint32 jitter = quint32(point) * 2654435761U;
            const int index = int(first + jitter % (std::max<qint64>)(1, end - first));
            data.samples.append(result.iq[index]);
            data.sampleTimes.append(double(index) / result.sampleRate);
        }
    }
    return data;
}

void SignalLabPreviewWidget::setResult(const SignalLabGenerationResult &result) {
    setData(buildSignalLabIqPreview(result));
}

void SignalLabPreviewWidget::setData(const SignalLabIqPreviewData &data) {
    timePoints = data.timePoints;
    constellation = data.samples;
    symbols = data.symbols;
    timeSpanSeconds = data.timeSpanSeconds;
    update();
}

void SignalLabPreviewWidget::clear() {
    timePoints.clear();
    constellation.clear();
    symbols.clear();
    timeSpanSeconds = 0.0;
    update();
}

void SignalLabPreviewWidget::setLanguage(const QString &language) {
    ukrainian = language.toLower().startsWith(QStringLiteral("uk"));
    update();
}

void SignalLabPreviewWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(12, 15, 19));
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRect timeRect = rect().adjusted(10, 10, -10, -height() / 2 - 5);
    const QRect iqRect = rect().adjusted(10, height() / 2 + 5, -10, -10);
    painter.setPen(QColor(52, 61, 71));
    painter.drawRect(timeRect);
    painter.drawLine(timeRect.left(), timeRect.center().y(),
                     timeRect.right(), timeRect.center().y());
    painter.drawRect(iqRect);
    painter.drawLine(iqRect.center().x(), iqRect.top(),
                     iqRect.center().x(), iqRect.bottom());
    painter.drawLine(iqRect.left(), iqRect.center().y(),
                     iqRect.right(), iqRect.center().y());

    if (timePoints.size() > 1) {
        QPolygonF iPath;
        QPolygonF qPath;
        iPath.reserve(timePoints.size());
        qPath.reserve(timePoints.size());
        const double half = timeRect.height() * 0.46;
        for (int index = 0; index < timePoints.size(); ++index) {
            const double x = timeRect.left() +
                static_cast<double>(index) * timeRect.width() / (timePoints.size() - 1);
            iPath.append(QPointF(x, timeRect.center().y() -
                (std::clamp)(timePoints[index].real(), -1.0f, 1.0f) * half));
            qPath.append(QPointF(x, timeRect.center().y() -
                (std::clamp)(timePoints[index].imag(), -1.0f, 1.0f) * half));
        }
        painter.setPen(QPen(QColor(42, 210, 154), 1.0));
        painter.drawPolyline(iPath);
        painter.setPen(QPen(QColor(75, 155, 255), 1.0));
        painter.drawPolyline(qPath);
    }

    painter.setPen(QColor(255, 190, 76, 150));
    const double radius = (std::min)(iqRect.width(), iqRect.height()) * 0.40;
    for (const std::complex<float> &sample : constellation) {
        const double x = iqRect.center().x() +
                         (std::clamp)(sample.real(), -1.0f, 1.0f) * radius;
        const double y = iqRect.center().y() -
                         (std::clamp)(sample.imag(), -1.0f, 1.0f) * radius;
        painter.drawPoint(QPointF(x, y));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(240, 244, 248));
    for (const auto &sample : symbols) {
        painter.drawEllipse(QPointF(iqRect.center().x() + sample.real() * radius,
                                   iqRect.center().y() - sample.imag() * radius), 2.1, 2.1);
    }
    painter.setPen(QColor(180, 190, 200));
    painter.drawText(timeRect.adjusted(6, 4, -6, -4),
                     Qt::AlignLeft | Qt::AlignTop,
                     (ukrainian ? QString::fromUtf8(u8"I / Q у часі") : QStringLiteral("I / Q time")) +
                         QStringLiteral("  0 - %1 ms").arg(timeSpanSeconds * 1000, 0, 'f', 3));
    painter.drawText(iqRect.adjusted(6, 4, -6, -4),
                     Qt::AlignLeft | Qt::AlignTop,
                     ukrainian ? QString::fromUtf8(u8"I / Q: траєкторія")
                               : QStringLiteral("I / Q: trajectory"));
    if (!symbols.isEmpty()) {
        painter.drawText(iqRect.adjusted(6, 4, -6, -4), Qt::AlignRight | Qt::AlignTop,
                         ukrainian ? QString::fromUtf8(u8"RRC: %1 символів").arg(symbols.size())
                                   : QStringLiteral("RRC: %1 symbols").arg(symbols.size()));
    }
    painter.drawText(QRectF(iqRect.center().x() + radius + 18, iqRect.center().y() + radius + 2, 20, 16),
                     Qt::AlignCenter, QStringLiteral("I"));
    painter.drawText(QRectF(iqRect.center().x() - radius - 42, iqRect.center().y() - radius - 24, 30, 16),
                     Qt::AlignCenter, QStringLiteral("Q"));
    for (double tick : {-1.0, -0.5, 0.5, 1.0}) {
        const QString text = QString::number(tick, 'f', 1);
        painter.drawText(QRectF(iqRect.center().x() + tick * radius - 16,
                               iqRect.center().y() + radius + 2, 32, 16),
                         Qt::AlignCenter, text);
        painter.drawText(QRectF(iqRect.center().x() - radius - 42,
                               iqRect.center().y() - tick * radius - 8, 35, 16),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }
}

SignalLabWaterfall2DWidget::SignalLabWaterfall2DWidget(QWidget *parent)
    : QWidget(parent) {
    setMinimumSize(430, 330);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void SignalLabWaterfall2DWidget::setData(const SignalLabSpectralPreviewData &data) {
    waterfall = data.image;
    fftSize = data.fftSize;
    rowCount = data.rowCount;
    sampleRate = data.sampleRate;
    windowSamples = data.windowSamples;
    rbwHz = data.rbwHz;
    durationSeconds = data.durationSeconds;
    minimumDb = data.minimumDb;
    maximumDb = data.maximumDb;
    update();
}

void SignalLabWaterfall2DWidget::clear() {
    waterfall = QImage();
    fftSize = 0;
    rowCount = 0;
    sampleRate = 0;
    windowSamples = 0;
    rbwHz = 0.0;
    durationSeconds = 0.0;
    update();
}

void SignalLabWaterfall2DWidget::setLanguage(const QString &language) {
    ukrainian = language.toLower().startsWith(QStringLiteral("uk"));
    update();
}

QString SignalLabWaterfall2DWidget::formatFrequency(double frequencyHz) const {
    const double absolute = std::abs(frequencyHz);
    if (absolute >= 1.0e6) {
        return QStringLiteral("%1 MHz").arg(frequencyHz / 1.0e6, 0, 'f', 2);
    }
    if (absolute >= 1.0e3) {
        return QStringLiteral("%1 kHz").arg(frequencyHz / 1.0e3, 0, 'f', 1);
    }
    return QStringLiteral("%1 Hz").arg(frequencyHz, 0, 'f', 0);
}

void SignalLabWaterfall2DWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(10, 13, 17));
    const QRect plot = rect().adjusted(68, 46, -16, -42);
    if (plot.width() <= 0 || plot.height() <= 0) {
        return;
    }
    painter.setPen(QColor(65, 72, 82));
    painter.drawRect(plot);
    if (waterfall.isNull()) {
        painter.setPen(QColor(170, 178, 188));
        painter.drawText(plot, Qt::AlignCenter,
                         ukrainian ? QString::fromUtf8(u8"Спочатку згенеруйте сигнал")
                                   : QStringLiteral("Generate a signal first"));
        return;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(plot, waterfall);
    painter.setPen(QColor(196, 205, 214));
    const double halfRate = sampleRate * 0.5;
    for (int tick = 0; tick <= 4; ++tick) {
        const double ratio = tick / 4.0;
        const int x = plot.left() + static_cast<int>(std::lround(ratio * plot.width()));
        painter.drawLine(x, plot.bottom(), x, plot.bottom() + 5);
        const double frequency = -halfRate + ratio * sampleRate;
        const QString label = formatFrequency(frequency);
        painter.drawText(QRect(x - 55, plot.bottom() + 7, 110, 20),
                         Qt::AlignHCenter | Qt::AlignTop, label);
    }
    for (int tick = 0; tick <= 4; ++tick) {
        const double ratio = tick / 4.0;
        const int y = plot.top() + static_cast<int>(std::lround(ratio * plot.height()));
        painter.drawLine(plot.left() - 5, y, plot.left(), y);
        painter.drawText(QRect(2, y - 10, plot.left() - 10, 20),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1 s").arg(durationSeconds * ratio, 0, 'f', 3));
    }
    painter.drawText(QRect(plot.left(), 2, plot.width(), 20),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(
                         ukrainian ? QString::fromUtf8(u8"2D-водоспад згенерованого IQ")
                                   : QStringLiteral("Generated IQ 2D waterfall"), Qt::ElideRight, plot.width()));
    const QString metadata = QStringLiteral("FFT %1 | N %2 | RBW %3 Hz | %4 %5")
                         .arg(fftSize)
                         .arg(windowSamples)
                         .arg(rbwHz, 0, 'f', 1)
                         .arg(rowCount)
                         .arg(ukrainian ? QString::fromUtf8(u8"рядків") : QStringLiteral("rows"));
    painter.drawText(QRect(plot.left(), 22, plot.width(), 20), Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(metadata, Qt::ElideRight, plot.width()));
}
