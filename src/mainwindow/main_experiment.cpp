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

extern float sensitivity;
extern float contrast;
extern bool colorf;

QJsonObject YourClassName::experimentBundleContext() const {
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
