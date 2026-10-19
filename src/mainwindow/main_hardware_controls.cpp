#include "main.h"
#include "appconstants.h"
#include "tuningutils.h"

#include <QCheckBox>
#include <QDebug>
#include <QSignalBlocker>
#include <QStandardItemModel>

#include <algorithm>
#include <cmath>

extern double globalFrequency;
extern double globalSampleRate;
extern double globalBandwidth;
extern int globalModulationType;
extern int globalMode;
extern double currentScale;
extern double minFrequency;
extern double maxFrequency;

void YourClassName::rebuildReceiverModeChoices() {
    if (!modeBox) {
        return;
    }

    QSignalBlocker blocker(modeBox);
    modeBox->clear();
    if (isHackRfNativeSelected()) {
        pendingSettings.inputMode = INPUT_RF;
        modeBox->addItem(uiText(QStringLiteral("hackrf_rx_mode"),
                                QStringLiteral("RX - antenna port")),
                         INPUT_RF);
        modeBox->addItem(uiText(QStringLiteral("hackrf_tx_locked_mode"),
                                QStringLiteral("TX - use Transmitter lab")));
        if (auto *model = qobject_cast<QStandardItemModel *>(modeBox->model())) {
            if (QStandardItem *item = model->item(1)) {
                item->setEnabled(false);
                item->setToolTip(uiText(
                    QStringLiteral("hackrf_tx_locked_tooltip"),
                    QStringLiteral("RF transmission is still locked; use the Transmitter lab for IQ generation and export.")));
            }
        }
        modeBox->setCurrentIndex(0);
        return;
    }

    if (isExternalReceiverBackendSelected()) {
        pendingSettings.inputMode = INPUT_RF;
        modeBox->addItem(uiText(QStringLiteral("receiver_rx_mode"),
                                QStringLiteral("RX - antenna port")),
                         INPUT_RF);
        modeBox->setCurrentIndex(0);
        return;
    }

    modeBox->addItem(QStringLiteral("RF"), INPUT_RF);
    modeBox->addItem(QStringLiteral("HF1 + HF2"), INPUT_HF_COMBINED);
    modeBox->addItem(QStringLiteral("HF1"), INPUT_HF1);
    modeBox->addItem(QStringLiteral("HF2"), INPUT_HF2);
    modeBox->addItem(uiText(QStringLiteral("hf_cancel_lab"),
                            QStringLiteral("HF interference lab")),
                     INPUT_HF_NOISE_CANCEL);
    const int index = modeBox->findData(pendingSettings.inputMode);
    modeBox->setCurrentIndex(index >= 0 ? index : 0);
}

void YourClassName::updateReceiverSpecificControls() {
    const bool hackRf = isHackRfNativeSelected();
    const bool fobosGainControls = !isExternalReceiverBackendSelected();

    if (lnaGainSlider) {
        QSignalBlocker blocker(lnaGainSlider);
        if (hackRf) {
            lnaGainSlider->setRange(0, 40);
            lnaGainSlider->setSingleStep(8);
            lnaGainSlider->setPageStep(8);
            pendingSettings.hackRfLnaGainDb =
                (std::clamp)(pendingSettings.hackRfLnaGainDb, 0, 40) / 8 * 8;
            lnaGainSlider->setValue(pendingSettings.hackRfLnaGainDb);
        } else {
            lnaGainSlider->setRange(1, 3);
            lnaGainSlider->setSingleStep(1);
            lnaGainSlider->setPageStep(1);
            lnaGainSlider->setValue((std::clamp)(pendingSettings.lnaGain, 1, 3));
        }
        lnaGainSlider->setEnabled(hackRf || fobosGainControls);
    }
    if (vgaGainSlider) {
        QSignalBlocker blocker(vgaGainSlider);
        if (hackRf) {
            vgaGainSlider->setRange(0, 62);
            vgaGainSlider->setSingleStep(2);
            vgaGainSlider->setPageStep(2);
            pendingSettings.hackRfVgaGainDb =
                (std::clamp)(pendingSettings.hackRfVgaGainDb, 0, 62) / 2 * 2;
            vgaGainSlider->setValue(pendingSettings.hackRfVgaGainDb);
        } else {
            vgaGainSlider->setRange(0, 31);
            vgaGainSlider->setSingleStep(1);
            vgaGainSlider->setPageStep(1);
            vgaGainSlider->setValue((std::clamp)(pendingSettings.vgaGain, 0, 31));
        }
        vgaGainSlider->setEnabled(hackRf || fobosGainControls);
    }
    if (lnaGainLabel) {
        lnaGainLabel->setText(
            hackRf
                ? QStringLiteral("HackRF LNA: %1 dB").arg(pendingSettings.hackRfLnaGainDb)
                : QStringLiteral("%1: %2")
                      .arg(uiText(QStringLiteral("lna_gain"), QStringLiteral("LNA Gain")))
                      .arg(pendingSettings.lnaGain));
    }
    if (vgaGainLabel) {
        vgaGainLabel->setText(
            hackRf
                ? QStringLiteral("HackRF VGA: %1 dB").arg(pendingSettings.hackRfVgaGainDb)
                : QStringLiteral("%1: %2")
                      .arg(uiText(QStringLiteral("vga_gain"), QStringLiteral("VGA Gain")))
                      .arg(pendingSettings.vgaGain));
    }

    if (clkBox && hackRf) {
        QSignalBlocker blocker(clkBox);
        clkBox->setCurrentIndex((std::max)(0, clkBox->findData(0)));
        pendingSettings.clockSource = 0;
    }
    if (hackRfSettingsButton) {
        hackRfSettingsButton->setVisible(hackRf);
        hackRfSettingsButton->setEnabled(hackRf &&
                                         (isIdle() || runState == RadioRunState::Running));
    }
}

void YourClassName::onDirectSamplingChanged(int index) {
    Q_UNUSED(index);

    if (!modeBox) {
        return;
    }

    const int value = modeBox->currentData().toInt();

    if (pendingSettings.inputMode == value) {
        return;
    }

    const int previousInputMode = pendingSettings.inputMode;
    pendingSettings.inputMode = value;

    if (value != INPUT_RF) {
        pendingSettings.centerFrequency = 0;
        if (previousInputMode == INPUT_RF ||
            !std::isfinite(pendingSettings.listeningFrequency)) {
            pendingSettings.listeningFrequency = value == INPUT_HF_COMBINED ? 0 : 1250000;
        }
    } else {
        const double minimumListening = rfMinimumListeningFrequency(pendingSettings);
        const double minimumCenter = rfMinimumCenterFrequency(pendingSettings);
        if (!std::isfinite(pendingSettings.listeningFrequency) ||
            pendingSettings.listeningFrequency < minimumListening) {
            pendingSettings.listeningFrequency = 100000000;
        }
        if (!std::isfinite(pendingSettings.centerFrequency) ||
            pendingSettings.centerFrequency < minimumCenter) {
            pendingSettings.centerFrequency = pendingSettings.listeningFrequency;
        }
    }

    normalizeTuning(pendingSettings);

    if (frequencyControl) {
        QSignalBlocker blocker(frequencyControl);
        frequencyControl->setValueHz(pendingSettings.centerFrequency);
    }

    if (listeningFrequencyControl) {
        QSignalBlocker blocker(listeningFrequencyControl);
        listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
    }

    publishSettingsToGlobals();
    settingRange();
    updateHfNoiseCancelControls();

    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
        return;
    }

    if (!isIdle()) {
        restartStreamForHardwareChange();
        return;
    }

    if (!restartStreamForHardwareChange()) {
        qDebug() << "Input mode will be applied on the next start.";
    }
}
void YourClassName::settingRange() {
    if (!scaleWidget || globalSampleRate <= 0.0) {
        return;
    }
    double overallMin = directMinFrequencyForMode(globalMode, globalSampleRate);
    double overallMax = directMaxFrequency(globalSampleRate);

    if (offlineIqPlaybackActive && !offlineIqPlaybackHasMetadata) {
        overallMin = globalFrequency - globalSampleRate / 2.0;
        overallMax = globalFrequency + globalSampleRate / 2.0;
    } else if (globalMode == INPUT_RF) {
        const double receiverMinimum = rfMinimumListeningFrequency(pendingSettings);
        overallMin = (std::max)(receiverMinimum,
                                globalFrequency - globalSampleRate / 2.0);
        overallMax = (std::clamp)(globalFrequency + globalSampleRate / 2.0,
                                  overallMin,
                                  rfMaximumFrequency(pendingSettings));
    }

    if (frequencyControl && globalMode == INPUT_RF) {
        const double controlMin = rfMinimumCenterFrequency(pendingSettings);
        const double controlMax = rfMaximumCenterFrequency(pendingSettings);
        QSignalBlocker blocker(frequencyControl);
        frequencyControl->setRangeHz(controlMin, controlMax);
        frequencyControl->setValueHz(
            (std::clamp)(pendingSettings.centerFrequency, controlMin, controlMax));
    }
    if (listeningFrequencyControl) {
        const double controlMin = globalMode == INPUT_RF
                                      ? rfMinimumListeningFrequency(pendingSettings)
                                      : overallMin;
        const double controlMax = globalMode == INPUT_RF
                                      ? rfMaximumFrequency(pendingSettings)
                                      : overallMax;
        QSignalBlocker blocker(listeningFrequencyControl);
        listeningFrequencyControl->setRangeHz(controlMin, controlMax);
        listeningFrequencyControl->setValueHz((std::clamp)(pendingSettings.listeningFrequency, controlMin, controlMax));
    }

    const double availableRange = (std::max)(1.0, overallMax - overallMin);
    double newRange = availableRange * (effectiveScalePercent() / 100.0);
    newRange = (std::clamp)(newRange, 1.0, availableRange);

    if (!std::isfinite(spectrumDisplayCenterHz) ||
        spectrumDisplayCenterHz < overallMin ||
        spectrumDisplayCenterHz > overallMax) {
        spectrumDisplayCenterHz = (std::clamp)(pendingSettings.listeningFrequency, overallMin, overallMax);
    }

    const double minCenter = overallMin + newRange / 2.0;
    const double maxCenter = overallMax - newRange / 2.0;
    if (maxCenter >= minCenter) {
        spectrumDisplayCenterHz = (std::clamp)(spectrumDisplayCenterHz, minCenter, maxCenter);
    } else {
        spectrumDisplayCenterHz = overallMin + availableRange / 2.0;
    }

	double newMin = spectrumDisplayCenterHz - newRange / 2.0;
    newMin = (std::clamp)(newMin, overallMin, overallMax - newRange);
    double newMax = newMin + newRange;
    minFrequency = newMin;
    maxFrequency = newMax;
    scaleWidget->setTuning(
        pendingSettings.listeningFrequency,
        globalFrequency,
        effectiveDemodulationBandwidth(globalBandwidth,
                                       globalModulationType,
                                       pendingSettings.sstvDemodulationMode),
        effectiveDemodulationModulationType(globalModulationType,
                                            pendingSettings.sstvDemodulationMode));
    scaleWidget->setRange(minFrequency, maxFrequency);
    updateFineTuneLabel();
}

void YourClassName::onCheckboxStateChanged(int state) {
    QCheckBox *senderCheckbox = qobject_cast<QCheckBox*>(sender());
    if (senderCheckbox) {
        const uint8_t value = currentGpoValue();
        pendingSettings.gpoValue = value;
        qDebug() << "Checkbox state changed. New GPO value:" << value;
        if (hasActiveFobosDevice() && !isIdle() && hardwareSettingsApplied) {
            const int result = setActiveGpoSafely(pendingSettings.gpoValue);
            qDebug() << "[Live] GPO apply result" << result;
            if (result == FOBOS_ERR_OK) {
                appliedHardwareSettings.gpoValue = pendingSettings.gpoValue;
            }
        } else {
            qDebug() << "GPO state will be applied on the next start.";
        }
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand();
        }
    }
}

void YourClassName::onLnaGainChanged(int value) {
    if (isHackRfNativeSelected()) {
        const int gainDb = (std::clamp)(value, 0, 40) / 8 * 8;
        pendingSettings.hackRfLnaGainDb = gainDb;
        if (lnaGainSlider && lnaGainSlider->value() != gainDb) {
            QSignalBlocker blocker(lnaGainSlider);
            lnaGainSlider->setValue(gainDb);
        }
        lnaGainLabel->setText(QStringLiteral("HackRF LNA: %1 dB").arg(gainDb));
        if (processor && processor->isRunning()) {
            processor->applyHackRfGainSettings(pendingSettings.hackRfLnaGainDb,
                                               pendingSettings.hackRfVgaGainDb);
        }
        savePersistentSettings();
        return;
    }
    pendingSettings.lnaGain = value;
    lnaGainLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("lna_gain"), QStringLiteral("LNA Gain"))).arg(value));

    if (hasActiveFobosDevice() && !isIdle() && hardwareSettingsApplied) {
        const int result = setActiveLnaGainSafely(static_cast<unsigned int>(value));
        qDebug() << "[Live] LNA gain apply result" << result;

        if (result == FOBOS_ERR_OK) {
            appliedHardwareSettings.lnaGain = value;
        }
    }

    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    }
}

void YourClassName::onVgaGainChanged(int value) {
    if (isHackRfNativeSelected()) {
        const int gainDb = (std::clamp)(value, 0, 62) / 2 * 2;
        pendingSettings.hackRfVgaGainDb = gainDb;
        if (vgaGainSlider && vgaGainSlider->value() != gainDb) {
            QSignalBlocker blocker(vgaGainSlider);
            vgaGainSlider->setValue(gainDb);
        }
        vgaGainLabel->setText(QStringLiteral("HackRF VGA: %1 dB").arg(gainDb));
        if (processor && processor->isRunning()) {
            processor->applyHackRfGainSettings(pendingSettings.hackRfLnaGainDb,
                                               pendingSettings.hackRfVgaGainDb);
        }
        savePersistentSettings();
        return;
    }
    pendingSettings.vgaGain = value;
    vgaGainLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("vga_gain"), QStringLiteral("VGA Gain"))).arg(value));

    if (hasActiveFobosDevice() && !isIdle() && hardwareSettingsApplied) {
        const int result = setActiveVgaGainSafely(static_cast<unsigned int>(value));
        qDebug() << "[Live] VGA gain apply result" << result;

        if (result == FOBOS_ERR_OK) {
            appliedHardwareSettings.vgaGain = value;
        }
    }

    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    }
}

void YourClassName::onRtlAgcChanged(int state) {
    pendingSettings.rtlAgc = state == Qt::Checked;
    if (rtlGainSlider) {
        rtlGainSlider->setEnabled(isRtlBackendSelected() && !pendingSettings.rtlAgc &&
                                  (isIdle() || runState == RadioRunState::Running));
    }

    if (processor && processor->isRunning() && isRtlSdrNativeSelected()) {
        processor->applyRtlGainSettings(pendingSettings.rtlAgc,
                                        (std::clamp)(pendingSettings.rtlTunerGainTenthsDb, 0, 496));
    }
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    }
    savePersistentSettings();
}

void YourClassName::onRtlGainChanged(int value) {
    pendingSettings.rtlTunerGainTenthsDb = (std::clamp)(value, 0, 496);
    if (rtlGainLabel) {
        rtlGainLabel->setText(QStringLiteral("%1: %2 dB")
                                  .arg(uiText(QStringLiteral("rtl_gain"), QStringLiteral("RTL gain")))
                                  .arg(pendingSettings.rtlTunerGainTenthsDb / 10.0, 0, 'f', 1));
    }

    if (!pendingSettings.rtlAgc && processor && processor->isRunning() && isRtlSdrNativeSelected()) {
        processor->applyRtlGainSettings(false, pendingSettings.rtlTunerGainTenthsDb);
    }
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    }
    savePersistentSettings();
}

void YourClassName::onClkChanged(int index) {
    if (!isIdle()) {
        qDebug() << "Stop processing before changing clock source.";
        revertHardwareControlsToSettings();
        return;
    }

    pendingSettings.clockSource = clkBox->currentData().toInt();
    qDebug() << "Clock source will be applied on the next start.";
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    }
}
