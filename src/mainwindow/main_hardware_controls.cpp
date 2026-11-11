#include "main.h"
#include "analyzer/multisdrdialog.h"
#include "appconstants.h"
#include "tuningutils.h"
#include "core/diagnosticlogging.h"

#include <QCheckBox>
#include <QDebug>
#include <QJsonObject>
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

void YourClassName::refreshReceiverInfoOverlays() {
    QStringList lines;
    if (hasActiveFobosDevice() && !isNetworkClientMode() && !offlineIqPlaybackActive) {
        if (activeFobosApiKind == FobosApiKind::Agile && adcOverloadStatusLabel)
            lines << adcOverloadStatusLabel->text();
        if (fobosIfStatusLabel) lines << fobosIfStatusLabel->text();
        if (activeFobosApiKind == FobosApiKind::Agile && fobosMetadataStatusLabel)
            lines << fobosMetadataStatusLabel->text();
    }
    const QString text = lines.join(QLatin1Char('\n'));
    if (waterfallWidget) waterfallWidget->setReceiverStatusText(text);
}

void YourClassName::refreshFobosIfStatus() {
    if (!fobosIfStatusLabel) return;
    // Both IF readback and firmware telemetry issue synchronous EP0 transfers.
    // libusb can dispatch RX callbacks while waiting: never enter it from the
    // GUI while the reader owns the device, including Start/Stop transitions.
    if (!isIdle() || (processor && processor->isRunning())) {
        const QString text = uiText("fobos_if_readout_stopped", "IF readback: available after Stop");
        fobosIfStatusLabel->setText(text);
        if (auto *settings=findChild<QLabel *>(QStringLiteral("fobosActualIfSettings"))) settings->setText(text);
        refreshReceiverInfoOverlays();
        return;
    }
    QString text = uiText("fobos_if_actual_unavailable", "Effective IF: unavailable");
    if (hasActiveFobosDevice() && !isNetworkClientMode()) {
        unsigned int mode=0,mhz=0,flags=0,capabilities=0;
        const int result=getActiveFobosIfPlanSafely(&mode,&flags,&mhz,&capabilities);
        if (result==FOBOS_ERR_OK && (capabilities&4u)) {
            const QString selection=mode==3 ? uiText("fobos_if_selection_auto","auto") :
                (mode ? uiText("fobos_if_selection_manual","manual") : uiText("fobos_if_selection_base","base plan"));
            text=mhz ? uiText("fobos_if_actual_value","Effective IF: %1 MHz (%2)").arg(mhz).arg(selection)
                : uiText("fobos_if_actual_bypass","IF: direct input / RF bypass / awaiting tune");
            if (mode==3 && pendingSettings.centerFrequency>=2929e6 && pendingSettings.centerFrequency<2946e6)
                text += QStringLiteral(" · ")+uiText("fobos_if_table_conflict","Both IFs may have interference");
            if (fobosVerboseLoggingEnabled() && text!=fobosIfStatusLabel->text())
                qInfo().noquote() << QStringLiteral("[Fobos IF actual] RF Hz %1 mode %2 IF MHz %3 corrected %4 policy2")
                    .arg(pendingSettings.actualFrequency,0,'f',0).arg(mode).arg(mhz).arg(useCorrectedFobosCommands);
        }
    }
    fobosIfStatusLabel->setText(text);
    if (auto *settings=findChild<QLabel *>(QStringLiteral("fobosActualIfSettings"))) settings->setText(text);
    logStoppedFobosDeviceMetrics();
    refreshReceiverInfoOverlays();
}

void YourClassName::logStoppedFobosDeviceMetrics() {
    if (!fobosVerboseLoggingEnabled() || !isIdle() || (processor && processor->isRunning()) ||
        activeFobosApiKind != FobosApiKind::Agile || !hasActiveFobosDevice() || isNetworkClientMode() ||
        (fobosStoppedMetricsTimer.isValid() && fobosStoppedMetricsTimer.elapsed() < 3000)) return;
    fobosStoppedMetricsTimer.start();
    FobosDeviceMetrics fw{};
    const int result = getFobosAgileDeviceMetricsSafely(agileDevice, &fw);
    if (result == FOBOS_ERR_OK) {
        double nativeRate = 0;
        std::memcpy(&nativeRate, &fw.words[20], sizeof(nativeRate));
        qDebug() << "[Fobos DMA] stopped snapshot; revision" << fw.words[2] << "snapshot" << fw.words[3]
                 << "produced" << fw.words[4] << "consumedEvents" << fw.words[5]
                 << "commits" << fw.words[6] << "commitFailures" << fw.words[7]
                 << "bytesLowHigh" << fw.words[8] << fw.words[9]
                 << "lastBytes" << fw.words[10] << "badSizes" << fw.words[11]
                 << "lastStatus" << fw.words[12] << "flags" << fw.words[15]
                 << "ifMode" << fw.words[16] << "iqSwap" << fw.words[17]
                 << "nativeSampleRate" << nativeRate << "directSampling" << fw.words[22];
    } else if (result != FOBOS_ERR_UNSUPPORTED) qWarning() << "[Fobos DMA] stopped metrics read failed" << result;
}

void YourClassName::updateFobosIntegrity(const QVariantMap &m) {
    if (!adcOverloadStatusLabel) return;
    if (activeFobosApiKind != FobosApiKind::Agile || !hasActiveFobosDevice() ||
        !processor || !processor->isRunning()) {
        adcOverloadGuard.reset();
        return;
    }
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - m.value(QStringLiteral("utcMs")).toLongLong();
    if (age < 0 || age > 750) {
        adcOverloadGuard.reset();
        return;
    }
    if (!m.contains(QStringLiteral("generation"))) {
        adcOverloadStatusLabel->setText(uiText(QStringLiteral("adc_metrics_unavailable"), QStringLiteral("ADC: metrics unavailable")));
        adcOverloadGuard.reset();
        return;
    }
    if(fobosMetadataStatusLabel) {
        const bool enabled=m.value(QStringLiteral("metadataEnabled")).toBool();
        fobosMetadataStatusLabel->setText(enabled ?
            uiText("fobos_metadata_status","IQ metadata: %1 blocks | gaps: %2 | rejected pairs: %3")
                .arg(m.value(QStringLiteral("metadataValid")).toUInt()).arg(m.value(QStringLiteral("metadataSequenceGaps")).toUInt())
                .arg(m.value(QStringLiteral("metadataRejectedPairs")).toULongLong()) :
            uiText("fobos_metadata_legacy","IQ metadata: native stream"));
        fobosMetadataStatusLabel->setToolTip(uiText("fobos_metadata_tooltip","Firmware tune epoch: %1. Missing headers: %2; CRC errors: %3; repeated/reordered sequence: %4. Rejection includes intentional tuning/scan settling. Sequence counts start after FX3 capture, not at the ADC clock." )
            .arg(m.value(QStringLiteral("firmwareTuneEpoch")).toUInt()).arg(m.value(QStringLiteral("metadataMissing")).toUInt())
            .arg(m.value(QStringLiteral("metadataCrcErrors")).toUInt()).arg(m.value(QStringLiteral("metadataReorders")).toUInt()));
    }
    if (!adcProtectionClock.isValid()) adcProtectionClock.start();
    const quint64 epoch = m.value(QStringLiteral("epoch")).toULongLong();
    const double center = m.value(QStringLiteral("centerHz")).toDouble();
    if (epoch != adcProtectionEpoch || adcProtectionVga != pendingSettings.vgaGain ||
        center != adcProtectionCenterHz) {
        adcOverloadGuard.reset();
        adcProtectionEpoch = epoch;
        adcProtectionVga = pendingSettings.vgaGain;
        adcProtectionCenterHz = center;
    }
    const bool eligible = runState == RadioRunState::Running &&
        !liveCenterRetuneThread && queuedLiveCenterRetuneGeneration == 0 &&
        pendingSettings.inputMode == INPUT_RF && !agileScanEnabled && !spurSweepActive &&
        !receiverNoiseBaseline.isCapturing() && !receiverYFactorCalibration.isCapturing() &&
        m.value(QStringLiteral("modeFlags")).toUInt() == 0 && hardwareSettingsApplied &&
        !isNetworkClientMode() &&
        (std::abs(center - pendingSettings.centerFrequency) < 1.0 ||
         std::abs(center - pendingSettings.actualFrequency) < 1.0);
    const auto reading = adcOverloadGuard.update(
        m.value(QStringLiteral("generation")).toUInt(), m.value(QStringLiteral("inspected")).toUInt(),
        m.value(QStringLiteral("rails")).toUInt(), pendingSettings.vgaGain,
        adcOverloadProtectionEnabled, eligible, adcProtectionClock.elapsed());
    const quint64 metadataErrors = m.value(QStringLiteral("badHeaders")).toULongLong() +
        m.value(QStringLiteral("orderErrors")).toULongLong() + m.value(QStringLiteral("iqChanges")).toULongLong();
    adcOverloadStatusLabel->setText(uiText(QStringLiteral("adc_status"), QStringLiteral("ADC rails: %1% | IQ events: %2"))
        .arg(reading.valid ? QString::number(reading.railPercent, 'f', 3) : QStringLiteral("—"))
        .arg(metadataErrors));
    adcOverloadStatusLabel->setStyleSheet(reading.valid && reading.railPercent >= AdcOverloadGuard::RailThresholdPercent
        ? QStringLiteral("color: #e68a00;") : QString());
    adcOverloadStatusLabel->setToolTip(uiText(QStringLiteral("adc_status_tooltip"),
        QStringLiteral("Pre-DC ADC14 codes, rotating 1/32 sample subset. Min/max since stream start: %1 / %2. Header disagreements: %3; order errors: %4; within-transfer I/Q changes: %5; short USB transfers: %6. HF and scanning do not use automatic VGA protection."))
        .arg(m.value(QStringLiteral("minCode")).toUInt()).arg(m.value(QStringLiteral("maxCode")).toUInt())
        .arg(m.value(QStringLiteral("badHeaders")).toUInt()).arg(m.value(QStringLiteral("orderErrors")).toUInt())
        .arg(m.value(QStringLiteral("iqChanges")).toUInt()).arg(m.value(QStringLiteral("shortTransfers")).toUInt()));
    refreshReceiverInfoOverlays();
    if (fobosVerboseLoggingEnabled() &&
        (!fobosDeviceMetricsTimer.isValid() || fobosDeviceMetricsTimer.elapsed() >= 3000)) {
        fobosDeviceMetricsTimer.start();
        qDebug() << "[Fobos ADC/IQ]" << m << "railPercent" << reading.railPercent
                 << "validWindow" << reading.valid << "autoProtection" << adcOverloadProtectionEnabled;
    }
    if (reading.requestedVga >= 0) {
        const int before = pendingSettings.vgaGain;
        const int result = setFobosAgileVgaGainSafely(agileDevice, static_cast<unsigned>(reading.requestedVga));
        if (result == FOBOS_ERR_OK) {
            pendingSettings.vgaGain = reading.requestedVga;
            appliedHardwareSettings.vgaGain = reading.requestedVga;
            if (vgaGainSlider) {
                const QSignalBlocker blocker(vgaGainSlider);
                vgaGainSlider->setValue(reading.requestedVga);
            }
            vgaGainLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("vga_gain"),QStringLiteral("VGA Gain"))).arg(reading.requestedVga));
            qInfo() << "[ADC protection] VGA" << before << "->" << reading.requestedVga
                    << "ADC railPercent" << reading.railPercent;
            savePersistentSettings();
        } else qWarning() << "[ADC protection] VGA change failed" << result;
    }
}

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
    if (uhdSettingsButton) {
        const bool local = isUhdNativeSelected() && !isNetworkClientMode();
        const bool remote = isNetworkClientMode() && remoteReceiverDeviceListValid &&
            networkClientConfirmedSettingsJson.value(QStringLiteral("uhdRx")).isObject();
        const bool canConfigureUhd = (local && (isIdle() || runState == RadioRunState::Running)) ||
            (remote && networkController && networkController->isControlReady() &&
             !isRemoteVfoProcessingMode());
        uhdSettingsButton->setVisible(local || remote);
        uhdSettingsButton->setEnabled(canConfigureUhd);
        if (multiSdrDialog) {
            multiSdrDialog->setPrimaryUhdSettingsAvailable(local || remote, canConfigureUhd);
            multiSdrDialog->setRemoteUhdCompanionChannel(remoteUhdCompanionChannel());
        }
    }
    if (virtualSignalSettingsButton) {
        const bool selected = isVirtualSignalSelected();
        virtualSignalSettingsButton->setVisible(selected);
        virtualSignalSettingsButton->setEnabled(selected && isIdle());
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

    double displayOverallMin = overallMin;
    double displayOverallMax = overallMax;
    AnalyzerSpectrumFrame multiSdrFrame;
    const bool multiSdrDisplayActive =
        multiSdrDialog && multiSdrDialog->mainMosaicFrame(multiSdrFrame);
    if (multiSdrDisplayActive) {
        displayOverallMin = multiSdrFrame.minimumHz;
        displayOverallMax = multiSdrFrame.maximumHz;
    }

    const double availableRange =
        (std::max)(1.0, displayOverallMax - displayOverallMin);
    double newRange = availableRange * (effectiveScalePercent() / 100.0);
    newRange = (std::clamp)(newRange, 1.0, availableRange);

    if (!std::isfinite(spectrumDisplayCenterHz) ||
        spectrumDisplayCenterHz < displayOverallMin ||
        spectrumDisplayCenterHz > displayOverallMax) {
        spectrumDisplayCenterHz = multiSdrDisplayActive
                                      ? multiSdrFrame.centerHz
                                      : (std::clamp)(pendingSettings.listeningFrequency,
                                                     displayOverallMin,
                                                     displayOverallMax);
    }

    const double minCenter = displayOverallMin + newRange / 2.0;
    const double maxCenter = displayOverallMax - newRange / 2.0;
    if (maxCenter >= minCenter) {
        spectrumDisplayCenterHz = (std::clamp)(spectrumDisplayCenterHz, minCenter, maxCenter);
    } else {
        spectrumDisplayCenterHz = displayOverallMin + availableRange / 2.0;
    }

	double newMin = spectrumDisplayCenterHz - newRange / 2.0;
    newMin = (std::clamp)(newMin,
                          displayOverallMin,
                          displayOverallMax - newRange);
    double newMax = newMin + newRange;
    minFrequency = newMin;
    maxFrequency = newMax;
    scaleWidget->setTuning(
        pendingSettings.listeningFrequency,
        multiSdrDisplayActive ? spectrumDisplayCenterHz : globalFrequency,
        effectiveDemodulationBandwidth(globalBandwidth,
                                       globalModulationType,
                                       pendingSettings.sstvDemodulationMode),
        effectiveDemodulationModulationType(globalModulationType,
                                            pendingSettings.sstvDemodulationMode));
    scaleWidget->setRange(minFrequency, maxFrequency);
    if (isNetworkClientMode() &&
        networkProcessingMode == NetworkProcessingMode::ServerSide &&
        networkController && networkController->isControlReady()) {
        QJsonObject viewport;
        viewport.insert(QStringLiteral("centerHz"), 0.5 * (minFrequency + maxFrequency));
        viewport.insert(QStringLiteral("spanHz"), maxFrequency - minFrequency);
        sendRemoteControlCommand(QStringLiteral("setViewport"), viewport);
    }
    updateFineTuneLabel();
}

void YourClassName::onCheckboxStateChanged(int state) {
    QCheckBox *senderCheckbox = qobject_cast<QCheckBox*>(sender());
    if (senderCheckbox) {
        const uint8_t value = currentGpoValue();
        pendingSettings.gpoValue = value;
        qDebug() << "Checkbox state changed. New GPO value:" << value;
        if (liveCenterRetuneThread && hasActiveFobosDevice()) {
            liveRetuneDeferredHardwareChange = true;
        } else if (hasActiveFobosDevice() && !isIdle() && hardwareSettingsApplied) {
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

    if (liveCenterRetuneThread && hasActiveFobosDevice()) {
        liveRetuneDeferredHardwareChange = true;
    } else if (hasActiveFobosDevice() && !isIdle() && hardwareSettingsApplied) {
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

    if (liveCenterRetuneThread && hasActiveFobosDevice()) {
        liveRetuneDeferredHardwareChange = true;
    } else if (hasActiveFobosDevice() && !isIdle() && hardwareSettingsApplied) {
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
