#include "main.h"
#include "audiofilterchainwidget.h"
#include "appconstants.h"
#include "appsettingsutils.h"
#include "diagnosticlogging.h"
#include "dmrbackendpaths.h"
#include "dmrprivacyutils.h"
#include "gnssqthhelpers.h"
#include "gnssserialutils.h"
#include "modulationutils.h"
#include "presethelpers.h"
#include "receiverdeviceutils.h"
#include "scanvisualutils.h"
#include "tuningutils.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>

extern bool secondGraph;
extern bool syncWariable;
extern float sensitivity;
extern float contrast;
extern bool colorf;
void YourClassName::updateUiFromPendingSettings() {
    if (comboBox) {
        comboBox->blockSignals(true);
        const int deviceComboValue =
            isNetworkClientMode() && remoteReceiverDeviceListValid
                ? remoteReceiverComboValue(pendingSettings.deviceIndex)
                : pendingSettings.deviceIndex;
        int deviceComboIndex = comboBox->findData(deviceComboValue);
        if (deviceComboIndex < 0 &&
            pendingSettings.deviceIndex >= 0 &&
            pendingSettings.deviceIndex < comboBox->count()) {
            deviceComboIndex = pendingSettings.deviceIndex;
        }
        if (deviceComboIndex >= 0) {
            comboBox->setCurrentIndex(deviceComboIndex);
            bool ok = false;
            const int selectedDevice = comboBox->itemData(deviceComboIndex).toInt(&ok);
            if (ok) {
                pendingSettings.deviceIndex = receiverDeviceIndexFromComboValue(selectedDevice);
            }
        }
        comboBox->blockSignals(false);
    }
    updateQuickHamPanel();
    rebuildReceiverModeChoices();
    updateReceiverSpecificControls();
    if (clkBox) {
        clkBox->blockSignals(true);
        const int index = clkBox->findData(pendingSettings.clockSource);
        if (index >= 0) {
            clkBox->setCurrentIndex(index);
        }
        clkBox->blockSignals(false);
    }
    if (modeBox) {
        modeBox->blockSignals(true);
        const int index = modeBox->findData(pendingSettings.inputMode);
        if (index >= 0) {
            modeBox->setCurrentIndex(index);
        }
        modeBox->blockSignals(false);
    }
    if (sampleBox) {
        populateSampleRates();
        sampleBox->blockSignals(true);
        int bestIndex = -1;
        double bestDelta = std::numeric_limits<double>::max();
        for (int i = 0; i < sampleBox->count(); ++i) {
            bool ok = false;
            const double value = sampleBox->itemData(i).toDouble(&ok);
            if (!ok) {
                continue;
            }
            const double delta = std::abs(value - pendingSettings.sampleRate);
            if (delta < bestDelta) {
                bestDelta = delta;
                bestIndex = i;
            }
        }
        if (bestIndex >= 0 && bestDelta <= 0.5) {
            sampleBox->setCurrentIndex(bestIndex);
        } else {
            sampleBox->addItem(formatSampleRate(pendingSettings.sampleRate), pendingSettings.sampleRate);
            sampleBox->setCurrentIndex(sampleBox->count() - 1);
        }
        sampleBox->blockSignals(false);
    }
    if (frequencyControl) {
        QSignalBlocker blocker(frequencyControl);
        frequencyControl->setValueHz(pendingSettings.centerFrequency);
        if (frequencyControlUiStateRestorePending) {
            frequencyControl->setSelectedStepName(centerFrequencyStepName);
            frequencyControl->setSelectedValuePresetName(centerFrequencyPresetName);
            frequencyControl->setSelectedUnitIndex(centerFrequencyUnitIndex);
        }
    }
    if (listeningFrequencyControl) {
        QSignalBlocker blocker(listeningFrequencyControl);
        if (pendingSettings.inputMode == INPUT_RF) {
            listeningFrequencyControl->setRangeHz(
                rfMinimumListeningFrequency(pendingSettings),
                rfMaximumFrequency(pendingSettings));
        } else {
            listeningFrequencyControl->setRangeHz(directMinFrequencyForMode(pendingSettings.inputMode,
                                                                            pendingSettings.sampleRate),
                                                  directMaxFrequency(pendingSettings.sampleRate));
        }
        listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
        if (frequencyControlUiStateRestorePending) {
            listeningFrequencyControl->setSelectedStepName(listeningFrequencyStepName);
            listeningFrequencyControl->setSelectedValuePresetName(listeningFrequencyPresetName);
            listeningFrequencyControl->setSelectedUnitIndex(listeningFrequencyUnitIndex);
        }
    }
    if (bandwidthControl) {
        QSignalBlocker blocker(bandwidthControl);
        bandwidthControl->setValueHz(pendingSettings.bandwidth);
        if (frequencyControlUiStateRestorePending) {
            bandwidthControl->setSelectedStepName(bandwidthStepName);
            bandwidthControl->setSelectedValuePresetName(bandwidthPresetName);
            bandwidthControl->setSelectedUnitIndex(bandwidthUnitIndex);
        }
    }
    frequencyControlUiStateRestorePending = false;
    syncFftResolutionControls();
    if (modulationButtonGroup) {
        QAbstractButton *button = modulationButtonGroup->button(pendingSettings.modulationType);
        if (button) {
            modulationButtonGroup->blockSignals(true);
            button->setChecked(true);
            modulationButtonGroup->blockSignals(false);
        }
    }
    if (lnaGainSlider) {
        lnaGainSlider->blockSignals(true);
        lnaGainSlider->setValue(isHackRfNativeSelected()
                                    ? pendingSettings.hackRfLnaGainDb
                                    : pendingSettings.lnaGain);
        lnaGainSlider->blockSignals(false);
    }
    if (lnaGainLabel) {
        lnaGainLabel->setText(isHackRfNativeSelected()
                                  ? QStringLiteral("HackRF LNA: %1 dB").arg(pendingSettings.hackRfLnaGainDb)
                                  : QStringLiteral("%1: %2").arg(uiText(QStringLiteral("lna_gain"), QStringLiteral("LNA Gain"))).arg(pendingSettings.lnaGain));
    }
    if (vgaGainSlider) {
        vgaGainSlider->blockSignals(true);
        vgaGainSlider->setValue(isHackRfNativeSelected()
                                    ? pendingSettings.hackRfVgaGainDb
                                    : pendingSettings.vgaGain);
        vgaGainSlider->blockSignals(false);
    }
    if (vgaGainLabel) {
        vgaGainLabel->setText(isHackRfNativeSelected()
                                  ? QStringLiteral("HackRF VGA: %1 dB").arg(pendingSettings.hackRfVgaGainDb)
                                  : QStringLiteral("%1: %2").arg(uiText(QStringLiteral("vga_gain"), QStringLiteral("VGA Gain"))).arg(pendingSettings.vgaGain));
    }
    if (audioCheckbox) {
        audioCheckbox->blockSignals(true);
        audioCheckbox->setChecked(pendingSettings.audioEnabled);
        audioCheckbox->blockSignals(false);
    }
    if (audioDeviceComboBox) {
        audioDeviceComboBox->blockSignals(true);
        const int index = audioDeviceComboBox->findData(pendingSettings.audioDeviceId);
        if (index >= 0) {
            audioDeviceComboBox->setCurrentIndex(index);
        }
        audioDeviceComboBox->blockSignals(false);
    }
    for (int i = 0; i < 8; ++i) {
        if (checkBoxes[i]) {
            checkBoxes[i]->blockSignals(true);
            checkBoxes[i]->setChecked((pendingSettings.gpoValue & (1 << i)) != 0);
            checkBoxes[i]->blockSignals(false);
        }
    }
    if (scaleWidget) {
        scaleWidget->setTuning(pendingSettings.listeningFrequency,
                               pendingSettings.centerFrequency,
                               effectiveDemodulationBandwidth(
                                   pendingSettings.bandwidth,
                                   pendingSettings.modulationType,
                                   pendingSettings.sstvDemodulationMode),
                               effectiveDemodulationModulationType(
                                   pendingSettings.modulationType,
                                   pendingSettings.sstvDemodulationMode));
    }
    if (scaleSlider) {
        scaleSlider->blockSignals(true);
        scaleSlider->setValue(scalePercentToSliderValue(currentScale));
        scaleSlider->blockSignals(false);
    }
    if (scaleLabel) {
        scaleLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("scale"), QStringLiteral("Scale")),
                                                        formatScalePercent(currentScale)));
    }
    if (additionalScaleDivisorSlider) {
        additionalScaleDivisorSlider->blockSignals(true);
        additionalScaleDivisorSlider->setValue((std::clamp)(additionalScaleDivisor,
                                                             1,
                                                             ADDITIONAL_SCALE_DIVISOR_MAX));
        additionalScaleDivisorSlider->blockSignals(false);
    }
    if (additionalScaleDivisorLabel) {
        additionalScaleDivisorLabel->setText(
            QStringLiteral("%1: 1:%2")
                .arg(uiText(QStringLiteral("additional_scale"), QStringLiteral("Extra zoom")))
                .arg((std::clamp)(additionalScaleDivisor,
                                  1,
                                  ADDITIONAL_SCALE_DIVISOR_MAX)));
    }
    if (contrastSlider) {
        contrastSlider->blockSignals(true);
        contrastSlider->setValue(static_cast<int>(std::lround(contrast)));
        contrastSlider->blockSignals(false);
    }
    if (contrastLabel) {
        contrastLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("contrast"), QStringLiteral("Contrast"))).arg(contrast));
    }
    if (sensitivitySlider) {
        sensitivitySlider->blockSignals(true);
        sensitivitySlider->setValue(static_cast<int>(std::lround(sensitivity)));
        sensitivitySlider->blockSignals(false);
    }
    if (sensitivityLabel) {
        sensitivityLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("sensitivity"), QStringLiteral("Sensitivity"))).arg(sensitivity));
    }
    if (levelMinSlider) {
        levelMinSlider->blockSignals(true);
        levelMinSlider->setValue(levelToSliderValue(displayLevelMin));
        levelMinSlider->blockSignals(false);
    }
    if (levelMaxSlider) {
        levelMaxSlider->blockSignals(true);
        levelMaxSlider->setValue(levelToSliderValue(displayLevelMax));
        levelMaxSlider->blockSignals(false);
    }
    if (levelMinLabel) {
        levelMinLabel->setText(levelLabelText(uiText(QStringLiteral("min"), QStringLiteral("Min")), displayLevelMin));
    }
    if (levelMaxLabel) {
        levelMaxLabel->setText(levelLabelText(uiText(QStringLiteral("max"), QStringLiteral("Max")), displayLevelMax));
    }
    if (graphCheckbox) {
        graphCheckbox->blockSignals(true);
        graphCheckbox->setChecked(secondGraph);
        graphCheckbox->blockSignals(false);
    }
    if (colorCheckbox) {
        colorCheckbox->blockSignals(true);
        colorCheckbox->setChecked(colorf);
        colorCheckbox->blockSignals(false);
    }
    if (volumeSlider) {
        volumeSlider->blockSignals(true);
        volumeSlider->setValue(volumePercent);
        volumeSlider->blockSignals(false);
    }
    if (volumeLabel) {
        volumeLabel->setText(QStringLiteral("%1: %2%").arg(uiText(QStringLiteral("volume"), QStringLiteral("Volume"))).arg(volumePercent));
    }
    if (audioLowPassSlider) {
        audioLowPassSlider->blockSignals(true);
        audioLowPassSlider->setValue(audioLowPassHzToSliderValue(pendingSettings.audioLowPassHz));
        audioLowPassSlider->blockSignals(false);
    }
    if (audioHighPassSlider) {
        audioHighPassSlider->blockSignals(true);
        audioHighPassSlider->setValue(audioHighPassHzToSliderValue(pendingSettings.audioHighPassHz));
        audioHighPassSlider->blockSignals(false);
    }
    if (audioFilterChainWidget) {
        audioFilterChainWidget->setConfigurationJson(pendingSettings.audioFilterChainJson);
    }
    if (hfNoiseCancelDepthSlider) {
        hfNoiseCancelDepthSlider->blockSignals(true);
        hfNoiseCancelDepthSlider->setValue(hfNoiseCancelDepthToSliderValue(pendingSettings.hfNoiseCancelDepth));
        hfNoiseCancelDepthSlider->blockSignals(false);
    }
    if (hfNoiseCancelRefGainSlider) {
        hfNoiseCancelRefGainSlider->blockSignals(true);
        hfNoiseCancelRefGainSlider->setValue(hfNoiseCancelRefGainToSliderValue(pendingSettings.hfNoiseCancelRefGainDb));
        hfNoiseCancelRefGainSlider->blockSignals(false);
    }
    if (hfNoiseCancelRefDelaySlider) {
        hfNoiseCancelRefDelaySlider->blockSignals(true);
        hfNoiseCancelRefDelaySlider->setValue(hfNoiseCancelRefDelayToSliderValue(pendingSettings.hfNoiseCancelRefDelayNs));
        hfNoiseCancelRefDelaySlider->blockSignals(false);
    }
    if (hfNoiseCancelRefTiltSlider) {
        hfNoiseCancelRefTiltSlider->blockSignals(true);
        hfNoiseCancelRefTiltSlider->setValue(hfNoiseCancelRefTiltToSliderValue(pendingSettings.hfNoiseCancelRefTiltDb));
        hfNoiseCancelRefTiltSlider->blockSignals(false);
    }
    if (hfNoiseCancelFreezeCheckbox) {
        hfNoiseCancelFreezeCheckbox->blockSignals(true);
        hfNoiseCancelFreezeCheckbox->setChecked(pendingSettings.hfNoiseCancelFreeze);
        hfNoiseCancelFreezeCheckbox->blockSignals(false);
    }
    if (hfAudioBlankerCheckbox) {
        QSignalBlocker blocker(hfAudioBlankerCheckbox);
        hfAudioBlankerCheckbox->setChecked(pendingSettings.hfAudioBlankerEnabled);
    }
    if (hfAudioBlankerThresholdSlider) {
        QSignalBlocker blocker(hfAudioBlankerThresholdSlider);
        hfAudioBlankerThresholdSlider->setValue(static_cast<int>(std::lround(
            (std::clamp)(pendingSettings.hfAudioBlankerThreshold, 2.0, 20.0) * 10.0)));
    }
    if (spectrumFrameBufferCheckbox) {
        QSignalBlocker blocker(spectrumFrameBufferCheckbox);
        spectrumFrameBufferCheckbox->setChecked(spectrumFrameBufferEnabled);
    }
    if (spectrumFramePrebufferSpin) {
        QSignalBlocker blocker(spectrumFramePrebufferSpin);
        spectrumFramePrebufferSpin->setValue(spectrumFramePrebufferSeconds);
    }
    if (spectrumEventModeCombo) {
        QSignalBlocker blocker(spectrumEventModeCombo);
        const int index = spectrumEventModeCombo->findData(spectrumEventCaptureMode);
        if (index >= 0) {
            spectrumEventModeCombo->setCurrentIndex(index);
        }
    }
    if (hfInterferenceBaselineCheckbox) {
        QSignalBlocker blocker(hfInterferenceBaselineCheckbox);
        hfInterferenceBaselineCheckbox->setChecked(hfInterferenceBaselineEnabled);
    }
    if (hfInterferenceRawOverlayCheckbox) {
        QSignalBlocker blocker(hfInterferenceRawOverlayCheckbox);
        hfInterferenceRawOverlayCheckbox->setChecked(hfInterferenceRawOverlayEnabled);
    }
    if (hfInterferenceBaselineDepthSlider) {
        QSignalBlocker blocker(hfInterferenceBaselineDepthSlider);
        hfInterferenceBaselineDepthSlider->setValue(static_cast<int>(std::lround(hfInterferenceBaselineDepth * 100.0)));
    }
    if (hfInterferenceBaselineSmoothSlider) {
        QSignalBlocker blocker(hfInterferenceBaselineSmoothSlider);
        hfInterferenceBaselineSmoothSlider->setValue(hfInterferenceBaselineSmoothBins);
    }
    if (agileScanCheckbox) {
        QSignalBlocker blocker(agileScanCheckbox);
        agileScanCheckbox->setChecked(agileScanEnabled);
    }
    if (agileScanAutoStepCheckbox) {
        QSignalBlocker blocker(agileScanAutoStepCheckbox);
        agileScanAutoStepCheckbox->setChecked(agileScanAutoStepSampleRate);
    }
    applyAgileScanAutoStep(false);
    if (agileScanRangesEdit) {
        QSignalBlocker blocker(agileScanRangesEdit);
        agileScanRangesEdit->setText(agileScanRangesMhz);
    }
    if (agileScanStepSpin) {
        QSignalBlocker blocker(agileScanStepSpin);
        agileScanStepSpin->setValue(agileScanStepMhz);
    }
    if (scanVisualModeCombo) {
        QSignalBlocker blocker(scanVisualModeCombo);
        const int index = scanVisualModeCombo->findData(normalizedScanVisualMode(scanVisualMode));
        if (index >= 0) {
            scanVisualModeCombo->setCurrentIndex(index);
        }
    }
    if (standardScanCheckbox) {
        QSignalBlocker blocker(standardScanCheckbox);
        standardScanCheckbox->setChecked(standardScanEnabled);
    }
    if (scanListeningLockCheckbox) {
        QSignalBlocker blocker(scanListeningLockCheckbox);
        scanListeningLockCheckbox->setChecked(scanListeningLockEnabled);
    }
    if (standardScanCentersEdit) {
        QSignalBlocker blocker(standardScanCentersEdit);
        standardScanCentersEdit->setText(standardScanCentersMhz);
    }
    if (standardScanDwellSpin) {
        QSignalBlocker blocker(standardScanDwellSpin);
        standardScanDwellSpin->setValue(standardScanDwellMs);
    }
    if (standardScanSettleSpin) {
        QSignalBlocker blocker(standardScanSettleSpin);
        standardScanSettleSpin->setValue(standardScanSettleMs);
    }
    if (standardScanRangeStartEdit) {
        QSignalBlocker blocker(standardScanRangeStartEdit);
        standardScanRangeStartEdit->setText(standardScanRangeStartMhz);
    }
    if (standardScanRangeEndEdit) {
        QSignalBlocker blocker(standardScanRangeEndEdit);
        standardScanRangeEndEdit->setText(standardScanRangeEndMhz);
    }
    if (listeningScanCheckbox) {
        QSignalBlocker blocker(listeningScanCheckbox);
        listeningScanCheckbox->setChecked(listeningScanEnabled);
    }
    if (listeningScanTargetsEdit) {
        QSignalBlocker blocker(listeningScanTargetsEdit);
        listeningScanTargetsEdit->setText(listeningScanTargetsMhz);
    }
    if (listeningScanDwellSpin) {
        QSignalBlocker blocker(listeningScanDwellSpin);
        listeningScanDwellSpin->setValue(listeningScanDwellMs);
    }
    if (listeningScanSettleSpin) {
        QSignalBlocker blocker(listeningScanSettleSpin);
        listeningScanSettleSpin->setValue(listeningScanSettleMs);
    }
    if (qthSourceCombo) {
        QSignalBlocker blocker(qthSourceCombo);
        const int index = qthSourceCombo->findData(qthSource);
        if (index >= 0) {
            qthSourceCombo->setCurrentIndex(index);
        }
    }
    if (gnssIntegrationSpin) {
        QSignalBlocker blocker(gnssIntegrationSpin);
        gnssIntegrationSpin->setValue(gnssAcquisitionIntegrationMs);
    }
    if (gnssChannelFilterSpin) {
        QSignalBlocker blocker(gnssChannelFilterSpin);
        gnssChannelFilterSpin->setValue(gnssChannelFilterCutoffHz / 1000000.0);
    }
    if (gnssDopplerSpanSpin) {
        QSignalBlocker blocker(gnssDopplerSpanSpin);
        gnssDopplerSpanSpin->setValue((std::clamp)(gnssDopplerSpanHz / 1000, 1, 50));
    }
    if (gnssDopplerStepSpin) {
        QSignalBlocker blocker(gnssDopplerStepSpin);
        gnssDopplerStepSpin->setValue((std::clamp)(gnssDopplerStepHz, 250, 5000));
    }
    if (gnssDeepAcquireButton) {
        QSignalBlocker blocker(gnssDeepAcquireButton);
        gnssDeepAcquireButton->setChecked(gnssContinuousAcquisitionEnabled);
    }
    if (gnssContinuousAcquisitionEnabled) {
        QTimer::singleShot(0, this, [this]() {
            if (gnssContinuousAcquisitionEnabled) {
                setGnssContinuousAcquisitionEnabled(true);
            }
        });
    }
    if (qthLatitudeSpin) {
        QSignalBlocker blocker(qthLatitudeSpin);
        qthLatitudeSpin->setValue(qthLatitude);
    }
    if (qthLongitudeSpin) {
        QSignalBlocker blocker(qthLongitudeSpin);
        qthLongitudeSpin->setValue(qthLongitude);
    }
    if (scanMeasurementCheckbox) {
        QSignalBlocker blocker(scanMeasurementCheckbox);
        scanMeasurementCheckbox->setChecked(scanMeasurementEnabled);
    }
    if (scanMeasurementBinSpin) {
        QSignalBlocker blocker(scanMeasurementBinSpin);
        scanMeasurementBinSpin->setValue(scanMeasurementBinMhz);
    }
    if (spectrumScienceMaxHoldCheckbox) {
        QSignalBlocker blocker(spectrumScienceMaxHoldCheckbox);
        spectrumScienceMaxHoldCheckbox->setChecked(spectrumScienceMaxHoldEnabled);
    }
    if (spectrumScienceMinHoldCheckbox) {
        QSignalBlocker blocker(spectrumScienceMinHoldCheckbox);
        spectrumScienceMinHoldCheckbox->setChecked(spectrumScienceMinHoldEnabled);
    }
    if (spectrumScienceAverageCheckbox) {
        QSignalBlocker blocker(spectrumScienceAverageCheckbox);
        spectrumScienceAverageCheckbox->setChecked(spectrumScienceAverageEnabled);
    }
    if (spectrumScienceAverageSpin) {
        QSignalBlocker blocker(spectrumScienceAverageSpin);
        spectrumScienceAverageSpin->setValue(spectrumScienceAverageSeconds);
    }
    spectrumScienceAnalyzer.setTraceEnabled(spectrumScienceMaxHoldEnabled,
                                            spectrumScienceMinHoldEnabled,
                                            spectrumScienceAverageEnabled);
    spectrumScienceAnalyzer.setAverageTimeSeconds(spectrumScienceAverageSeconds);
    if (dmrHunterControls) {
        dmrHunterControls->setDetectChecked(dmrHunterSettings.enabled);
        dmrHunterControls->setWidthValues(dmrHunterSettings.minWidthKhz,
                                          dmrHunterSettings.maxWidthKhz,
                                          dmrHunterSettings.thresholdDb);
    }
    if (fpvHunterControls) {
        fpvHunterControls->setDetectChecked(fpvHunterSettings.enabled);
        fpvHunterControls->setWidthValues(fpvHunterSettings.minWidthMhz,
                                          fpvHunterSettings.maxWidthMhz,
                                          fpvHunterSettings.thresholdDb);
        fpvHunterControls->setAutoScanChecked(fpvAutoScanArmed);
        fpvHunterControls->setAutoScanValues(fpvAutoScanDwellMs,
                                             fpvAutoScanLossTimeoutMs,
                                             fpvAutoScanMinimumLockPercent);
    }
    if (digitalVideoHunterControls) {
        digitalVideoHunterControls->setDetectChecked(digitalVideoHunterSettings.enabled);
        digitalVideoHunterControls->setWidthValues(digitalVideoHunterSettings.minWidthMhz,
                                                   digitalVideoHunterSettings.maxWidthMhz,
                                                   digitalVideoHunterSettings.thresholdDb);
    }
    if (scanMeasurementBaselineButton) {
        QSignalBlocker blocker(scanMeasurementBaselineButton);
        scanMeasurementBaselineButton->setChecked(scanMeasurementBaselineRecording);
        scanMeasurementBaselineButton->setText(scanMeasurementBaselineRecording
                                                   ? uiText(QStringLiteral("stop_bg"), QStringLiteral("Stop BG"))
                                                   : uiText(QStringLiteral("bg_rec"), QStringLiteral("BG Rec")));
    }
    updateAgileScanControls();
    updateQthControls();
    updateScanMeasurementStatus();
    updateDmrHunterControls();
    updateFpvHunterControls();
    updateDigitalVideoHunterControls();
    updateAudioFilterLabels();
    updateHfNoiseCancelControls();
    if (digitalDecodeCheckbox) {
        digitalDecodeCheckbox->blockSignals(true);
        digitalDecodeCheckbox->setChecked(digitalDecodeEnabled);
        digitalDecodeCheckbox->blockSignals(false);
    }
    if (dmrBasebandRateCombo) {
        QSignalBlocker blocker(dmrBasebandRateCombo);
        const int index = dmrBasebandRateCombo->findData(
            normalizedDmrBasebandSampleRate(pendingSettings.dmrBasebandSampleRate));
        if (index >= 0) {
            dmrBasebandRateCombo->setCurrentIndex(index);
        }
    }
    if (dmrAmbeLayoutCombo) {
        QSignalBlocker blocker(dmrAmbeLayoutCombo);
        const int index = dmrAmbeLayoutCombo->findData(
            normalizedDmrAmbeLayout(pendingSettings.dmrAmbeLayout));
        if (index >= 0) {
            dmrAmbeLayoutCombo->setCurrentIndex(index);
        }
    }
    if (dmrManualTimingCheckbox) {
        QSignalBlocker blocker(dmrManualTimingCheckbox);
        dmrManualTimingCheckbox->setChecked(pendingSettings.dmrManualTimingEnabled);
    }
    if (dmrTimingOffsetSpin) {
        QSignalBlocker blocker(dmrTimingOffsetSpin);
        dmrTimingOffsetSpin->setValue(pendingSettings.dmrManualTimingOffset);
        dmrTimingOffsetSpin->setEnabled(!dmrManualTimingCheckbox ||
                                        dmrManualTimingCheckbox->isChecked());
    }
    if (dmrSlicerRatioSpin) {
        QSignalBlocker blocker(dmrSlicerRatioSpin);
        dmrSlicerRatioSpin->setValue(pendingSettings.dmrSlicerRatio);
    }
    if (dmrAdaptiveSlicerCheckbox) {
        QSignalBlocker blocker(dmrAdaptiveSlicerCheckbox);
        dmrAdaptiveSlicerCheckbox->setChecked(pendingSettings.dmrAdaptiveSlicer);
    }
    if (videoDecodeCheckbox) {
        videoDecodeCheckbox->blockSignals(true);
        videoDecodeCheckbox->setChecked(videoDecodeEnabled);
        videoDecodeCheckbox->blockSignals(false);
    }
    const float volume = volumePercent / 100.0f;
    if (audioProcessor) {
        audioProcessor->setVolume(volume);
    }
    if (remoteAudioPlayer) {
        remoteAudioPlayer->setVolume(volume);
    }
    if (graphWidget) {
        graphWidget->setLevelRange(displayLevelMin, displayLevelMax);
    }
    if (waterfallWidget) {
        waterfallWidget->setLevelRange(displayLevelMin, displayLevelMax);
    }
    applyUiLanguage();
    updateSpectrumTimerInterval();
    settingRange();
    updateDigitalDecoderMode();
    updateVideoProcessorMode();
}
