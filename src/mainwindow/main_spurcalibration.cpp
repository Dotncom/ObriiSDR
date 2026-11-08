#include "main.h"
#include "tuningutils.h"
#include "backends/fobosfrequencylimits.h"
#include "iqbuffer.h"
#include "core/audiolifecycle.h"
#include "core/fobosifselection.h"
#include "core/fobosruntimeif.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

QJsonObject YourClassName::spurCalibrationContext() const {
    auto context = receiverCalibrationContext(pendingSettings).toJson();
    const auto device = selectedFobosDeviceInfo();
    context.insert("serial", device.serial);
    context.insert("hardware", device.hardwareRevision);
    context.insert("firmware", device.firmwareVersion);
    context.insert("correctedIf", useCorrectedFobosCommands);
    if (activeFobosApiKind == FobosApiKind::Agile && fobosActiveMixerBiasCode > 1)
        context.insert("mixerBiasProfile", static_cast<int>(fobosActiveMixerBiasCode));
    if (fobosIfTestMode == 2)
        context.insert("globalIfMode", 2);
    if (fobosIfTestMode == 3)
        context.insert("globalIfPolicy", static_cast<int>(FobosRuntimeIf::PolicyVersion));
    if (fobosIfTestMode == 1)
        context.insert("globalIfMode", 1);
    if (fobosIfTestMode == 3 && fobosCustomIfRulesActive) {
        context.insert("globalIfCustomRulesActive", true);
        context.insert("globalIfCustomRulesCrc", static_cast<double>(FobosRuntimeIf::rulesCrc(fobosActiveIfRulesFrame)));
        context.insert("globalIfCustomRules", FobosRuntimeIf::frameToJson(fobosActiveIfRulesFrame));
    }

    context.insert("fftLength", pendingSettings.fftLength);
    context.insert("clock", pendingSettings.clockSource);
    context.insert("gpo", pendingSettings.gpoValue);
    context.insert("frequencyCorrectionHz", frequencyCalibrationOffsetHz);
    return context;
}

void YourClassName::startSpurCalibration() {
    if (spurSweepDialog) { spurSweepDialog->raise(); return; }
    const bool uk = uiLanguage == QStringLiteral("uk");
    const QString title = uk ? QStringLiteral("РљР°Р»С–Р±СЂСѓРІР°РЅРЅСЏ СЃРїСѓСЂС–РІ") : QStringLiteral("Spur calibration");
    const bool busy = (!isIdle() && runState != RadioRunState::Running) ||
        pendingSettings.inputMode != INPUT_RF || !hasActiveFobosDevice() ||
        isNetworkClientMode() || networkMode != NetworkMode::Disabled ||
        offlineIqPlaybackActive || (playbackManager && playbackManager->isPlaying()) ||
        (recordingManager && recordingManager->isRecording()) || spectrumFrameRecorder.isRecording() ||
        spectrumIqEventRecorder.isRecording() || agileScanEnabled || standardScanEnabled ||
        listeningScanEnabled || fpvAutoScanArmed || fpvHunterFollowEnabled ||
        receiverNoiseBaseline.isCapturing() || receiverYFactorCalibration.isCapturing();
    if (busy) {
        QMessageBox::information(QApplication::activeWindow(), title, uk
            ? QStringLiteral("РџРѕС‚СЂС–Р±РµРЅ Р»РѕРєР°Р»СЊРЅРёР№ Fobos Сѓ СЂРµР¶РёРјС– RF. Р—СѓРїРёРЅС–С‚СЊ Р·Р°РїРёСЃРё, РІС–РґС‚РІРѕСЂРµРЅРЅСЏ, РјРµСЂРµР¶РµРІРёР№ СЂРµР¶РёРј, СЃРєР°РЅРµСЂРё С‚Р° Р°РІС‚РѕРјР°С‚РёС‡РЅРµ СЃС‚РµР¶РµРЅРЅСЏ. РџСЂРёР№РјР°РЅРЅСЏ РјРѕР¶РЅР° Р·Р°Р»РёС€РёС‚Рё СѓРІС–РјРєРЅРµРЅРёРј.")
            : QStringLiteral("Use a local Fobos in RF mode. Stop recording, playback, network mode, scanners and automatic following. Reception may remain running."));
        return;
    }
    const auto device = selectedFobosDeviceInfo();
    double maximum = rfMaximumCenterFrequency(pendingSettings);
    if (activeFobosApiKind == FobosApiKind::Agile && fobosAgileHasKnown6900MhzLimit(
            device.hardwareRevision.toLatin1().constData(), device.firmwareVersion.toLatin1().constData()))
        maximum = (std::min)(maximum, FOBOS_AGILE_LEGACY_MAX_RF_HZ);
    auto centers = SpurCalibrationProfile::sweepCenters(50e6, maximum, pendingSettings.sampleRate);
    // Also measure the user's exact center; a coarse RF grid alone can miss
    // center-dependent spurs when returning from the sweep.
    if (pendingSettings.centerFrequency >= 50e6 && pendingSettings.centerFrequency <= maximum) {
        const auto pos = std::lower_bound(centers.begin(), centers.end(), pendingSettings.centerFrequency);
        if (pos == centers.end() || std::abs(*pos - pendingSettings.centerFrequency) > 1.0)
            centers.insert(pos, pendingSettings.centerFrequency);
    }
    if (fobosIfTestMode == 3 &&
        fobosIfAutoRateEligible(pendingSettings.sampleRate)) {
        // A coarse Fs/2 sweep can miss a narrow automatic-IF interval entirely.
        // Always sample its interior and the native plan on both sides.
        const auto addCenter = [&](double frequency) {
            if (frequency < 50e6 || frequency > maximum) return;
            const auto pos = std::lower_bound(centers.begin(), centers.end(), frequency);
            if (pos == centers.end() || std::abs(*pos - frequency) > 1.0) centers.insert(pos, frequency);
        };
        const auto activeRulesFrame = FobosRuntimeIf::validFrame(fobosActiveIfRulesFrame)
            ? fobosActiveIfRulesFrame : FobosRuntimeIf::builtinFrame();
        for (const auto &rule : FobosRuntimeIf::rules(activeRulesFrame)) {
            addCenter(rule.fromHz - 1000.0);
            addCenter((static_cast<double>(rule.fromHz) + rule.toHz) * 0.5);
            addCenter(rule.toHz + 1000.0);
        }
    }
    if (centers.isEmpty()) return;

    QDialog dialog(QApplication::activeWindow());
    dialog.setWindowTitle(title);
    dialog.setObjectName(QStringLiteral("spurCalibrationDialog"));
    dialog.setMinimumWidth(440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(uk
        ? QStringLiteral("Р’С–Рґ'С”РґРЅР°Р№С‚Рµ Р°РЅС‚РµРЅСѓ РІС–Рґ RF-РІС…РѕРґСѓ С‚Р° РїС–РґРєР»СЋС‡С–С‚СЊ РЅР°РІР°РЅС‚Р°Р¶РµРЅРЅСЏ 50 РћРј. РљР— С‚Р°РєРѕР¶ РјРѕР¶РЅР° РІРёРєРѕСЂРёСЃС‚Р°С‚Рё, Р°Р»Рµ РІРѕРЅРѕ РЅРµ С” СѓР·РіРѕРґР¶РµРЅРёРј РЅР°РІР°РЅС‚Р°Р¶РµРЅРЅСЏРј С– РјРѕР¶Рµ РґР°С‚Рё С–РЅС€РёР№ РїСЂРѕС„С–Р»СЊ.\n\nРџСЂРѕРіСЂР°РјР° РїРµСЂРµСЃС‚СЂРѕС—С‚СЊ РїСЂРёР№РјР°С‡ РїРѕ РІСЃСЊРѕРјСѓ RF-РґС–Р°РїР°Р·РѕРЅСѓ. РџС–СЃР»СЏ Р·Р°РІРµСЂС€РµРЅРЅСЏ Р°Р±Рѕ СЃРєР°СЃСѓРІР°РЅРЅСЏ РїРѕРІРµСЂРЅРµС‚СЊСЃСЏ РїРѕРїРµСЂРµРґРЅСЏ С‡Р°СЃС‚РѕС‚Р°. РџС–РґСЃРёР»РµРЅРЅСЏ РїС–Рґ С‡Р°СЃ РїСЂРѕС…РѕРґСѓ РЅРµ Р·РјС–РЅСЋС”С‚СЊСЃСЏ.")
        : QStringLiteral("Disconnect the antenna from the RF input and connect a 50-ohm termination. A short may also be used, but it is not a matched termination and may produce a different profile.\n\nThe receiver will sweep the full RF range. The previous frequency is restored on completion or cancellation. Gain stays fixed during the sweep."), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *summary = new QLabel((uk
        ? QStringLiteral("50вЂ“%1 РњР“С†; РєСЂРѕРє %2 РњР“С†; %3 С‚РѕС‡РѕРє; Fs %4 MS/s; FFT %5; LNA %6, VGA %7.\nР—РјС–РЅР° РїС–РґСЃРёР»РµРЅРЅСЏ, FFT Р°Р±Рѕ РІС–РєРЅР° РЅРµ РІРёРјРёРєР°С” РјР°СЃРєСѓ. Р†РЅС€Р° Fs С‡Рё РџР§ РїРѕС‚СЂРµР±СѓС” С–РЅС€РѕС— РєР°Р»С–Р±СЂРѕРІРєРё. Р’СѓР·СЊРєР° РјР°СЃРєР° РЅРµ РїСЂРёРіР»СѓС€СѓС” С€РёСЂРѕРєС– VGA-РіРѕСЂР±РёРєРё. РћРєСЂРµРјРёР№ С€РёСЂРѕРєРёР№ РїСЂРѕС„С–Р»СЊ С‚Р°РєРѕР¶ Р·Р°РїРёСЃСѓС”С‚СЊСЃСЏ РґР»СЏ С†СЊРѕРіРѕ РїС–РґСЃРёР»РµРЅРЅСЏ.")
        : QStringLiteral("50вЂ“%1 MHz; %2 MHz step; %3 points; Fs %4 MS/s; FFT %5; LNA %6, VGA %7.\nGain, FFT and window changes do not disable the mask. Different Fs or IF needs another calibration. The narrow mask leaves broad VGA humps alone. A separate broad profile is also captured for this gain."))
        .arg(maximum / 1e6, 0, 'f', 1).arg(pendingSettings.sampleRate / 2e6, 0, 'f', 2)
        .arg(centers.size()).arg(pendingSettings.sampleRate / 1e6, 0, 'f', 2).arg(pendingSettings.fftLength)
        .arg(pendingSettings.lnaGain).arg(pendingSettings.vgaGain), &dialog);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    const QString fullSummary = summary->text();
    const auto fullCenters = centers;
    auto *scope = new QComboBox(&dialog);
    scope->addItem(uk ? QStringLiteral("РЈРІРµСЃСЊ RF-РґС–Р°РїР°Р·РѕРЅ") : QStringLiteral("Full RF range"));
    scope->addItem(uk ? QStringLiteral("РџРѕС‚РѕС‡РЅРёР№ С†РµРЅС‚СЂ вЂ” РґРѕРїРѕРІРЅРёС‚Рё РїСЂРѕС„С–Р»СЊ") : QStringLiteral("Current center вЂ” update profile"));
    layout->addWidget(scope);
    auto *confirmed = new QCheckBox(uk ? QStringLiteral("РђРЅС‚РµРЅСѓ РІС–Рґ'С”РґРЅР°РЅРѕ, РЅР°РІР°РЅС‚Р°Р¶РµРЅРЅСЏ РїС–РґРєР»СЋС‡РµРЅРµ")
        : QStringLiteral("Antenna disconnected, termination connected"), &dialog);
    confirmed->setObjectName(QStringLiteral("spurLoadConfirmed"));
    layout->addWidget(confirmed);
    auto *frames = new QSpinBox(&dialog);
    frames->setRange(8, 64);
    frames->setValue(12);
    frames->setPrefix(uk ? QStringLiteral("РљР°РґСЂС–РІ РЅР° С‚РѕС‡РєСѓ: ") : QStringLiteral("Frames per point: "));
    layout->addWidget(frames);
    auto *progress = new QProgressBar(&dialog);
    progress->setRange(0, centers.size());
    layout->addWidget(progress);
    auto *status = new QLabel(uk ? QStringLiteral("РћС‡С–РєСѓРІР°РЅРЅСЏ РїС–РґС‚РІРµСЂРґР¶РµРЅРЅСЏ РЅР°РІР°РЅС‚Р°Р¶РµРЅРЅСЏ")
        : QStringLiteral("Waiting for termination confirmation"), &dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *start = buttons->addButton(uk ? QStringLiteral("РџРѕС‡Р°С‚Рё РєР°Р»С–Р±СЂСѓРІР°РЅРЅСЏ") : QStringLiteral("Start calibration"), QDialogButtonBox::ActionRole);
    auto *cancel = buttons->addButton(uk ? QStringLiteral("РЎРєР°СЃСѓРІР°С‚Рё РїСЂРѕС…С–Рґ") : QStringLiteral("Cancel sweep"), QDialogButtonBox::ActionRole);
    cancel->setEnabled(false);
    start->setEnabled(false);
    layout->addWidget(buttons);
    spurSweepDialog = &dialog;
    spurSweepLabel = status;
    spurSweepProgress = progress;
    QTimer poll(&dialog);
    poll.setInterval(150);
    connect(scope, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&, this](int index) {
        centers = index == 0 ? fullCenters : QVector<double>{pendingSettings.centerFrequency};
        summary->setText(index == 0 ? fullSummary : (uk
            ? QStringLiteral("РљР°Р»С–Р±СЂРѕРІРєР° РѕРґРЅС–С”С— С‡Р°СЃС‚РѕС‚Рё Р±РµР· РїРѕРІРЅРѕРіРѕ РїСЂРѕС…РѕРґСѓ. РЎСѓРјС–СЃРЅС– С‚РѕС‡РєРё РІР¶Рµ Р·Р°РїРёСЃР°РЅРѕРіРѕ РїСЂРѕС„С–Р»СЋ Р·Р±РµСЂРµР¶СѓС‚СЊСЃСЏ. Fs С– СЂРµР¶РёРј РџР§ РјР°СЋС‚СЊ РІС–РґРїРѕРІС–РґР°С‚Рё РїСЂРѕС„С–Р»СЋ; РїС–РґСЃРёР»РµРЅРЅСЏ С‚Р° FFT РјРѕР¶СѓС‚СЊ РІС–РґСЂС–Р·РЅСЏС‚РёСЃСЏ. Р’СѓР·СЊРєР° РјР°СЃРєР° РЅРµ РїСЂРёРіР»СѓС€СѓС” С€РёСЂРѕРєС– VGA-РіРѕСЂР±РёРєРё. РћРєСЂРµРјРёР№ С€РёСЂРѕРєРёР№ РїСЂРѕС„С–Р»СЊ С‚Р°РєРѕР¶ Р·Р°РїРёСЃСѓС”С‚СЊСЃСЏ РґР»СЏ С†СЊРѕРіРѕ РїС–РґСЃРёР»РµРЅРЅСЏ.")
            : QStringLiteral("Calibrate one center without a full sweep. Existing compatible profile points are preserved. Fs and IF must match; gain and FFT may differ. The narrow mask leaves broad VGA humps alone. A separate broad profile is also captured for this gain.")));
        progress->setRange(0, centers.size());
        status->setText(index == 0 ? (uk ? QStringLiteral("РџРѕРІРЅРёР№ РїСЂРѕС…С–Рґ: %1 С‚РѕС‡РѕРє").arg(centers.size())
            : QStringLiteral("Full sweep: %1 points").arg(centers.size()))
            : (uk ? QStringLiteral("РћРґРЅР° С‚РѕС‡РєР°: %1 РњР“С†. Р РµС€С‚Р° СЃСѓРјС–СЃРЅРѕРіРѕ РїСЂРѕС„С–Р»СЋ Р·Р±РµСЂРµР¶РµС‚СЊСЃСЏ.").arg(pendingSettings.centerFrequency / 1e6, 0, 'f', 3)
                  : QStringLiteral("One point: %1 MHz. Other compatible profile points are preserved.").arg(pendingSettings.centerFrequency / 1e6, 0, 'f', 3)));
    });
    connect(confirmed, &QCheckBox::toggled, start, &QPushButton::setEnabled);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(cancel, &QPushButton::clicked, &dialog, [this, uk] {
        endSpurCalibration(uk ? QStringLiteral("РЎРєР°СЃРѕРІР°РЅРѕ. РџРѕРїРµСЂРµРґРЅС–Р№ РїСЂРѕС„С–Р»СЊ Р·Р±РµСЂРµР¶РµРЅРѕ.")
            : QStringLiteral("Cancelled. Previous profile preserved."));
    });
    connect(start, &QPushButton::clicked, &dialog, [&, this] {
        if (!confirmed->isChecked() || spurSweepActive) return;
        start->setEnabled(false);
        confirmed->setEnabled(false);
        frames->setEnabled(false);
        scope->setEnabled(false);
        cancel->setEnabled(true);
        spurSettingsBeforeSweep = pendingSettings;
        spurWasRunning = runState == RadioRunState::Running;
        spurAudioWasEnabled = audioCheckbox && audioCheckbox->isChecked();
        spurSweepActive = true;
        spurSweepError.clear();
        spurSweepCenters = centers;
        spurSweepIndex = -1;
        spurSweepProfile = {};
        spurSweepProfile.stepHz = pendingSettings.sampleRate * 0.5;
        spurCalibrationTargetFrames = frames->value();
        spurSweepContext = spurCalibrationContext();
        spurSweepProfile.context = spurSweepContext;
        spurSweepNoiseBaseline = receiverNoiseBaseline;
        spurPointClock.start();
        pendingAudioStartAfterStreamReady = false;
        if (audioCheckbox) {
            const QSignalBlocker blocker(audioCheckbox);
            audioCheckbox->setChecked(false);
        }
        pendingSettings.audioEnabled = false;
        publishSettingsToGlobals();
        QElapsedTimer pauseClock; pauseClock.start();
        // Keep the prepared output for a running receiver; calibration does
        // not require closing/reopening the Windows audio device.
        if (audioProcessor) audioProcessor->stopDemodulationForStreamRestart();
        qInfo() << "[Spur] audio paused for calibration" << "elapsedMs" << pauseClock.elapsed()
                << "receiverWasRunning" << spurWasRunning;
        if (!spurWasRunning) startFobosProcessing();
        poll.start();
    });
    connect(&poll, &QTimer::timeout, &dialog, [&, this] {
        if (!spurSweepActive) {
            poll.stop(); cancel->setEnabled(false);
            // One pass per wizard; reopening recomputes capabilities and conditions.
            return;
        }
        if (spurCalibrationContext() != spurSweepContext) {
            endSpurCalibration(uk ? QStringLiteral("РЈРјРѕРІРё РїСЂРёР№РѕРјСѓ Р·РјС–РЅРёР»РёСЃСЏ. РџСЂРѕС„С–Р»СЊ РЅРµ Р·Р°РјС–РЅРµРЅРѕ.")
                : QStringLiteral("Reception settings changed. Profile not replaced.")); return;
        }
        const qint64 timeout = (std::max)(qint64(15000), qint64(spurCalibrationTargetFrames) *
            (updateTimer ? updateTimer->interval() : 1000) * 8 + 5000);
        if (!hasActiveFobosDevice() || (spurSweepIndex >= 0 && runState != RadioRunState::Running) ||
            spurPointClock.elapsed() > timeout) {
            endSpurCalibration(uk ? QStringLiteral("РќРµРјР°С” СЃРІС–Р¶РёС… РґР°РЅРёС… РІС–Рґ РїСЂРёР№РјР°С‡Р°. РџСЂРѕС„С–Р»СЊ РЅРµ Р·Р°РјС–РЅРµРЅРѕ.")
                : QStringLiteral("No fresh receiver data. Profile not replaced.")); return;
        }
        if (spurSweepIndex < 0 && runState == RadioRunState::Running) advanceSpurCalibration();
        if (spurSweepActive && spurSweepIndex >= 0) {
            status->setText((uk ? QStringLiteral("РўРѕС‡РєР° %1/%2: %3 РњР“С†; РєР°РґСЂРё %4/%5")
                : QStringLiteral("Point %1/%2: %3 MHz; frames %4/%5"))
                .arg(spurSweepIndex + 1).arg(centers.size())
                .arg(centers.at(spurSweepIndex) / 1e6, 0, 'f', 3)
                .arg(spurCalibrationFramesDone).arg(spurCalibrationTargetFrames));
            progress->setValue(spurSweepProfile.points.size());
        }
    });
    dialog.exec();
    poll.stop();
    if (spurSweepActive) endSpurCalibration(uk ? QStringLiteral("РЎРєР°СЃРѕРІР°РЅРѕ") : QStringLiteral("Cancelled"));
    spurSweepDialog.clear(); spurSweepLabel.clear(); spurSweepProgress.clear();
}

void YourClassName::advanceSpurCalibration() {
    if (!spurSweepActive) return;
    if (++spurSweepIndex >= spurSweepCenters.size()) {
        endSpurCalibration(); return;
    }
    if (!processor || runState != RadioRunState::Running || !hasActiveFobosDevice()) {
        endSpurCalibration(QStringLiteral("Receiver stopped; previous profile preserved.")); return;
    }
    spurCalibrationActive = false;
    spurCalibrationBins.clear();
    spurLineCalibrator.reset();
    spurCalibrationFramesDone = 0;
    spurCalibrationBinHz = 0;
    ++liveCenterRetuneGeneration;
    queuedLiveCenterRetuneGeneration = 0;
    pendingSettings.centerFrequency = spurSweepCenters.at(spurSweepIndex);
    const auto before = processor->beginIqRetuneBarrier();
    clearLiveSpectrumSnapshot(false, before);
    double actual = pendingSettings.centerFrequency;
    const int result = setCalibratedActiveFrequencySafely(pendingSettings.centerFrequency, &actual);
    if (result != FOBOS_ERR_OK || !std::isfinite(actual) || std::abs(actual - pendingSettings.centerFrequency) > 1e6) {
        endSpurCalibration(QStringLiteral("Retune failed at %1 MHz (error %2); previous profile preserved.")
            .arg(pendingSettings.centerFrequency / 1e6, 0, 'f', 3).arg(result)); return;
    }
    pendingSettings.actualFrequency = actual;
    spurSweepNoiseBaseline.beginCapture(receiverNoiseBaselineKey(pendingSettings), spurCalibrationTargetFrames, 1024);
    appliedHardwareSettings.centerFrequency = pendingSettings.centerFrequency;
    appliedHardwareSettings.actualFrequency = actual;
    publishSettingsToGlobals();
    updateIqFrameProducerSettings();
    processor->setCenterFrequencyHint(actual);
    const auto epoch = processor->beginIqRetuneBarrier();
    clearLiveSpectrumSnapshot(false, epoch);
    spurFrameGate = {epoch, 0, pendingSettings.centerFrequency};
    liveRetuneSettleDurationMs = 350;
    liveRetuneSettleTimer.start();
    spurSettlingClock.start();
    spurPointClock.restart();
    spurCalibrationActive = true;
}

void YourClassName::endSpurCalibration(const QString &error) {
    if (!spurSweepActive) return;
    spurCalibrationActive = false;
    spurSweepError = error;
    bool restored = spurSweepIndex < 0;
    pendingSettings = spurSettingsBeforeSweep;
    ++liveCenterRetuneGeneration;
    queuedLiveCenterRetuneGeneration = 0;
    if (processor && processor->isRunning() && runState == RadioRunState::Running && hasActiveFobosDevice()) {
        const auto before = processor->beginIqRetuneBarrier();
        clearLiveSpectrumSnapshot(false, before);
        double actual = pendingSettings.centerFrequency;
        restored = setCalibratedActiveFrequencySafely(pendingSettings.centerFrequency, &actual) == FOBOS_ERR_OK;
        if (restored) {
            pendingSettings.actualFrequency = actual;
            appliedHardwareSettings = pendingSettings;
            processor->setCenterFrequencyHint(actual);
            const auto epoch = processor->beginIqRetuneBarrier();
            clearLiveSpectrumSnapshot(false, epoch);
            liveRetuneSettleDurationMs = 350;
            liveRetuneSettleTimer.start();
        } else hardwareSettingsApplied = false;
    }
    if (!restored) spurSweepError += QStringLiteral("\nCould not restore tuning. Stop and restart reception.");
    const bool complete = error.isEmpty() && restored && spurSweepProfile.points.size() == spurSweepCenters.size();
    if (complete) {
        receiverNoiseBaseline = std::move(spurSweepNoiseBaseline);
        if (spurSweepCenters.size() == 1 && !spurCalibrationProfile.points.isEmpty() &&
            spurCalibrationProfile.compatibleWith(spurSweepProfile.context)) {
            const auto point = spurSweepProfile.points.front();
            auto pos = std::lower_bound(spurCalibrationProfile.points.begin(), spurCalibrationProfile.points.end(), point.centerHz,
                [](const auto &old, double hz) { return old.centerHz < hz; });
            if (pos != spurCalibrationProfile.points.end() && std::abs(pos->centerHz - point.centerHz) < 1) *pos = point;
            else spurCalibrationProfile.points.insert(pos, point);
        } else spurCalibrationProfile = std::move(spurSweepProfile);
        spurMaskEntries.clear();
        spurSuppressionEnabled = true;
        if (spurSuppressionCheckbox) {
            const QSignalBlocker blocker(spurSuppressionCheckbox);
            spurSuppressionCheckbox->setChecked(true);
        }
    }
    publishSettingsToGlobals();
    updateIqFrameProducerSettings();
    revertHardwareControlsToSettings();
    if (!spurWasRunning) stopFobosProcessing();
    spurSweepActive = false;
    pendingAudioStartAfterStreamReady = false;
    if (audioCheckbox) {
        const QSignalBlocker blocker(audioCheckbox);
        audioCheckbox->setChecked(spurAudioWasEnabled);
    }
    const bool resume = restored && resumeAudioAfterCalibration(spurWasRunning, runState, spurAudioWasEnabled,
                                                               processor && processor->isRunning());
    qInfo() << "[Spur] audio preference restored" << spurAudioWasEnabled << "resume" << resume
            << "receiverWasRunning" << spurWasRunning << "runState" << static_cast<int>(runState);
    if (resume) QTimer::singleShot(350, this, [this] {
        if (!spurSweepActive && localAudioAction(runState, pendingSettings.audioEnabled,
                processor && processor->isRunning() && !(streamWatchdogTimer && streamWatchdogTimer->isActive()),
                false) == LocalAudioAction::Start && audioProcessor)
            audioProcessor->startDemodulation();
    });
    adcOverloadGuard.reset();
    spurCalibrationBins.clear();
    spurLineCalibrator.reset();
    spurSweepProfile = {};
    spurSweepNoiseBaseline = {};
    updateSpurSuppressionStatus();
    if (complete) saveReceiverNoiseBaselineProfiles();
    savePersistentSettings();
    if (spurSweepProgress && complete) spurSweepProgress->setValue(spurSweepCenters.size());
    if (spurSweepLabel) spurSweepLabel->setText(complete
        ? (uiLanguage == QStringLiteral("uk") ? QStringLiteral("РљР°Р»С–Р±СЂСѓРІР°РЅРЅСЏ Р·Р°РІРµСЂС€РµРЅРѕ. РџСЂРѕС„С–Р»СЊ Р·Р±РµСЂРµР¶РµРЅРѕ. РњРѕР¶РЅР° РїРѕРІРµСЂРЅСѓС‚Рё Р°РЅС‚РµРЅСѓ.")
            : QStringLiteral("Calibration complete. Profile saved. You can reconnect the antenna."))
        : spurSweepError);
    qInfo() << "[Spur] full sweep" << (complete ? "committed" : "discarded")
        << "points" << spurSweepCenters.size() << "restored" << restored << spurSweepError;
}
