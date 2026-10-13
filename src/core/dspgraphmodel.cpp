#include "dspgraphmodel.h"
#include "radiosettings.h"
#include "audiofilterchain.h"

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
bool mainDisplaySettings(const QString &type) {
    return type == "spectrum_display" || type == "waterfall_2d" || type == "waterfall_3d";
}
bool mainDisplayMatch(const QString &settings, const QString &view) {
    return (settings == "spectrum_display" && view == "workspace_spectrum") ||
        ((settings == "waterfall_2d" || settings == "waterfall_3d") && view == "workspace_waterfall");
}
double finiteValue(const QJsonObject &s, const char *key, double fallback, double low, double high) {
    const double value = s.value(QLatin1String(key)).toDouble(fallback);
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
}

bool dspIsBranchType(const QString &type) {
    return type == "ddc_channelizer" || type == "branch_fft" ||
           type == "branch_spectrum" || type == "branch_waterfall" ||
           type == "pfb_channelizer" || type == "branch_demodulator" ||
           type == "branch_audio_filter" || type == "branch_audio_output";
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
    if ((source == "receiver_source" || source == "iq_source") && target == "fft") return DspLinkKind::Iq;
    if (source == "fft" && (target == "workspace_spectrum" || target == "workspace_waterfall" ||
                            mainDisplaySettings(target))) return DspLinkKind::Spectrum;
    if ((rawSource(source) && (target == "ddc_channelizer" || target == "pfb_channelizer")) ||
        (source == "pfb_channelizer" && target == "ddc_channelizer") ||
        (source == "ddc_channelizer" && (target == "branch_fft" || target == "branch_demodulator" || researchSettings(target)))) return DspLinkKind::Iq;
    if ((source == "branch_demodulator" || source == "branch_audio_filter") &&
        (target == "branch_audio_filter" || target == "branch_audio_output")) return DspLinkKind::Audio;
    if (source == "branch_fft" && (target == "branch_spectrum" || target == "branch_waterfall")) return DspLinkKind::Spectrum;
    if (dspIsBranchType(source) || dspIsBranchType(target)) return DspLinkKind::Invalid;
    return DspLinkKind::Documentation;
}

QString dspLinkDescription(DspLinkKind kind, bool uk) {
    switch (kind) {
    case DspLinkKind::Control: return uk ? QStringLiteral("Керування штатним трактом") : QStringLiteral("Controls the existing pipeline");
    case DspLinkKind::Settings: return uk ? QStringLiteral("Прив'язка налаштувань віджета") : QStringLiteral("Widget settings binding");
    case DspLinkKind::Vfo: return uk ? QStringLiteral("Відображення каналу спільного FFT") : QStringLiteral("Channel display from the shared FFT");
    case DspLinkKind::Iq: return uk ? QStringLiteral("Потік IQ") : QStringLiteral("IQ stream");
    case DspLinkKind::Spectrum: return uk ? QStringLiteral("Спектральні кадри") : QStringLiteral("Spectrum frames");
    case DspLinkKind::Audio: return uk ? QStringLiteral("Незалежний потік PCM 48 кГц") : QStringLiteral("Independent 48 kHz PCM stream");
    case DspLinkKind::Invalid: return uk ? QStringLiteral("Несумісні типи потоків") : QStringLiteral("Incompatible stream types");
    default: return uk ? QStringLiteral("Лише схема: цей зв'язок не передає сигнал і не змінює порядок DSP")
                      : QStringLiteral("Diagram only: this link does not carry samples or change DSP order");
    }
}

DspGraphPlan compileDspGraph(const QJsonObject &graph) {
    DspGraphPlan plan;
    QHash<QString, QJsonObject> blocks;
    QHash<QString, QString> inputs;
    QHash<QString, QString> displaySettingsInputs;
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
        if (kind == DspLinkKind::Settings && mainDisplaySettings(blocks[source].value("type").toString())) {
            if (displaySettingsInputs.contains(target)) plan.errors << "Multiple display settings inputs are not supported";
            else displaySettingsInputs.insert(target, source);
        }
        if (kind != DspLinkKind::Iq && kind != DspLinkKind::Spectrum && kind != DspLinkKind::Audio) continue;
        if (inputs.contains(target)) { plan.errors << "Multiple stream inputs are not supported"; continue; }
        inputs.insert(target, source);
    }
    for (auto it = inputs.cbegin(); it != inputs.cend(); ++it) {
        QSet<QString> seen;
        QString node = it.key();
        while (inputs.contains(node)) {
            if (seen.contains(node)) { plan.errors << "DSP stream cycle"; break; }
            seen.insert(node); node = inputs.value(node);
        }
    }
    // Main FFT bindings use the existing active source and optimized shared frame path.
    // Missing source links leave the explicit display route inactive, not implicit.
    for (auto it = inputs.cbegin(); it != inputs.cend(); ++it) {
        if (blocks[it.key()].value("type") == "fft") plan.mainFftSources.insert(it.key(), it.value());
    }
    for (auto it = blocks.cbegin(); it != blocks.cend(); ++it) {
        const auto type = it.value().value("type").toString();
        if (type != "workspace_spectrum" && type != "workspace_waterfall") continue;
        const auto settings = it.value().value("settings").toObject();
        QString controller = displaySettingsInputs.value(it.key());
        QStringList controllers;
        const QString binding = settings.value("bindingId").toString().trimmed();
        if (!binding.isEmpty()) {
            // Legacy 2D/3D controllers may share a binding. Preserve their settings
            // selection (last in scene stacking order); only conflicting FFT sources are ambiguous.
            controller.clear();
            for (const auto &value : graph.value("blocks").toArray()) {
                const auto candidate = value.toObject();
                if (!mainDisplayMatch(candidate.value("type").toString(), type) ||
                    candidate.value("settings").toObject().value("bindingId").toString().trimmed() != binding) continue;
                controller = candidate.value("id").toString();
                controllers.append(controller);
            }
        } else if (!controller.isEmpty()) controllers.append(controller);
        if (!controller.isEmpty()) plan.mainDisplayControllers.insert(it.key(), controller);
        const QString directFft = inputs.value(it.key());
        QString controllerFft;
        bool explicitRoute = settings.value("mainFftRouteExplicit").toBool(false) || !directFft.isEmpty();
        for (const auto &id : controllers) {
            const QString candidateFft = inputs.value(id);
            explicitRoute |= !candidateFft.isEmpty() ||
                blocks.value(id).value("settings").toObject().value("mainFftRouteExplicit").toBool(false);
            if (candidateFft.isEmpty()) continue;
            if (!controllerFft.isEmpty() && controllerFft != candidateFft)
                plan.errors << "Display binding has conflicting main FFT sources";
            controllerFft = candidateFft;
        }
        if (explicitRoute) plan.explicitMainSpectrumViews.insert(it.key());
        if (!directFft.isEmpty() && !controllerFft.isEmpty() && directFft != controllerFft)
            plan.errors << "Direct and settings-mediated main FFT routes disagree";
        const QString fft = directFft.isEmpty() ? controllerFft : directFft;
        if (plan.mainFftSources.contains(fft)) plan.mainSpectrumRoutes.insert(it.key(), fft);
    }
    // Compile only bounded typed branches; legacy main-path wires never replace the live receiver.
    QJsonArray execution;
    int activeBranches = 0;
    QStringList ids = blocks.keys();
    std::sort(ids.begin(), ids.end());
    for (const QString &id : ids) {
        const auto block = blocks[id];
        if (block.value("type") != "ddc_channelizer" || !inputs.contains(id)) continue;
        const auto s = block.value("settings").toObject();
        DspBranchConfig branch;
        branch.id = id;
        branch.enabled = s.value("enabled").toBool(false);
        // Retain inactive routes for the board without reserving worker resources.
        if (branch.enabled && ++activeBranches > DspMaxBranches) { plan.errors << "At most eight DDC branches can run"; continue; }
        branch.frequencyHz = finiteValue(s, "frequencyHz", 0.0, 0.0, 8.0e9);
        branch.bandwidthHz = finiteValue(s, "bandwidthHz", 12500.0, 100.0, 1.5e6);
        branch.outputRateHz = finiteValue(s, "outputRateHz", 192000.0, 1000.0, 2.0e6);
        branch.decimation = std::clamp(s.value("decimation").toInt(0), 0, 4096);
        branch.firTaps = s.value("firTaps").toInt(32);
        if (branch.firTaps != 16 && branch.firTaps != 32 && branch.firTaps != 64 &&
            branch.firTaps != 128 && branch.firTaps != 256) branch.firTaps = 32;
        branch.transitionHz = finiteValue(s, "transitionHz", 0.0, 0.0, 500000.0);
        const QString parent = inputs.value(id);
        branch.pfb = blocks.value(parent).value("type") == "pfb_channelizer";
        if (branch.pfb) {
            if (!inputs.contains(parent) || !rawSource(blocks.value(inputs.value(parent)).value("type").toString())) {
                plan.errors << "PFB requires a connected IQ source"; continue;
            }
            const auto pfb = blocks.value(parent).value("settings").toObject();
            int channels = pfb.value("channels").toInt(16);
            if (channels != 8 && channels != 16 && channels != 32 && channels != 64) channels = 16;
            const int taps = std::clamp(pfb.value("tapsPerPhase").toInt(16), 8, 64) & ~1;
            if (branch.enabled) {
                if (plan.pfbChannels && (plan.pfbChannels != channels || plan.pfbTaps != taps))
                    plan.errors << "All active PFB branches must share the same bank settings";
                plan.pfbChannels = channels; plan.pfbTaps = taps;
            }
        }
        branch.external = s.value("externalProcessing").toBool(false);
        for (const QString &sinkId : ids) {
            if (blocks[sinkId].value("type") != "branch_audio_output" || !inputs.contains(sinkId)) continue;
            QString node = inputs.value(sinkId);
            QStringList filterNodes;
            QSet<QString> seen;
            while (blocks.value(node).value("type") == "branch_audio_filter" && !seen.contains(node)) {
                seen.insert(node); filterNodes.prepend(node); node = inputs.value(node);
            }
            if (blocks.value(node).value("type") != "branch_demodulator" || inputs.value(node) != id) continue;
            if (!branch.audioOutputId.isEmpty()) { plan.errors << "One audio output per DDC branch is supported"; continue; }
            const auto demod = blocks[node].value("settings").toObject();
            const auto output = blocks[sinkId].value("settings").toObject();
            branch.demodulatorId = node; branch.audioOutputId = sinkId;
            branch.modulation = std::clamp(demod.value("modulation").toInt(MOD_NFM), int(MOD_AM), int(MOD_WFM));
            branch.audioBandwidthHz = finiteValue(demod, "bandwidthHz", 12500.0, 100.0, 250000.0);
            branch.audioLowPassHz = finiteValue(demod, "lowPassHz", 3000.0, 100.0, 20000.0);
            branch.squelchDb = finiteValue(demod, "squelchDb", -160.0, -200.0, 0.0);
            branch.audioEnabled = output.value("enabled").toBool(false);
            branch.audioDevice = std::clamp(output.value("device").toInt(-1), -1, 255);
            branch.volume = finiteValue(output, "volume", 0.5, 0.0, 2.0);
            QVector<AudioFilterStage> stages;
            for (const auto &filter : filterNodes) {
                bool ok = false;
                auto decoded = decodeAudioFilterChain(blocks[filter].value("settings").toObject().value("filters").toString(), &ok);
                if (!ok && !blocks[filter].value("settings").toObject().value("filters").toString().isEmpty())
                    plan.errors << "Invalid branch audio filter chain";
                stages += decoded; plan.audioRoutes.insert(filter, id);
            }
            branch.audioFilters = encodeAudioFilterChain(stages);
            if (stages.size() > 32) plan.errors << "At most 32 audio filters per branch are supported";
            plan.audioRoutes.insert(node, id); plan.audioRoutes.insert(sinkId, id);
        }
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
        // Consumer visibility and explicitly armed local sinks are resolved by the board.
        QJsonArray ffts;
        for (const auto &f : branch.ffts) ffts.append(QJsonObject{{"id", f.id}, {"length", f.length}, {"window", f.window}, {"refreshMs", f.refreshMs}});
        execution.append(QJsonObject{{"id", id}, {"source", inputs[id]}, {"frequency", branch.frequencyHz},
            {"bandwidth", branch.bandwidthHz}, {"rate", branch.outputRateHz}, {"decimation", branch.decimation},
            {"enabled", branch.enabled}, {"external", branch.external}, {"iqConsumer", branch.iqConsumer}, {"iqSamples", branch.iqSamples}, {"ffts", ffts},
            {"firTaps", branch.firTaps}, {"transitionHz", branch.transitionHz}, {"pfb", branch.pfb},
            {"audioId", branch.audioOutputId}, {"audioEnabled", branch.audioEnabled}, {"modulation", branch.modulation},
            {"audioBandwidth", branch.audioBandwidthHz}, {"audioLowPass", branch.audioLowPassHz},
            {"squelch", branch.squelchDb}, {"filters", branch.audioFilters}, {"device", branch.audioDevice}});
        plan.branches.append(branch);
    }
    if (!plan.errors.isEmpty()) {
        plan.branches.clear(); plan.spectrumRoutes.clear(); plan.researchRoutes.clear();
        plan.mainFftSources.clear(); plan.mainSpectrumRoutes.clear();
        plan.mainDisplayControllers.clear();
        plan.audioRoutes.clear(); plan.pfbChannels = 0;
        execution = QJsonArray();
    }
    plan.executionKey = QJsonDocument(QJsonObject{{"branches", execution}, {"pfbChannels", plan.pfbChannels},
        {"pfbTaps", plan.pfbTaps}}).toJson(QJsonDocument::Compact);
    return plan;
}

QStringList compileDspAudioFilterOrder(const QJsonObject &graph, QString *error) {
    if (error) error->clear();
    QHash<QString, QString> types, next;
    QHash<QString, int> incoming;
    for (const auto &v : graph.value("blocks").toArray()) {
        auto b = v.toObject(); types.insert(b.value("id").toString(), b.value("type").toString());
    }
    const auto relevant = [](const QString &t) { return t == "demodulator" || t == "audio_output" || t.startsWith("filter_"); };
    for (const auto &v : graph.value("connections").toArray()) {
        auto e = v.toObject(); const auto a = e.value("source").toString(), b = e.value("target").toString();
        if (!relevant(types.value(a)) || !relevant(types.value(b))) continue;
        if (next.contains(a) || ++incoming[b] > 1) {
            if (error) *error = "Ambiguous audio filter chain: branches or multiple inputs";
            return {};
        }
        next.insert(a, b);
    }
    QStringList result;
    int paths = 0;
    for (auto it = types.cbegin(); it != types.cend(); ++it) {
        if (it.value() != "demodulator" || !next.contains(it.key())) continue;
        QString node = next.value(it.key()); QSet<QString> seen;
        QStringList path;
        while (types.value(node).startsWith("filter_") && !seen.contains(node)) {
            seen.insert(node); path.append(node); node = next.value(node);
        }
        if (types.value(node) != "audio_output" || path.empty() || ++paths > 1) {
            if (error) *error = "Connect one complete Demodulator -> filters -> Audio output chain without cycles";
            return {};
        }
        result = path;
    }
    if (paths != 1 && error) *error = "No complete main audio filter path";
    return result;
}
