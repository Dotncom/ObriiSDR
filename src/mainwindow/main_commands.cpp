#include "main.h"
#include "commanddispatcher.h"
#include "commandcontrolserver.h"
#include "commandconsoledialog.h"
#include "appruntimeutils.h"
#include "iqbuffer.h"
#include "tuningutils.h"
#include "frequencycontrol.h"

#include <QSignalBlocker>

QJsonObject YourClassName::commandStatus(bool metrics) const {
    QJsonObject reply{{"ok", true}, {"frontend", "gui"},
        {"runState", QString::fromLatin1(runStateName(runState))},
        {"running", runState == RadioRunState::Running},
        {"centerFrequencyHz", pendingSettings.centerFrequency},
        {"listeningFrequencyHz", pendingSettings.listeningFrequency},
        {"sampleRateHz", pendingSettings.sampleRate}, {"fftSize", pendingSettings.fftLength},
        {"binHz", pendingSettings.fftLength > 0 ? pendingSettings.sampleRate / pendingSettings.fftLength : 0},
        {"audioBandwidthHz", pendingSettings.bandwidth}, {"modulationType", pendingSettings.modulationType},
        {"lna", lnaGainSlider ? lnaGainSlider->value() : 0}, {"vga", vgaGainSlider ? vgaGainSlider->value() : 0},
        {"audioEnabled", pendingSettings.audioEnabled}, {"inputMode", pendingSettings.inputMode},
        {"sourceScope", "primary"}, {"settingsSource", "gui-configured-settings"}, {"hardwareVerified", false},
        {"spectrumFrameSerial", QString::number(commandSpectrumSerial)}};
    if (metrics) {
        const auto stats = IqBuffer::stats();
        reply.insert("iq", QJsonObject{{"epoch", QString::number(stats.epoch)},
            {"sequence", QString::number(stats.sequence)},
            {"complexSamples", QString::number(stats.totalFloatCount / 2)},
            {"queuedBlocks", static_cast<double>(stats.queuedBlocks)},
            {"queuedComplexSamples", static_cast<double>(stats.queuedFloatCount / 2)},
            {"droppedBlocks", QString::number(stats.droppedQueuedBlocks)},
            {"droppedComplexSamples", QString::number(stats.droppedQueuedFloats / 2)},
            {"skippedSnapshotBlocks", QString::number(stats.skippedSnapshotBlocks)},
            {"measuredSampleRateHz", stats.sampleRateEstimate}});
        reply.insert("callbacks", processor ? QString::number(processor->callbackCount()) : QStringLiteral("0"));
        if (commandControlServer) reply.insert("commandApi", commandControlServer->status());
    }
    return reply;
}

QJsonObject YourClassName::commandTrace(const QJsonObject &arguments) const {
    const auto fail = [](const QString &message) { return CommandDispatcher::failure("no_data", message); };
    if (arguments.value("source").toString(QStringLiteral("live")) != QLatin1String("live"))
        return CommandDispatcher::failure("unsupported_source", "GUI command trace currently exposes the primary live FFT only");
    const auto count = spectrumMagnitudeScratch.size();
    if (!commandSpectrumSerial || !count || spectrumFrequencyScratch.size() != count || commandSpectrumRateHz <= 0)
        return fail(QStringLiteral("No retained primary FFT; enable spectrum and wait for a frame"));
    if (commandSpectrumInput != INPUT_RF || agileScanRunning || standardScanRunning)
        return CommandDispatcher::failure("unsupported_geometry", "Command trace requires one primary complex RF span; not a scan mosaic or direct HF input");
    if (std::abs(commandSpectrumCenterHz - pendingSettings.centerFrequency) > 0.5 ||
        std::abs(commandSpectrumRateHz - pendingSettings.sampleRate) > 0.5)
        return fail(QStringLiteral("FFT geometry differs from current receiver settings; wait for a new frame"));
    const auto stats = IqBuffer::stats();
    if (commandSpectrumEpoch && stats.epoch && commandSpectrumEpoch != stats.epoch)
        return fail(QStringLiteral("FFT predates the current source epoch; wait for a new frame"));
    const int offset = arguments.value("offset").toInt(0);
    const int limit = arguments.value("limit").toInt(512);
    if (offset < 0 || static_cast<std::size_t>(offset) >= count)
        return CommandDispatcher::failure("invalid_arguments", "Trace offset is outside the retained FFT");
    const auto end = (std::min)(count, static_cast<std::size_t>(offset + limit));
    QJsonArray levels;
    const auto firstNegative = (count + 1) / 2;
    for (auto i = static_cast<std::size_t>(offset); i < end; ++i) {
        const float value = spectrumMagnitudeScratch[(i + firstNegative) % count];
        levels.append(std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null));
    }
    const double bin = commandSpectrumRateHz / count;
    return {{"ok", true}, {"source", "live"}, {"sourceScope", "primary"},
        {"frameSerial", QString::number(commandSpectrumSerial)}, {"epoch", QString::number(commandSpectrumEpoch)},
        {"startHz", commandSpectrumCenterHz - (count / 2) * bin + offset * bin}, {"binHz", bin},
        {"totalBins", static_cast<double>(count)}, {"offset", offset}, {"returnedBins", levels.size()},
        {"levelsDbfs", levels}, {"amplitudeCorrectionsApplied", commandSpectrumCorrected},
        {"absoluteCalibrationVerified", false}, {"frozen", runState != RadioRunState::Running},
        {"reduced", false}, {"newFftComputed", false}};
}

void YourClassName::ensureCommandDispatcher() {
    if (commandDispatcher) return;
    commandDispatcher = std::make_unique<CommandDispatcher>(QStringLiteral("gui"));
    commandControlServer = new CommandControlServer(commandDispatcher.get(), this);
    commandDispatcher->addStandard("status", [this](const QJsonObject &) { return commandStatus(false); });
    commandDispatcher->addStandard("metrics", [this](const QJsonObject &) { return commandStatus(true); });
    commandDispatcher->addStandard("capabilities", [this](const QJsonObject &) {
        return QJsonObject{{"ok", true}, {"frontend", "gui"}, {"headless", false}, {"rfTransmitExposed", false},
            {"traceSource", "retained-primary-rf-fft"}, {"traceMaximumPage", 1024},
            {"backgroundModel", false}, {"commandApiEnabled", commandControlServer->isListening()}};
    });
    commandDispatcher->addStandard("spectrum", [this](const QJsonObject &) {
        return QJsonObject{{"ok", true}, {"availableBins", static_cast<double>(spectrumMagnitudeScratch.size())},
            {"frameSerial", QString::number(commandSpectrumSerial)}, {"centerHz", commandSpectrumCenterHz},
            {"sampleRateHz", commandSpectrumRateHz}, {"newFftComputed", false}, {"sourceScope", "primary"}};
    });
    commandDispatcher->addStandard("trace", [this](const QJsonObject &a) { return commandTrace(a); });
    const auto mayControl = [this](bool allowTransition = false) -> QJsonObject {
        if (offlineIqPlaybackActive) return CommandDispatcher::failure("busy", "Stop offline playback before receiver control");
        if (!allowTransition && (runState == RadioRunState::Starting || runState == RadioRunState::Stopping))
            return CommandDispatcher::failure("busy", "Receiver transition is pending");
        if (isNetworkClientMode() && (!networkController || !networkController->clientHasControl()))
            return CommandDispatcher::failure("permission_denied", "This GUI is an observer of the remote receiver");
        return {};
    };
    for (const QString name : {QStringLiteral("start"), QStringLiteral("stop")})
        commandDispatcher->addStandard(name, [this, name, mayControl](const QJsonObject &) {
            const auto blocked = mayControl(name == QLatin1String("stop")); if (!blocked.isEmpty()) return blocked;
            if (name == QLatin1String("start") && runState == RadioRunState::Idle) {
                startFobosProcessing();
                if (runState == RadioRunState::Idle)
                    return CommandDispatcher::failure("start_failed", "Receiver did not start; inspect device selection and receiver status");
            }
            else if (name == QLatin1String("stop") && runState != RadioRunState::Idle && runState != RadioRunState::Stopping) stopFobosProcessing();
            auto reply = commandStatus(false);
            reply.insert("pending", runState == RadioRunState::Starting || runState == RadioRunState::Stopping);
            return reply;
        });
    for (const QString name : {QStringLiteral("tune"), QStringLiteral("listen")})
        commandDispatcher->addStandard(name, [this, name, mayControl](const QJsonObject &a) {
            const auto blocked = mayControl(); if (!blocked.isEmpty()) return blocked;
            const double hz = a.value("frequencyHz").toDouble();
            if (name == QLatin1String("tune")) {
                if (pendingSettings.inputMode != INPUT_RF || hz < rfMinimumCenterFrequency(pendingSettings) ||
                    hz > rfMaximumCenterFrequency(pendingSettings))
                    return CommandDispatcher::failure("out_of_range", "Center frequency is outside the selected receiver/input limits");
                centerReceiverAt(hz);
            } else {
                const double half = pendingSettings.bandwidth / 2;
                if (hz < (pendingSettings.inputMode == INPUT_RF ? rfMinimumListeningFrequency(pendingSettings) : 0.0) ||
                    std::abs(hz - pendingSettings.centerFrequency) + half > pendingSettings.sampleRate / 2)
                    return CommandDispatcher::failure("out_of_range", "Listening channel must fit in the current primary IQ band");
                updateTuningFromScale(hz, pendingSettings.centerFrequency);
            }
            auto reply = commandStatus(false);
            reply.insert("remotePending", isNetworkClientMode());
            return reply;
        });
    commandDispatcher->addStandard("demod", [this, mayControl](const QJsonObject &a) {
        const auto blocked = mayControl(); if (!blocked.isEmpty()) return blocked;
        const QMap<QString, int> modes{{"am", MOD_AM}, {"nfm", MOD_NFM}, {"wfm", MOD_WFM},
            {"usb", MOD_USB}, {"lsb", MOD_LSB}, {"cw", MOD_CW}};
        const int mode = modes.value(a.value("mode").toString(), -1);
        if (mode < 0) return CommandDispatcher::failure("invalid_arguments", "Unsupported analog mode");
        if (modulationButtonGroup && modulationButtonGroup->button(mode)) {
            const QSignalBlocker blocker(modulationButtonGroup);
            modulationButtonGroup->button(mode)->setChecked(true);
        }
        onModulationChanged(mode); return commandStatus(false);
    });
    commandDispatcher->addStandard("bandwidth", [this, mayControl](const QJsonObject &a) {
        const auto blocked = mayControl(); if (!blocked.isEmpty()) return blocked;
        const double hz = a.value("bandwidthHz").toDouble();
        if (!bandwidthControl || hz < bandwidthControl->minimumHz() || hz > bandwidthControl->maximumHz() ||
            std::abs(pendingSettings.listeningFrequency - pendingSettings.centerFrequency) + hz / 2 > pendingSettings.sampleRate / 2)
            return CommandDispatcher::failure("out_of_range", "Bandwidth is outside the current input/control range");
        { const QSignalBlocker blocker(bandwidthControl); bandwidthControl->setValueHz(hz); }
        onBandwidthChanged(); return commandStatus(false);
    });
    commandDispatcher->addStandard("audio", [this, mayControl](const QJsonObject &a) {
        const auto blocked = mayControl(); if (!blocked.isEmpty()) return blocked;
        if (!audioCheckbox) return CommandDispatcher::failure("unavailable", "Audio control is unavailable");
        const bool enabled = a.value("enabled").toBool();
        if (audioCheckbox->isChecked() != enabled) audioCheckbox->setChecked(enabled);
        return commandStatus(false);
    });
    commandDispatcher->addStandard("gain", [this, mayControl](const QJsonObject &a) {
        const auto blocked = mayControl(); if (!blocked.isEmpty()) return blocked;
        const int lna = a.value("lna").toInt(), vga = a.value("vga").toInt();
        if (!lnaGainSlider || !vgaGainSlider || !lnaGainSlider->isEnabled() || !vgaGainSlider->isEnabled() ||
            lna < lnaGainSlider->minimum() || lna > lnaGainSlider->maximum() ||
            vga < vgaGainSlider->minimum() || vga > vgaGainSlider->maximum())
            return CommandDispatcher::failure("out_of_range", "LNA/VGA indices are unavailable or outside this backend's controls");
        lnaGainSlider->setValue(lna); vgaGainSlider->setValue(vga);
        return commandStatus(false);
    });
}

void YourClassName::showCommandConsole() {
    ensureCommandDispatcher();
    if (!commandConsoleDialog) {
        commandConsoleDialog = new CommandConsoleDialog(commandDispatcher.get(),
            [this](const QString &key, const QString &fallback) { return uiText(key, fallback); }, this);
        commandConsoleDialog->setControlServer(commandControlServer);
        commandConsoleDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    commandConsoleDialog->show(); commandConsoleDialog->raise(); commandConsoleDialog->activateWindow();
}
