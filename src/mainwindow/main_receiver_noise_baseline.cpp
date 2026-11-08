#include "main.h"
#include "core/fobosifselection.h"
#include "core/fobosruntimeif.h"

#include "appsettingsutils.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

ReceiverNoiseBaselineKey YourClassName::receiverNoiseBaselineKey(
    const RadioSettings &settings) const {
    ReceiverNoiseBaselineKey key;
    key.receiver = receiverCalibrationContext(settings);
    key.centerFrequencyHz = isDirectInputMode(settings.inputMode)
                                ? 0.0
                                : settings.centerFrequency;
    key.fftLength = settings.fftLength;
    key.fftWindowType = settings.fftWindowType;
    key.rfBandwidthHz = isHackRfNativeSelected() && !settings.hackRfAutomaticBandwidth
                            ? settings.hackRfBandwidthHz
                            : 0;
    key.amplitudeOffsetDb = amplitudeCalibrationOffsetDb;
    key.calibrationTableEnabled = calibrationTableEnabled;
    key.calibrationPointCount = calibrationTableEnabled
                                    ? receiverCalibrationTable.points().size()
                                    : 0;
    if (key.receiver.backendId.startsWith(QStringLiteral("fobos"))) {
        const auto info = selectedFobosDeviceInfo();
        key.hardwareIdentity = QStringLiteral("%1/%2/IF%3/CLK%4/GPO%5")
            .arg(info.serial, info.hardwareRevision).arg(useCorrectedFobosCommands ? 1 : 0)
            .arg(settings.clockSource).arg(settings.gpoValue);
        if (activeFobosApiKind == FobosApiKind::Agile && fobosActiveMixerBiasCode > 1 &&
            !isDirectInputMode(settings.inputMode))
            key.hardwareIdentity += QStringLiteral("/MIXER%1").arg(fobosActiveMixerBiasCode);
        if (fobosIfTestMode == 1)
            key.hardwareIdentity += QStringLiteral("/GLOBALIF2350");
        if (fobosIfTestMode == 2)
            key.hardwareIdentity += QStringLiteral("/GLOBALIF2450");
        if (fobosIfTestMode == 3) {
            key.hardwareIdentity += QStringLiteral("/GLOBALAUTO2/IF%1")
                .arg(FobosRuntimeIf::chooseEffectiveIf(fobosActiveIfRulesFrame, settings.centerFrequency,
                    settings.sampleRate, useCorrectedFobosCommands, 3));
            if (fobosCustomIfRulesActive)
                key.hardwareIdentity += FobosRuntimeIf::validFrame(fobosActiveIfRulesFrame)
                    ? QStringLiteral("/CUSTOMIF%1").arg(QString::number(FobosRuntimeIf::rulesCrc(fobosActiveIfRulesFrame), 16))
                    : QStringLiteral("/CUSTOMIFINVALID");
        }
    }
    return key;
}

void YourClassName::saveReceiverNoiseBaselineProfiles() const {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("calibration/noiseBaselineProfiles"),
                      receiverNoiseBaseline.serialize());
    settings.sync();
}

void YourClassName::openReceiverNoiseBaselineDialog() {
    const bool ukrainian = normalizedUiLanguage(uiLanguage) == QStringLiteral("uk");
    QDialog dialog(this);
    dialog.setWindowTitle(uiText(QStringLiteral("receiver_noise_baseline"),
                                 QStringLiteral("Receiver noise baseline")));
    dialog.setMinimumWidth(590);

    auto *outer = new QVBoxLayout(&dialog);
    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *root = new QVBoxLayout(content);
    scroll->setWidget(content); outer->addWidget(scroll);
    const int availableHeight = QApplication::primaryScreen() ? QApplication::primaryScreen()->availableGeometry().height() : 900;
    dialog.resize(680, (std::min)(800, availableHeight - 80));
    auto *instructions = new QLabel(uiText(
        QStringLiteral("receiver_noise_baseline_instructions"),
        QStringLiteral("Connect a 50-ohm termination instead of an antenna, turn bias tee off, and let the receiver warm up. The captured profile is valid only for the current receiver, input, center frequency, sample rate, FFT/window and gain settings. Do not short a powered antenna connector.")),
        &dialog);
    instructions->setWordWrap(true);
    instructions->setStyleSheet(QStringLiteral("QLabel { padding: 6px; background: #342d18; color: #f2d786; border: 1px solid #76622b; }"));
    root->addWidget(instructions);

    auto *contextLabel = new QLabel(&dialog);
    contextLabel->setWordWrap(true);
    contextLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(contextLabel);

    auto *form = new QFormLayout();
    auto *modeCombo = new QComboBox(&dialog);
    modeCombo->setObjectName(QStringLiteral("receiverNoiseBaselineMode"));
    modeCombo->addItem(uiText(QStringLiteral("receiver_noise_baseline_off"), QStringLiteral("Off / raw levels")),
                       static_cast<int>(ReceiverNoiseBaselineMode::Off));
    modeCombo->addItem(uiText(QStringLiteral("receiver_noise_baseline_relative"), QStringLiteral("Flatten receiver floor")),
                       static_cast<int>(ReceiverNoiseBaselineMode::RelativeFloor));
    modeCombo->addItem(uiText(QStringLiteral("receiver_noise_baseline_excess"), QStringLiteral("Excess power above receiver floor")),
                       static_cast<int>(ReceiverNoiseBaselineMode::ExcessPower));
    modeCombo->addItem(uiText("receiver_noise_shape", "Broad noise shape (VGA / receiver)"),
                       static_cast<int>(ReceiverNoiseBaselineMode::WideShape));
    const int modeIndex = modeCombo->findData(receiverNoiseBaselineMode);
    modeCombo->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);
    modeCombo->setToolTip(uiText(
        QStringLiteral("receiver_noise_baseline_mode_tooltip"),
        QStringLiteral("Visual correction only. Raw IQ, audio, recordings, network data and scientific measurements are unchanged.")));

    auto *targetSpin = new QDoubleSpinBox(&dialog);
    targetSpin->setRange(-220.0, 20.0);
    targetSpin->setDecimals(1);
    targetSpin->setSingleStep(1.0);
    targetSpin->setSuffix(QStringLiteral(" dB"));
    targetSpin->setValue(receiverNoiseBaselineTargetDb);
    targetSpin->setKeyboardTracking(false);

    auto *framesSpin = new QSpinBox(&dialog);
    framesSpin->setRange(8, 512);
    framesSpin->setSingleStep(8);
    framesSpin->setValue(receiverNoiseBaselineCaptureFrames);

    form->addRow(uiText(QStringLiteral("receiver_noise_baseline_mode"), QStringLiteral("Display mode")), modeCombo);
    form->addRow(uiText(QStringLiteral("receiver_noise_baseline_target"), QStringLiteral("Reference floor")), targetSpin);
    form->addRow(uiText(QStringLiteral("receiver_noise_baseline_frames"), QStringLiteral("Capture frames")), framesSpin);
    auto *widthSpin = new QDoubleSpinBox(&dialog);
    widthSpin->setObjectName(QStringLiteral("receiverNoiseShapeWidth"));
    widthSpin->setRange(50, 2000); widthSpin->setSuffix(QStringLiteral(" kHz"));
    widthSpin->setValue(receiverNoiseShapeWidthHz / 1000); widthSpin->setKeyboardTracking(false);
    auto *strengthSpin = new QSpinBox(&dialog);
    strengthSpin->setObjectName(QStringLiteral("receiverNoiseShapeStrength"));
    strengthSpin->setRange(0, 100); strengthSpin->setSuffix(QStringLiteral(" %"));
    strengthSpin->setValue(static_cast<int>(receiverNoiseShapeStrength * 100));
    form->addRow(uiText("receiver_noise_shape_width", "Broad-shape scale"), widthSpin);
    form->addRow(uiText("receiver_noise_shape_strength", "Broad-shape suppression"), strengthSpin);
    auto updateModeControls = [=] {
        const bool wide = modeCombo->currentData().toInt() == static_cast<int>(ReceiverNoiseBaselineMode::WideShape);
        widthSpin->setEnabled(wide); strengthSpin->setEnabled(wide); targetSpin->setEnabled(!wide);
    };
    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [=] { updateModeControls(); });
    updateModeControls();
    auto *shapeHint = new QLabel(uiText("receiver_noise_shape_hint", "Broad mode uses a terminated-input profile, removes only its smooth excess, and leaves IQ/audio untouched. It accepts FFT/window changes and nearby RF centers; different gain, Fs or IF needs another profile. Spur calibration also captures broad profiles."), &dialog);
    shapeHint->setWordWrap(true); root->addWidget(shapeHint);
    root->addLayout(form);

    auto *progress = new QProgressBar(&dialog);
    progress->setRange(0, receiverNoiseBaselineCaptureFrames);
    progress->setValue(0);
    root->addWidget(progress);

    auto *statusLabel = new QLabel(&dialog);
    statusLabel->setWordWrap(true);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(statusLabel);

    auto *actions = new QHBoxLayout();
    auto *captureButton = new QPushButton(
        uiText(QStringLiteral("receiver_noise_baseline_capture"), QStringLiteral("Capture")), &dialog);
    auto *cancelButton = new QPushButton(
        uiText(QStringLiteral("cancel"), QStringLiteral("Cancel")), &dialog);
    auto *clearCurrentButton = new QPushButton(
        uiText(QStringLiteral("receiver_noise_baseline_clear_current"), QStringLiteral("Clear current")), &dialog);
    auto *clearAllButton = new QPushButton(
        uiText(QStringLiteral("receiver_noise_baseline_clear_all"), QStringLiteral("Clear all")), &dialog);
    actions->addWidget(captureButton);
    actions->addWidget(cancelButton);
    actions->addStretch(1);
    actions->addWidget(clearCurrentButton);
    actions->addWidget(clearAllButton);
    root->addLayout(actions);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto currentKey = [this]() {
        return receiverNoiseBaselineKey(spectrumProcessingSettings());
    };
    auto refresh = [this,
                    ukrainian,
                    currentKey,
                    contextLabel,
                    statusLabel,
                    progress,
                    captureButton,
                    cancelButton,
                    clearCurrentButton, modeCombo]() {
        const ReceiverNoiseBaselineKey key = currentKey();
        contextLabel->setText((ukrainian ? QStringLiteral("РџРѕС‚РѕС‡РЅР° РєРѕРЅС„С–РіСѓСЂР°С†С–СЏ: ")
                                         : QStringLiteral("Current configuration: ")) +
                              key.description());
        progress->setMaximum((std::max)(1, receiverNoiseBaseline.targetFrames()));
        progress->setValue(receiverNoiseBaseline.capturedFrames());
        const bool capturing = receiverNoiseBaseline.isCapturing();
        captureButton->setEnabled(!capturing);
        cancelButton->setEnabled(capturing);
        const bool wide = modeCombo->currentData().toInt() == static_cast<int>(ReceiverNoiseBaselineMode::WideShape);
        clearCurrentButton->setEnabled(!capturing && receiverNoiseBaseline.hasMatchingProfile(key, wide));

        if (capturing) {
            statusLabel->setText(ukrainian
                ? QStringLiteral("РќР°РєРѕРїРёС‡РµРЅРЅСЏ РІ Р»С–РЅС–Р№РЅС–Р№ РїРѕС‚СѓР¶РЅРѕСЃС‚С–: %1/%2 РєР°РґСЂС–РІ")
                      .arg(receiverNoiseBaseline.capturedFrames())
                      .arg(receiverNoiseBaseline.targetFrames())
                : QStringLiteral("Linear-power accumulation: %1/%2 frames")
                      .arg(receiverNoiseBaseline.capturedFrames())
                      .arg(receiverNoiseBaseline.targetFrames()));
            return;
        }
        const ReceiverNoiseBaselineProfile profile = receiverNoiseBaseline.matchingProfile(key, wide);
        if (profile.isValid()) {
            statusLabel->setText(ukrainian
                ? QStringLiteral("РџСЂРѕС„С–Р»СЊ Р·Р±С–РіР°С”С‚СЊСЃСЏ: %1 РєР°РґСЂС–РІ, СЃРµСЂРµРґРЅСЏ РІР°СЂС–Р°С†С–СЏ %2 РґР‘, %3. РЈСЃСЊРѕРіРѕ РїСЂРѕС„С–Р»С–РІ: %4")
                      .arg(profile.capturedFrames)
                      .arg(profile.spreadDb, 0, 'f', 2)
                      .arg(QDateTime::fromMSecsSinceEpoch(profile.capturedUtcMs).toString(Qt::ISODate))
                      .arg(receiverNoiseBaseline.profileCount())
                : QStringLiteral("Matching profile: %1 frames, mean variation %2 dB, %3. Total profiles: %4")
                      .arg(profile.capturedFrames)
                      .arg(profile.spreadDb, 0, 'f', 2)
                      .arg(QDateTime::fromMSecsSinceEpoch(profile.capturedUtcMs).toString(Qt::ISODate))
                      .arg(receiverNoiseBaseline.profileCount()));
            if (wide) statusLabel->setText(statusLabel->text() + (ukrainian
                ? QStringLiteral("\nРЁРёСЂРѕРєРёР№ РїСЂРѕС„С–Р»СЊ: С†РµРЅС‚СЂ %1 РњР“С†, VGA %2.").arg(profile.key.centerFrequencyHz / 1e6, 0, 'f', 3).arg(profile.key.receiver.vgaGain)
                : QStringLiteral("\nBroad profile: center %1 MHz, VGA %2.").arg(profile.key.centerFrequencyHz / 1e6, 0, 'f', 3).arg(profile.key.receiver.vgaGain)));
        } else {
            statusLabel->setText(ukrainian
                ? QStringLiteral("Р”Р»СЏ РїРѕС‚РѕС‡РЅРѕС— РєРѕРЅС„С–РіСѓСЂР°С†С–С— РїСЂРѕС„С–Р»СЋ РЅРµРјР°С”. Р—Р±РµСЂРµР¶РµРЅРѕ РїСЂРѕС„С–Р»С–РІ: %1")
                      .arg(receiverNoiseBaseline.profileCount())
                : QStringLiteral("No profile matches the current configuration. Stored profiles: %1")
                      .arg(receiverNoiseBaseline.profileCount()));
        }
    };

    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [this, modeCombo](int) {
                receiverNoiseBaselineMode = modeCombo->currentData().toInt();
                savePersistentSettings();
            });
    connect(targetSpin, &QDoubleSpinBox::editingFinished, &dialog, [this, targetSpin]() {
        receiverNoiseBaselineTargetDb = targetSpin->value();
        savePersistentSettings();
    });
    connect(widthSpin, &QDoubleSpinBox::editingFinished, &dialog, [this, widthSpin] {
        receiverNoiseShapeWidthHz = widthSpin->value() * 1000; savePersistentSettings();
    });
    connect(strengthSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [this](int value) {
        receiverNoiseShapeStrength = value / 100.0; savePersistentSettings();
    });
    connect(framesSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
            [this, framesSpin, progress](int) {
                receiverNoiseBaselineCaptureFrames = framesSpin->value();
                progress->setMaximum(receiverNoiseBaselineCaptureFrames);
                savePersistentSettings();
            });
    connect(captureButton, &QPushButton::clicked, &dialog,
            [this, ukrainian, currentKey, refresh]() {
                if (runState != RadioRunState::Running) {
                    QMessageBox::warning(this,
                                         uiText(QStringLiteral("receiver_noise_baseline"), QStringLiteral("Receiver noise baseline")),
                                         ukrainian ? QStringLiteral("РЎРїРѕС‡Р°С‚РєСѓ Р·Р°РїСѓСЃС‚С–С‚СЊ РїСЂРёР№РјР°С‡.")
                                                   : QStringLiteral("Start the receiver first."));
                    return;
                }
                if (agileScanRunning || standardScanRunning || listeningScanRunning) {
                    QMessageBox::warning(this,
                                         uiText(QStringLiteral("receiver_noise_baseline"), QStringLiteral("Receiver noise baseline")),
                                         ukrainian ? QStringLiteral("Р—СѓРїРёРЅС–С‚СЊ СЃРєР°РЅСѓРІР°РЅРЅСЏ РїРµСЂРµРґ РєР°Р»С–Р±СЂСѓРІР°РЅРЅСЏРј.")
                                                   : QStringLiteral("Stop scanning before calibration."));
                    return;
                }
                if (isHackRfNativeSelected() && pendingSettings.hackRfBiasTeeEnabled) {
                    QMessageBox::warning(this,
                                         uiText(QStringLiteral("receiver_noise_baseline"), QStringLiteral("Receiver noise baseline")),
                                         ukrainian ? QStringLiteral("Р’РёРјРєРЅС–С‚СЊ bias tee РїРµСЂРµРґ РїС–РґРєР»СЋС‡РµРЅРЅСЏРј 50-РѕРјРЅРѕРіРѕ РЅР°РІР°РЅС‚Р°Р¶РµРЅРЅСЏ.")
                                                   : QStringLiteral("Turn bias tee off before connecting a 50-ohm termination."));
                    return;
                }
                receiverYFactorCalibration.cancelCapture();
                receiverNoiseBaseline.beginCapture(currentKey(), receiverNoiseBaselineCaptureFrames,
                    receiverNoiseBaselineMode == static_cast<int>(ReceiverNoiseBaselineMode::WideShape) ? 1024 : 32768);
                receiverNoiseBaselineCaptureTimer.invalidate();
                refresh();
            });
    connect(cancelButton, &QPushButton::clicked, &dialog, [this, refresh]() {
        receiverNoiseBaseline.cancelCapture();
        refresh();
    });
    connect(clearCurrentButton, &QPushButton::clicked, &dialog,
            [this, currentKey, refresh]() {
                if (receiverNoiseBaseline.clearMatching(currentKey(),
                        receiverNoiseBaselineMode == static_cast<int>(ReceiverNoiseBaselineMode::WideShape))) {
                    saveReceiverNoiseBaselineProfiles();
                }
                refresh();
            });
    connect(clearAllButton, &QPushButton::clicked, &dialog,
            [this, ukrainian, refresh]() {
                const auto answer = QMessageBox::question(
                    this,
                    uiText(QStringLiteral("receiver_noise_baseline"), QStringLiteral("Receiver noise baseline")),
                    ukrainian ? QStringLiteral("Р’РёРґР°Р»РёС‚Рё РІСЃС– РїСЂРѕС„С–Р»С– РІР»Р°СЃРЅРѕРіРѕ С€СѓРјСѓ РїСЂРёР№РјР°С‡С–РІ?")
                              : QStringLiteral("Remove all receiver-noise profiles?"));
                if (answer == QMessageBox::Yes) {
                    receiverNoiseBaseline.clearAll();
                    saveReceiverNoiseBaselineProfiles();
                    refresh();
                }
            });

    QTimer refreshTimer(&dialog);
    refreshTimer.setInterval(100);
    connect(&refreshTimer, &QTimer::timeout, &dialog, refresh);
    refreshTimer.start();
    refresh();
    dialog.exec();
    if (receiverNoiseBaseline.isCapturing()) receiverNoiseBaseline.cancelCapture();
}
