#include "researchanalysisdialog.h"

#include "iqbuffer.h"
#include "radiosettings.h"
#include "scientificsessiontools.h"
#include "spectrumpersistencetools.h"
#include "iqconstellationpreview.h"
#include "signallabconstellation3d.h"
#include "appsettingsutils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QJsonDocument>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSpinBox>
#include <QSettings>
#include <QStackedWidget>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <numeric>
#include <utility>

namespace {

constexpr float kFloorDb = -200.0f;
constexpr double kTwoPi = 6.28318530717958647692;

double dbToPower(double db) {
    return std::pow(10.0, db / 10.0);
}

double safeDb(double power) {
    return power > 1.0e-20 ? 10.0 * std::log10(power) : -200.0;
}

float percentile(std::vector<float> values, double fraction) {
    if (values.empty()) return kFloorDb;
    const std::size_t index = static_cast<std::size_t>(
        std::clamp(fraction, 0.0, 1.0) * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

QString frequencyText(double frequencyHz) {
    if (!std::isfinite(frequencyHz)) return QStringLiteral("--");
    if (std::abs(frequencyHz) >= 1.0e9) {
        return QStringLiteral("%1 GHz").arg(frequencyHz / 1.0e9, 0, 'f', 6);
    }
    if (std::abs(frequencyHz) >= 1.0e6) {
        return QStringLiteral("%1 MHz").arg(frequencyHz / 1.0e6, 0, 'f', 6);
    }
    if (std::abs(frequencyHz) >= 1.0e3) {
        return QStringLiteral("%1 kHz").arg(frequencyHz / 1.0e3, 0, 'f', 3);
    }
    return QStringLiteral("%1 Hz").arg(frequencyHz, 0, 'f', 1);
}

std::vector<float> reduced(const std::vector<float> &values, std::size_t limit) {
    if (values.size() <= limit || limit == 0) return values;
    std::vector<float> result;
    result.reserve(limit);
    for (std::size_t pixel = 0; pixel < limit; ++pixel) {
        const std::size_t first = pixel * values.size() / limit;
        const std::size_t last = (pixel + 1) * values.size() / limit;
        double sum = 0.0;
        int count = 0;
        for (std::size_t i = first; i < last; ++i) {
            if (std::isfinite(values[i])) {
                sum += values[i];
                ++count;
            }
        }
        result.push_back(count > 0 ? static_cast<float>(sum / count) : 0.0f);
    }
    return result;
}

std::complex<double> interpolatedComplex(const std::vector<float> &interleaved,
                                         double position) {
    const std::size_t sampleCount = interleaved.size() / 2U;
    if (sampleCount == 0U) return {0.0, 0.0};
    if (position <= 0.0) return {interleaved[0], interleaved[1]};
    const double maximum = static_cast<double>(sampleCount - 1U);
    if (position >= maximum) {
        return {interleaved[2U * (sampleCount - 1U)],
                interleaved[2U * (sampleCount - 1U) + 1U]};
    }
    const std::size_t left = static_cast<std::size_t>(position);
    const double fraction = position - static_cast<double>(left);
    const std::complex<double> a(interleaved[2U * left], interleaved[2U * left + 1U]);
    const std::complex<double> b(interleaved[2U * (left + 1U)],
                                 interleaved[2U * (left + 1U) + 1U]);
    return a + (b - a) * fraction;
}

void fftRadix2(std::vector<std::complex<float>> &values) {
    const std::size_t count = values.size();
    if (count < 2 || (count & (count - 1U)) != 0U) return;
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        std::size_t bit = count >> 1U;
        for (; j & bit; bit >>= 1U) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1U) {
        const float angle = static_cast<float>(-kTwoPi / static_cast<double>(length));
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t base = 0; base < count; base += length) {
            std::complex<float> twiddle(1.0f, 0.0f);
            const std::size_t half = length >> 1U;
            for (std::size_t offset = 0; offset < half; ++offset) {
                const std::complex<float> even = values[base + offset];
                const std::complex<float> odd = values[base + offset + half] * twiddle;
                values[base + offset] = even + odd;
                values[base + offset + half] = even - odd;
                twiddle *= step;
            }
        }
    }
}

class ResearchPlotWidgetImpl : public QWidget {
public:
    enum Mode { Interference, Statistics, Iq, Synchronization, Dual };
    enum IqView { CombinedIq = 0, OscilloscopeIq = 1, ConstellationIq = 2, EyeIq = 3 };

    explicit ResearchPlotWidgetImpl(Mode mode, QWidget *parent = nullptr)
        : QWidget(parent), mode(mode) {
        setMinimumHeight(220);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setLabels(const QString &firstLabel,
                   const QString &secondLabel,
                   const QString &thirdLabel) {
        labelFirst = firstLabel;
        labelSecond = secondLabel;
        labelThird = thirdLabel;
        update();
    }

    void setInterference(const std::vector<float> &current,
                         const std::vector<float> &reference,
                         const std::vector<int> &peakBins) {
        first = reduced(current, 2048);
        second = reduced(reference, 2048);
        markers.clear();
        if (!current.empty() && !first.empty()) {
            for (int bin : peakBins) {
                markers.push_back(static_cast<int>(
                    std::clamp(static_cast<double>(bin) * first.size() / current.size(),
                               0.0,
                               static_cast<double>(first.size() - 1))));
            }
        }
        update();
    }

    void setStatistics(const std::vector<float> &history,
                       const std::vector<float> &histogram,
                       const std::vector<float> &ccdf,
                       const std::vector<float> &heatmap,
                       int heatmapColumns,
                       int heatmapRows) {
        first = history;
        second = histogram;
        third = ccdf;
        heatmapValues = heatmap;
        heatmapWidth = heatmapColumns;
        heatmapHeight = heatmapRows;
        update();
    }

    void setIq(const std::vector<float> &iData,
               const std::vector<float> &qData,
               const std::vector<float> &autocorrelation,
               const std::vector<float> &constellationI,
               const std::vector<float> &constellationQ,
               const std::vector<float> &eyeI,
               const std::vector<float> &eyeQ,
               int eyePoints,
               int viewMode) {
        first = reduced(iData, 1024);
        second = reduced(qData, 1024);
        third = autocorrelation;
        iqScatterFirst = reduced(constellationI, 3000);
        iqScatterSecond = reduced(constellationQ, 3000);
        iqEyeFirst = eyeI;
        iqEyeSecond = eyeQ;
        iqEyePoints = eyePoints;
        iqView = (std::clamp)(viewMode,
                              static_cast<int>(CombinedIq),
                              static_cast<int>(EyeIq));
        update();
    }

    void setSynchronization(const std::vector<float> &beforeI,
                            const std::vector<float> &beforeQ,
                            const std::vector<float> &afterI,
                            const std::vector<float> &afterQ,
                            const std::vector<float> &phaseMetric,
                            const std::vector<float> &timingError,
                            int selectedPhaseBin) {
        syncBeforeI = reduced(beforeI, 3000);
        syncBeforeQ = reduced(beforeQ, 3000);
        syncAfterI = reduced(afterI, 3000);
        syncAfterQ = reduced(afterQ, 3000);
        syncPhaseMetric = phaseMetric;
        syncTimingError = reduced(timingError, 2048);
        syncSelectedPhaseBin = selectedPhaseBin;
        update();
    }

    void setDual(const std::vector<float> &mainData,
                 const std::vector<float> &referenceData,
                 const std::vector<float> &difference,
                 const std::vector<float> &crossCorrelation,
                 const std::vector<float> &coherence) {
        first = mainData;
        second = referenceData;
        third = difference;
        fourth = crossCorrelation;
        fifth = coherence;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(15, 18, 22));
        const QRectF area = rect().adjusted(42, 12, -12, -28);
        painter.setPen(QColor(58, 64, 72));
        for (int i = 0; i <= 4; ++i) {
            const double y = area.top() + area.height() * i / 4.0;
            painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        }
        for (int i = 0; i <= 8; ++i) {
            const double x = area.left() + area.width() * i / 8.0;
            painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        }

        if (mode == Statistics) {
            drawStatistics(painter, area);
        } else if (mode == Iq) {
            drawIq(painter, area);
        } else if (mode == Synchronization) {
            drawSynchronization(painter, area);
        } else if (mode == Dual) {
            drawDual(painter, area);
        } else {
            drawLines(painter, area);
        }
        painter.setPen(QColor(175, 182, 190));
        painter.drawRect(area);
    }

private:
    static std::pair<float, float> finiteRange(const std::vector<float> &a,
                                                const std::vector<float> &b = {}) {
        float minimum = std::numeric_limits<float>::infinity();
        float maximum = -std::numeric_limits<float>::infinity();
        auto scan = [&](const std::vector<float> &values) {
            for (float value : values) {
                if (!std::isfinite(value)) continue;
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        };
        scan(a);
        scan(b);
        if (!std::isfinite(minimum) || !std::isfinite(maximum)) return {-1.0f, 1.0f};
        if (maximum - minimum < 1.0e-6f) {
            minimum -= 1.0f;
            maximum += 1.0f;
        }
        return {minimum, maximum};
    }

    static void drawVector(QPainter &painter,
                           const QRectF &area,
                           const std::vector<float> &values,
                           const QColor &color,
                           float minimum,
                           float maximum) {
        if (values.size() < 2) return;
        QPainterPath path;
        bool started = false;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (!std::isfinite(values[i])) continue;
            const double x = area.left() + area.width() * i / (values.size() - 1);
            const double normalized = (values[i] - minimum) / (maximum - minimum);
            const double y = area.bottom() - area.height() * std::clamp(normalized, 0.0, 1.0);
            if (!started) {
                path.moveTo(x, y);
                started = true;
            } else {
                path.lineTo(x, y);
            }
        }
        painter.setPen(QPen(color, 1.4));
        painter.drawPath(path);
    }

    void drawLines(QPainter &painter, const QRectF &area) {
        const auto range = finiteRange(first, second);
        drawVector(painter, area, first, QColor(70, 220, 255), range.first, range.second);
        drawVector(painter, area, second, QColor(255, 185, 65), range.first, range.second);
        if (!third.empty()) {
            const auto thirdRange = finiteRange(third);
            drawVector(painter, area, third, QColor(215, 90, 255), thirdRange.first, thirdRange.second);
        }
        if (mode == Interference && !first.empty()) {
            painter.setPen(QPen(QColor(255, 90, 80), 1));
            for (int marker : markers) {
                if (marker < 0 || marker >= static_cast<int>(first.size())) continue;
                const double x = area.left() + area.width() * marker / (first.size() - 1);
                painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            }
        }
    }

    void drawStatistics(QPainter &painter, const QRectF &area) {
        const QRectF historyArea(area.left(), area.top(), area.width() * 0.58, area.height() * 0.43);
        const QRectF heatmapArea(area.left(), area.top() + area.height() * 0.51,
                                 area.width() * 0.58, area.height() * 0.49);
        const QRectF distributionArea(area.left() + area.width() * 0.62,
                                      area.top(),
                                      area.width() * 0.38,
                                      area.height());
        const auto historyRange = finiteRange(first);
        drawVector(painter, historyArea, first, QColor(70, 220, 255), historyRange.first, historyRange.second);
        if (heatmapWidth > 0 && heatmapHeight > 0 &&
            static_cast<int>(heatmapValues.size()) >= heatmapWidth * heatmapHeight) {
            QImage image(heatmapWidth, heatmapHeight, QImage::Format_RGB32);
            const auto heatmapRange = finiteRange(heatmapValues);
            const float heatmapSpan = std::max(1.0f, heatmapRange.second - heatmapRange.first);
            for (int row = 0; row < heatmapHeight; ++row) {
                const float *source = heatmapValues.data() + static_cast<std::size_t>(row * heatmapWidth);
                for (int column = 0; column < heatmapWidth; ++column) {
                    const float normalized = std::clamp(
                        (source[column] - heatmapRange.first) / heatmapSpan, 0.0f, 1.0f);
                    const QColor color = QColor::fromHsvF(0.66 - normalized * 0.66,
                                                          0.88,
                                                          0.12 + normalized * 0.88);
                    image.setPixelColor(column, heatmapHeight - 1 - row, color);
                }
            }
            painter.drawImage(heatmapArea, image);
        }
        float maxHistogram = 1.0f;
        for (float value : second) maxHistogram = std::max(maxHistogram, value);
        if (!second.empty()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(70, 220, 255, 110));
            const double barWidth = distributionArea.width() / second.size();
            for (std::size_t i = 0; i < second.size(); ++i) {
                const double height = distributionArea.height() * second[i] / maxHistogram;
                painter.drawRect(QRectF(distributionArea.left() + i * barWidth,
                                        distributionArea.bottom() - height,
                                        std::max(1.0, barWidth - 1.0),
                                        height));
            }
        }
        drawVector(painter, distributionArea, third, QColor(255, 185, 65), 0.0f, 100.0f);
        painter.setPen(QColor(175, 182, 190));
        painter.drawText(historyArea.adjusted(4, 4, -4, -4), Qt::AlignLeft | Qt::AlignTop,
                         labelFirst);
        painter.drawText(distributionArea.adjusted(4, 4, -4, -4), Qt::AlignLeft | Qt::AlignTop,
                         labelSecond);
        painter.drawText(heatmapArea.adjusted(4, 4, -4, -4), Qt::AlignLeft | Qt::AlignTop,
                         labelThird);
    }

    void drawConstellation(QPainter &painter, const QRectF &area) {
        const std::vector<float> &scatterI = iqScatterFirst.empty() ? first : iqScatterFirst;
        const std::vector<float> &scatterQ = iqScatterSecond.empty() ? second : iqScatterSecond;
        const auto range = finiteRange(scatterI, scatterQ);
        const float absolute = std::max(std::abs(range.first), std::abs(range.second));
        const float scale = absolute > 1.0e-9f ? absolute : 1.0f;
        const double side = std::min(area.width(), area.height());
        const QRectF square(area.center().x() - side * 0.5,
                            area.center().y() - side * 0.5,
                            side,
                            side);
        painter.setPen(QPen(QColor(82, 90, 100), 1));
        painter.drawEllipse(square.center(), square.width() * 0.24, square.height() * 0.24);
        painter.drawEllipse(square.center(), square.width() * 0.48, square.height() * 0.48);
        painter.drawLine(QPointF(square.left(), square.center().y()),
                         QPointF(square.right(), square.center().y()));
        painter.drawLine(QPointF(square.center().x(), square.top()),
                         QPointF(square.center().x(), square.bottom()));
        painter.setPen(QPen(QColor(125, 235, 160, 165), 2));
        const std::size_t count = std::min(scatterI.size(), scatterQ.size());
        for (std::size_t i = 0; i < count; ++i) {
            const double x = square.center().x() +
                             scatterI[i] / scale * square.width() * 0.48;
            const double y = square.center().y() -
                             scatterQ[i] / scale * square.height() * 0.48;
            painter.drawPoint(QPointF(x, y));
        }
        painter.setPen(QColor(175, 182, 190));
        painter.drawText(square.adjusted(5, 5, -5, -5),
                         Qt::AlignLeft | Qt::AlignTop,
                         QStringLiteral("Q"));
        painter.drawText(square.adjusted(5, 5, -5, -5),
                         Qt::AlignRight | Qt::AlignBottom,
                         QStringLiteral("I"));
    }

    void drawEye(QPainter &painter, const QRectF &area) {
        if (iqEyePoints < 2 ||
            iqEyeFirst.size() < static_cast<std::size_t>(iqEyePoints)) {
            painter.setPen(QColor(175, 182, 190));
            painter.drawText(area, Qt::AlignCenter, QStringLiteral("Eye: no complete traces"));
            return;
        }
        const auto range = finiteRange(iqEyeFirst, iqEyeSecond);
        const float maximum = std::max(std::abs(range.first), std::abs(range.second));
        const float scale = maximum > 1.0e-9f ? maximum : 1.0f;
        painter.setPen(QPen(QColor(90, 98, 108), 1));
        painter.drawLine(QPointF(area.left(), area.center().y()),
                         QPointF(area.right(), area.center().y()));
        painter.drawLine(QPointF(area.center().x(), area.top()),
                         QPointF(area.center().x(), area.bottom()));
        auto drawTraces = [&](const std::vector<float> &values, const QColor &color) {
            const std::size_t traceCount =
                values.size() / static_cast<std::size_t>(iqEyePoints);
            painter.setPen(QPen(color, 1.15));
            for (std::size_t trace = 0; trace < traceCount; ++trace) {
                QPainterPath path;
                for (int point = 0; point < iqEyePoints; ++point) {
                    const float value =
                        values[trace * static_cast<std::size_t>(iqEyePoints) +
                               static_cast<std::size_t>(point)];
                    const double x = area.left() +
                                     area.width() * point /
                                         static_cast<double>(iqEyePoints - 1);
                    const double y = area.center().y() -
                                     value / scale * area.height() * 0.46;
                    if (point == 0) path.moveTo(x, y);
                    else path.lineTo(x, y);
                }
                painter.drawPath(path);
            }
        };
        drawTraces(iqEyeFirst, QColor(70, 220, 255, 72));
        drawTraces(iqEyeSecond, QColor(255, 185, 65, 58));
        painter.setPen(QColor(175, 182, 190));
        painter.drawText(area.adjusted(5, 5, -5, -5),
                         Qt::AlignLeft | Qt::AlignTop,
                         QStringLiteral("2 symbols"));
    }

    void drawIq(QPainter &painter, const QRectF &area) {
        if (iqView == OscilloscopeIq) {
            const auto range = finiteRange(first, second);
            drawVector(painter, area, first, QColor(70, 220, 255), range.first, range.second);
            drawVector(painter, area, second, QColor(255, 185, 65), range.first, range.second);
            painter.setPen(QColor(175, 182, 190));
            painter.drawText(area.adjusted(5, 5, -5, -5),
                             Qt::AlignLeft | Qt::AlignTop,
                             QStringLiteral("I"));
            painter.drawText(area.adjusted(5, 5, -5, -5),
                             Qt::AlignRight | Qt::AlignTop,
                             QStringLiteral("Q"));
            return;
        }
        if (iqView == ConstellationIq) {
            drawConstellation(painter, area);
            return;
        }
        if (iqView == EyeIq) {
            drawEye(painter, area);
            return;
        }

        const QRectF timeArea(area.left(), area.top(), area.width() * 0.58, area.height());
        const QRectF scatterArea(area.left() + area.width() * 0.62,
                                 area.top(),
                                 area.width() * 0.38,
                                 area.height());
        const auto range = finiteRange(first, second);
        drawVector(painter, timeArea, first, QColor(70, 220, 255), range.first, range.second);
        drawVector(painter, timeArea, second, QColor(255, 185, 65), range.first, range.second);
        drawConstellation(painter, scatterArea);
        if (!third.empty()) {
            const QRectF autoArea(timeArea.left(), timeArea.bottom() - timeArea.height() * 0.26,
                                  timeArea.width(), timeArea.height() * 0.26);
            drawVector(painter, autoArea, third, QColor(215, 90, 255), -1.0f, 1.0f);
        }
    }

    static void drawScatter(QPainter &painter,
                            const QRectF &area,
                            const std::vector<float> &iValues,
                            const std::vector<float> &qValues,
                            const QColor &color,
                            const QString &title) {
        const std::size_t count = std::min(iValues.size(), qValues.size());
        if (count == 0U) {
            painter.setPen(QColor(175, 182, 190));
            painter.drawText(area, Qt::AlignCenter, title);
            return;
        }
        float scale = 1.0e-9f;
        for (std::size_t index = 0; index < count; ++index) {
            if (!std::isfinite(iValues[index]) || !std::isfinite(qValues[index])) continue;
            scale = std::max(scale, std::abs(iValues[index]));
            scale = std::max(scale, std::abs(qValues[index]));
        }
        const double side = std::min(area.width(), area.height());
        const QRectF square(area.center().x() - side * 0.5,
                            area.center().y() - side * 0.5,
                            side,
                            side);
        painter.setPen(QPen(QColor(82, 90, 100), 1));
        painter.drawEllipse(square.center(), square.width() * 0.24, square.height() * 0.24);
        painter.drawEllipse(square.center(), square.width() * 0.48, square.height() * 0.48);
        painter.drawLine(QPointF(square.left(), square.center().y()),
                         QPointF(square.right(), square.center().y()));
        painter.drawLine(QPointF(square.center().x(), square.top()),
                         QPointF(square.center().x(), square.bottom()));
        painter.setPen(QPen(color, 2));
        for (std::size_t index = 0; index < count; ++index) {
            const double x = square.center().x() + iValues[index] / scale * square.width() * 0.47;
            const double y = square.center().y() - qValues[index] / scale * square.height() * 0.47;
            painter.drawPoint(QPointF(x, y));
        }
        painter.setPen(QColor(205, 211, 218));
        painter.drawText(square.adjusted(5, 5, -5, -5), Qt::AlignLeft | Qt::AlignTop, title);
    }

    void drawSynchronization(QPainter &painter, const QRectF &area) {
        const double gap = 10.0;
        const double scatterWidth = (area.width() - gap * 2.0) * 0.31;
        const QRectF beforeArea(area.left(), area.top(), scatterWidth, area.height());
        const QRectF afterArea(beforeArea.right() + gap, area.top(), scatterWidth, area.height());
        const QRectF phaseArea(afterArea.right() + gap,
                               area.top(),
                               area.right() - afterArea.right() - gap,
                               area.height() * 0.46);
        const QRectF errorArea(phaseArea.left(),
                               area.top() + area.height() * 0.56,
                               phaseArea.width(),
                               area.height() * 0.44);
        drawScatter(painter, beforeArea, syncBeforeI, syncBeforeQ,
                    QColor(255, 185, 65, 175), labelFirst);
        drawScatter(painter, afterArea, syncAfterI, syncAfterQ,
                    QColor(125, 235, 160, 185), labelSecond);
        const auto metricRange = finiteRange(syncPhaseMetric);
        drawVector(painter, phaseArea, syncPhaseMetric, QColor(70, 220, 255),
                   metricRange.first, metricRange.second);
        if (syncSelectedPhaseBin >= 0 &&
            syncSelectedPhaseBin < static_cast<int>(syncPhaseMetric.size()) &&
            syncPhaseMetric.size() > 1U) {
            const double x = phaseArea.left() + phaseArea.width() * syncSelectedPhaseBin /
                                                   static_cast<double>(syncPhaseMetric.size() - 1U);
            painter.setPen(QPen(QColor(255, 95, 80), 1.5));
            painter.drawLine(QPointF(x, phaseArea.top()), QPointF(x, phaseArea.bottom()));
        }
        const auto errorRange = finiteRange(syncTimingError);
        const float errorMagnitude = std::max(std::abs(errorRange.first), std::abs(errorRange.second));
        drawVector(painter, errorArea, syncTimingError, QColor(215, 90, 255),
                   -std::max(1.0e-6f, errorMagnitude), std::max(1.0e-6f, errorMagnitude));
        painter.setPen(QColor(205, 211, 218));
        painter.drawText(phaseArea.adjusted(5, 5, -5, -5),
                         Qt::AlignLeft | Qt::AlignTop, labelThird);
        painter.drawText(errorArea.adjusted(5, 5, -5, -5),
                         Qt::AlignLeft | Qt::AlignTop, QStringLiteral("Gardner error"));
    }

    void drawDual(QPainter &painter, const QRectF &area) {
        const QRectF spectrumArea(area.left(), area.top(), area.width(), area.height() * 0.62);
        const QRectF correlationArea(area.left(), area.top() + area.height() * 0.70,
                                     area.width() * 0.48, area.height() * 0.30);
        const QRectF coherenceArea(area.left() + area.width() * 0.52,
                                   area.top() + area.height() * 0.70,
                                   area.width() * 0.48, area.height() * 0.30);
        const auto spectrumRange = finiteRange(first, second);
        drawVector(painter, spectrumArea, first, QColor(70, 220, 255), spectrumRange.first, spectrumRange.second);
        drawVector(painter, spectrumArea, second, QColor(255, 185, 65), spectrumRange.first, spectrumRange.second);
        if (!third.empty()) {
            const auto differenceRange = finiteRange(third);
            drawVector(painter, spectrumArea, third, QColor(215, 90, 255), differenceRange.first, differenceRange.second);
        }
        drawVector(painter, correlationArea, fourth, QColor(125, 235, 160), -1.0f, 1.0f);
        drawVector(painter, coherenceArea, fifth, QColor(255, 110, 90), 0.0f, 1.0f);
        painter.setPen(QColor(175, 182, 190));
        painter.drawText(spectrumArea.adjusted(4, 4, -4, -4), Qt::AlignLeft | Qt::AlignTop,
                         labelFirst);
        painter.drawText(correlationArea.adjusted(4, 4, -4, -4), Qt::AlignLeft | Qt::AlignTop,
                         labelSecond);
        painter.drawText(coherenceArea.adjusted(4, 4, -4, -4), Qt::AlignLeft | Qt::AlignTop,
                         labelThird);
    }

    Mode mode;
    std::vector<float> first;
    std::vector<float> second;
    std::vector<float> third;
    std::vector<float> fourth;
    std::vector<float> fifth;
    std::vector<float> iqScatterFirst;
    std::vector<float> iqScatterSecond;
    std::vector<float> iqEyeFirst;
    std::vector<float> iqEyeSecond;
    int iqEyePoints = 0;
    int iqView = CombinedIq;
    std::vector<float> syncBeforeI;
    std::vector<float> syncBeforeQ;
    std::vector<float> syncAfterI;
    std::vector<float> syncAfterQ;
    std::vector<float> syncPhaseMetric;
    std::vector<float> syncTimingError;
    int syncSelectedPhaseBin = -1;
    std::vector<int> markers;
    std::vector<float> heatmapValues;
    int heatmapWidth = 0;
    int heatmapHeight = 0;
    QString labelFirst;
    QString labelSecond;
    QString labelThird;
};

} // namespace

class ResearchPlotWidget : public ResearchPlotWidgetImpl {
public:
    using ResearchPlotWidgetImpl::ResearchPlotWidgetImpl;
};

ResearchAnalysisDialog::ResearchAnalysisDialog(Translator translator,
                                               ContextProvider contextProvider,
                                               SpectrumSettingsProvider settingsProvider,
                                               SpectrumSettingsApplier settingsApplier,
                                               LiveSyncApplier liveSyncApplier,
                                               MaskTriggerHandler maskTriggerHandler,
                                               SessionFrequencySetter sessionFrequencySetter,
                                               QWidget *parent)
    : QDialog(parent),
      translator(std::move(translator)),
      contextProvider(std::move(contextProvider)),
      spectrumSettingsProvider(std::move(settingsProvider)),
      spectrumSettingsApplier(std::move(settingsApplier)),
      liveSyncApplier(std::move(liveSyncApplier)),
      maskTriggerHandler(std::move(maskTriggerHandler)),
      sessionFrequencySetter(std::move(sessionFrequencySetter)) {
    setWindowTitle(trText(QStringLiteral("research_tools_title"), QStringLiteral("Research tools")));
    resize(980, 680);
    setAttribute(Qt::WA_DeleteOnClose, false);

    QVBoxLayout *root = new QVBoxLayout(this);
    tabs = new QTabWidget(this);
    root->addWidget(tabs);

    QWidget *interferencePage = new QWidget(tabs);
    QVBoxLayout *interferenceLayout = new QVBoxLayout(interferencePage);
    QGridLayout *interferenceControls = new QGridLayout();
    interferenceProminenceSpin = new QDoubleSpinBox(interferencePage);
    interferenceProminenceSpin->setRange(1.0, 60.0);
    interferenceProminenceSpin->setValue(6.0);
    interferenceProminenceSpin->setSuffix(QStringLiteral(" dB"));
    interferenceMinSpacingSpin = new QDoubleSpinBox(interferencePage);
    interferenceMinSpacingSpin->setRange(0.01, 10000.0);
    interferenceMinSpacingSpin->setValue(1.0);
    interferenceMinSpacingSpin->setSuffix(QStringLiteral(" kHz"));
    interferenceMaxSpacingSpin = new QDoubleSpinBox(interferencePage);
    interferenceMaxSpacingSpin->setRange(0.1, 50000.0);
    interferenceMaxSpacingSpin->setValue(2000.0);
    interferenceMaxSpacingSpin->setSuffix(QStringLiteral(" kHz"));
    interferenceCaptureButton = new QPushButton(interferencePage);
    interferenceClearButton = new QPushButton(interferencePage);
    interferenceControls->addWidget(new QLabel(trText(QStringLiteral("research_prominence"), QStringLiteral("Prominence:")), interferencePage), 0, 0);
    interferenceControls->addWidget(interferenceProminenceSpin, 0, 1);
    interferenceControls->addWidget(new QLabel(trText(QStringLiteral("research_spacing_range"), QStringLiteral("Comb spacing:")), interferencePage), 0, 2);
    interferenceControls->addWidget(interferenceMinSpacingSpin, 0, 3);
    interferenceControls->addWidget(interferenceMaxSpacingSpin, 0, 4);
    interferenceControls->addWidget(interferenceCaptureButton, 0, 5);
    interferenceControls->addWidget(interferenceClearButton, 0, 6);
    interferenceControls->setColumnStretch(7, 1);
    interferenceLayout->addLayout(interferenceControls);
    interferenceStatus = new QLabel(interferencePage);
    interferenceStatus->setWordWrap(true);
    interferenceLayout->addWidget(interferenceStatus);
    interferencePlot = new ResearchPlotWidget(ResearchPlotWidgetImpl::Interference, interferencePage);
    interferenceLayout->addWidget(interferencePlot, 2);
    interferenceTable = new QTableWidget(interferencePage);
    interferenceTable->setColumnCount(6);
    interferenceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    interferenceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    interferenceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    interferenceLayout->addWidget(interferenceTable, 1);
    tabs->addTab(interferencePage, trText(QStringLiteral("research_interference_tab"), QStringLiteral("Interference / combs")));

    QWidget *statisticsPage = new QWidget(tabs);
    QVBoxLayout *statisticsLayout = new QVBoxLayout(statisticsPage);
    QGridLayout *statisticsControls = new QGridLayout();
    statisticsThresholdSpin = new QDoubleSpinBox(statisticsPage);
    statisticsThresholdSpin->setRange(-200.0, 50.0);
    statisticsThresholdSpin->setValue(-80.0);
    statisticsThresholdSpin->setSuffix(QStringLiteral(" dB"));
    statisticsHistorySpin = new QSpinBox(statisticsPage);
    statisticsHistorySpin->setRange(10, 36000);
    statisticsHistorySpin->setValue(1800);
    statisticsHistorySpin->setSuffix(QStringLiteral(" samples"));
    statisticsResetButton = new QPushButton(statisticsPage);
    statisticsControls->addWidget(new QLabel(trText(QStringLiteral("research_threshold"), QStringLiteral("Threshold:")), statisticsPage), 0, 0);
    statisticsControls->addWidget(statisticsThresholdSpin, 0, 1);
    statisticsControls->addWidget(new QLabel(trText(QStringLiteral("research_history"), QStringLiteral("History:")), statisticsPage), 0, 2);
    statisticsControls->addWidget(statisticsHistorySpin, 0, 3);
    statisticsControls->addWidget(statisticsResetButton, 0, 4);
    statisticsControls->setColumnStretch(5, 1);
    statisticsLayout->addLayout(statisticsControls);
    statisticsStatus = new QLabel(statisticsPage);
    statisticsStatus->setWordWrap(true);
    statisticsLayout->addWidget(statisticsStatus);
    statisticsPlot = new ResearchPlotWidget(ResearchPlotWidgetImpl::Statistics, statisticsPage);
    statisticsLayout->addWidget(statisticsPlot, 1);
    tabs->addTab(statisticsPage, trText(QStringLiteral("research_statistics_tab"), QStringLiteral("Signal statistics")));

    QWidget *iqPage = new QWidget(tabs);
    QVBoxLayout *iqLayout = new QVBoxLayout(iqPage);
    QGridLayout *iqControls = new QGridLayout();
    iqFreezeCheckbox = new QCheckBox(trText(QStringLiteral("freeze"), QStringLiteral("Freeze")), iqPage);
    iqSourceCombo = new QComboBox(iqPage);
    iqSourceCombo->addItem(
        trText(QStringLiteral("research_iq_source_raw"), QStringLiteral("Raw IQ")), 0);
    iqSourceCombo->addItem(
        trText(QStringLiteral("research_iq_source_channel"), QStringLiteral("Channel IQ")), 1);
    iqSourceCombo->setCurrentIndex(1);
    iqViewCombo = new QComboBox(iqPage);
    iqViewCombo->addItem(
        trText(QStringLiteral("research_iq_view_combined"), QStringLiteral("Combined")), 0);
    iqViewCombo->addItem(
        trText(QStringLiteral("research_iq_view_scope"), QStringLiteral("Oscilloscope")), 1);
    iqViewCombo->addItem(
        trText(QStringLiteral("research_iq_view_constellation"), QStringLiteral("Constellation")), 2);
    iqViewCombo->addItem(
        trText(QStringLiteral("research_iq_view_eye"), QStringLiteral("Eye diagram")), 3);
    iqViewCombo->addItem(
        trText(QStringLiteral("research_iq_view_constellation_3d"), QStringLiteral("3D constellation")), 4);
    iqSurfaceCheckbox = new QCheckBox(trText(QStringLiteral("research_iq_constellation_surface"), QStringLiteral("Radial surface")), iqPage);
    iqSurfaceCheckbox->setToolTip(trText(QStringLiteral("research_iq_surface_hint"),
        QStringLiteral("Fill from I=Q=0 to displayed samples. Interpolation is visual only, not reconstructed IQ.")));
    iqSampleCountSpin = new QSpinBox(iqPage);
    iqSampleCountSpin->setRange(1024, 262144);
    iqSampleCountSpin->setSingleStep(1024);
    iqSampleCountSpin->setValue(131072);
    iqSymbolRateSpin = new QDoubleSpinBox(iqPage);
    iqSymbolRateSpin->setDecimals(1);
    iqSymbolRateSpin->setRange(10.0, 1000000.0);
    iqSymbolRateSpin->setValue(4800.0);
    iqSymbolRateSpin->setSuffix(QStringLiteral(" Bd"));
    iqPhaseSpin = new QSpinBox(iqPage);
    iqPhaseSpin->setRange(0, 99);
    iqPhaseSpin->setValue(50);
    iqPhaseSpin->setSuffix(QStringLiteral(" %"));
    iqTraceCountSpin = new QSpinBox(iqPage);
    iqTraceCountSpin->setRange(2, 128);
    iqTraceCountSpin->setValue(32);
    iqControls->addWidget(iqFreezeCheckbox, 0, 0);
    iqControls->addWidget(new QLabel(
        trText(QStringLiteral("research_iq_source"), QStringLiteral("Source:")), iqPage), 0, 1);
    iqControls->addWidget(iqSourceCombo, 0, 2);
    iqControls->addWidget(new QLabel(
        trText(QStringLiteral("research_iq_view"), QStringLiteral("View:")), iqPage), 0, 3);
    iqControls->addWidget(iqViewCombo, 0, 4);
    iqControls->addWidget(iqSurfaceCheckbox, 0, 5, 1, 3);
    iqControls->addWidget(new QLabel(
        trText(QStringLiteral("research_iq_samples"), QStringLiteral("Samples:")), iqPage), 1, 0);
    iqControls->addWidget(iqSampleCountSpin, 1, 1);
    iqControls->addWidget(new QLabel(
        trText(QStringLiteral("research_symbol_rate"), QStringLiteral("Symbol rate:")), iqPage), 1, 2);
    iqControls->addWidget(iqSymbolRateSpin, 1, 3);
    iqControls->addWidget(new QLabel(
        trText(QStringLiteral("research_eye_phase"), QStringLiteral("Phase:")), iqPage), 1, 4);
    iqControls->addWidget(iqPhaseSpin, 1, 5);
    iqControls->addWidget(new QLabel(
        trText(QStringLiteral("research_eye_traces"), QStringLiteral("Traces:")), iqPage), 1, 6);
    iqControls->addWidget(iqTraceCountSpin, 1, 7);
    iqControls->setColumnStretch(8, 1);
    iqLayout->addLayout(iqControls);
    iqStatus = new QLabel(iqPage);
    iqStatus->setWordWrap(true);
    iqLayout->addWidget(iqStatus);
    iqPlot = new ResearchPlotWidget(ResearchPlotWidgetImpl::Iq, iqPage);
    iqPlotStack = new QStackedWidget(iqPage);
    iqPlotStack->addWidget(iqPlot);
    iqConstellation3D = new SignalLabConstellation3DWidget(iqPage);
    QSettings researchSettings(persistentSettingsFilePath(), QSettings::IniFormat);
    iqSurfaceCheckbox->setChecked(researchSettings.value(QStringLiteral("research/iq/constellationSurface"), false).toBool());
    iqConstellation3D->setLanguage(researchSettings.value(QStringLiteral("ui/language"), QStringLiteral("en")).toString());
    iqConstellation3D->setCameraState(QJsonDocument::fromJson(researchSettings.value(
        QStringLiteral("research/iq/constellation3DCamera")).toByteArray()).object());
    iqPlotStack->addWidget(iqConstellation3D);
    iqLayout->addWidget(iqPlotStack, 1);
    const int savedView = iqViewCombo->findData(researchSettings.value(QStringLiteral("research/iq/view"), 0).toInt());
    if (savedView >= 0) iqViewCombo->setCurrentIndex(savedView);
    updateIqView();
    connect(iqViewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { updateIqView(); });
    connect(iqSurfaceCheckbox, &QCheckBox::toggled, this, [this]() { updateIqView(); });
    connect(iqSymbolRateSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this]() { updateIqView(); });
    connect(iqPhaseSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { updateIqView(); });
    tabs->addTab(iqPage, trText(QStringLiteral("research_iq_tab"), QStringLiteral("IQ analysis")));

    QWidget *syncPage = new QWidget(tabs);
    QVBoxLayout *syncLayout = new QVBoxLayout(syncPage);
    QGridLayout *syncControls = new QGridLayout();
    syncFreezeCheckbox = new QCheckBox(
        trText(QStringLiteral("freeze"), QStringLiteral("Freeze")), syncPage);
    syncModulationCombo = new QComboBox(syncPage);
    syncModulationCombo->addItem(
        trText(QStringLiteral("auto"), QStringLiteral("Auto")), 0);
    syncModulationCombo->addItem(QStringLiteral("BPSK"), 2);
    syncModulationCombo->addItem(QStringLiteral("QPSK"), 4);
    syncModulationCombo->addItem(QStringLiteral("8PSK"), 8);
    syncModulationCombo->addItem(QStringLiteral("FSK / FM"), 1);
    syncSampleCountSpin = new QSpinBox(syncPage);
    syncSampleCountSpin->setRange(16384, 1048576);
    syncSampleCountSpin->setSingleStep(16384);
    syncSampleCountSpin->setValue(262144);
    syncSymbolRateSpin = new QDoubleSpinBox(syncPage);
    syncSymbolRateSpin->setDecimals(1);
    syncSymbolRateSpin->setRange(10.0, 1000000.0);
    syncSymbolRateSpin->setValue(4800.0);
    syncSymbolRateSpin->setSuffix(QStringLiteral(" Bd"));
    syncPhaseBinsSpin = new QSpinBox(syncPage);
    syncPhaseBinsSpin->setRange(8, 256);
    syncPhaseBinsSpin->setValue(64);
    syncCarrierCorrectionCheckbox = new QCheckBox(
        trText(QStringLiteral("research_sync_carrier_correction"),
               QStringLiteral("Correct carrier")), syncPage);
    syncCarrierCorrectionCheckbox->setChecked(true);
    syncPhaseCorrectionCheckbox = new QCheckBox(
        trText(QStringLiteral("research_sync_phase_correction"),
               QStringLiteral("Correct phase")), syncPage);
    syncPhaseCorrectionCheckbox->setChecked(true);
    syncLiveButton = new QPushButton(syncPage);
    syncLiveButton->setCheckable(true);
    syncControls->addWidget(syncFreezeCheckbox, 0, 0);
    syncControls->addWidget(new QLabel(
        trText(QStringLiteral("research_sync_signal"), QStringLiteral("Signal:")), syncPage), 0, 1);
    syncControls->addWidget(syncModulationCombo, 0, 2);
    syncControls->addWidget(new QLabel(
        trText(QStringLiteral("research_symbol_rate"), QStringLiteral("Symbol rate:")), syncPage), 0, 3);
    syncControls->addWidget(syncSymbolRateSpin, 0, 4);
    syncControls->addWidget(new QLabel(
        trText(QStringLiteral("research_iq_samples"), QStringLiteral("Samples:")), syncPage), 1, 0);
    syncControls->addWidget(syncSampleCountSpin, 1, 1);
    syncControls->addWidget(new QLabel(
        trText(QStringLiteral("research_sync_phase_bins"), QStringLiteral("Phase bins:")), syncPage), 1, 2);
    syncControls->addWidget(syncPhaseBinsSpin, 1, 3);
    syncControls->addWidget(syncCarrierCorrectionCheckbox, 1, 4);
    syncControls->addWidget(syncPhaseCorrectionCheckbox, 1, 5);
    syncControls->addWidget(syncLiveButton, 0, 5);
    syncControls->setColumnStretch(6, 1);
    syncLayout->addLayout(syncControls);
    syncStatus = new QLabel(syncPage);
    syncStatus->setWordWrap(true);
    syncLayout->addWidget(syncStatus);
    syncPlot = new ResearchPlotWidget(ResearchPlotWidgetImpl::Synchronization, syncPage);
    syncLayout->addWidget(syncPlot, 1);
    tabs->addTab(syncPage,
                 trText(QStringLiteral("research_sync_tab"),
                        QStringLiteral("Digital synchronization")));

    QWidget *dualPage = new QWidget(tabs);
    QVBoxLayout *dualLayout = new QVBoxLayout(dualPage);
    dualFreezeCheckbox = new QCheckBox(trText(QStringLiteral("freeze"), QStringLiteral("Freeze")), dualPage);
    dualLayout->addWidget(dualFreezeCheckbox);
    dualStatus = new QLabel(dualPage);
    dualStatus->setWordWrap(true);
    dualLayout->addWidget(dualStatus);
    dualPlot = new ResearchPlotWidget(ResearchPlotWidgetImpl::Dual, dualPage);
    dualLayout->addWidget(dualPlot, 1);
    tabs->addTab(dualPage, trText(QStringLiteral("research_dual_tab"), QStringLiteral("Dual HF inputs")));

    QWidget *analyzerPage = new QWidget(tabs);
    QVBoxLayout *analyzerLayout = new QVBoxLayout(analyzerPage);
    QGridLayout *analyzerControls = new QGridLayout();
    detectorCombo = new QComboBox(analyzerPage);
    detectorCombo->addItem(trText(QStringLiteral("detector_sample"), QStringLiteral("Sample")), SPECTRUM_DETECTOR_SAMPLE);
    detectorCombo->addItem(trText(QStringLiteral("detector_positive_peak"), QStringLiteral("Positive peak")), SPECTRUM_DETECTOR_POSITIVE_PEAK);
    detectorCombo->addItem(trText(QStringLiteral("detector_negative_peak"), QStringLiteral("Negative peak")), SPECTRUM_DETECTOR_NEGATIVE_PEAK);
    detectorCombo->addItem(QStringLiteral("RMS"), SPECTRUM_DETECTOR_RMS);
    detectorCombo->addItem(trText(QStringLiteral("detector_average"), QStringLiteral("Average")), SPECTRUM_DETECTOR_AVERAGE);
    detectorCombo->addItem(trText(QStringLiteral("detector_median"), QStringLiteral("Median")), SPECTRUM_DETECTOR_MEDIAN);
    detectorCombo->addItem(trText(QStringLiteral("detector_quasi_peak"), QStringLiteral("Quasi-peak")), SPECTRUM_DETECTOR_QUASI_PEAK);
    detectorFramesSpin = new QSpinBox(analyzerPage);
    detectorFramesSpin->setRange(1, 256);
    vbwSpin = new QDoubleSpinBox(analyzerPage);
    vbwSpin->setRange(0.0, 10000.0);
    vbwSpin->setDecimals(2);
    vbwSpin->setSpecialValueText(trText(QStringLiteral("off"), QStringLiteral("Off")));
    vbwSpin->setSuffix(QStringLiteral(" Hz"));
    fftOverlapCombo = new QComboBox(analyzerPage);
    for (const int overlap : {0, 25, 50, 75}) {
        fftOverlapCombo->addItem(QStringLiteral("%1 %").arg(overlap), overlap);
    }
    averageFramesSpin = new QSpinBox(analyzerPage);
    averageFramesSpin->setRange(0, 10000);
    averageFramesSpin->setSpecialValueText(trText(QStringLiteral("research_exponential"), QStringLiteral("Exponential")));
    percentile50Checkbox = new QCheckBox(QStringLiteral("P50"), analyzerPage);
    percentile90Checkbox = new QCheckBox(QStringLiteral("P90"), analyzerPage);
    percentile99Checkbox = new QCheckBox(QStringLiteral("P99"), analyzerPage);
    amplitudeUnitCombo = new QComboBox(analyzerPage);
    amplitudeUnitCombo->addItem(QStringLiteral("dBFS"), 0);
    amplitudeUnitCombo->addItem(QStringLiteral("dBm"), 1);
    amplitudeUnitCombo->addItem(QStringLiteral("dBuV / 50 ohm"), 2);
    amplitudeUnitCombo->addItem(QStringLiteral("uV / 50 ohm"), 3);

    analyzerControls->addWidget(new QLabel(trText(QStringLiteral("research_detector"), QStringLiteral("Detector:")), analyzerPage), 0, 0);
    analyzerControls->addWidget(detectorCombo, 0, 1);
    analyzerControls->addWidget(new QLabel(trText(QStringLiteral("research_detector_frames"), QStringLiteral("Detector frames:")), analyzerPage), 0, 2);
    analyzerControls->addWidget(detectorFramesSpin, 0, 3);
    analyzerControls->addWidget(new QLabel(QStringLiteral("VBW:"), analyzerPage), 1, 0);
    analyzerControls->addWidget(vbwSpin, 1, 1);
    analyzerControls->addWidget(new QLabel(trText(QStringLiteral("research_fft_overlap"), QStringLiteral("FFT overlap:")), analyzerPage), 1, 2);
    analyzerControls->addWidget(fftOverlapCombo, 1, 3);
    analyzerControls->addWidget(new QLabel(trText(QStringLiteral("research_average_frames"), QStringLiteral("Average frames:")), analyzerPage), 2, 0);
    analyzerControls->addWidget(averageFramesSpin, 2, 1);
    analyzerControls->addWidget(percentile50Checkbox, 2, 2);
    analyzerControls->addWidget(percentile90Checkbox, 2, 3);
    analyzerControls->addWidget(percentile99Checkbox, 2, 4);
    analyzerControls->addWidget(new QLabel(trText(QStringLiteral("research_amplitude_unit"), QStringLiteral("Amplitude unit:")), analyzerPage), 3, 0);
    analyzerControls->addWidget(amplitudeUnitCombo, 3, 1, 1, 2);
    analyzerControls->setColumnStretch(5, 1);
    analyzerLayout->addLayout(analyzerControls);
    analyzerStatus = new QLabel(analyzerPage);
    analyzerStatus->setWordWrap(true);
    analyzerLayout->addWidget(analyzerStatus);
    analyzerLayout->addStretch(1);
    tabs->addTab(analyzerPage, trText(QStringLiteral("research_analyzer_tab"), QStringLiteral("Spectrum analyzer")));

    densityWidget = new SignalDensityWidget(translator, tabs);
    tabs->addTab(densityWidget,
                 trText(QStringLiteral("research_density_tab"),
                        QStringLiteral("Density / persistence")));
    maskWidget = new SpectrumMaskWidget(translator, this->maskTriggerHandler, tabs);
    tabs->addTab(maskWidget,
                 trText(QStringLiteral("research_masks_tab"),
                        QStringLiteral("Masks / baseline")));
    pulseWidget = new PulseAnalysisWidget(translator, tabs);
    tabs->addTab(pulseWidget,
                 trText(QStringLiteral("research_pulse_tab"),
                        QStringLiteral("Pulses / periodicity")));
    sessionWidget = new MeasurementSessionWidget(
        translator, this->sessionFrequencySetter, tabs);
    tabs->addTab(sessionWidget,
                 trText(QStringLiteral("research_session_tab"),
                        QStringLiteral("Measurement session")));

    const ResearchSpectrumSettings initialSettings = spectrumSettingsProvider
                                                          ? spectrumSettingsProvider()
                                                          : ResearchSpectrumSettings{};
    detectorCombo->setCurrentIndex((std::max)(0, detectorCombo->findData(initialSettings.detectorMode)));
    detectorFramesSpin->setValue(initialSettings.detectorFrames);
    vbwSpin->setValue(initialSettings.vbwHz);
    fftOverlapCombo->setCurrentIndex((std::max)(0, fftOverlapCombo->findData(initialSettings.fftOverlapPercent)));
    averageFramesSpin->setValue(initialSettings.averageFrameCount);
    percentile50Checkbox->setChecked(initialSettings.percentile50);
    percentile90Checkbox->setChecked(initialSettings.percentile90);
    percentile99Checkbox->setChecked(initialSettings.percentile99);
    amplitudeUnitCombo->setCurrentIndex((std::max)(0, amplitudeUnitCombo->findData(initialSettings.amplitudeUnit)));

    connect(interferenceCaptureButton, &QPushButton::clicked, this, [this]() { captureInterferenceReference(); });
    connect(interferenceClearButton, &QPushButton::clicked, this, [this]() { clearInterferenceReference(); });
    connect(statisticsResetButton, &QPushButton::clicked, this, [this]() { resetStatistics(); });
    auto reanalyze = [this]() {
        if (!latestSpectrumLevels.empty()) updateInterference(latestSpectrumFrequencies, latestSpectrumLevels);
    };
    connect(interferenceProminenceSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [reanalyze](double) { reanalyze(); });
    connect(interferenceMinSpacingSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [reanalyze](double) { reanalyze(); });
    connect(interferenceMaxSpacingSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [reanalyze](double) { reanalyze(); });
    connect(detectorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { applySpectrumSettings(); });
    connect(detectorFramesSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { applySpectrumSettings(); });
    connect(vbwSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { applySpectrumSettings(); });
    connect(fftOverlapCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { applySpectrumSettings(); });
    connect(averageFramesSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { applySpectrumSettings(); });
    connect(percentile50Checkbox, &QCheckBox::toggled, this, [this](bool) { applySpectrumSettings(); });
    connect(percentile90Checkbox, &QCheckBox::toggled, this, [this](bool) { applySpectrumSettings(); });
    connect(percentile99Checkbox, &QCheckBox::toggled, this, [this](bool) { applySpectrumSettings(); });
    connect(amplitudeUnitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { applySpectrumSettings(); });
    connect(syncLiveButton, &QPushButton::toggled, this, [this](bool enabled) {
        syncLiveHasEstimate = false;
        syncLiveCarrierHz = 0.0;
        syncLivePhaseRadians = 0.0;
        if (!enabled && this->liveSyncApplier) {
            this->liveSyncApplier(false, 0.0, 0.0, 0.5, 0.0);
        }
        updateLiveSyncButton();
    });
    connect(this, &QDialog::finished, this, [this](int) {
        if (syncLiveButton && syncLiveButton->isChecked()) {
            syncLiveButton->setChecked(false);
        }
    });
    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index != static_cast<int>(SynchronizationTab) &&
            syncLiveButton && syncLiveButton->isChecked()) {
            syncLiveButton->setChecked(false);
        }
    });

    iqTimer = new QTimer(this);
    iqTimer->setInterval(120);
    connect(iqTimer, &QTimer::timeout, this, [this]() { refreshIqSnapshot(); });
    iqTimer->start();
    rebuildTexts();
    updateAnalyzerStatus();
}

QString ResearchAnalysisDialog::trText(const QString &key, const QString &fallback) const {
    return translator ? translator(key, fallback) : fallback;
}

void ResearchAnalysisDialog::rebuildTexts() {
    interferenceCaptureButton->setText(trText(QStringLiteral("research_capture_reference"), QStringLiteral("Capture reference")));
    interferenceClearButton->setText(trText(QStringLiteral("research_clear_reference"), QStringLiteral("Clear reference")));
    statisticsResetButton->setText(trText(QStringLiteral("reset"), QStringLiteral("Reset")));
    interferenceTable->setHorizontalHeaderLabels({
        trText(QStringLiteral("frequency"), QStringLiteral("Frequency")),
        trText(QStringLiteral("level"), QStringLiteral("Level")),
        trText(QStringLiteral("research_prominence"), QStringLiteral("Prominence")),
        trText(QStringLiteral("research_reference_delta"), QStringLiteral("Delta ref")),
        trText(QStringLiteral("research_harmonic"), QStringLiteral("Harmonic")),
        trText(QStringLiteral("research_family"), QStringLiteral("Family"))
    });
    interferenceStatus->setText(trText(QStringLiteral("research_waiting_spectrum"), QStringLiteral("Waiting for spectrum data")));
    statisticsStatus->setText(trText(QStringLiteral("research_waiting_statistics"), QStringLiteral("Waiting for signal statistics")));
    iqStatus->setText(trText(QStringLiteral("research_waiting_iq"), QStringLiteral("Waiting for IQ data")));
    syncStatus->setText(trText(QStringLiteral("research_sync_waiting"),
                               QStringLiteral("Waiting for channel IQ data")));
    updateLiveSyncButton();
    dualStatus->setText(trText(QStringLiteral("research_dual_hint"), QStringLiteral("Select HF combined or HF interference lab to compare HF1 and HF2.")));
    statisticsPlot->setLabels(
        trText(QStringLiteral("research_level_history"), QStringLiteral("Level history")),
        trText(QStringLiteral("research_histogram_ccdf"), QStringLiteral("Histogram / CCDF")),
        trText(QStringLiteral("research_frequency_time"), QStringLiteral("Frequency x time")));
    dualPlot->setLabels(
        trText(QStringLiteral("research_dual_spectra"), QStringLiteral("HF1 / HF2 / difference")),
        trText(QStringLiteral("research_cross_correlation"), QStringLiteral("Cross-correlation")),
        trText(QStringLiteral("research_coherence"), QStringLiteral("Coherence")));
    syncPlot->setLabels(
        trText(QStringLiteral("research_sync_before"), QStringLiteral("Before synchronization")),
        trText(QStringLiteral("research_sync_after"), QStringLiteral("After synchronization")),
        trText(QStringLiteral("research_sync_phase_metric"), QStringLiteral("Symbol-phase quality")));
}

void ResearchAnalysisDialog::updateLiveSyncButton() {
    if (!syncLiveButton) return;
    syncLiveButton->setText(
        syncLiveButton->isChecked()
            ? trText(QStringLiteral("research_sync_live_on"),
                     QStringLiteral("Live assist: ON"))
            : trText(QStringLiteral("research_sync_live_off"),
                     QStringLiteral("Live assist: OFF")));
    syncLiveButton->setToolTip(
        trText(QStringLiteral("research_sync_live_tooltip"),
               QStringLiteral("Apply smoothed carrier correction to the live demodulator. PSK/FT8 also receive phase correction; symbol timing remains a diagnostic metric until a decoder-specific clock-recovery loop is enabled.")));
}

void ResearchAnalysisDialog::applySpectrumSettings() {
    if (!detectorCombo || !spectrumSettingsApplier) return;
    ResearchSpectrumSettings settings;
    settings.detectorMode = detectorCombo->currentData().toInt();
    settings.detectorFrames = detectorFramesSpin->value();
    settings.vbwHz = vbwSpin->value();
    settings.fftOverlapPercent = fftOverlapCombo->currentData().toInt();
    settings.averageFrameCount = averageFramesSpin->value();
    settings.percentile50 = percentile50Checkbox->isChecked();
    settings.percentile90 = percentile90Checkbox->isChecked();
    settings.percentile99 = percentile99Checkbox->isChecked();
    settings.amplitudeUnit = amplitudeUnitCombo->currentData().toInt();
    spectrumSettingsApplier(settings);
    updateAnalyzerStatus();
}

void ResearchAnalysisDialog::updateAnalyzerStatus() {
    if (!analyzerStatus) return;
    const ResearchRadioContext context = contextProvider ? contextProvider() : ResearchRadioContext{};
    const double binWidthHz = context.fftLength > 0 ? context.sampleRateHz / context.fftLength : 0.0;
    const double rbwHz = binWidthHz * fftWindowEnbwBins(context.fftWindowType);
    analyzerStatus->setText(trText(
        QStringLiteral("research_analyzer_status"),
        QStringLiteral("FFT %1 | Window %2 | Bin %3 Hz | RBW %4 Hz. VBW is applied in linear power; overlap controls how many new IQ samples are required for the next frame."))
        .arg(context.fftLength)
        .arg(QString::fromLatin1(fftWindowTypeName(context.fftWindowType)))
        .arg(binWidthHz, 0, 'f', 3)
        .arg(rbwHz, 0, 'f', 3));
}

void ResearchAnalysisDialog::selectTab(Tab tab) {
    if (tabs) tabs->setCurrentIndex(static_cast<int>(tab));
    show();
    raise();
    activateWindow();
}

void ResearchAnalysisDialog::setIqView(int viewMode) {
    if (!iqViewCombo) return;
    const int index = iqViewCombo->findData((std::clamp)(viewMode, 0, 4));
    if (index >= 0) {
        iqViewCombo->setCurrentIndex(index);
    }
    selectTab(IqTab);
}

void ResearchAnalysisDialog::updateIqView() {
    if (!iqPlotStack) return;
    const bool threeD = iqViewCombo->currentData().toInt() == 4;
    iqSurfaceCheckbox->setEnabled(threeD);
    iqConstellation3D->setSurfaceEnabled(iqSurfaceCheckbox->isChecked());
    iqPlotStack->setCurrentIndex(threeD ? 1 : 0);
    if (threeD) iqConstellation3D->setData(buildResearchIqConstellation(
        iqConstellationFirst, iqConstellationSecond, iqConstellationRate,
        iqSymbolRateSpin->value(), iqPhaseSpin->value()/100.0));
}

void ResearchAnalysisDialog::hideEvent(QHideEvent *event) {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("research/iq/view"), iqViewCombo->currentData());
    settings.setValue(QStringLiteral("research/iq/constellationSurface"), iqSurfaceCheckbox->isChecked());
    settings.setValue(QStringLiteral("research/iq/constellation3DCamera"),
                      QJsonDocument(iqConstellation3D->cameraState()).toJson(QJsonDocument::Compact));
    QDialog::hideEvent(event);
}

bool ResearchAnalysisDialog::hasActiveMeasurementSession() const {
    return sessionWidget && sessionWidget->isRunning();
}

void ResearchAnalysisDialog::appendSpectrumFrame(const std::vector<float> &frequencies,
                                                 const std::vector<float> &levels,
                                                 const SpectrumScienceMetrics &metrics) {
    if ((!isVisible() && !hasActiveMeasurementSession()) ||
        frequencies.empty() || levels.empty()) return;
    if (isVisible()) updateAnalyzerStatus();
    const int tab = tabs ? tabs->currentIndex() : -1;
    if (isVisible() && tab == InterferenceTab) updateInterference(frequencies, levels);
    if (isVisible() && tab == StatisticsTab) updateStatistics(levels, metrics);
    const int amplitudeUnit = amplitudeUnitCombo
                                  ? amplitudeUnitCombo->currentData().toInt()
                                  : 0;
    if (isVisible() && tab == DensityTab && densityWidget) {
        const ResearchRadioContext context =
            contextProvider ? contextProvider() : ResearchRadioContext{};
        densityWidget->appendSpectrumFrame(
            frequencies, levels, amplitudeUnit, context.listeningFrequencyHz);
    }
    if (isVisible() && tab == MasksTab && maskWidget) {
        maskWidget->appendSpectrumFrame(frequencies, levels, amplitudeUnit);
    }
    ResearchRadioContext radioContext =
        contextProvider ? contextProvider() : ResearchRadioContext{};
    ScientificSessionContext scientificContext;
    scientificContext.sampleRateHz = radioContext.sampleRateHz;
    scientificContext.centerFrequencyHz = radioContext.centerFrequencyHz;
    scientificContext.listeningFrequencyHz = radioContext.listeningFrequencyHz;
    scientificContext.bandwidthHz = radioContext.bandwidthHz;
    scientificContext.inputMode = radioContext.inputMode;
    scientificContext.modulationType = radioContext.modulationType;
    scientificContext.fftLength = radioContext.fftLength;
    scientificContext.fftWindowType = radioContext.fftWindowType;
    if (isVisible() && tab == PulseTab && pulseWidget) {
        pulseWidget->appendSpectrumFrame(
            frequencies, levels, scientificContext, metrics, amplitudeUnit);
    }
    if (sessionWidget &&
        (tab == SessionTab || sessionWidget->isRunning())) {
        sessionWidget->appendSpectrumFrame(
            frequencies, levels, scientificContext, metrics, amplitudeUnit);
    }
}

void ResearchAnalysisDialog::captureInterferenceReference() {
    interferenceReferenceFrequencies = latestSpectrumFrequencies;
    interferenceReferenceLevels = latestSpectrumLevels;
    if (!latestSpectrumLevels.empty()) updateInterference(latestSpectrumFrequencies, latestSpectrumLevels);
}

void ResearchAnalysisDialog::clearInterferenceReference() {
    interferenceReferenceFrequencies.clear();
    interferenceReferenceLevels.clear();
    if (!latestSpectrumLevels.empty()) updateInterference(latestSpectrumFrequencies, latestSpectrumLevels);
}

void ResearchAnalysisDialog::updateInterference(const std::vector<float> &frequencies,
                                                const std::vector<float> &levels) {
    const std::size_t count = std::min(frequencies.size(), levels.size());
    if (count < 5) return;
    latestSpectrumFrequencies.assign(frequencies.begin(), frequencies.begin() + count);
    latestSpectrumLevels.assign(levels.begin(), levels.begin() + count);
    const float noise = percentile(latestSpectrumLevels, 0.4);
    const float prominenceLimit = static_cast<float>(interferenceProminenceSpin->value());
    std::vector<Peak> peaks;
    peaks.reserve(128);
    for (std::size_t i = 2; i + 2 < count; ++i) {
        const float level = levels[i];
        if (!std::isfinite(level) || level < noise + prominenceLimit ||
            level < levels[i - 1] || level <= levels[i + 1]) continue;
        const float localBase = (levels[i - 2] + levels[i - 1] + levels[i + 1] + levels[i + 2]) * 0.25f;
        Peak peak;
        peak.bin = static_cast<int>(i);
        peak.frequencyHz = frequencies[i];
        peak.levelDb = level;
        peak.prominenceDb = level - std::max(noise, localBase);
        if (interferenceReferenceLevels.size() == count &&
            interferenceReferenceFrequencies.size() == count) {
            peak.referenceDeltaDb = level - interferenceReferenceLevels[i];
        }
        peaks.push_back(peak);
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak &a, const Peak &b) {
        return a.prominenceDb > b.prominenceDb;
    });
    if (peaks.size() > 96) peaks.resize(96);
    std::sort(peaks.begin(), peaks.end(), [](const Peak &a, const Peak &b) {
        return a.frequencyHz < b.frequencyHz;
    });

    const double minSpacingHz = interferenceMinSpacingSpin->value() * 1000.0;
    const double maxSpacingHz = interferenceMaxSpacingSpin->value() * 1000.0;
    const double binHz = count > 1 ? std::abs(frequencies.back() - frequencies.front()) / (count - 1) : 1.0;
    const double spacingResolution = std::max(10.0, binHz * 2.0);
    struct SpacingVote { double sum = 0.0; int count = 0; };
    struct CombFamily {
        double stepHz = 0.0;
        double anchorHz = 0.0;
        std::vector<int> members;
    };
    auto detectFamily = [&](const std::vector<int> &available) {
        std::map<qint64, SpacingVote> votes;
        for (std::size_t i = 0; i < available.size(); ++i) {
            for (std::size_t j = i + 1; j < available.size(); ++j) {
                const double difference = peaks[static_cast<std::size_t>(available[j])].frequencyHz -
                                          peaks[static_cast<std::size_t>(available[i])].frequencyHz;
                if (difference < minSpacingHz) continue;
                if (difference > maxSpacingHz) break;
                const qint64 key = static_cast<qint64>(std::llround(difference / spacingResolution));
                votes[key].sum += difference;
                ++votes[key].count;
            }
        }
        CombFamily best;
        for (const auto &entry : votes) {
            const double candidate = entry.second.sum / std::max(1, entry.second.count);
            if (!(candidate > 0.0)) continue;
            const double tolerance = std::max(binHz * 3.0, candidate * 0.015);
            for (int anchorIndex : available) {
                CombFamily trial;
                trial.stepHz = candidate;
                trial.anchorHz = peaks[static_cast<std::size_t>(anchorIndex)].frequencyHz;
                for (int peakIndex : available) {
                    const double frequency = peaks[static_cast<std::size_t>(peakIndex)].frequencyHz;
                    const double harmonic = std::round((frequency - trial.anchorHz) / candidate);
                    const double error = std::abs(frequency - (trial.anchorHz + harmonic * candidate));
                    if (error <= tolerance) trial.members.push_back(peakIndex);
                }
                if (trial.members.size() > best.members.size() ||
                    (trial.members.size() == best.members.size() &&
                     trial.members.size() >= 4 &&
                     (best.stepHz <= 0.0 || trial.stepHz < best.stepHz))) {
                    best = std::move(trial);
                }
            }
        }
        if (best.members.size() < 4) return CombFamily{};
        return best;
    };

    std::vector<int> available(peaks.size());
    std::iota(available.begin(), available.end(), 0);
    std::vector<CombFamily> families;
    for (int familyIndex = 1; familyIndex <= 3 && available.size() >= 4; ++familyIndex) {
        CombFamily family = detectFamily(available);
        if (family.members.empty()) break;
        for (int peakIndex : family.members) {
            Peak &peak = peaks[static_cast<std::size_t>(peakIndex)];
            peak.family = familyIndex;
            peak.harmonic = static_cast<int>(std::llround(
                (peak.frequencyHz - family.anchorHz) / family.stepHz));
        }
        std::vector<int> remaining;
        remaining.reserve(available.size() - family.members.size());
        for (int peakIndex : available) {
            if (std::find(family.members.begin(), family.members.end(), peakIndex) == family.members.end()) {
                remaining.push_back(peakIndex);
            }
        }
        available = std::move(remaining);
        families.push_back(std::move(family));
    }
    const double combStepHz = families.empty() ? 0.0 : families.front().stepHz;
    const int bestMatches = families.empty() ? 0 : static_cast<int>(families.front().members.size());

    interferenceTable->setSortingEnabled(false);
    interferenceTable->setRowCount(static_cast<int>(peaks.size()));
    std::vector<int> peakBins;
    peakBins.reserve(peaks.size());
    for (int row = 0; row < static_cast<int>(peaks.size()); ++row) {
        const Peak &peak = peaks[static_cast<std::size_t>(row)];
        peakBins.push_back(peak.bin);
        interferenceTable->setItem(row, 0, new QTableWidgetItem(frequencyText(peak.frequencyHz)));
        interferenceTable->setItem(row, 1, new QTableWidgetItem(QString::number(peak.levelDb, 'f', 2)));
        interferenceTable->setItem(row, 2, new QTableWidgetItem(QString::number(peak.prominenceDb, 'f', 2)));
        interferenceTable->setItem(row, 3, new QTableWidgetItem(
            interferenceReferenceLevels.size() == count ? QString::number(peak.referenceDeltaDb, 'f', 2) : QStringLiteral("--")));
        interferenceTable->setItem(row, 4, new QTableWidgetItem(peak.family > 0 ? QString::number(peak.harmonic) : QStringLiteral("--")));
        interferenceTable->setItem(row, 5, new QTableWidgetItem(peak.family > 0 ? QString::number(peak.family) : QStringLiteral("--")));
    }
    const double driftHz = previousCombStepHz > 0.0 && combStepHz > 0.0
                               ? combStepHz - previousCombStepHz
                               : 0.0;
    double primaryMeanLevelDb = std::numeric_limits<double>::quiet_NaN();
    if (!families.empty() && !families.front().members.empty()) {
        double levelSum = 0.0;
        for (int peakIndex : families.front().members) {
            levelSum += peaks[static_cast<std::size_t>(peakIndex)].levelDb;
        }
        primaryMeanLevelDb = levelSum / families.front().members.size();
    }
    const double amplitudeDriftDb = std::isfinite(previousCombMeanLevelDb) &&
                                    std::isfinite(primaryMeanLevelDb)
        ? primaryMeanLevelDb - previousCombMeanLevelDb
        : 0.0;
    if (combStepHz > 0.0) previousCombStepHz = combStepHz;
    if (std::isfinite(primaryMeanLevelDb)) previousCombMeanLevelDb = primaryMeanLevelDb;
    QStringList familyDescriptions;
    for (std::size_t familyIndex = 0; familyIndex < families.size(); ++familyIndex) {
        familyDescriptions.append(QStringLiteral("#%1 %2 (%3)")
                                      .arg(familyIndex + 1)
                                      .arg(frequencyText(families[familyIndex].stepHz))
                                      .arg(families[familyIndex].members.size()));
    }
    QString sourceClass = QStringLiteral("--");
    if (combStepHz > 0.0) {
        if (combStepHz < 1000.0) {
            sourceClass = trText(QStringLiteral("research_source_slow_pwm"),
                                 QStringLiteral("slow PWM / control loop"));
        } else if (combStepHz < 30000.0) {
            sourceClass = trText(QStringLiteral("research_source_inverter"),
                                 QStringLiteral("power inverter / PWM"));
        } else if (combStepHz < 500000.0) {
            sourceClass = trText(QStringLiteral("research_source_smps"),
                                 QStringLiteral("SMPS / DC-DC converter"));
        } else {
            sourceClass = trText(QStringLiteral("research_source_clock"),
                                 QStringLiteral("high-frequency converter / digital clock"));
        }
    }
    interferenceStatus->setText(
        trText(QStringLiteral("research_interference_status"),
               QStringLiteral("Noise %1 dB | peaks %2 | primary step %3 | matches %4 | step drift %5 Hz | amplitude drift %6 dB | reference %7 | families %8 | likely class %9"))
            .arg(noise, 0, 'f', 1)
            .arg(peaks.size())
            .arg(combStepHz > 0.0 ? frequencyText(combStepHz) : QStringLiteral("--"))
            .arg(bestMatches)
            .arg(driftHz, 0, 'f', 1)
            .arg(amplitudeDriftDb, 0, 'f', 2)
            .arg(interferenceReferenceLevels.empty() ? trText(QStringLiteral("off"), QStringLiteral("Off"))
                                                     : trText(QStringLiteral("on"), QStringLiteral("On")))
            .arg(familyDescriptions.isEmpty() ? QStringLiteral("--") : familyDescriptions.join(QStringLiteral(", ")))
            .arg(sourceClass));
    interferencePlot->setInterference(latestSpectrumLevels, interferenceReferenceLevels, peakBins);
}

void ResearchAnalysisDialog::resetStatistics() {
    statisticsHistorySeconds.clear();
    statisticsHistoryDb.clear();
    statisticsActivityHeatmap.clear();
    statisticsHeatmapColumns = 0;
    statisticsHeatmapRows = 0;
    statisticsStartMs = 0;
    statisticsPlot->setStatistics({}, {}, {}, {}, 0, 0);
}

void ResearchAnalysisDialog::updateStatistics(const std::vector<float> &levels,
                                              const SpectrumScienceMetrics &metrics) {
    std::vector<float> finite;
    finite.reserve(levels.size());
    for (float level : levels) if (std::isfinite(level)) finite.push_back(level);
    if (finite.empty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (statisticsStartMs == 0) statisticsStartMs = now;
    const float tracked = metrics.valid ? metrics.channelPowerDb : *std::max_element(finite.begin(), finite.end());
    statisticsHistorySeconds.push_back((now - statisticsStartMs) / 1000.0);
    statisticsHistoryDb.push_back(tracked);
    const int historyLimit = statisticsHistorySpin->value();
    if (static_cast<int>(statisticsHistoryDb.size()) > historyLimit) {
        const int remove = static_cast<int>(statisticsHistoryDb.size()) - historyLimit;
        statisticsHistoryDb.erase(statisticsHistoryDb.begin(), statisticsHistoryDb.begin() + remove);
        statisticsHistorySeconds.erase(statisticsHistorySeconds.begin(), statisticsHistorySeconds.begin() + remove);
    }

    constexpr int heatmapColumns = 128;
    constexpr int maximumHeatmapRows = 240;
    const std::vector<float> heatmapRow = reduced(finite, heatmapColumns);
    if (static_cast<int>(heatmapRow.size()) == heatmapColumns) {
        statisticsHeatmapColumns = heatmapColumns;
        statisticsActivityHeatmap.insert(statisticsActivityHeatmap.end(), heatmapRow.begin(), heatmapRow.end());
        statisticsHeatmapRows = static_cast<int>(statisticsActivityHeatmap.size()) / heatmapColumns;
        if (statisticsHeatmapRows > maximumHeatmapRows) {
            const int rowsToRemove = statisticsHeatmapRows - maximumHeatmapRows;
            statisticsActivityHeatmap.erase(
                statisticsActivityHeatmap.begin(),
                statisticsActivityHeatmap.begin() + rowsToRemove * heatmapColumns);
            statisticsHeatmapRows = maximumHeatmapRows;
        }
    }

    const auto minMax = std::minmax_element(finite.begin(), finite.end());
    const float minDb = *minMax.first;
    const float maxDb = *minMax.second;
    constexpr int bins = 64;
    std::vector<float> histogram(bins, 0.0f);
    std::vector<float> ccdf(bins, 0.0f);
    const double span = std::max(1.0, static_cast<double>(maxDb - minDb));
    double powerSum = 0.0;
    const double threshold = statisticsThresholdSpin->value();
    for (float level : finite) {
        const int bin = std::clamp(static_cast<int>((level - minDb) / span * (bins - 1)), 0, bins - 1);
        histogram[static_cast<std::size_t>(bin)] += 1.0f;
        const double power = dbToPower(level);
        powerSum += power;
    }
    for (int bin = 0; bin < bins; ++bin) {
        double above = 0.0;
        for (int j = bin; j < bins; ++j) above += histogram[static_cast<std::size_t>(j)];
        ccdf[static_cast<std::size_t>(bin)] = static_cast<float>(above * 100.0 / finite.size());
    }
    const double meanPower = powerSum / finite.size();
    const double paprDb = maxDb - safeDb(meanPower);
    const double varianceDb = std::accumulate(finite.begin(), finite.end(), 0.0,
        [mean = std::accumulate(finite.begin(), finite.end(), 0.0) / finite.size()](double sum, float value) {
            const double delta = value - mean;
            return sum + delta * delta;
        }) / finite.size();
    const int temporalExceedCount = static_cast<int>(std::count_if(
        statisticsHistoryDb.begin(), statisticsHistoryDb.end(),
        [threshold](float value) { return value >= threshold; }));
    const double temporalExceedPercent = statisticsHistoryDb.empty()
        ? 0.0
        : 100.0 * temporalExceedCount / statisticsHistoryDb.size();
    statisticsStatus->setText(
        trText(QStringLiteral("research_statistics_status"),
               QStringLiteral("Median %1 dB | RMS power %2 dB | PAPR %3 dB | deviation %4 dB | above threshold %5% | history %6"))
            .arg(percentile(finite, 0.5), 0, 'f', 2)
            .arg(safeDb(meanPower), 0, 'f', 2)
            .arg(paprDb, 0, 'f', 2)
            .arg(std::sqrt(std::max(0.0, varianceDb)), 0, 'f', 2)
            .arg(temporalExceedPercent, 0, 'f', 2)
            .arg(statisticsHistoryDb.size()));
    statisticsPlot->setStatistics(statisticsHistoryDb,
                                  histogram,
                                  ccdf,
                                  statisticsActivityHeatmap,
                                  statisticsHeatmapColumns,
                                  statisticsHeatmapRows);
}

void ResearchAnalysisDialog::refreshIqSnapshot() {
    if (!isVisible() || isMinimized() || !tabs) return;
    const int activeTab = tabs->currentIndex();
    if (activeTab != IqTab && activeTab != SynchronizationTab &&
        activeTab != DualInputTab) return;
    if ((activeTab == IqTab && iqFreezeCheckbox->isChecked()) ||
        (activeTab == SynchronizationTab && syncFreezeCheckbox->isChecked()) ||
        (activeTab == DualInputTab && dualFreezeCheckbox->isChecked())) return;

    const int requestedSamples = activeTab == SynchronizationTab
                                     ? syncSampleCountSpin->value()
                                     : iqSampleCountSpin->value();
    std::vector<float> raw;
    if (!IqBuffer::snapshotRecent(raw, static_cast<std::size_t>(requestedSamples) * 2U) || raw.size() < 8) {
        return;
    }
    const ResearchRadioContext context = contextProvider ? contextProvider() : ResearchRadioContext{};
    const IqBuffer::Stats bufferStats = IqBuffer::stats();
    const qint64 statsNowMs = QDateTime::currentMSecsSinceEpoch();
    if (lastIqStatsTimeMs > 0 && bufferStats.epoch == lastIqEpoch &&
        bufferStats.totalFloatCount >= lastIqTotalFloatCount && statsNowMs > lastIqStatsTimeMs) {
        const double instantaneousRate =
            static_cast<double>(bufferStats.totalFloatCount - lastIqTotalFloatCount) * 500.0 /
            static_cast<double>(statsNowMs - lastIqStatsTimeMs);
        measuredIqRateHz = measuredIqRateHz > 0.0
                               ? measuredIqRateHz * 0.75 + instantaneousRate * 0.25
                               : instantaneousRate;
    } else if (bufferStats.epoch != lastIqEpoch) {
        measuredIqRateHz = 0.0;
    }
    lastIqEpoch = bufferStats.epoch;
    lastIqTotalFloatCount = bufferStats.totalFloatCount;
    lastIqStatsTimeMs = statsNowMs;
    const std::vector<float> *analysisRaw = &raw;
    double analysisSampleRate = context.sampleRateHz;
    bool channelized = false;
    QString iqSourceName =
        trText(QStringLiteral("research_iq_source_raw"), QStringLiteral("Raw IQ"));
    const bool requestChannel = activeTab == SynchronizationTab ||
                                (iqSourceCombo && iqSourceCombo->currentData().toInt() == 1);
    if (requestChannel &&
        context.sampleRateHz > 0.0) {
        RadioSettings channelSettings;
        channelSettings.sampleRate = context.sampleRateHz;
        channelSettings.centerFrequency = context.centerFrequencyHz;
        channelSettings.listeningFrequency = context.listeningFrequencyHz;
        channelSettings.inputMode = context.inputMode;
        channelSettings.modulationType = context.modulationType;
        channelSettings.bandwidth = context.bandwidthHz;
        const std::uint64_t totalComplexSamples = bufferStats.totalFloatCount / 2U;
        const std::uint64_t snapshotComplexSamples = raw.size() / 2U;
        const std::uint64_t firstComplexSample =
            totalComplexSamples > snapshotComplexSamples
                ? totalComplexSamples - snapshotComplexSamples
                : 0U;
        const double shiftHz =
            context.listeningFrequencyHz - context.centerFrequencyHz;
        const double initialPhase =
            -kTwoPi * shiftHz / context.sampleRateHz *
            static_cast<double>(firstComplexSample);
        iqAnalysisChannelizer.reset(initialPhase);
        const IqChannelizer::Result channelResult =
            iqAnalysisChannelizer.processFloatIq(raw.data(),
                                                 raw.size(),
                                                 channelSettings,
                                                 iqChannelizedSnapshot,
                                                 false);
        if (channelResult.valid && iqChannelizedSnapshot.size() >= 8U) {
            analysisRaw = &iqChannelizedSnapshot;
            analysisSampleRate = channelResult.outputRate;
            channelized = true;
            iqSourceName =
                trText(QStringLiteral("research_iq_source_channel"),
                       QStringLiteral("Channel IQ"));
        }
    }
    const std::size_t sampleCount = analysisRaw->size() / 2U;
    if (sampleCount < 4U) {
        return;
    }
    std::vector<float> first(sampleCount);
    std::vector<float> second(sampleCount);
    double meanFirst = 0.0;
    double meanSecond = 0.0;
    std::size_t nonFiniteCount = 0;
    std::size_t clippedCount = 0;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const float rawFirst = (*analysisRaw)[2U * i];
        const float rawSecond = (*analysisRaw)[2U * i + 1U];
        if (!std::isfinite(rawFirst) || !std::isfinite(rawSecond)) ++nonFiniteCount;
        if (std::isfinite(rawFirst) && std::isfinite(rawSecond) &&
            (std::abs(rawFirst) >= 0.999f || std::abs(rawSecond) >= 0.999f)) ++clippedCount;
        first[i] = std::isfinite(rawFirst) ? rawFirst : 0.0f;
        second[i] = std::isfinite(rawSecond) ? rawSecond : 0.0f;
        meanFirst += first[i];
        meanSecond += second[i];
    }
    meanFirst /= sampleCount;
    meanSecond /= sampleCount;
    double powerFirst = 0.0;
    double powerSecond = 0.0;
    double covariance = 0.0;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const double a = first[i] - meanFirst;
        const double b = second[i] - meanSecond;
        powerFirst += a * a;
        powerSecond += b * b;
        covariance += a * b;
    }
    powerFirst /= sampleCount;
    powerSecond /= sampleCount;
    covariance /= sampleCount;
    const double rmsFirst = std::sqrt(std::max(0.0, powerFirst));
    const double rmsSecond = std::sqrt(std::max(0.0, powerSecond));
    const double correlation = covariance / std::sqrt(std::max(1.0e-20, powerFirst * powerSecond));

    if (activeTab == SynchronizationTab) {
        if (!channelized || isDirectInputMode(context.inputMode)) {
            syncStatus->setText(
                trText(QStringLiteral("research_sync_channel_unavailable"),
                       QStringLiteral("Digital synchronization requires quadrature Channel IQ in an RF input mode.")));
            syncPlot->setSynchronization({}, {}, {}, {}, {}, {}, -1);
            return;
        }
        const double symbolRate = syncSymbolRateSpin ? syncSymbolRateSpin->value() : 4800.0;
        const double samplesPerSymbol = symbolRate > 0.0 ? analysisSampleRate / symbolRate : 0.0;
        if (samplesPerSymbol < 2.0 || sampleCount < 16U) {
            syncStatus->setText(
                trText(QStringLiteral("research_sync_rate_invalid"),
                       QStringLiteral("Need at least 2 channel samples per symbol. Lower the symbol rate or widen the channel.")));
            syncPlot->setSynchronization({}, {}, {}, {}, {}, {}, -1);
            return;
        }

        std::vector<std::complex<double>> centered(sampleCount);
        std::vector<float> centeredInterleaved(sampleCount * 2U);
        double totalPower = 0.0;
        for (std::size_t index = 0; index < sampleCount; ++index) {
            centered[index] = {first[index] - meanFirst, second[index] - meanSecond};
            centeredInterleaved[2U * index] = static_cast<float>(centered[index].real());
            centeredInterleaved[2U * index + 1U] = static_cast<float>(centered[index].imag());
            totalPower += std::norm(centered[index]);
        }
        totalPower /= static_cast<double>(sampleCount);

        struct CarrierEstimate {
            int order = 1;
            double frequencyHz = 0.0;
            double coherence = 0.0;
        };
        auto estimateCarrier = [&](int order,
                                   const std::vector<std::complex<double>> &samples) {
            CarrierEstimate estimate;
            estimate.order = order;
            std::complex<double> accumulator(0.0, 0.0);
            int count = 0;
            for (std::size_t index = 1; index < samples.size(); ++index) {
                std::complex<double> step = samples[index] * std::conj(samples[index - 1U]);
                const double magnitude = std::abs(step);
                if (!(magnitude > 1.0e-15) || !std::isfinite(magnitude)) continue;
                step /= magnitude;
                accumulator += std::pow(step, order);
                ++count;
            }
            if (count > 0) {
                estimate.frequencyHz = std::arg(accumulator) * analysisSampleRate /
                                       (kTwoPi * static_cast<double>(order));
                estimate.coherence = std::abs(accumulator) / static_cast<double>(count);
            }
            return estimate;
        };

        const int requestedOrder = syncModulationCombo
                                       ? syncModulationCombo->currentData().toInt()
                                       : 0;
        CarrierEstimate carrier;
        if (requestedOrder > 0) {
            carrier = estimateCarrier(requestedOrder, centered);
        } else if (context.modulationType == MOD_DMR ||
                   context.modulationType == MOD_FSK ||
                   context.modulationType == MOD_RTTY) {
            carrier = estimateCarrier(1, centered);
        } else {
            double bestScore = -1.0;
            for (const int order : {2, 4, 8}) {
                const CarrierEstimate candidate = estimateCarrier(order, centered);
                const double complexityPenalty = 0.012 * std::log2(static_cast<double>(order));
                const double score = candidate.coherence - complexityPenalty;
                if (score > bestScore) {
                    bestScore = score;
                    carrier = candidate;
                }
            }
        }

        const bool correctCarrier = syncCarrierCorrectionCheckbox &&
                                    syncCarrierCorrectionCheckbox->isChecked();
        std::vector<std::complex<double>> carrierCorrected(sampleCount);
        const double carrierStep = correctCarrier
                                       ? -kTwoPi * carrier.frequencyHz / analysisSampleRate
                                       : 0.0;
        std::complex<double> oscillator(1.0, 0.0);
        const std::complex<double> oscillatorStep(std::cos(carrierStep), std::sin(carrierStep));
        for (std::size_t index = 0; index < sampleCount; ++index) {
            carrierCorrected[index] = centered[index] * oscillator;
            oscillator *= oscillatorStep;
            if ((index & 4095U) == 4095U) {
                const double magnitude = std::abs(oscillator);
                if (magnitude > 0.0) oscillator /= magnitude;
            }
        }
        std::vector<float> carrierInterleaved(sampleCount * 2U);
        for (std::size_t index = 0; index < sampleCount; ++index) {
            carrierInterleaved[2U * index] = static_cast<float>(carrierCorrected[index].real());
            carrierInterleaved[2U * index + 1U] = static_cast<float>(carrierCorrected[index].imag());
        }

        const int phaseBins = syncPhaseBinsSpin ? syncPhaseBinsSpin->value() : 64;
        std::vector<float> phaseMetric(static_cast<std::size_t>(phaseBins), 0.0f);
        int selectedPhaseBin = 0;
        double selectedMetric = -1.0;
        for (int phaseBin = 0; phaseBin < phaseBins; ++phaseBin) {
            const double phaseSamples = samplesPerSymbol * phaseBin / phaseBins;
            std::complex<double> angularSum(0.0, 0.0);
            double magnitudeSum = 0.0;
            double powerSum = 0.0;
            double localDerivativePower = 0.0;
            int count = 0;
            for (double position = phaseSamples + samplesPerSymbol;
                 position + samplesPerSymbol < static_cast<double>(sampleCount) && count < 4096;
                 position += samplesPerSymbol) {
                const std::complex<double> value = interpolatedComplex(carrierInterleaved, position);
                const double magnitude = std::abs(value);
                if (!(magnitude > 1.0e-12) || !std::isfinite(magnitude)) continue;
                magnitudeSum += magnitude;
                powerSum += std::norm(value);
                if (carrier.order > 1) {
                    angularSum += std::pow(value / magnitude, carrier.order);
                }
                const std::complex<double> early = interpolatedComplex(
                    carrierInterleaved, position - samplesPerSymbol * 0.25);
                const std::complex<double> late = interpolatedComplex(
                    carrierInterleaved, position + samplesPerSymbol * 0.25);
                localDerivativePower += std::norm(late - early);
                ++count;
            }
            if (count == 0) continue;
            const double radialConsistency = magnitudeSum * magnitudeSum /
                                             (static_cast<double>(count) *
                                              std::max(1.0e-20, powerSum));
            const double flatness = 1.0 / (1.0 + localDerivativePower /
                                                   std::max(1.0e-20, powerSum));
            const double angularConcentration = carrier.order > 1
                                                    ? std::abs(angularSum) / count
                                                    : 0.0;
            const double metric = carrier.order > 1
                                      ? 0.65 * angularConcentration +
                                            0.20 * radialConsistency + 0.15 * flatness
                                      : 0.70 * flatness + 0.30 * radialConsistency;
            phaseMetric[static_cast<std::size_t>(phaseBin)] = static_cast<float>(metric);
            if (metric > selectedMetric) {
                selectedMetric = metric;
                selectedPhaseBin = phaseBin;
            }
        }

        const double selectedPhaseSamples = samplesPerSymbol * selectedPhaseBin / phaseBins;
        std::vector<std::complex<double>> symbolBefore;
        std::vector<std::complex<double>> symbolAfterCarrier;
        for (double position = selectedPhaseSamples + samplesPerSymbol;
             position + samplesPerSymbol < static_cast<double>(sampleCount) &&
             symbolBefore.size() < 3000U;
             position += samplesPerSymbol) {
            symbolBefore.push_back(interpolatedComplex(centeredInterleaved, position));
            symbolAfterCarrier.push_back(interpolatedComplex(carrierInterleaved, position));
        }

        double phaseCorrection = 0.0;
        if (carrier.order > 1 && !symbolAfterCarrier.empty()) {
            std::complex<double> phaseAccumulator(0.0, 0.0);
            for (const std::complex<double> &value : symbolAfterCarrier) {
                const double magnitude = std::abs(value);
                if (magnitude > 1.0e-12) {
                    phaseAccumulator += std::pow(value / magnitude, carrier.order);
                }
            }
            phaseCorrection = std::arg(phaseAccumulator) / carrier.order;
        }
        const bool correctPhase = syncPhaseCorrectionCheckbox &&
                                  syncPhaseCorrectionCheckbox->isChecked() &&
                                  carrier.order > 1;
        const std::complex<double> phaseRotation = std::polar(
            1.0, correctPhase ? -phaseCorrection : 0.0);
        std::vector<std::complex<double>> correctedSamples(sampleCount);
        for (std::size_t index = 0; index < sampleCount; ++index) {
            correctedSamples[index] = carrierCorrected[index] * phaseRotation;
        }
        std::vector<float> correctedInterleaved(sampleCount * 2U);
        for (std::size_t index = 0; index < sampleCount; ++index) {
            correctedInterleaved[2U * index] = static_cast<float>(correctedSamples[index].real());
            correctedInterleaved[2U * index + 1U] = static_cast<float>(correctedSamples[index].imag());
        }

        std::vector<float> beforeI;
        std::vector<float> beforeQ;
        std::vector<float> afterI;
        std::vector<float> afterQ;
        std::vector<float> timingError;
        beforeI.reserve(symbolBefore.size());
        beforeQ.reserve(symbolBefore.size());
        afterI.reserve(symbolBefore.size());
        afterQ.reserve(symbolBefore.size());
        timingError.reserve(symbolBefore.size());
        double timingErrorSum = 0.0;
        double timingErrorPower = 0.0;
        double phaseJitterPower = 0.0;
        double evmErrorPower = 0.0;
        double evmReferencePower = 0.0;
        int metricCount = 0;
        double idealRadius = 0.0;
        for (std::size_t symbol = 0; symbol < symbolBefore.size(); ++symbol) {
            const double position = selectedPhaseSamples + samplesPerSymbol +
                                    symbol * samplesPerSymbol;
            idealRadius += std::abs(interpolatedComplex(correctedInterleaved, position));
        }
        if (!symbolBefore.empty()) {
            idealRadius /= static_cast<double>(symbolBefore.size());
        }
        for (std::size_t symbol = 0; symbol < symbolBefore.size(); ++symbol) {
            const double position = selectedPhaseSamples + samplesPerSymbol +
                                    symbol * samplesPerSymbol;
            const std::complex<double> corrected = interpolatedComplex(correctedInterleaved, position);
            beforeI.push_back(static_cast<float>(symbolBefore[symbol].real()));
            beforeQ.push_back(static_cast<float>(symbolBefore[symbol].imag()));
            afterI.push_back(static_cast<float>(corrected.real()));
            afterQ.push_back(static_cast<float>(corrected.imag()));
            const std::complex<double> early = interpolatedComplex(
                correctedInterleaved, position - samplesPerSymbol * 0.5);
            const std::complex<double> late = interpolatedComplex(
                correctedInterleaved, position + samplesPerSymbol * 0.5);
            const double error = std::real((late - early) * std::conj(corrected)) /
                                 std::max(1.0e-20, totalPower);
            timingError.push_back(static_cast<float>(error));
            timingErrorSum += error;
            timingErrorPower += error * error;
            if (carrier.order > 1) {
                const double angle = std::arg(corrected);
                const double idealStep = kTwoPi / carrier.order;
                const double idealAngle = std::round(angle / idealStep) * idealStep;
                const std::complex<double> ideal = std::polar(idealRadius, idealAngle);
                evmErrorPower += std::norm(corrected - ideal);
                evmReferencePower += std::norm(ideal);
                const double decisionPhase = std::remainder(angle - idealAngle, idealStep);
                phaseJitterPower += decisionPhase * decisionPhase;
            }
            ++metricCount;
        }

        const CarrierEstimate residualCarrier = estimateCarrier(carrier.order, correctedSamples);
        const double timingMean = metricCount > 0 ? timingErrorSum / metricCount : 0.0;
        const double timingRms = metricCount > 0
                                     ? std::sqrt(timingErrorPower / metricCount)
                                     : 0.0;
        const double phaseJitterDegrees = metricCount > 0 && carrier.order > 1
                                              ? std::sqrt(phaseJitterPower / metricCount) *
                                                    180.0 / 3.14159265358979323846
                                              : 0.0;
        const double evmPercent = evmReferencePower > 1.0e-20
                                      ? 100.0 * std::sqrt(evmErrorPower / evmReferencePower)
                                      : 0.0;
        const double lockPercent = 100.0 * std::clamp(
            carrier.coherence * std::max(0.0, selectedMetric), 0.0, 1.0);
        const QString detectedMode = carrier.order == 1
                                         ? QStringLiteral("FSK / FM")
                                         : QStringLiteral("%1PSK").arg(carrier.order);
        QString liveState = trText(QStringLiteral("research_sync_live_inactive"),
                                   QStringLiteral("live correction off"));
        if (syncLiveButton && syncLiveButton->isChecked()) {
            const bool estimateValid = metricCount >= 8 &&
                                       std::isfinite(carrier.frequencyHz) &&
                                       std::isfinite(phaseCorrection) &&
                                       carrier.coherence >= 0.08 &&
                                       std::abs(carrier.frequencyHz) <= analysisSampleRate * 0.20;
            if (estimateValid) {
                constexpr double liveAlpha = 0.18;
                if (!syncLiveHasEstimate) {
                    syncLiveCarrierHz = carrier.frequencyHz;
                    syncLivePhaseRadians = phaseCorrection;
                    syncLiveHasEstimate = true;
                } else {
                    syncLiveCarrierHz += liveAlpha *
                                         (carrier.frequencyHz - syncLiveCarrierHz);
                    const std::complex<double> previousPhase =
                        std::polar(1.0, syncLivePhaseRadians);
                    const std::complex<double> measuredPhase =
                        std::polar(1.0, phaseCorrection);
                    const std::complex<double> smoothedPhase =
                        previousPhase * (1.0 - liveAlpha) + measuredPhase * liveAlpha;
                    if (std::abs(smoothedPhase) > 1.0e-12) {
                        syncLivePhaseRadians = std::arg(smoothedPhase);
                    }
                }
                const double appliedCarrier = correctCarrier ? syncLiveCarrierHz : 0.0;
                const double appliedPhase = correctPhase ? syncLivePhaseRadians : 0.0;
                const double timingPhase = static_cast<double>(selectedPhaseBin) /
                                           static_cast<double>(phaseBins);
                if (liveSyncApplier) {
                    liveSyncApplier(true,
                                    appliedCarrier,
                                    appliedPhase,
                                    timingPhase,
                                    lockPercent / 100.0);
                }
                liveState = trText(QStringLiteral("research_sync_live_applied"),
                                   QStringLiteral("live carrier %1, phase %2 deg; timing monitored"))
                                .arg(frequencyText(appliedCarrier))
                                .arg(appliedPhase * 180.0 /
                                         3.14159265358979323846,
                                     0,
                                     'f',
                                     1);
            } else {
                liveState = trText(QStringLiteral("research_sync_live_holding"),
                                   QStringLiteral("live holding the last stable estimate"));
            }
        }
        const QString statusText =
            trText(QStringLiteral("research_sync_status"),
                   QStringLiteral("%1 | channel rate %2 | %3 samples/symbol | carrier %4 (coherence %5) | residual %6 | phase %7 deg | timing phase %8% | Gardner mean/RMS %9 / %10 | phase jitter %11 deg | EVM %12% | lock %13% | symbols %14"))
                .arg(detectedMode)
                .arg(frequencyText(analysisSampleRate))
                .arg(samplesPerSymbol, 0, 'f', 2)
                .arg(frequencyText(carrier.frequencyHz))
                .arg(carrier.coherence, 0, 'f', 3)
                .arg(frequencyText(residualCarrier.frequencyHz))
                .arg(phaseCorrection * 180.0 / 3.14159265358979323846, 0, 'f', 1)
                .arg(100.0 * selectedPhaseBin / phaseBins, 0, 'f', 1)
                .arg(timingMean, 0, 'f', 4)
                .arg(timingRms, 0, 'f', 4)
                .arg(phaseJitterDegrees, 0, 'f', 2)
                .arg(evmPercent, 0, 'f', 2)
                .arg(lockPercent, 0, 'f', 1)
                .arg(metricCount);
        syncStatus->setText(statusText + QStringLiteral(" | ") + liveState);
        syncPlot->setSynchronization(beforeI,
                                     beforeQ,
                                     afterI,
                                     afterQ,
                                     phaseMetric,
                                     timingError,
                                     selectedPhaseBin);
        return;
    }

    if (activeTab == IqTab) {
        const int viewMode = iqViewCombo ? iqViewCombo->currentData().toInt() : 0;
        std::vector<float> autocorrelation(129, 0.0f);
        for (int lag = 0; viewMode != 4 && lag < static_cast<int>(autocorrelation.size()); ++lag) {
            double sum = 0.0;
            int count = 0;
            for (std::size_t i = 0; i + static_cast<std::size_t>(lag) < sampleCount; i += 4) {
                sum += (first[i] - meanFirst) * (first[i + static_cast<std::size_t>(lag)] - meanFirst);
                ++count;
            }
            autocorrelation[static_cast<std::size_t>(lag)] = count > 0 && powerFirst > 1.0e-20
                ? static_cast<float>(sum / count / powerFirst) : 0.0f;
        }
        double phaseStepSum = 0.0;
        double phaseStepSquared = 0.0;
        std::complex<double> pseudoPower(0.0, 0.0);
        double complexPower = 0.0;
        int phaseCount = 0;
        for (std::size_t i = 1; i < sampleCount; ++i) {
            const std::complex<double> previous(first[i - 1] - meanFirst, second[i - 1] - meanSecond);
            const std::complex<double> current(first[i] - meanFirst, second[i] - meanSecond);
            const double phaseStep = std::arg(current * std::conj(previous));
            if (std::isfinite(phaseStep)) {
                phaseStepSum += phaseStep;
                phaseStepSquared += phaseStep * phaseStep;
                ++phaseCount;
            }
            pseudoPower += current * current;
            complexPower += std::norm(current);
        }
        const double meanPhaseStep = phaseCount > 0 ? phaseStepSum / phaseCount : 0.0;
        const double phaseStd = phaseCount > 0
            ? std::sqrt(std::max(0.0, phaseStepSquared / phaseCount - meanPhaseStep * meanPhaseStep)) : 0.0;
        const double instantFrequency = analysisSampleRate > 0.0
            ? meanPhaseStep * analysisSampleRate / kTwoPi : 0.0;
        const double instantFrequencyStd = analysisSampleRate > 0.0
            ? phaseStd * analysisSampleRate / kTwoPi : 0.0;
        const double pseudo = std::abs(pseudoPower);
        const double imageRatio = complexPower > pseudo + 1.0e-20
            ? 10.0 * std::log10((complexPower + pseudo) / (complexPower - pseudo))
            : 99.0;
        std::vector<float> centeredFirst(sampleCount);
        std::vector<float> centeredSecond(sampleCount);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            centeredFirst[i] = static_cast<float>(first[i] - meanFirst);
            centeredSecond[i] = static_cast<float>(second[i] - meanSecond);
        }

        const double symbolRate = iqSymbolRateSpin
                                      ? iqSymbolRateSpin->value()
                                      : 4800.0;
        const double samplesPerSymbol =
            symbolRate > 0.0 ? analysisSampleRate / symbolRate : 0.0;
        const double phaseFraction = iqPhaseSpin
                                         ? iqPhaseSpin->value() / 100.0
                                         : 0.5;
        auto interpolated = [](const std::vector<float> &values, double position) {
            if (values.empty()) return 0.0f;
            if (position <= 0.0) return values.front();
            const double maximum = static_cast<double>(values.size() - 1U);
            if (position >= maximum) return values.back();
            const std::size_t left = static_cast<std::size_t>(position);
            const float fraction = static_cast<float>(
                position - static_cast<double>(left));
            return values[left] +
                   (values[left + 1U] - values[left]) * fraction;
        };

        std::vector<float> constellationFirst;
        std::vector<float> constellationSecond;
        if (viewMode != 4 && samplesPerSymbol >= 2.0) {
            const double phaseSamples = phaseFraction * samplesPerSymbol;
            for (double position = phaseSamples;
                 position + 1.0 < static_cast<double>(sampleCount) &&
                 constellationFirst.size() < 3000U;
                 position += samplesPerSymbol) {
                constellationFirst.push_back(interpolated(centeredFirst, position));
                constellationSecond.push_back(interpolated(centeredSecond, position));
            }
        }
        if (viewMode != 4 && constellationFirst.empty()) {
            const std::size_t stride =
                (std::max<std::size_t>)(1U, sampleCount / 2000U);
            for (std::size_t i = 0; i < sampleCount; i += stride) {
                constellationFirst.push_back(centeredFirst[i]);
                constellationSecond.push_back(centeredSecond[i]);
            }
        }

        constexpr int eyePoints = 192;
        std::vector<float> eyeFirst;
        std::vector<float> eyeSecond;
        int eyeTraceCount = 0;
        if (viewMode != 4 && samplesPerSymbol >= 2.0) {
            const int requestedTraces = iqTraceCountSpin
                                            ? iqTraceCountSpin->value()
                                            : 32;
            const double eyeLengthSamples = 2.0 * samplesPerSymbol;
            const double firstTraceStart = phaseFraction * samplesPerSymbol;
            for (int trace = 0; trace < requestedTraces; ++trace) {
                const double traceStart =
                    firstTraceStart + static_cast<double>(trace) * samplesPerSymbol;
                if (traceStart + eyeLengthSamples + 1.0 >=
                    static_cast<double>(sampleCount)) {
                    break;
                }
                for (int point = 0; point < eyePoints; ++point) {
                    const double position =
                        traceStart + eyeLengthSamples * point /
                                         static_cast<double>(eyePoints - 1);
                    eyeFirst.push_back(interpolated(centeredFirst, position));
                    eyeSecond.push_back(interpolated(centeredSecond, position));
                }
                ++eyeTraceCount;
            }
        }

        const QString viewName = iqViewCombo
                                     ? iqViewCombo->currentText()
                                     : QStringLiteral("Combined");
        iqStatus->setText(
            trText(QStringLiteral("research_iq_status"),
                    QStringLiteral("%1 / %2 | rate %3 | samples %4 | %5 samples/symbol | eye traces %6 | DC I/Q %7 / %8 | RMS I/Q %9 / %10 | imbalance %11 dB | correlation %12 | frequency %13 +/- %14 | image estimate %15 dB | arrival %16 MS/s | epoch/seq %17/%18 | queue %19 | clip %20% | invalid %21%"))
                .arg(iqSourceName)
                .arg(viewName)
                .arg(frequencyText(analysisSampleRate))
                .arg(sampleCount)
                .arg(samplesPerSymbol, 0, 'f', 2)
                .arg(eyeTraceCount)
                .arg(meanFirst, 0, 'g', 5)
                .arg(meanSecond, 0, 'g', 5)
                .arg(rmsFirst, 0, 'g', 5)
                .arg(rmsSecond, 0, 'g', 5)
                .arg(20.0 * std::log10(std::max(1.0e-12, rmsFirst) / std::max(1.0e-12, rmsSecond)), 0, 'f', 2)
                .arg(correlation, 0, 'f', 4)
                .arg(frequencyText(instantFrequency))
                .arg(frequencyText(instantFrequencyStd))
                .arg(imageRatio, 0, 'f', 1)
                .arg(measuredIqRateHz / 1.0e6, 0, 'f', 3)
                .arg(static_cast<qulonglong>(bufferStats.epoch))
                .arg(static_cast<qulonglong>(bufferStats.sequence))
                .arg(bufferStats.queuedBlocks)
                .arg(100.0 * clippedCount / (std::max<std::size_t>)(1, sampleCount), 0, 'f', 3)
                .arg(100.0 * nonFiniteCount / (std::max<std::size_t>)(1, sampleCount), 0, 'f', 4));
        iqConstellationFirst = std::move(centeredFirst);
        iqConstellationSecond = std::move(centeredSecond);
        iqConstellationRate = analysisSampleRate;
        if (viewMode == 4) updateIqView();
        else iqPlot->setIq(first,
                      second,
                      autocorrelation,
                      constellationFirst,
                      constellationSecond,
                      eyeFirst,
                      eyeSecond,
                      eyePoints,
                      viewMode);
        return;
    }

    if (!isDirectInputMode(context.inputMode)) {
        dualStatus->setText(trText(QStringLiteral("research_dual_rf_unavailable"),
                                   QStringLiteral("RF mode contains quadrature I/Q, not two independent HF inputs. Select HF combined or HF interference lab.")));
        dualPlot->setDual({}, {}, {}, {}, {});
        return;
    }

    constexpr int maxLag = 96;
    std::vector<float> crossCorrelation(2 * maxLag + 1, 0.0f);
    int bestLag = 0;
    double bestAbsoluteCorrelation = -1.0;
    double bestCorrelation = 0.0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double sum = 0.0;
        int count = 0;
        for (int i = std::max(0, -lag); i < static_cast<int>(sampleCount) - std::max(0, lag); i += 2) {
            sum += (first[static_cast<std::size_t>(i)] - meanFirst) *
                   (second[static_cast<std::size_t>(i + lag)] - meanSecond);
            ++count;
        }
        const double value = count > 0
            ? sum / count / std::sqrt(std::max(1.0e-20, powerFirst * powerSecond)) : 0.0;
        crossCorrelation[static_cast<std::size_t>(lag + maxLag)] = static_cast<float>(value);
        if (std::abs(value) > bestAbsoluteCorrelation) {
            bestAbsoluteCorrelation = std::abs(value);
            bestCorrelation = value;
            bestLag = lag;
        }
    }
    const double optimalGain = covariance / std::max(1.0e-20, powerSecond);
    double residualPower = 0.0;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const double residual = (first[i] - meanFirst) - optimalGain * (second[i] - meanSecond);
        residualPower += residual * residual;
    }
    residualPower /= sampleCount;
    const double cancellationDb = 10.0 * std::log10(std::max(1.0e-20, powerFirst) /
                                                     std::max(1.0e-20, residualPower));
    std::size_t dualFftLength = 4096;
    while (dualFftLength > sampleCount && dualFftLength > 256) dualFftLength >>= 1U;
    std::vector<std::complex<float>> firstFft(dualFftLength);
    std::vector<std::complex<float>> secondFft(dualFftLength);
    const std::size_t start = sampleCount - dualFftLength;
    for (std::size_t i = 0; i < dualFftLength; ++i) {
        const float window = dualFftLength > 1
            ? 0.5f - 0.5f * std::cos(static_cast<float>(kTwoPi * i / (dualFftLength - 1)))
            : 1.0f;
        firstFft[i] = std::complex<float>((first[start + i] - static_cast<float>(meanFirst)) * window, 0.0f);
        secondFft[i] = std::complex<float>((second[start + i] - static_cast<float>(meanSecond)) * window, 0.0f);
    }
    fftRadix2(firstFft);
    fftRadix2(secondFft);
    const std::size_t spectrumBins = dualFftLength / 2U + 1U;
    if (dualCrossAverage.size() != spectrumBins) {
        dualCrossAverage.assign(spectrumBins, std::complex<float>(0.0f, 0.0f));
        dualFirstPowerAverage.assign(spectrumBins, 0.0f);
        dualSecondPowerAverage.assign(spectrumBins, 0.0f);
    }
    constexpr float spectralAlpha = 0.16f;
    std::vector<float> firstSpectrum(spectrumBins, kFloorDb);
    std::vector<float> secondSpectrum(spectrumBins, kFloorDb);
    std::vector<float> differenceSpectrum(spectrumBins, 0.0f);
    std::vector<float> coherenceSpectrum(spectrumBins, 0.0f);
    double coherenceSum = 0.0;
    double coherencePeak = 0.0;
    for (std::size_t bin = 0; bin < spectrumBins; ++bin) {
        const float firstPower = std::norm(firstFft[bin]);
        const float secondPower = std::norm(secondFft[bin]);
        const std::complex<float> cross = firstFft[bin] * std::conj(secondFft[bin]);
        dualFirstPowerAverage[bin] += spectralAlpha * (firstPower - dualFirstPowerAverage[bin]);
        dualSecondPowerAverage[bin] += spectralAlpha * (secondPower - dualSecondPowerAverage[bin]);
        dualCrossAverage[bin] += spectralAlpha * (cross - dualCrossAverage[bin]);
        firstSpectrum[bin] = static_cast<float>(safeDb(dualFirstPowerAverage[bin]));
        secondSpectrum[bin] = static_cast<float>(safeDb(dualSecondPowerAverage[bin]));
        differenceSpectrum[bin] = firstSpectrum[bin] - secondSpectrum[bin];
        const double denominator = static_cast<double>(dualFirstPowerAverage[bin]) *
                                   dualSecondPowerAverage[bin] + 1.0e-20;
        const float coherence = static_cast<float>(std::clamp(
            static_cast<double>(std::norm(dualCrossAverage[bin])) / denominator, 0.0, 1.0));
        coherenceSpectrum[bin] = coherence;
        if (bin > 0) {
            coherenceSum += coherence;
            coherencePeak = std::max(coherencePeak, static_cast<double>(coherence));
        }
    }
    const double targetHz = std::clamp(std::abs(context.listeningFrequencyHz),
                                       0.0,
                                       std::max(0.0, context.sampleRateHz * 0.5));
    const std::size_t targetBin = context.sampleRateHz > 0.0
        ? std::min(spectrumBins - 1U,
                   static_cast<std::size_t>(std::llround(targetHz / context.sampleRateHz * dualFftLength)))
        : 0U;
    const std::complex<float> transfer = std::abs(dualCrossAverage[targetBin]) > 1.0e-20f &&
                                          dualSecondPowerAverage[targetBin] > 1.0e-20f
        ? dualCrossAverage[targetBin] / dualSecondPowerAverage[targetBin]
        : std::complex<float>(0.0f, 0.0f);
    const double meanCoherence = spectrumBins > 1U ? coherenceSum / (spectrumBins - 1U) : 0.0;
    const double delayNs = context.sampleRateHz > 0.0 ? bestLag / context.sampleRateHz * 1.0e9 : 0.0;
    dualStatus->setText(
        trText(QStringLiteral("research_dual_status"),
               QStringLiteral("HF1 RMS %1 | HF2 RMS %2 | gain difference %3 dB | correlation %4 | best lag %5 samples / %6 ns | phase at %7: %8 deg | transfer gain %9 dB | coherence %10 / peak %11 | cancellation potential %12 dB"))
            .arg(rmsFirst, 0, 'g', 5)
            .arg(rmsSecond, 0, 'g', 5)
            .arg(20.0 * std::log10(std::max(1.0e-12, rmsFirst) / std::max(1.0e-12, rmsSecond)), 0, 'f', 2)
            .arg(bestCorrelation, 0, 'f', 4)
            .arg(bestLag)
            .arg(delayNs, 0, 'f', 1)
            .arg(frequencyText(targetHz))
            .arg(std::arg(transfer) * 180.0 / 3.14159265358979323846, 0, 'f', 1)
            .arg(20.0 * std::log10(std::max(1.0e-20f, std::abs(transfer))), 0, 'f', 2)
            .arg(meanCoherence, 0, 'f', 3)
            .arg(coherencePeak, 0, 'f', 3)
            .arg(cancellationDb, 0, 'f', 2));
    dualPlot->setDual(firstSpectrum,
                      secondSpectrum,
                      differenceSpectrum,
                      crossCorrelation,
                      coherenceSpectrum);
}
