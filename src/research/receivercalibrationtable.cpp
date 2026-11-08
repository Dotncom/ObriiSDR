#include "receivercalibrationtable.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <QJsonDocument>
#include <QStringList>

namespace {

constexpr double kMinimumFrequencySpacingHz = 0.001;

ReceiverCalibrationCorrection interpolate(const ReceiverCalibrationPoint &lower,
                                          const ReceiverCalibrationPoint &upper,
                                          double frequencyHz) {
    ReceiverCalibrationCorrection result;
    result.valid = true;
    const double spanHz = upper.frequencyHz - lower.frequencyHz;
    const double ratio = spanHz > kMinimumFrequencySpacingHz
                             ? std::clamp((frequencyHz - lower.frequencyHz) / spanHz, 0.0, 1.0)
                             : 0.0;
    result.frequencyOffsetHz = lower.frequencyOffsetHz +
                               (upper.frequencyOffsetHz - lower.frequencyOffsetHz) * ratio;
    result.amplitudeOffsetDb = lower.amplitudeOffsetDb +
                               (upper.amplitudeOffsetDb - lower.amplitudeOffsetDb) * ratio;
    result.uncertaintyDb = lower.uncertaintyDb +
                           (upper.uncertaintyDb - lower.uncertaintyDb) * ratio;
    return result;
}

} // namespace

QJsonObject ReceiverCalibrationContext::toJson() const {
    return {{"backend", backendId}, {"input", inputMode}, {"rate", sampleRateHz},
            {"lna", lnaGain}, {"vga", vgaGain}, {"rtlGain", rtlGainTenthsDb}, {"gainState", gainState}};
}

ReceiverCalibrationContext ReceiverCalibrationContext::fromJson(const QJsonObject &o) {
    ReceiverCalibrationContext c;
    c.backendId = o.value("backend").toString().trimmed();
    c.inputMode = o.value("input").toInt(-1);
    c.sampleRateHz = o.value("rate").toDouble(0.0);
    c.lnaGain = o.value("lna").toInt(-1);
    c.vgaGain = o.value("vga").toInt(-1);
    c.rtlGainTenthsDb = o.value("rtlGain").toInt(-1);
    c.gainState = o.value("gainState").toInt(-1);
    if (!std::isfinite(c.sampleRateHz) || c.sampleRateHz < 0) c.sampleRateHz = 0;
    return c;
}

QString ReceiverCalibrationContext::description() const {
    QStringList parts;
    if (!backendId.isEmpty()) parts << backendId;
    if (inputMode >= 0) parts << QStringLiteral("Input=%1").arg(inputMode);
    if (sampleRateHz > 0) parts << QStringLiteral("Fs=%1 MS/s").arg(sampleRateHz/1e6, 0, 'g', 8);
    if (lnaGain >= 0) parts << QStringLiteral("LNA=%1").arg(lnaGain);
    if (vgaGain >= 0) parts << QStringLiteral("VGA=%1").arg(vgaGain);
    if (rtlGainTenthsDb >= 0) parts << QStringLiteral("RTL=%1 dB").arg(rtlGainTenthsDb/10.0);
    if (gainState >= 0) parts << QStringLiteral("AGC/RFamp=%1").arg(gainState);
    return parts.isEmpty() ? QStringLiteral("*") : parts.join(QStringLiteral(" / "));
}

QVector<ReceiverCalibrationPoint> ReceiverCalibrationTable::selectedPoints(const ReceiverCalibrationContext &c) const {
    QVector<ReceiverCalibrationPoint> selected;
    int best = -1;
    for (const auto &p : calibrationPoints) {
        const auto &s = p.conditions;
        if ((!s.backendId.isEmpty() && s.backendId != c.backendId) ||
            (s.inputMode >= 0 && s.inputMode != c.inputMode) ||
            (s.sampleRateHz > 0 && std::abs(s.sampleRateHz-c.sampleRateHz) > 0.5) ||
            (s.lnaGain >= 0 && s.lnaGain != c.lnaGain) ||
            (s.vgaGain >= 0 && s.vgaGain != c.vgaGain) ||
            (s.rtlGainTenthsDb >= 0 && s.rtlGainTenthsDb != c.rtlGainTenthsDb) ||
            (s.gainState >= 0 && s.gainState != c.gainState)) continue;
        const int specificity = (!s.backendId.isEmpty() ? 64 : 0) + (s.inputMode >= 0 ? 32 : 0) +
            (s.sampleRateHz > 0 ? 16 : 0) + (s.lnaGain >= 0 ? 8 : 0) +
            (s.vgaGain >= 0 ? 4 : 0) + (s.rtlGainTenthsDb >= 0 ? 2 : 0) + (s.gainState >= 0 ? 1 : 0);
        if (specificity < best) continue;
        if (specificity > best) { selected.clear(); best = specificity; }
        selected.append(p);
    }
    std::sort(selected.begin(), selected.end(), [](const auto &a, const auto &b) { return a.frequencyHz < b.frequencyHz; });
    return selected;
}

void ReceiverCalibrationTable::setContext(const ReceiverCalibrationContext &context) {
    if (contextInitialized && currentContext.backendId == context.backendId &&
        currentContext.inputMode == context.inputMode && currentContext.sampleRateHz == context.sampleRateHz &&
        currentContext.lnaGain == context.lnaGain && currentContext.vgaGain == context.vgaGain &&
        currentContext.rtlGainTenthsDb == context.rtlGainTenthsDb && currentContext.gainState == context.gainState) return;
    currentContext = context;
    contextInitialized = true;
    activePoints = selectedPoints(context);
}

void ReceiverCalibrationTable::setPoints(const QVector<ReceiverCalibrationPoint> &points) {
    calibrationPoints.clear();
    calibrationPoints.reserve(points.size());
    for (const ReceiverCalibrationPoint &point : points) {
        if (!std::isfinite(point.frequencyHz) || point.frequencyHz < 0.0 ||
            !std::isfinite(point.frequencyOffsetHz) ||
            !std::isfinite(point.amplitudeOffsetDb) ||
            !std::isfinite(point.uncertaintyDb) || point.uncertaintyDb < 0.0) {
            continue;
        }
        ReceiverCalibrationPoint clean = point;
        clean.note = clean.note.trimmed().left(256);
        calibrationPoints.append(clean);
    }
    std::sort(calibrationPoints.begin(), calibrationPoints.end(),
              [](const ReceiverCalibrationPoint &a, const ReceiverCalibrationPoint &b) {
                  const QByteArray aKey = QJsonDocument(a.conditions.toJson()).toJson(QJsonDocument::Compact);
                  const QByteArray bKey = QJsonDocument(b.conditions.toJson()).toJson(QJsonDocument::Compact);
                  if (aKey != bKey) return aKey < bKey;
                  return a.frequencyHz < b.frequencyHz;
              });

    QVector<ReceiverCalibrationPoint> unique;
    unique.reserve(calibrationPoints.size());
    for (const ReceiverCalibrationPoint &point : std::as_const(calibrationPoints)) {
        if (!unique.isEmpty() &&
            std::abs(unique.last().frequencyHz - point.frequencyHz) < kMinimumFrequencySpacingHz &&
            unique.last().conditions.toJson() == point.conditions.toJson()) {
            unique.last() = point;
        } else {
            unique.append(point);
        }
    }
    calibrationPoints = std::move(unique);
    activePoints = selectedPoints(currentContext);
}

ReceiverCalibrationCorrection ReceiverCalibrationTable::correctionAt(double frequencyHz, const ReceiverCalibrationContext &context) const {
    ReceiverCalibrationTable selected;
    selected.setPoints(selectedPoints(context));
    // Explicitly select this context so specific profiles are not treated as wildcard rows.
    selected.setContext(context);
    return selected.correctionAt(frequencyHz);
}

ReceiverCalibrationCorrection ReceiverCalibrationTable::correctionAt(double frequencyHz) const {
    const auto &calibrationPoints = activePoints;
    ReceiverCalibrationCorrection result;
    if (calibrationPoints.isEmpty() || !std::isfinite(frequencyHz)) {
        return result;
    }
    result.valid = true;
    if (calibrationPoints.size() == 1 || frequencyHz <= calibrationPoints.first().frequencyHz) {
        result.frequencyOffsetHz = calibrationPoints.first().frequencyOffsetHz;
        result.amplitudeOffsetDb = calibrationPoints.first().amplitudeOffsetDb;
        result.uncertaintyDb = calibrationPoints.first().uncertaintyDb;
        return result;
    }
    if (frequencyHz >= calibrationPoints.last().frequencyHz) {
        result.frequencyOffsetHz = calibrationPoints.last().frequencyOffsetHz;
        result.amplitudeOffsetDb = calibrationPoints.last().amplitudeOffsetDb;
        result.uncertaintyDb = calibrationPoints.last().uncertaintyDb;
        return result;
    }

    const auto upper = std::lower_bound(
        calibrationPoints.cbegin(), calibrationPoints.cend(), frequencyHz,
        [](const ReceiverCalibrationPoint &point, double value) {
            return point.frequencyHz < value;
        });
    const int upperIndex = static_cast<int>(std::distance(calibrationPoints.cbegin(), upper));
    return interpolate(calibrationPoints.at(upperIndex - 1),
                       calibrationPoints.at(upperIndex),
                       frequencyHz);
}

double ReceiverCalibrationTable::interpolatedAmplitudeOffset(double frequencyHz,
                                                              int &upperIndex) const {
    const auto &calibrationPoints = activePoints;
    if (calibrationPoints.isEmpty() || !std::isfinite(frequencyHz)) {
        return 0.0;
    }
    if (calibrationPoints.size() == 1 || frequencyHz <= calibrationPoints.first().frequencyHz) {
        upperIndex = calibrationPoints.size() > 1 ? 1 : 0;
        return calibrationPoints.first().amplitudeOffsetDb;
    }
    while (upperIndex < calibrationPoints.size() &&
           frequencyHz > calibrationPoints.at(upperIndex).frequencyHz) {
        ++upperIndex;
    }
    if (upperIndex >= calibrationPoints.size()) {
        return calibrationPoints.last().amplitudeOffsetDb;
    }
    return interpolate(calibrationPoints.at(upperIndex - 1),
                       calibrationPoints.at(upperIndex),
                       frequencyHz).amplitudeOffsetDb;
}

void ReceiverCalibrationTable::applyAmplitudeCorrection(const std::vector<float> &frequencies,
                                                         std::vector<float> &primary,
                                                         std::vector<float> *secondary,
                                                         double baseOffsetDb) const {
    const auto &calibrationPoints = activePoints;
    const std::size_t count = (std::min)(frequencies.size(), primary.size());
    if (count == 0) {
        return;
    }
    const bool haveCurve = !calibrationPoints.isEmpty();
    if (!haveCurve && (!std::isfinite(baseOffsetDb) || std::abs(baseOffsetDb) <= 0.000001)) {
        return;
    }

    int upperIndex = calibrationPoints.size() > 1 ? 1 : 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0 && frequencies[i] < frequencies[i - 1]) {
            upperIndex = calibrationPoints.size() > 1 ? 1 : 0;
        }
        const double curveDb = haveCurve
                                   ? interpolatedAmplitudeOffset(frequencies[i], upperIndex)
                                   : 0.0;
        const float correctionDb = static_cast<float>(
            (std::isfinite(baseOffsetDb) ? baseOffsetDb : 0.0) + curveDb);
        if (std::isfinite(primary[i])) {
            primary[i] += correctionDb;
        }
        if (secondary && i < secondary->size() && std::isfinite((*secondary)[i])) {
            (*secondary)[i] += correctionDb;
        }
    }
}
