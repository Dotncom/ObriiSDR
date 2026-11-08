#include "main.h"
#include "appsettingsutils.h"
#include "dspflowpanel.h"
#include "dspgraphmodel.h"
#include "experimentbundle.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QJsonValue finiteJson(double value) {
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

double powerDb(float power) {
    return power > 0.0f && std::isfinite(power)
               ? 10.0 * std::log10(power)
               : std::numeric_limits<double>::quiet_NaN();
}

QJsonObject calibrationConfiguration(const ReceiverNoiseBaselineKey &key) {
    return QJsonObject{
        {"backendId", key.receiver.backendId},
        {"inputMode", key.receiver.inputMode},
        {"sampleRateHz", key.receiver.sampleRateHz},
        {"lnaGain", key.receiver.lnaGain},
        {"vgaGain", key.receiver.vgaGain},
        {"rtlGainTenthsDb", key.receiver.rtlGainTenthsDb},
        {"gainState", key.receiver.gainState},
        {"centerFrequencyHz", key.centerFrequencyHz},
        {"fftLength", key.fftLength},
        {"fftWindowType", key.fftWindowType},
        {"rfBandwidthHz", key.rfBandwidthHz},
        {"amplitudeOffsetDb", key.amplitudeOffsetDb},
        {"calibrationTableEnabled", key.calibrationTableEnabled},
        {"calibrationPointCount", key.calibrationPointCount}};
}

QJsonObject baselineExport(const ReceiverNoiseBaselineProfile &profile, bool includeTable) {
    if (!profile.isValid()) return {};
    QJsonObject object{
        {"configuration", calibrationConfiguration(profile.key)},
        {"minimumFrequencyHz", profile.minimumFrequencyHz},
        {"maximumFrequencyHz", profile.maximumFrequencyHz},
        {"capturedFrames", profile.capturedFrames},
        {"spreadDb", profile.spreadDb},
        {"capturedUtcMs", double(profile.capturedUtcMs)}};
    if (includeTable) {
        QJsonArray table;
        const int count = profile.averagePower.size();
        for (int index = 0; index < count; ++index) {
            const double frequencyHz = profile.minimumFrequencyHz +
                (profile.maximumFrequencyHz - profile.minimumFrequencyHz) * index /
                    (std::max)(1, count - 1);
            table.append(QJsonArray{frequencyHz, finiteJson(powerDb(profile.averagePower[index]))});
        }
        object["table"] = table;
    }
    return object;
}

QJsonObject yFactorExport(const ReceiverYFactorProfile &profile, bool includeTable) {
    if (!profile.isValid()) return {};
    QJsonObject object{
        {"configuration", calibrationConfiguration(profile.key)},
        {"sourceMode", profile.sourceMode == YFactorSourceMode::Enr ? "enr" : "temperatures"},
        {"coldTemperatureKelvin", profile.coldTemperatureKelvin},
        {"hotTemperatureKelvin", profile.hotTemperatureKelvin},
        {"enrDb", profile.enrDb},
        {"minimumFrequencyHz", profile.minimumFrequencyHz},
        {"maximumFrequencyHz", profile.maximumFrequencyHz},
        {"coldFrames", profile.coldFrames},
        {"hotFrames", profile.hotFrames},
        {"validBins", profile.validBins},
        {"capturedUtcMs", double(profile.capturedUtcMs)}};
    if (includeTable) {
        QJsonArray table;
        const int count = profile.coldPower.size();
        for (int index = 0; index < count; ++index) {
            const double frequencyHz = profile.minimumFrequencyHz +
                (profile.maximumFrequencyHz - profile.minimumFrequencyHz) * index /
                    (std::max)(1, count - 1);
            const double coldDb = powerDb(profile.coldPower[index]);
            const double hotDb = powerDb(profile.hotPower[index]);
            table.append(QJsonArray{
                frequencyHz, finiteJson(coldDb), finiteJson(hotDb), finiteJson(hotDb - coldDb),
                finiteJson(profile.equivalentNoiseTemperatureKelvin[index]),
                finiteJson(profile.noiseFigureDb[index])});
        }
        object["table"] = table;
    }
    return object;
}
}

extern float sensitivity;
extern float contrast;
extern bool colorf;

QJsonObject YourClassName::experimentBundleContext(bool includeCalibrationTables) const {
    QJsonObject context;
    context["appVersion"] = QCoreApplication::applicationVersion();
    context["capturedAtUtc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    context["settings"] = settingsToJson();
    context["display"] = QJsonObject{
        {"levelMin", displayLevelMin}, {"levelMax", displayLevelMax},
        {"contrast", contrast}, {"sensitivity", sensitivity}, {"coloredSpectrum", colorf},
        {"waterfallDisplayMode", waterfallDisplayMode}, {"waterfallRowsPerFrame", waterfallRowsPerFrame},
        {"waterfall3DResolutionDivisor", waterfall3DResolutionDivisor}};
    // Reading the saved layout must not instantiate the board or any DSP worker.
    const QString layout = dspFlowPanel ? dspFlowPanel->configurationJson()
        : QSettings(persistentSettingsFilePath(), QSettings::IniFormat)
            .value(QStringLiteral("dspFlow/configuration")).toString();
    context["workspace"] = QJsonDocument::fromJson(layout.toUtf8()).object();
    const ReceiverNoiseBaselineKey calibrationKey =
        receiverNoiseBaselineKey(spectrumProcessingSettings());
    QJsonObject receiverCalibration;
    const QJsonObject baseline = baselineExport(
        receiverNoiseBaseline.matchingProfile(calibrationKey), includeCalibrationTables);
    const QJsonObject yFactor = yFactorExport(
        receiverYFactorCalibration.matchingProfile(calibrationKey), includeCalibrationTables);
    if (!baseline.isEmpty()) receiverCalibration["noiseBaseline"] = baseline;
    if (!yFactor.isEmpty()) receiverCalibration["yFactor"] = yFactor;
    if (!receiverCalibration.isEmpty()) context["receiverCalibration"] = receiverCalibration;
    return ExperimentBundle::sanitizeContext(context);
}

bool YourClassName::applyExperimentWorkspace(const QJsonObject &context, QString *error) {
    const auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    if (isRunningOrTransitioning())
        return fail(uiText("experiment_stop_rx", "Stop reception before restoring the DSP workspace."));
    const QJsonObject workspace = ExperimentBundle::sanitizeContext(context).value("workspace").toObject();
    if (workspace.isEmpty() || !workspace.value("blocks").isArray() ||
        !workspace.value("connections").isArray() || workspace.value("blocks").toArray().size() > 512)
        return fail(uiText("experiment_no_workspace", "The experiment has no supported DSP workspace."));
    const auto plan = compileDspGraph(workspace);
    if (!plan.errors.isEmpty()) return fail(plan.errors.join(QStringLiteral("; ")));
    ensureDspFlowPanel();
    const auto json = QString::fromUtf8(QJsonDocument(workspace).toJson(QJsonDocument::Compact));
    if (!dspFlowPanel->importExperimentLayout(json, error)) return false;
    refreshDspControlStates();
    dspFlowPanel->show();
    dspFlowPanel->raise();
    return true;
}
