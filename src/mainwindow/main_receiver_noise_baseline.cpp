#include "main.h"

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

    auto *root = new QVBoxLayout(&dialog);
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
    modeCombo->addItem(uiText(QStringLiteral("receiver_noise_baseline_off"), QStringLiteral("Off / raw levels")),
                       static_cast<int>(ReceiverNoiseBaselineMode::Off));
    modeCombo->addItem(uiText(QStringLiteral("receiver_noise_baseline_relative"), QStringLiteral("Flatten receiver floor")),
                       static_cast<int>(ReceiverNoiseBaselineMode::RelativeFloor));
    modeCombo->addItem(uiText(QStringLiteral("receiver_noise_baseline_excess"), QStringLiteral("Excess power above receiver floor")),
                       static_cast<int>(ReceiverNoiseBaselineMode::ExcessPower));
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
    root->addWidget(buttons);
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
                    clearCurrentButton]() {
        const ReceiverNoiseBaselineKey key = currentKey();
        contextLabel->setText((ukrainian ? QStringLiteral("Поточна конфігурація: ")
                                         : QStringLiteral("Current configuration: ")) +
                              key.description());
        progress->setMaximum((std::max)(1, receiverNoiseBaseline.targetFrames()));
        progress->setValue(receiverNoiseBaseline.capturedFrames());
        const bool capturing = receiverNoiseBaseline.isCapturing();
        captureButton->setEnabled(!capturing);
        cancelButton->setEnabled(capturing);
        clearCurrentButton->setEnabled(!capturing && receiverNoiseBaseline.hasMatchingProfile(key));

        if (capturing) {
            statusLabel->setText(ukrainian
                ? QStringLiteral("Накопичення в лінійній потужності: %1/%2 кадрів")
                      .arg(receiverNoiseBaseline.capturedFrames())
                      .arg(receiverNoiseBaseline.targetFrames())
                : QStringLiteral("Linear-power accumulation: %1/%2 frames")
                      .arg(receiverNoiseBaseline.capturedFrames())
                      .arg(receiverNoiseBaseline.targetFrames()));
            return;
        }
        const ReceiverNoiseBaselineProfile profile = receiverNoiseBaseline.matchingProfile(key);
        if (profile.isValid()) {
            statusLabel->setText(ukrainian
                ? QStringLiteral("Профіль збігається: %1 кадрів, середня варіація %2 дБ, %3. Усього профілів: %4")
                      .arg(profile.capturedFrames)
                      .arg(profile.spreadDb, 0, 'f', 2)
                      .arg(QDateTime::fromMSecsSinceEpoch(profile.capturedUtcMs).toString(Qt::ISODate))
                      .arg(receiverNoiseBaseline.profileCount())
                : QStringLiteral("Matching profile: %1 frames, mean variation %2 dB, %3. Total profiles: %4")
                      .arg(profile.capturedFrames)
                      .arg(profile.spreadDb, 0, 'f', 2)
                      .arg(QDateTime::fromMSecsSinceEpoch(profile.capturedUtcMs).toString(Qt::ISODate))
                      .arg(receiverNoiseBaseline.profileCount()));
        } else {
            statusLabel->setText(ukrainian
                ? QStringLiteral("Для поточної конфігурації профілю немає. Збережено профілів: %1")
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
                                         ukrainian ? QStringLiteral("Спочатку запустіть приймач.")
                                                   : QStringLiteral("Start the receiver first."));
                    return;
                }
                if (agileScanRunning || standardScanRunning || listeningScanRunning) {
                    QMessageBox::warning(this,
                                         uiText(QStringLiteral("receiver_noise_baseline"), QStringLiteral("Receiver noise baseline")),
                                         ukrainian ? QStringLiteral("Зупиніть сканування перед калібруванням.")
                                                   : QStringLiteral("Stop scanning before calibration."));
                    return;
                }
                if (isHackRfNativeSelected() && pendingSettings.hackRfBiasTeeEnabled) {
                    QMessageBox::warning(this,
                                         uiText(QStringLiteral("receiver_noise_baseline"), QStringLiteral("Receiver noise baseline")),
                                         ukrainian ? QStringLiteral("Вимкніть bias tee перед підключенням 50-омного навантаження.")
                                                   : QStringLiteral("Turn bias tee off before connecting a 50-ohm termination."));
                    return;
                }
                receiverYFactorCalibration.cancelCapture();
                receiverNoiseBaseline.beginCapture(currentKey(), receiverNoiseBaselineCaptureFrames);
                receiverNoiseBaselineCaptureTimer.invalidate();
                refresh();
            });
    connect(cancelButton, &QPushButton::clicked, &dialog, [this, refresh]() {
        receiverNoiseBaseline.cancelCapture();
        refresh();
    });
    connect(clearCurrentButton, &QPushButton::clicked, &dialog,
            [this, currentKey, refresh]() {
                if (receiverNoiseBaseline.clearMatching(currentKey())) {
                    saveReceiverNoiseBaselineProfiles();
                }
                refresh();
            });
    connect(clearAllButton, &QPushButton::clicked, &dialog,
            [this, ukrainian, refresh]() {
                const auto answer = QMessageBox::question(
                    this,
                    uiText(QStringLiteral("receiver_noise_baseline"), QStringLiteral("Receiver noise baseline")),
                    ukrainian ? QStringLiteral("Видалити всі профілі власного шуму приймачів?")
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
}
