#include "analyzerinstrumentplan.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
double finiteOr(const QJsonObject &object, const char *key, double fallback) {
    const double value = object.value(QLatin1String(key)).toDouble(fallback);
    return std::isfinite(value) ? value : fallback;
}
}

QJsonObject AnalyzerHardwareCapabilities::toJson() const {
    return {{QStringLiteral("receiverAvailable"), receiverAvailable},
            {QStringLiteral("transmitterAvailable"), transmitterAvailable},
            {QStringLiteral("fullDuplex"), fullDuplex},
            {QStringLiteral("coherentReference"), coherentReference},
            {QStringLiteral("timestampedIq"), timestampedIq},
            {QStringLiteral("externalGenerator"), externalGenerator},
            {QStringLiteral("directionalCoupler"), directionalCoupler},
            {QStringLiteral("receiverChannels"), receiverChannels},
            {QStringLiteral("transmitterChannels"), transmitterChannels},
            {QStringLiteral("minimumFrequencyHz"), minimumFrequencyHz},
            {QStringLiteral("maximumFrequencyHz"), maximumFrequencyHz},
            {QStringLiteral("maximumSafeOutputDbm"), maximumSafeOutputDbm}};
}

AnalyzerHardwareCapabilities AnalyzerHardwareCapabilities::fromJson(const QJsonObject &object) {
    AnalyzerHardwareCapabilities value;
    value.receiverAvailable = object.value(QStringLiteral("receiverAvailable")).toBool();
    value.transmitterAvailable = object.value(QStringLiteral("transmitterAvailable")).toBool();
    value.fullDuplex = object.value(QStringLiteral("fullDuplex")).toBool();
    value.coherentReference = object.value(QStringLiteral("coherentReference")).toBool();
    value.timestampedIq = object.value(QStringLiteral("timestampedIq")).toBool();
    value.externalGenerator = object.value(QStringLiteral("externalGenerator")).toBool();
    value.directionalCoupler = object.value(QStringLiteral("directionalCoupler")).toBool();
    value.receiverChannels = object.value(QStringLiteral("receiverChannels")).toInt();
    value.transmitterChannels = object.value(QStringLiteral("transmitterChannels")).toInt();
    value.minimumFrequencyHz = finiteOr(object, "minimumFrequencyHz", 0.0);
    value.maximumFrequencyHz = finiteOr(object, "maximumFrequencyHz", 0.0);
    value.maximumSafeOutputDbm = finiteOr(object, "maximumSafeOutputDbm", -100.0);
    return value;
}

QJsonObject AnalyzerScpiProfile::toJson() const {
    return {{QStringLiteral("enabled"), enabled},
            {QStringLiteral("resource"), resource},
            {QStringLiteral("identityQuery"), identityQuery},
            {QStringLiteral("frequencyCommand"), frequencyCommand},
            {QStringLiteral("powerCommand"), powerCommand},
            {QStringLiteral("outputCommand"), outputCommand},
            {QStringLiteral("triggerCommand"), triggerCommand},
            {QStringLiteral("readCommand"), readCommand},
            {QStringLiteral("timeoutMs"), timeoutMs}};
}

AnalyzerScpiProfile AnalyzerScpiProfile::fromJson(const QJsonObject &object) {
    AnalyzerScpiProfile value;
    value.enabled = object.value(QStringLiteral("enabled")).toBool();
    value.resource = object.value(QStringLiteral("resource")).toString();
    value.identityQuery = object.value(QStringLiteral("identityQuery")).toString(value.identityQuery);
    value.frequencyCommand = object.value(QStringLiteral("frequencyCommand")).toString(value.frequencyCommand);
    value.powerCommand = object.value(QStringLiteral("powerCommand")).toString(value.powerCommand);
    value.outputCommand = object.value(QStringLiteral("outputCommand")).toString(value.outputCommand);
    value.triggerCommand = object.value(QStringLiteral("triggerCommand")).toString(value.triggerCommand);
    value.readCommand = object.value(QStringLiteral("readCommand")).toString();
    value.timeoutMs = std::clamp(object.value(QStringLiteral("timeoutMs")).toInt(2000), 100, 60000);
    return value;
}

QJsonObject AnalyzerCalibrationState::toJson() const {
    return {{QStringLiteral("calibrationId"), calibrationId},
            {QStringLiteral("notes"), notes},
            {QStringLiteral("capturedUtc"), capturedUtc.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("referenceImpedanceOhm"), referenceImpedanceOhm},
            {QStringLiteral("scalarNormalization"), scalarNormalization},
            {QStringLiteral("openMeasured"), openMeasured},
            {QStringLiteral("shortMeasured"), shortMeasured},
            {QStringLiteral("loadMeasured"), loadMeasured},
            {QStringLiteral("thruMeasured"), thruMeasured},
            {QStringLiteral("phaseReferenceMeasured"), phaseReferenceMeasured}};
}

AnalyzerCalibrationState AnalyzerCalibrationState::fromJson(const QJsonObject &object) {
    AnalyzerCalibrationState value;
    value.calibrationId = object.value(QStringLiteral("calibrationId")).toString();
    value.notes = object.value(QStringLiteral("notes")).toString();
    value.capturedUtc = QDateTime::fromString(object.value(QStringLiteral("capturedUtc")).toString(), Qt::ISODateWithMs);
    value.referenceImpedanceOhm = finiteOr(object, "referenceImpedanceOhm", 50.0);
    value.scalarNormalization = object.value(QStringLiteral("scalarNormalization")).toBool();
    value.openMeasured = object.value(QStringLiteral("openMeasured")).toBool();
    value.shortMeasured = object.value(QStringLiteral("shortMeasured")).toBool();
    value.loadMeasured = object.value(QStringLiteral("loadMeasured")).toBool();
    value.thruMeasured = object.value(QStringLiteral("thruMeasured")).toBool();
    value.phaseReferenceMeasured = object.value(QStringLiteral("phaseReferenceMeasured")).toBool();
    return value;
}

QJsonObject AnalyzerInstrumentPlan::toJson() const {
    return {{QStringLiteral("schema"), QStringLiteral("obrii-analyzer-plan-v1")},
            {QStringLiteral("mode"), AnalyzerInstrumentPlanner::modeId(mode)},
            {QStringLiteral("name"), name},
            {QStringLiteral("transmitDeviceId"), transmitDeviceId},
            {QStringLiteral("receiveDeviceId"), receiveDeviceId},
            {QStringLiteral("useExternalGenerator"), useExternalGenerator},
            {QStringLiteral("startHz"), startHz},
            {QStringLiteral("stopHz"), stopHz},
            {QStringLiteral("points"), points},
            {QStringLiteral("sourceLevelDbm"), sourceLevelDbm},
            {QStringLiteral("dwellMs"), dwellMs},
            {QStringLiteral("settleMs"), settleMs},
            {QStringLiteral("averages"), averages},
            {QStringLiteral("bidirectionalSweep"), bidirectionalSweep},
            {QStringLiteral("capabilities"), capabilities.toJson()},
            {QStringLiteral("calibration"), calibration.toJson()},
            {QStringLiteral("scpi"), scpi.toJson()},
            {QStringLiteral("operator"), operatorName},
            {QStringLiteral("deviceUnderTest"), deviceUnderTest},
            {QStringLiteral("comment"), comment}};
}

AnalyzerInstrumentPlan AnalyzerInstrumentPlan::fromJson(const QJsonObject &object) {
    AnalyzerInstrumentPlan value;
    value.mode = AnalyzerInstrumentPlanner::modeFromId(object.value(QStringLiteral("mode")).toString());
    value.name = object.value(QStringLiteral("name")).toString(value.name);
    value.transmitDeviceId = object.value(QStringLiteral("transmitDeviceId")).toString();
    value.receiveDeviceId = object.value(QStringLiteral("receiveDeviceId")).toString();
    value.useExternalGenerator = object.value(QStringLiteral("useExternalGenerator")).toBool();
    value.startHz = finiteOr(object, "startHz", value.startHz);
    value.stopHz = finiteOr(object, "stopHz", value.stopHz);
    value.points = object.value(QStringLiteral("points")).toInt(value.points);
    value.sourceLevelDbm = finiteOr(object, "sourceLevelDbm", value.sourceLevelDbm);
    value.dwellMs = finiteOr(object, "dwellMs", value.dwellMs);
    value.settleMs = finiteOr(object, "settleMs", value.settleMs);
    value.averages = object.value(QStringLiteral("averages")).toInt(value.averages);
    value.bidirectionalSweep = object.value(QStringLiteral("bidirectionalSweep")).toBool();
    value.capabilities = AnalyzerHardwareCapabilities::fromJson(object.value(QStringLiteral("capabilities")).toObject());
    value.calibration = AnalyzerCalibrationState::fromJson(object.value(QStringLiteral("calibration")).toObject());
    value.scpi = AnalyzerScpiProfile::fromJson(object.value(QStringLiteral("scpi")).toObject());
    value.operatorName = object.value(QStringLiteral("operator")).toString();
    value.deviceUnderTest = object.value(QStringLiteral("deviceUnderTest")).toString();
    value.comment = object.value(QStringLiteral("comment")).toString();
    return value;
}

QString AnalyzerPlanValidation::summary() const {
    QStringList lines;
    lines << (executable ? QStringLiteral("Plan is executable")
                         : QStringLiteral("RF execution is blocked"));
    lines << (calibratedResult ? QStringLiteral("Calibrated result requirements are satisfied")
                               : QStringLiteral("Result must not be presented as calibrated"));
    for (const QString &error : errors) lines << QStringLiteral("ERROR: %1").arg(error);
    for (const QString &warning : warnings) lines << QStringLiteral("WARNING: %1").arg(warning);
    for (const QString &requirement : requirements) lines << QStringLiteral("NEEDS: %1").arg(requirement);
    return lines.join(QLatin1Char('\n'));
}

QJsonObject AnalyzerMeasurementPoint::toJson() const {
    return {{QStringLiteral("frequencyHz"), frequencyHz},
            {QStringLiteral("stimulusDbm"), stimulusDbm},
            {QStringLiteral("referenceDb"), referenceDb},
            {QStringLiteral("forwardDb"), forwardDb},
            {QStringLiteral("reflectedDb"), reflectedDb},
            {QStringLiteral("phaseDegrees"), phaseDegrees},
            {QStringLiteral("s11Magnitude"), s11Magnitude},
            {QStringLiteral("s21Magnitude"), s21Magnitude},
            {QStringLiteral("valid"), valid}};
}

AnalyzerMeasurementPoint AnalyzerMeasurementPoint::fromJson(const QJsonObject &object) {
    AnalyzerMeasurementPoint value;
    value.frequencyHz = finiteOr(object, "frequencyHz", 0.0);
    value.stimulusDbm = finiteOr(object, "stimulusDbm", 0.0);
    value.referenceDb = finiteOr(object, "referenceDb", 0.0);
    value.forwardDb = finiteOr(object, "forwardDb", 0.0);
    value.reflectedDb = finiteOr(object, "reflectedDb", 0.0);
    value.phaseDegrees = finiteOr(object, "phaseDegrees", 0.0);
    value.s11Magnitude = finiteOr(object, "s11Magnitude", 0.0);
    value.s21Magnitude = finiteOr(object, "s21Magnitude", 0.0);
    value.valid = object.value(QStringLiteral("valid")).toBool();
    return value;
}

QJsonObject AnalyzerMeasurementSession::toJson() const {
    QJsonArray pointArray;
    for (const AnalyzerMeasurementPoint &point : points) pointArray.append(point.toJson());
    QJsonArray logArray;
    for (const QString &entry : eventLog) logArray.append(entry);
    return {{QStringLiteral("schema"), QStringLiteral("obrii-analyzer-session-v1")},
            {QStringLiteral("sessionId"), sessionId},
            {QStringLiteral("applicationVersion"), applicationVersion},
            {QStringLiteral("startedUtc"), startedUtc.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("completedUtc"), completedUtc.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("plan"), plan.toJson()},
            {QStringLiteral("points"), pointArray},
            {QStringLiteral("eventLog"), logArray}};
}

AnalyzerMeasurementSession AnalyzerMeasurementSession::fromJson(const QJsonObject &object) {
    AnalyzerMeasurementSession value;
    value.sessionId = object.value(QStringLiteral("sessionId")).toString();
    value.applicationVersion = object.value(QStringLiteral("applicationVersion")).toString();
    value.startedUtc = QDateTime::fromString(object.value(QStringLiteral("startedUtc")).toString(), Qt::ISODateWithMs);
    value.completedUtc = QDateTime::fromString(object.value(QStringLiteral("completedUtc")).toString(), Qt::ISODateWithMs);
    value.plan = AnalyzerInstrumentPlan::fromJson(object.value(QStringLiteral("plan")).toObject());
    for (const QJsonValue &point : object.value(QStringLiteral("points")).toArray())
        value.points.append(AnalyzerMeasurementPoint::fromJson(point.toObject()));
    for (const QJsonValue &entry : object.value(QStringLiteral("eventLog")).toArray())
        value.eventLog.append(entry.toString());
    return value;
}

QString AnalyzerInstrumentPlanner::modeId(AnalyzerInstrumentMode mode) {
    switch (mode) {
    case AnalyzerInstrumentMode::TrackingGenerator: return QStringLiteral("tracking-generator");
    case AnalyzerInstrumentMode::ScalarNetworkAnalyzer: return QStringLiteral("sna");
    case AnalyzerInstrumentMode::ReturnLossSwr: return QStringLiteral("return-loss-swr");
    case AnalyzerInstrumentMode::VectorNetworkAnalyzer: return QStringLiteral("vna");
    }
    return QStringLiteral("tracking-generator");
}

QString AnalyzerInstrumentPlanner::modeName(AnalyzerInstrumentMode mode) {
    switch (mode) {
    case AnalyzerInstrumentMode::TrackingGenerator: return QStringLiteral("Tracking generator");
    case AnalyzerInstrumentMode::ScalarNetworkAnalyzer: return QStringLiteral("Scalar network analyzer");
    case AnalyzerInstrumentMode::ReturnLossSwr: return QStringLiteral("Return loss / SWR");
    case AnalyzerInstrumentMode::VectorNetworkAnalyzer: return QStringLiteral("Vector network analyzer");
    }
    return QStringLiteral("Tracking generator");
}

AnalyzerInstrumentMode AnalyzerInstrumentPlanner::modeFromId(const QString &id) {
    if (id == QStringLiteral("sna")) return AnalyzerInstrumentMode::ScalarNetworkAnalyzer;
    if (id == QStringLiteral("return-loss-swr")) return AnalyzerInstrumentMode::ReturnLossSwr;
    if (id == QStringLiteral("vna")) return AnalyzerInstrumentMode::VectorNetworkAnalyzer;
    return AnalyzerInstrumentMode::TrackingGenerator;
}

AnalyzerPlanValidation AnalyzerInstrumentPlanner::validate(const AnalyzerInstrumentPlan &plan) {
    AnalyzerPlanValidation result;
    if (!std::isfinite(plan.startHz) || !std::isfinite(plan.stopHz) ||
        plan.startHz < 0.0 || plan.stopHz <= plan.startHz)
        result.errors << QStringLiteral("Stop frequency must be above start frequency");
    if (!std::isfinite(plan.sourceLevelDbm))
        result.errors << QStringLiteral("Source level must be finite");
    if (!std::isfinite(plan.capabilities.maximumSafeOutputDbm))
        result.errors << QStringLiteral("Safe output limit must be finite");
    if (!std::isfinite(plan.capabilities.minimumFrequencyHz) ||
        !std::isfinite(plan.capabilities.maximumFrequencyHz) ||
        plan.capabilities.minimumFrequencyHz < 0.0 ||
        plan.capabilities.maximumFrequencyHz < plan.capabilities.minimumFrequencyHz)
        result.errors << QStringLiteral("Hardware frequency range must be finite and ordered");
    if (!std::isfinite(plan.calibration.referenceImpedanceOhm) ||
        plan.calibration.referenceImpedanceOhm <= 0.0)
        result.errors << QStringLiteral("Reference impedance must be finite and positive");
    if (plan.points < 2 || plan.points > 1000001)
        result.errors << QStringLiteral("Point count must be between 2 and 1,000,001");
    if (!std::isfinite(plan.dwellMs) || plan.dwellMs < 0.0 || plan.dwellMs > 60000.0)
        result.errors << QStringLiteral("Dwell must be between 0 and 60,000 ms");
    if (!std::isfinite(plan.settleMs) || plan.settleMs < 0.0 || plan.settleMs > 60000.0)
        result.errors << QStringLiteral("Settling time must be between 0 and 60,000 ms");
    if (plan.averages < 1 || plan.averages > 100000)
        result.errors << QStringLiteral("Average count must be between 1 and 100,000");
    if (!plan.capabilities.receiverAvailable)
        result.requirements << QStringLiteral("a confirmed RX channel");
    else if (plan.capabilities.receiverChannels < 1)
        result.requirements << QStringLiteral("at least one declared RX channel");
    if (!plan.useExternalGenerator && plan.capabilities.transmitterAvailable &&
        plan.capabilities.transmitterChannels < 1)
        result.requirements << QStringLiteral("at least one declared TX channel");

    const bool sourceAvailable = plan.useExternalGenerator
                                     ? plan.capabilities.externalGenerator
                                     : plan.capabilities.transmitterAvailable;
    if (!sourceAvailable)
        result.requirements << (plan.useExternalGenerator
                                    ? QStringLiteral("a reachable external generator")
                                    : QStringLiteral("a confirmed TX channel"));
    if (plan.useExternalGenerator && (!plan.scpi.enabled || plan.scpi.resource.trimmed().isEmpty()))
        result.requirements << QStringLiteral("an enabled SCPI resource for the external generator");
    if (!plan.useExternalGenerator && plan.sourceLevelDbm > plan.capabilities.maximumSafeOutputDbm)
        result.errors << QStringLiteral("Requested source level exceeds the declared safe TX limit");
    if (plan.capabilities.maximumFrequencyHz > plan.capabilities.minimumFrequencyHz &&
        (plan.startHz < plan.capabilities.minimumFrequencyHz ||
         plan.stopHz > plan.capabilities.maximumFrequencyHz))
        result.errors << QStringLiteral("Sweep is outside the declared hardware frequency range");

    switch (plan.mode) {
    case AnalyzerInstrumentMode::TrackingGenerator:
        result.calibratedResult = false;
        result.warnings << QStringLiteral("Tracking output is a stimulus plan, not a calibrated DUT measurement");
        break;
    case AnalyzerInstrumentMode::ScalarNetworkAnalyzer:
        if (!plan.useExternalGenerator && !plan.capabilities.fullDuplex)
            result.requirements << QStringLiteral("full-duplex RX/TX or a separate external generator");
        if (!plan.calibration.scalarNormalization)
            result.requirements << QStringLiteral("a scalar through/reference normalization");
        result.calibratedResult = plan.calibration.scalarNormalization;
        break;
    case AnalyzerInstrumentMode::ReturnLossSwr:
        if (!plan.useExternalGenerator && !plan.capabilities.fullDuplex)
            result.requirements << QStringLiteral("full-duplex RX/TX or a separate external generator");
        if (!plan.capabilities.directionalCoupler)
            result.requirements << QStringLiteral("a characterized directional bridge/coupler");
        if (!plan.calibration.oslComplete())
            result.requirements << QStringLiteral("Open, Short and Load calibration captures");
        result.calibratedResult = plan.capabilities.directionalCoupler && plan.calibration.oslComplete();
        break;
    case AnalyzerInstrumentMode::VectorNetworkAnalyzer:
        if (!plan.capabilities.fullDuplex)
            result.requirements << QStringLiteral("simultaneous full-duplex RX/TX");
        if (plan.capabilities.receiverChannels < 2)
            result.requirements << QStringLiteral("two coherent RX channels (reference and measurement)");
        if (!plan.capabilities.coherentReference)
            result.requirements << QStringLiteral("a shared coherent clock/reference");
        if (!plan.capabilities.timestampedIq)
            result.requirements << QStringLiteral("timestamped phase-stable IQ");
        if (!plan.calibration.vectorComplete())
            result.requirements << QStringLiteral("OSL, thru and phase-reference calibration");
        result.calibratedResult = plan.capabilities.fullDuplex &&
                                  plan.capabilities.receiverChannels >= 2 &&
                                  plan.capabilities.coherentReference &&
                                  plan.capabilities.timestampedIq &&
                                  plan.calibration.vectorComplete();
        break;
    }
    result.executable = result.errors.isEmpty() && result.requirements.isEmpty();
    if (!result.calibratedResult && plan.mode != AnalyzerInstrumentMode::TrackingGenerator)
        result.warnings << QStringLiteral("Uncalibrated values must be labelled diagnostic only");
    return result;
}

QVector<double> AnalyzerInstrumentPlanner::frequencyGrid(const AnalyzerInstrumentPlan &plan) {
    QVector<double> frequencies;
    if (plan.points < 2 || plan.points > 1000001 ||
        !std::isfinite(plan.startHz) || !std::isfinite(plan.stopHz) ||
        plan.startHz < 0.0 || plan.stopHz <= plan.startHz) return frequencies;
    frequencies.resize(plan.points);
    const double step = (plan.stopHz - plan.startHz) / static_cast<double>(plan.points - 1);
    for (int index = 0; index < plan.points; ++index)
        frequencies[index] = plan.startHz + step * static_cast<double>(index);
    frequencies.last() = plan.stopHz;
    return frequencies;
}

double AnalyzerInstrumentPlanner::returnLossDb(double reflectionMagnitude) {
    if (!std::isfinite(reflectionMagnitude) || reflectionMagnitude < 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    if (reflectionMagnitude == 0.0) return std::numeric_limits<double>::infinity();
    return -20.0 * std::log10(reflectionMagnitude);
}

double AnalyzerInstrumentPlanner::swrFromReflectionMagnitude(double reflectionMagnitude) {
    if (!std::isfinite(reflectionMagnitude) || reflectionMagnitude < 0.0 || reflectionMagnitude >= 1.0)
        return std::numeric_limits<double>::infinity();
    return (1.0 + reflectionMagnitude) / (1.0 - reflectionMagnitude);
}

double AnalyzerInstrumentPlanner::reflectionMagnitudeFromReturnLoss(double returnLoss) {
    if (!std::isfinite(returnLoss)) return returnLoss > 0.0 ? 0.0 : 1.0;
    return std::pow(10.0, -returnLoss / 20.0);
}

bool AnalyzerInstrumentPlanner::correctOnePort(
    const std::array<std::complex<double>, 3> &standards,
    const std::array<std::complex<double>, 3> &measuredStandards,
    std::complex<double> measuredDevice, std::complex<double> &corrected) {
    using Complex = std::complex<double>;
    // Solve m = (a + b * gamma) / (1 + c * gamma) for the three measured standards.
    std::array<std::array<Complex, 4>, 3> rows;
    double scale = 1.0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i] = {1.0, standards[i], -measuredStandards[i] * standards[i], measuredStandards[i]};
        for (const auto value : rows[i]) {
            const double magnitude = std::abs(value);
            if (!std::isfinite(magnitude)) return false;
            scale = std::max(scale, magnitude);
        }
    }
    for (std::size_t column = 0; column < rows.size(); ++column) {
        auto pivot = std::max_element(rows.begin() + column, rows.end(), [column](const auto &a, const auto &b) {
            return std::abs(a[column]) < std::abs(b[column]);
        });
        const Complex divisor = (*pivot)[column];
        if (!(std::abs(divisor) > 1e-12 * scale)) return false;
        std::iter_swap(rows.begin() + column, pivot);
        for (std::size_t j = column; j < rows[column].size(); ++j) rows[column][j] /= divisor;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i == column) continue;
            const Complex factor = rows[i][column];
            for (std::size_t j = column; j < rows[i].size(); ++j) rows[i][j] -= factor * rows[column][j];
        }
    }
    const Complex tracking = rows[1][3];
    const Complex mismatch = measuredDevice * rows[2][3];
    const Complex denominator = tracking - mismatch;
    if (!std::isfinite(std::abs(denominator)) ||
        !(std::abs(denominator) > 1e-12 * std::max({1.0, std::abs(tracking), std::abs(mismatch)}))) return false;
    const Complex gamma = (measuredDevice - rows[0][3]) / denominator;
    if (!std::isfinite(std::abs(gamma))) return false;
    corrected = gamma;
    return true;
}
