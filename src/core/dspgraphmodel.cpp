#include "dspgraphmodel.h"
#include "radiosettings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {
bool rawSource(const QString &type) {
    return type == "receiver_source" || type == "iq_source" || type == "network_input" || type == "playback";
}
bool researchSettings(const QString &type) {
    return type == "oscilloscope" || type == "constellation" || type == "eye_diagram" || type == "digital_sync_lab";
}
double finiteValue(const QJsonObject &s, const char *key, double fallback, double low, double high) {
    const double value = s.value(QLatin1String(key)).toDouble(fallback);
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
}

bool dspIsBranchType(const QString &type) {
    return type == "ddc_channelizer" || type == "branch_fft" ||
           type == "branch_spectrum" || type == "branch_waterfall";
}

DspLinkKind dspLinkKind(const QString &source, const QString &target) {
    if (source == "run_control" || source == "enable_control" || source == "frequency_control" || source == "fine_tune_control")
        return DspLinkKind::Control;
    if ((source == "spectrum_display" && target == "workspace_spectrum") ||
        ((source == "waterfall_2d" || source == "waterfall_3d") && target == "workspace_waterfall") ||
        (source == "oscilloscope" && target == "oscilloscope_view") ||
        (source == "constellation" && target == "constellation_view") ||
        (source == "eye_diagram" && target == "eye_diagram_view") ||
        (source == "digital_sync_lab" && target == "digital_sync_view")) return DspLinkKind::Settings;
    if (source == "vfo_channel" && (target == "vfo_spectrum" || target == "vfo_waterfall")) return DspLinkKind::Vfo;
    if ((rawSource(source) && target == "ddc_channelizer") ||
        (source == "ddc_channelizer" && (target == "branch_fft" || researchSettings(target)))) return DspLinkKind::Iq;
    if (source == "branch_fft" && (target == "branch_spectrum" || target == "branch_waterfall")) return DspLinkKind::Spectrum;
    if (dspIsBranchType(source) || dspIsBranchType(target)) return DspLinkKind::Invalid;
    return DspLinkKind::Documentation;
}

QString dspLinkDescription(DspLinkKind kind, bool uk) {
    switch (kind) {
    case DspLinkKind::Control: return uk ? QStringLiteral("Керування штатним трактом") : QStringLiteral("Controls the existing pipeline");
    case DspLinkKind::Settings: return uk ? QStringLiteral("Прив'язка налаштувань віджета") : QStringLiteral("Widget settings binding");
    case DspLinkKind::Vfo: return uk ? QStringLiteral("Відображення каналу спільного FFT") : QStringLiteral("Channel display from the shared FFT");
    case DspLinkKind::Iq: return uk ? QStringLiteral("Потік IQ додаткової гілки") : QStringLiteral("Additional branch IQ stream");
    case DspLinkKind::Spectrum: return uk ? QStringLiteral("Спектральні кадри додаткової гілки") : QStringLiteral("Additional branch spectrum frames");
    case DspLinkKind::Invalid: return uk ? QStringLiteral("Несумісні типи потоків") : QStringLiteral("Incompatible stream types");
    default: return uk ? QStringLiteral("Лише схема: цей зв'язок не передає сигнал і не змінює порядок DSP")
                      : QStringLiteral("Diagram only: this link does not carry samples or change DSP order");
    }
}

DspGraphPlan compileDspGraph(const QJsonObject &graph) {
    DspGraphPlan plan;
    QHash<QString, QJsonObject> blocks;
    QHash<QString, QString> inputs;
    for (const auto &v : graph.value("blocks").toArray()) {
        const auto block = v.toObject();
        const QString id = block.value("id").toString();
        if (id.isEmpty() || blocks.contains(id)) { plan.errors << "Invalid or duplicate block ID"; continue; }
        blocks.insert(id, block);
    }
    for (const auto &v : graph.value("connections").toArray()) {
        const auto edge = v.toObject();
        const QString source = edge.value("source").toString(), target = edge.value("target").toString();
        if (!blocks.contains(source) || !blocks.contains(target) || source == target) {
            plan.errors << "Invalid connection endpoint";
            continue;
        }
        const auto kind = dspLinkKind(blocks[source].value("type").toString(), blocks[target].value("type").toString());
        if (kind == DspLinkKind::Invalid) { plan.errors << "Incompatible stream connection"; continue; }
        if (kind != DspLinkKind::Iq && kind != DspLinkKind::Spectrum) continue;
        if (inputs.contains(target)) { plan.errors << "Multiple stream inputs are not supported"; continue; }
        inputs.insert(target, source);
    }
    // Only the explicit, acyclic Source -> DDC -> FFT/View grammar is executable.
    QJsonArray execution;
    QStringList ids = blocks.keys();
    std::sort(ids.begin(), ids.end());
    for (const QString &id : ids) {
        const auto block = blocks[id];
        if (block.value("type") != "ddc_channelizer" || !inputs.contains(id)) continue;
        if (plan.branches.size() >= 2) { plan.errors << "At most two DDC branches can run"; continue; }
        const auto s = block.value("settings").toObject();
        DspBranchConfig branch;
        branch.id = id;
        branch.frequencyHz = finiteValue(s, "frequencyHz", 0.0, 0.0, 8.0e9);
        branch.bandwidthHz = finiteValue(s, "bandwidthHz", 12500.0, 100.0, 1.5e6);
        branch.outputRateHz = finiteValue(s, "outputRateHz", 192000.0, 1000.0, 2.0e6);
        branch.decimation = std::clamp(s.value("decimation").toInt(0), 0, 4096);
        branch.enabled = s.value("enabled").toBool(false);
        for (const QString &consumer : ids) {
            if (inputs.value(consumer) != id) continue;
            const auto target = blocks[consumer];
            const QString type = target.value("type").toString();
            if (researchSettings(type)) {
                branch.iqConsumer = true;
                branch.iqSamples = std::max(branch.iqSamples, std::clamp(target.value("settings").toObject().value("samples").toInt(4096), 256, 65536));
                plan.researchRoutes.insert(consumer, id);
            } else if (type == "branch_fft") {
                if (branch.ffts.size() >= 2) { plan.errors << "At most two FFTs per DDC can run"; continue; }
                DspBranchFftConfig fft;
                fft.id = consumer;
                const auto f = target.value("settings").toObject();
                fft.length = std::clamp(f.value("fftLength").toInt(2048), 512, 65536);
                // Keep branch plans bounded and use predictable radix-2 FFTs.
                int power = 512;
                while (power < fft.length) power *= 2;
                fft.length = power;
                fft.window = normalizedFftWindowType(f.value("window").toInt(FFT_WINDOW_HANN));
                fft.refreshMs = std::clamp(f.value("refreshMs").toInt(100), 20, 2000);
                bool hasDisplay = false;
                for (const QString &view : ids) {
                    if (inputs.value(view) == consumer) {
                        plan.spectrumRoutes.insert(view, consumer);
                        hasDisplay = true;
                    }
                }
                if (hasDisplay) branch.ffts.append(fft);
            }
        }
        if (!branch.iqConsumer && branch.ffts.isEmpty()) continue;
        QJsonArray ffts;
        for (const auto &f : branch.ffts) ffts.append(QJsonObject{{"id", f.id}, {"length", f.length}, {"window", f.window}, {"refreshMs", f.refreshMs}});
        execution.append(QJsonObject{{"id", id}, {"source", inputs[id]}, {"frequency", branch.frequencyHz},
            {"bandwidth", branch.bandwidthHz}, {"rate", branch.outputRateHz}, {"decimation", branch.decimation},
            {"enabled", branch.enabled}, {"iqConsumer", branch.iqConsumer}, {"iqSamples", branch.iqSamples}, {"ffts", ffts}});
        plan.branches.append(branch);
    }
    if (!plan.errors.isEmpty()) {
        plan.branches.clear(); plan.spectrumRoutes.clear(); plan.researchRoutes.clear();
        execution = QJsonArray();
    }
    plan.executionKey = QJsonDocument(execution).toJson(QJsonDocument::Compact);
    return plan;
}
