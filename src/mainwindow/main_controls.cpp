#include "main.h"
#include "appconstants.h"
#include "appruntimeutils.h"
#include "diagnosticlogging.h"
#include "finetunewidget.h"
#include "modulationutils.h"
#include "receiverdeviceutils.h"
#include "tuningutils.h"

#include <QAction>
#if !defined(_WIN32) && defined(FOBOSAPP_HAS_QT_AUDIO)
#include <QAudioDeviceInfo>
#endif
#include <QDebug>
#include <QMenu>
#include <QPoint>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

extern double globalFrequency;
extern double actualFrequency;
extern double listeningFrequency;
extern double globalBandwidth;
extern int globalModulationType;
extern double currentScale;
extern bool secondGraph;
extern bool syncWariable;
extern float sensitivity;
extern float contrast;
extern bool colorf;
void YourClassName::updateFrequency() {
   const double frequency = scaleWidget ? scaleWidget->currentListeningFrequency() : listeningFrequency;
   if (listeningFrequencyControl) {
       listeningFrequencyControl->setValueHz(frequency);
   }
   onListeningFrequencyEntered();
}

void YourClassName::updateCentralFrequency() {
   if (pendingSettings.inputMode != INPUT_RF) {
       normalizeTuning(pendingSettings);
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
       return;
   }
   const double frequency = scaleWidget ? scaleWidget->currentCenterFrequency() : globalFrequency;
   if (frequencyControl) {
       frequencyControl->setValueHz(frequency);
   }
   onFrequencyEntered();
}

void YourClassName::updateTuningFromScale(double tunedListeningFrequency, double tunedCenterFrequency) {
    const RadioSettings previousSettings = pendingSettings;
    pendingSettings.listeningFrequency = tunedListeningFrequency;
    pendingSettings.centerFrequency = tunedCenterFrequency;
    normalizeTuning(pendingSettings);

    applyCenterFrequencyToHardwareIfNeeded(previousSettings, "scale");

    publishSettingsToGlobals();
    if (frequencyControl) {
        QSignalBlocker blocker(frequencyControl);
        frequencyControl->setValueHz(pendingSettings.centerFrequency);
    }
    if (listeningFrequencyControl) {
        QSignalBlocker blocker(listeningFrequencyControl);
        listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
    }
    settingRange();
    updateQuickHamPanel();
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand(0);
    }
}

void YourClassName::updateQuickHamPanel() {
    if (quickHamBandButtonGroup) {
        const double frequency = pendingSettings.inputMode == INPUT_RF
                                     ? pendingSettings.centerFrequency
                                     : pendingSettings.listeningFrequency;
        for (QAbstractButton *button : quickHamBandButtonGroup->buttons()) {
            const double low = button->property("bandLowHz").toDouble();
            const double high = button->property("bandHighHz").toDouble();
            const QSignalBlocker blocker(button);
            button->setChecked(std::isfinite(frequency) && frequency >= low && frequency <= high);
        }
    }
    if (quickHamModeButtonGroup) {
        for (QAbstractButton *button : quickHamModeButtonGroup->buttons()) {
            const QSignalBlocker blocker(button);
            button->setChecked(quickHamModeButtonGroup->id(button) ==
                               pendingSettings.modulationType);
        }
    }
}

void YourClassName::showTuneContextMenu(double frequency, const QPoint &globalPos) {
    if (!std::isfinite(frequency)) {
        return;
    }

    const QString frequencyText = QString::number(frequency / 1e6, 'f', 6) + " MHz";
    QMenu menu(this);
    QAction *tuneCenterAction = menu.addAction("Tune signal center here (" + frequencyText + ")");
    QAction *usbEdgeAction = menu.addAction("Set USB lower edge here");
    QAction *lsbEdgeAction = menu.addAction("Set LSB upper edge here");
    menu.addSeparator();
    QAction *centerReceiverAction = menu.addAction("Center receiver here");

    QAction *selected = menu.exec(globalPos);
    if (!selected) {
        return;
    }

    if (selected == tuneCenterAction) {
        tuneSignalCenterAt(frequency);
    } else if (selected == usbEdgeAction) {
        tuneSidebandEdgeAt(frequency, MOD_USB);
    } else if (selected == lsbEdgeAction) {
        tuneSidebandEdgeAt(frequency, MOD_LSB);
    } else if (selected == centerReceiverAction) {
        centerReceiverAt(frequency);
    }
}

void YourClassName::tuneSignalCenterAt(double frequency) {
    double listeningTarget = frequency;
    const int effectiveModulationType = effectiveDemodulationModulationType(
        pendingSettings.modulationType,
        pendingSettings.sstvDemodulationMode);
    if (isUpperSidebandMode(effectiveModulationType)) {
        listeningTarget = frequency - pendingSettings.bandwidth * 0.5;
    } else if (isLowerSidebandMode(effectiveModulationType)) {
        listeningTarget = frequency + pendingSettings.bandwidth * 0.5;
    }
    const double visibleSpanHz = std::isfinite(maxFrequency) && std::isfinite(minFrequency) && maxFrequency > minFrequency
                                     ? maxFrequency - minFrequency
                                     : pendingSettings.sampleRate * (effectiveScalePercent() / 100.0);
    const double roundedTarget = roundAutoTuneFrequencyHz(listeningTarget, visibleSpanHz);
    qDebug() << "[Tune] auto center"
             << "detected" << frequency
             << "target" << listeningTarget
             << "rounded" << roundedTarget
             << "step" << autoTuneRoundingStepHz(listeningTarget, visibleSpanHz)
             << "visibleSpan" << visibleSpanHz;
    updateTuningFromScale(roundedTarget, pendingSettings.centerFrequency);
}

void YourClassName::tuneSidebandEdgeAt(double frequency, int modulationType) {
    if (pendingSettings.modulationType != modulationType) {
        if (modulationButtonGroup) {
            if (QAbstractButton *button = modulationButtonGroup->button(modulationType)) {
                modulationButtonGroup->blockSignals(true);
                button->setChecked(true);
                modulationButtonGroup->blockSignals(false);
            }
        }
        onModulationChanged(modulationType);
    }
    updateTuningFromScale(frequency, pendingSettings.centerFrequency);
}

void YourClassName::centerReceiverAt(double frequency) {
    spectrumDisplayCenterHz = frequency;
    updateTuningFromScale(pendingSettings.listeningFrequency, frequency);
}

void YourClassName::panSpectrumView(int deltaPixels, int widthPixels) {
    if (deltaPixels == 0 || widthPixels <= 0 || !std::isfinite(minFrequency) || !std::isfinite(maxFrequency)) {
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

    const double visibleSpan = maxFrequency - minFrequency;
    const double overallSpan = overallMax - overallMin;
    if (!std::isfinite(visibleSpan) || visibleSpan <= 0.0 ||
        !std::isfinite(overallSpan) || overallSpan <= 0.0) {
        return;
    }

    const double shiftHz = -static_cast<double>(deltaPixels) * visibleSpan /
                           static_cast<double>((std::max)(1, widthPixels));
    const double desiredMin = minFrequency + shiftHz;
    const double desiredMax = maxFrequency + shiftHz;
    double retuneDeltaHz = 0.0;
    if (desiredMin < overallMin) {
        retuneDeltaHz = desiredMin - overallMin;
    } else if (desiredMax > overallMax) {
        retuneDeltaHz = desiredMax - overallMax;
    }

    const double previousDisplayCenter = std::isfinite(spectrumDisplayCenterHz)
                                             ? spectrumDisplayCenterHz
                                             : (minFrequency + maxFrequency) * 0.5;
    spectrumDisplayCenterHz = previousDisplayCenter + shiftHz;

    if (std::abs(retuneDeltaHz) > 0.5 && pendingSettings.inputMode == INPUT_RF && !offlineIqPlaybackActive) {
        const RadioSettings previousSettings = pendingSettings;
        pendingSettings.centerFrequency += retuneDeltaHz;
        normalizeTuning(pendingSettings, true);
        applyCenterFrequencyToHardwareIfNeeded(previousSettings, "spectrum pan");
        publishSettingsToGlobals();
        if (frequencyControl) {
            QSignalBlocker blocker(frequencyControl);
            frequencyControl->setValueHz(pendingSettings.centerFrequency);
        }
        if (listeningFrequencyControl) {
            QSignalBlocker blocker(listeningFrequencyControl);
            listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
        }
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand(0);
        } else if (networkMode == NetworkMode::Server) {
            sendServerStateToClients();
        }
    }

    settingRange();
}

void YourClassName::onModulationChanged(int id) {
    pendingSettings.modulationType = id;
    updateQuickHamPanel();
    if (cwDecoderControlsWidget) {
        cwDecoderControlsWidget->setVisible(id == MOD_CW);
    }
    if (pskDecoderControlsWidget) pskDecoderControlsWidget->setVisible(id == MOD_PSK);
    pendingSettings.bandwidth = defaultBandwidthForModulation(id);
    if (bandwidthControl) {
        QSignalBlocker blocker(bandwidthControl);
        bandwidthControl->setValueHz(pendingSettings.bandwidth);
    }
    publishSettingsToGlobals();
    if (scaleWidget) {
        scaleWidget->setModulationType(id);
    }
    updateDigitalDecoderMode();
    updateVideoProcessorMode();
    updateIqFrameProducerSettings();
    settingRange();
    qDebug() << "Modulation type changed to:" << id
             << "bandwidth preset" << pendingSettings.bandwidth;
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    }
}

void YourClassName::onDigitalTextDecoded(const QString &text) {
    if (!digitalTextEdit || text.isEmpty()) {
        return;
    }

    qDebug() << "[DigitalText] append" << text.left(120);
    QTextCursor cursor = digitalTextEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    digitalTextEdit->setTextCursor(cursor);
    digitalTextEdit->ensureCursorVisible();
}

void YourClassName::onDigitalDecoderStatusChanged(const QString &status) {
    if (digitalStatusLabel) {
        digitalStatusLabel->setToolTip(status);
        const int textWidth = (std::max)(160, digitalStatusLabel->width() - 8);
        digitalStatusLabel->setText(digitalStatusLabel->fontMetrics().elidedText(status,
                                                                                 Qt::ElideRight,
                                                                                 textWidth));
    }
}

void YourClassName::onDmrMetadataDetected(int colorCode,
                                          int timeslot,
                                          quint32 targetId,
                                          quint32 sourceId,
                                          int flco) {
    if (dmrLabCaptureCheckbox && dmrLabCaptureCheckbox->isChecked()) {
        return;
    }

    const bool sameMetadata =
        colorCode == pendingDmrMetadataColorCode &&
        timeslot == pendingDmrMetadataTimeslot &&
        targetId == pendingDmrMetadataTargetId &&
        sourceId == pendingDmrMetadataSourceId &&
        flco == pendingDmrMetadataFlco;
    if (sameMetadata) {
        ++pendingDmrMetadataStableHits;
    } else {
        pendingDmrMetadataColorCode = colorCode;
        pendingDmrMetadataTimeslot = timeslot;
        pendingDmrMetadataTargetId = targetId;
        pendingDmrMetadataSourceId = sourceId;
        pendingDmrMetadataFlco = flco;
        pendingDmrMetadataStableHits = 1;
    }

    const bool hasIdMetadata = targetId > 0 || sourceId > 0;
    const int requiredStableHits = hasIdMetadata ? 2 : 4;
    if (pendingDmrMetadataStableHits < requiredStableHits) {
        if (fobosVerboseLoggingEnabled() && pendingDmrMetadataStableHits == 1) {
            qDebug() << "[DMR metadata] waiting for stable repeat"
                     << "cc" << colorCode
                     << "ts" << timeslot
                     << "target" << targetId
                     << "source" << sourceId
                     << "flco" << flco
                     << "need" << requiredStableHits;
        }
        return;
    }

    const auto setComboToData = [](QComboBox *combo, const QVariant &data) {
        if (!combo) {
            return;
        }
        const int index = combo->findData(data);
        if (index >= 0 && combo->currentIndex() != index) {
            QSignalBlocker blocker(combo);
            combo->setCurrentIndex(index);
        }
    };
    const auto setLineEditNumber = [](QLineEdit *edit, quint32 value) {
        if (!edit || value == 0) {
            return;
        }
        const QString text = QString::number(value);
        if (edit->text().trimmed() != text) {
            QSignalBlocker blocker(edit);
            edit->setText(text);
        }
    };

    if (colorCode >= 0 && colorCode <= 15) {
        setComboToData(dmrLabColorCodeCombo, colorCode);
    }
    if (timeslot == 1 || timeslot == 2) {
        setComboToData(dmrLabSlotCombo, timeslot);
    }
    if (targetId == 0x00ffffffU) {
        setComboToData(dmrLabCallTypeCombo, QStringLiteral("all_call"));
    } else if (flco == 3) {
        setComboToData(dmrLabCallTypeCombo, QStringLiteral("private"));
    } else if (flco == 0) {
        setComboToData(dmrLabCallTypeCombo, QStringLiteral("group"));
    }
    setLineEditNumber(dmrLabTargetIdEdit, targetId);
    setLineEditNumber(dmrLabSourceIdEdit, sourceId);
}

void YourClassName::onVideoStatusChanged(const QString &status) {
    if (videoStatusLabel) {
        auto videoText = [this](const QString &raw) {
            if (raw == QStringLiteral("Video decoder disabled")) {
                return uiText(QStringLiteral("video_decoder_disabled"), raw);
            }
            if (raw == QStringLiteral("Video decoder ready")) {
                return uiText(QStringLiteral("video_decoder_ready"), raw);
            }
            if (raw == QStringLiteral("Video test pattern")) {
                return uiText(QStringLiteral("video_test_pattern"), raw);
            }
            return raw;
        };
        if (pendingSettings.modulationType == MOD_SSTV) {
            if (status.startsWith(QStringLiteral("SSTV"))) {
                videoStatusLabel->setText(videoText(status));
                return;
            }
            const bool sstvTest = videoTestPatternCheckbox && videoTestPatternCheckbox->isChecked();
            videoStatusLabel->setText(sstvTest
                                      ? QStringLiteral("SSTV: internal image test pattern")
                                      : QStringLiteral("SSTV: image decoder setup ready"));
            return;
        }
        if (pendingSettings.modulationType == MOD_APT) {
            if (status.startsWith(QStringLiteral("NOAA APT"))) {
                videoStatusLabel->setText(status);
                return;
            }
            const bool aptTest = videoTestPatternCheckbox && videoTestPatternCheckbox->isChecked();
            videoStatusLabel->setText(aptTest
                                      ? QStringLiteral("NOAA APT test stream")
                                      : QStringLiteral("NOAA APT: image decoder setup ready"));
            return;
        }
        if (pendingSettings.modulationType == MOD_WEFAX) {
            if (status.startsWith(QStringLiteral("WEFAX"))) {
                videoStatusLabel->setText(status);
                return;
            }
            const bool wefaxTest = videoTestPatternCheckbox && videoTestPatternCheckbox->isChecked();
            videoStatusLabel->setText(wefaxTest
                                      ? QStringLiteral("WEFAX test stream")
                                      : QStringLiteral("WEFAX: image decoder setup ready"));
            return;
        }
        if (pendingSettings.modulationType == MOD_LRPT) {
            if (status.startsWith(QStringLiteral("Meteor LRPT"))) {
                videoStatusLabel->setText(status);
                return;
            }
            videoStatusLabel->setText(QStringLiteral("Meteor LRPT beta: QPSK IQ monitor ready"));
            return;
        }
        videoStatusLabel->setText(videoText(status));
    }
}

void YourClassName::onScaleChanged(int value) {
    currentScale = sliderValueToScalePercent(value);

    scaleLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("scale"), QStringLiteral("Scale")),
                                                    formatScalePercent(currentScale)));
    settingRange();
    savePersistentSettings();
}

double YourClassName::effectiveScalePercent() const {
    const int divisor = (std::clamp)(additionalScaleDivisor, 1, ADDITIONAL_SCALE_DIVISOR_MAX);
    return currentScale / static_cast<double>(divisor);
}

void YourClassName::onAdditionalScaleDivisorChanged(int value) {
    additionalScaleDivisor = (std::clamp)(value, 1, ADDITIONAL_SCALE_DIVISOR_MAX);
    if (additionalScaleDivisorLabel) {
        additionalScaleDivisorLabel->setText(
            QStringLiteral("%1: 1:%2")
                .arg(uiText(QStringLiteral("additional_scale"), QStringLiteral("Extra zoom")))
                .arg(additionalScaleDivisor));
    }
    settingRange();
    savePersistentSettings();
}

void YourClassName::onSensitivityChanged(int value) {
    sensitivity = value;
    sensitivityLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("sensitivity"), QStringLiteral("Sensitivity"))).arg(value));
    settingRange();
    refreshDspControlStates();
    savePersistentSettings();
}

void YourClassName::onContrastChanged(int value) {
    contrast = value;
    contrastLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("contrast"), QStringLiteral("Contrast"))).arg(value));
    settingRange();
    refreshDspControlStates();
    savePersistentSettings();
}

void YourClassName::onLevelMinChanged(int value) {
    displayLevelMin = sliderValueToLevel(value);
    if (displayLevelMin >= displayLevelMax) {
        displayLevelMin = (std::max)(sliderValueToLevel(MIN_LEVEL_SLIDER_VALUE), displayLevelMax - MIN_LEVEL_GAP);
        if (levelMinSlider) {
            levelMinSlider->blockSignals(true);
            levelMinSlider->setValue(levelToSliderValue(displayLevelMin));
            levelMinSlider->blockSignals(false);
        }
    }
    if (levelMinLabel) {
        levelMinLabel->setText(levelLabelText(uiText(QStringLiteral("min"), QStringLiteral("Min")), displayLevelMin));
    }
    if (levelMaxLabel) {
        levelMaxLabel->setText(levelLabelText(uiText(QStringLiteral("max"), QStringLiteral("Max")), displayLevelMax));
    }
    if (graphWidget) {
        graphWidget->setLevelRange(displayLevelMin, displayLevelMax);
    }
    if (waterfallWidget) {
        waterfallWidget->setLevelRange(displayLevelMin, displayLevelMax);
    }
    refreshDspControlStates();
    savePersistentSettings();
}

void YourClassName::onLevelMaxChanged(int value) {
    displayLevelMax = sliderValueToLevel(value);
    if (displayLevelMax <= displayLevelMin) {
        displayLevelMax = (std::min)(sliderValueToLevel(MAX_LEVEL_SLIDER_VALUE), displayLevelMin + MIN_LEVEL_GAP);
        if (levelMaxSlider) {
            levelMaxSlider->blockSignals(true);
            levelMaxSlider->setValue(levelToSliderValue(displayLevelMax));
            levelMaxSlider->blockSignals(false);
        }
    }
    if (levelMinLabel) {
        levelMinLabel->setText(levelLabelText(uiText(QStringLiteral("min"), QStringLiteral("Min")), displayLevelMin));
    }
    if (levelMaxLabel) {
        levelMaxLabel->setText(levelLabelText(uiText(QStringLiteral("max"), QStringLiteral("Max")), displayLevelMax));
    }
    if (graphWidget) {
        graphWidget->setLevelRange(displayLevelMin, displayLevelMax);
    }
    if (waterfallWidget) {
        waterfallWidget->setLevelRange(displayLevelMin, displayLevelMax);
    }
    refreshDspControlStates();
    savePersistentSettings();
}

void YourClassName::onWaterfallScaleChanged(int delta) {
    int value = scaleSlider->value();

    if (delta > 0) {
        value += scaleSlider->singleStep();
    } else {
        value -= scaleSlider->singleStep();
    }

    value = std::clamp(value, scaleSlider->minimum(), scaleSlider->maximum());

    if (value == scaleSlider->value()) {
        return;
    }

    scaleSlider->setValue(value);

    if (isNetworkClientMode() && !isFullIqProcessingMode()) {
        scheduleRemoteSettingsCommand();
    }
}

void YourClassName::doubleGraphEnable(bool checked) {
    if (checked){
        secondGraph = true;
        qDebug()<<"secondgraph enabled";
    } else {
        secondGraph = false;
        qDebug()<<"secondgraph disabled";
    }
    applyAlternativeInterfaceMode();
}

void YourClassName::colorGraphEnable(bool checked) {
    if (checked){
        colorf = true;
        qDebug()<<"color graph enabled";
    } else {
        colorf = false;
        qDebug()<<"color graph disabled";
    }
}

void YourClassName::syncEnable(bool checked) {
    Q_UNUSED(checked);
    pendingSettings.syncEnabled = false;
    if (syncCheckbox && syncCheckbox->isChecked()) {
        syncCheckbox->blockSignals(true);
        syncCheckbox->setChecked(false);
        syncCheckbox->blockSignals(false);
    }
    publishSettingsToGlobals();
    qDebug() << "Sync reader disabled; async reader is forced.";
}

void YourClassName::wheelEvent(QWheelEvent *event) {
    if (event->angleDelta().y() != 0) {
        QLineEdit *focusedLineEdit = qobject_cast<QLineEdit*>(focusWidget());
        if (focusedLineEdit) {
            bool ok;
            double currentValue = focusedLineEdit->text().toDouble(&ok);
            if (ok) {
                double delta = event->angleDelta().y() > 0 ? 1.0 : -1.0;
                currentValue += delta;
                focusedLineEdit->setText(QString::number(currentValue, 'f', 0));

                focusedLineEdit->emit textEdited(focusedLineEdit->text());
            }
        }
    }
    QMainWindow::wheelEvent(event);
}

void YourClassName::populateAudioDevices() {
#ifdef _WIN32
    UINT numDevices = waveOutGetNumDevs();
    qDebug() << "Number of waveOut devices found:" << numDevices;

    for (UINT i = 0; i < numDevices; i++) {
        WAVEOUTCAPS caps;
        if (waveOutGetDevCaps(i, &caps, sizeof(WAVEOUTCAPS)) == MMSYSERR_NOERROR) {
            QString deviceName = QString::fromLocal8Bit(caps.szPname);  // РСЃРїСЂР°РІР»РµРЅРѕ
            qDebug() << "Device" << i << ":" << deviceName;
            audioDeviceComboBox->addItem(deviceName, QVariant(i));
        }
    }
#elif defined(FOBOSAPP_HAS_QT_AUDIO)
    const QList<QAudioDeviceInfo> devices = QAudioDeviceInfo::availableDevices(QAudio::AudioOutput);
    qDebug() << "Number of Qt audio output devices found:" << devices.size();
    if (devices.isEmpty()) {
        audioDeviceComboBox->addItem(QStringLiteral("Default audio output"), QVariant(0));
        return;
    }
    for (int i = 0; i < devices.size(); ++i) {
        const QString deviceName = devices.at(i).deviceName();
        qDebug() << "Audio device" << i << ":" << deviceName;
        audioDeviceComboBox->addItem(deviceName, QVariant(i));
    }
#else
    qDebug() << "Native audio device enumeration is not implemented on this platform yet.";
    audioDeviceComboBox->addItem(QStringLiteral("Default audio output"), QVariant(0));
#endif
}

void YourClassName::onAudioDeviceChanged(int index) {
    if (index < 0) return;
    if (!isIdle()) {
        qDebug() << "Audio device change is locked while radio is running.";
        revertHardwareControlsToSettings();
        return;
    }

    QVariant data = audioDeviceComboBox->currentData();
    if (!data.isValid()) {
        qDebug() << "Error: Invalid audio device selected!";
        return;
    }

    // РџРѕР»СѓС‡Р°РµРј СЃС‚СЂРѕРєСѓ СЃ РёРјРµРЅРµРј СѓСЃС‚СЂРѕР№СЃС‚РІР°
    QString deviceName = data.toString();
    qDebug() << "Selected audio device:" << deviceName;
    pendingSettings.audioDeviceId = data.toInt();
    publishSettingsToGlobals();

    deviceID = pendingSettings.audioDeviceId;  // РџРµСЂРµРґР°С‘Рј ID РІРјРµСЃС‚Рѕ РёРјРµРЅРё
    qDebug() << "Selected audio device ID:" << deviceID;

    if (audioProcessor) {
        audioProcessor->setAudioDevice(deviceID);
    } else {
        qDebug() << "Error: audioProcessor is null!";
    }
}


void YourClassName::onBandwidthChanged() {
    if (!bandwidthControl) {
        return;
    }

    const double bandwidth = bandwidthControl->valueHz();
    if (bandwidth <= 0.0) {
        return;
    }

    pendingSettings.bandwidth = bandwidth;
    publishSettingsToGlobals();
    settingRange();

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

    savePersistentSettings();
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    }
}

void YourClassName::updateAudioFilterLabels() {
    if (audioLowPassLabel) {
        const QString value = pendingSettings.audioLowPassHz > 0.0
                                  ? audioFilterFrequencyText(pendingSettings.audioLowPassHz)
                                  : uiText(QStringLiteral("auto"), QStringLiteral("Auto"));
        audioLowPassLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("audio_lpf"), QStringLiteral("Audio LPF")), value));
    }
    if (audioHighPassLabel) {
        const QString value = pendingSettings.audioHighPassHz > 0.0
                                  ? audioFilterFrequencyText(pendingSettings.audioHighPassHz)
                                  : uiText(QStringLiteral("off"), QStringLiteral("Off"));
        audioHighPassLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("audio_hpf"), QStringLiteral("Audio HPF")), value));
    }
}

void YourClassName::updateHfNoiseCancelControls() {
    const bool enabled = pendingSettings.inputMode == INPUT_HF_NOISE_CANCEL;
    const bool hfVisualEnabled = isDirectInputMode(pendingSettings.inputMode);

    if (hfInterferenceBaselineCheckbox) {
        hfInterferenceBaselineCheckbox->setEnabled(hfVisualEnabled);
        hfInterferenceBaselineCheckbox->setToolTip(
            uiText(QStringLiteral("hf_baseline_tooltip"),
                   QStringLiteral("Subtract a learned HF noise-floor curve from the visual spectrum and waterfall only.")));
    }
    if (hfInterferenceRawOverlayCheckbox) {
        hfInterferenceRawOverlayCheckbox->setEnabled(hfVisualEnabled && !hfInterferenceBaselineFrequencies.empty());
        hfInterferenceRawOverlayCheckbox->setToolTip(
            uiText(QStringLiteral("hf_raw_overlay_tooltip"),
                   QStringLiteral("Draw the original raw spectrum over the cleaned HF baseline graph.")));
    }
    if (hfInterferenceBaselineLearnButton) {
        hfInterferenceBaselineLearnButton->setEnabled(hfVisualEnabled && !spectrumFrequencyScratch.empty());
        hfInterferenceBaselineLearnButton->setToolTip(
            uiText(QStringLiteral("hf_baseline_learn_tooltip"),
                   QStringLiteral("Capture the current visible spectrum as the HF interference baseline.")));
    }
    if (hfInterferenceBaselineClearButton) {
        hfInterferenceBaselineClearButton->setEnabled(!hfInterferenceBaselineFrequencies.empty());
        hfInterferenceBaselineClearButton->setToolTip(
            uiText(QStringLiteral("hf_baseline_clear_tooltip"),
                   QStringLiteral("Clear the learned HF interference baseline.")));
    }
    if (hfInterferenceDefaultsButton) {
        hfInterferenceDefaultsButton->setEnabled(hfVisualEnabled);
        hfInterferenceDefaultsButton->setToolTip(
            uiText(QStringLiteral("hf_interference_default_tooltip"),
                   QStringLiteral("Reset HF interference lab controls and clear the learned baseline.")));
    }
    if (hfInterferenceBaselineDepthLabel) {
        hfInterferenceBaselineDepthLabel->setText(
            QStringLiteral("%1: %2%")
                .arg(uiText(QStringLiteral("hf_baseline_depth"), QStringLiteral("Baseline depth")))
                .arg(static_cast<int>(std::lround(hfInterferenceBaselineDepth * 100.0))));
        hfInterferenceBaselineDepthLabel->setEnabled(hfVisualEnabled);
    }
    if (hfInterferenceBaselineDepthSlider) {
        hfInterferenceBaselineDepthSlider->setEnabled(hfVisualEnabled);
    }
    if (hfInterferenceBaselineSmoothLabel) {
        hfInterferenceBaselineSmoothLabel->setText(
            QStringLiteral("%1: %2")
                .arg(uiText(QStringLiteral("hf_baseline_smooth"), QStringLiteral("Smooth")))
                .arg(hfInterferenceBaselineSmoothBins));
        hfInterferenceBaselineSmoothLabel->setEnabled(hfVisualEnabled);
    }
    if (hfInterferenceBaselineSmoothSlider) {
        hfInterferenceBaselineSmoothSlider->setEnabled(hfVisualEnabled);
    }
    if (hfInterferenceBaselineStatusLabel) {
        QString status;
        if (hfInterferenceBaselineFrequencies.empty()) {
            status = uiText(QStringLiteral("hf_baseline_empty"), QStringLiteral("Baseline: empty"));
        } else {
            status = QStringLiteral("%1: %2 %3")
                         .arg(uiText(QStringLiteral("hf_baseline_ready"), QStringLiteral("Baseline ready")))
                         .arg(hfInterferenceBaselineFrequencies.size())
                         .arg(uiText(QStringLiteral("bins"), QStringLiteral("bins")));
        }
        if (hfInterferenceBaselineEnabled && !hfInterferenceBaselineFrequencies.empty()) {
            status += QStringLiteral(" / %1").arg(uiText(QStringLiteral("enabled"), QStringLiteral("enabled")));
        }
        hfInterferenceBaselineStatusLabel->setText(status);
        hfInterferenceBaselineStatusLabel->setEnabled(hfVisualEnabled);
    }

    if (hfNoiseCancelDepthLabel) {
        hfNoiseCancelDepthLabel->setText(QStringLiteral("%1: %2%").arg(uiText(QStringLiteral("hf_cancel"), QStringLiteral("HF cancel")))
                                             .arg(hfNoiseCancelDepthToSliderValue(pendingSettings.hfNoiseCancelDepth)));
        hfNoiseCancelDepthLabel->setEnabled(enabled);
    }
    if (hfNoiseCancelRefGainLabel) {
        hfNoiseCancelRefGainLabel->setText(QStringLiteral("%1: %2 dB").arg(uiText(QStringLiteral("ref_gain"), QStringLiteral("Ref gain")))
                                               .arg(clampHfNoiseCancelRefGainDb(pendingSettings.hfNoiseCancelRefGainDb), 0, 'f', 1));
        hfNoiseCancelRefGainLabel->setEnabled(enabled);
    }
    if (hfNoiseCancelRefDelayLabel) {
        hfNoiseCancelRefDelayLabel->setText(QStringLiteral("%1: %2 ns").arg(uiText(QStringLiteral("ref_delay"), QStringLiteral("Ref delay")))
                                                .arg(static_cast<int>(std::lround(clampHfNoiseCancelRefDelayNs(pendingSettings.hfNoiseCancelRefDelayNs)))));
        hfNoiseCancelRefDelayLabel->setEnabled(enabled);
    }
    if (hfNoiseCancelRefTiltLabel) {
        hfNoiseCancelRefTiltLabel->setText(QStringLiteral("%1: %2 dB").arg(uiText(QStringLiteral("ref_tilt"), QStringLiteral("Ref tilt")))
                                               .arg(clampHfNoiseCancelRefTiltDb(pendingSettings.hfNoiseCancelRefTiltDb), 0, 'f', 1));
        hfNoiseCancelRefTiltLabel->setEnabled(enabled);
    }
    if (hfNoiseCancelDepthSlider) {
        hfNoiseCancelDepthSlider->setEnabled(enabled);
        hfNoiseCancelDepthSlider->setToolTip(
            QStringLiteral("Subtract the part of HF2 that stays correlated with HF1. 0% = HF1 only, 100% = normal cancellation."));
    }
    if (hfNoiseCancelRefGainSlider) {
        hfNoiseCancelRefGainSlider->setEnabled(enabled);
        hfNoiseCancelRefGainSlider->setToolTip(
            QStringLiteral("Manual HF2 reference level before subtraction."));
    }
    if (hfNoiseCancelRefDelaySlider) {
        hfNoiseCancelRefDelaySlider->setEnabled(enabled);
        hfNoiseCancelRefDelaySlider->setToolTip(
            QStringLiteral("Manual HF2 reference delay/phase correction before subtraction."));
    }
    if (hfNoiseCancelRefTiltSlider) {
        hfNoiseCancelRefTiltSlider->setEnabled(enabled);
        hfNoiseCancelRefTiltSlider->setToolTip(
            QStringLiteral("Manual HF2 reference slope across the visible HF band."));
    }
    if (hfNoiseCancelFreezeCheckbox) {
        hfNoiseCancelFreezeCheckbox->setEnabled(enabled);
        hfNoiseCancelFreezeCheckbox->setToolTip(
            QStringLiteral("Hold the small adaptive trim added on top of the manual HF2 reference"));
    }
    if (hfAudioBlankerCheckbox) {
        hfAudioBlankerCheckbox->setEnabled(hfVisualEnabled);
        hfAudioBlankerCheckbox->setToolTip(
            uiText(QStringLiteral("hf_audio_blanker_tooltip"),
                   QStringLiteral("Blank short impulse spikes in the selected HF audio channel.")));
    }
    if (hfAudioBlankerThresholdLabel) {
        hfAudioBlankerThresholdLabel->setText(
            QStringLiteral("%1: %2x")
                .arg(uiText(QStringLiteral("hf_audio_blanker_threshold"), QStringLiteral("Blanker threshold")))
                .arg((std::clamp)(pendingSettings.hfAudioBlankerThreshold, 2.0, 20.0), 0, 'f', 1));
        hfAudioBlankerThresholdLabel->setEnabled(hfVisualEnabled && pendingSettings.hfAudioBlankerEnabled);
    }
    if (hfAudioBlankerThresholdSlider) {
        hfAudioBlankerThresholdSlider->setEnabled(hfVisualEnabled && pendingSettings.hfAudioBlankerEnabled);
        hfAudioBlankerThresholdSlider->setToolTip(
            uiText(QStringLiteral("hf_audio_blanker_threshold_tooltip"),
                   QStringLiteral("Higher values blank only stronger impulse spikes; lower values are more aggressive.")));
    }
}

void YourClassName::onSampleRateChanged(int index) {
    qDebug() << "[FobosLifecycle] onSampleRateChanged enter"
             << "index" << index
             << "state" << runStateName(runState)
             << "deviceOpened" << deviceOpened
             << "processorRunning" << (processor && processor->isRunning());

    if (!sampleBox || index < 0) {
        return;
    }

    bool ok = false;
    const double selectedSampleRate = sampleBox->itemData(index).toDouble(&ok);

    if (!ok || selectedSampleRate <= 0.0) {
        qDebug() << "Invalid sample rate selected.";
        return;
    }

    const double previousSampleRate = pendingSettings.sampleRate;
    const bool sampleRateChanged =
        std::abs(previousSampleRate - selectedSampleRate) > 0.5;

    qDebug() << "[FobosLifecycle] sample rate selected"
             << "previous" << pendingSettings.sampleRate
             << "selected" << selectedSampleRate
             << "changed" << sampleRateChanged
             << "device" << activeFobosDevice()
             << "apiKind" << fobosApiKindName(activeFobosApiKind);

    if (!sampleRateChanged) {
        return;
    }

    pendingSettings.sampleRate = selectedSampleRate;
    if (fftBinWidthModeEnabled) {
        applyFftBinWidthTarget(false);
    } else {
        syncFftResolutionControls();
    }
    applyAgileScanAutoStep(true);
    normalizeStandardScanCentersUi(false);
    normalizeTuning(pendingSettings);
    publishSettingsToGlobals();
    settingRange();

    if (runState == RadioRunState::Idle && processor && processor->isRunning()) {
        qDebug() << "[FobosLifecycle] repairing Idle state with active processor before sample-rate restart";
        deviceOpened = true;
        runState = RadioRunState::Running;
        updateUiForRunState();
    }

    if (isNetworkClientMode()) {
        if (isClientIqProcessingMode()) {
            resetNetworkIqReceptionState(false, false, pendingSettings.audioEnabled && !isFullIqProcessingMode());
        }
        scheduleRemoteSettingsCommand();
        return;
    }

    if (!isIdle()) {
        // A second selection can arrive before the previous reader has delivered
        // its first callback. Apply only the last requested rate once it is ready.
        if (!sampleRateChangeTimer) {
            sampleRateChangeTimer = new QTimer(this);
            sampleRateChangeTimer->setSingleShot(true);
            connect(sampleRateChangeTimer, &QTimer::timeout, this, [this]() {
                if (isNetworkClientMode()) return;
                if (runState == RadioRunState::Starting || runState == RadioRunState::Stopping ||
                    (runState == RadioRunState::Running && processor && processor->isRunning() &&
                     streamStartElapsedTimer.isValid() && streamStartElapsedTimer.elapsed() < 1200 &&
                     processor->callbackCount() <= streamStartCallbackCount)) {
                    sampleRateChangeTimer->start(100);
                    return;
                }
                if (isIdle()) {
                    sampleRateReopenRequired = hasActiveFobosDevice() &&
                        (appliedSampleRate <= 0.0 ||
                         std::abs(appliedSampleRate - pendingSettings.sampleRate) > 0.5);
                    return;
                }
                if (hasActiveFobosDevice() && appliedSampleRate > 0.0 &&
                    std::abs(appliedSampleRate - pendingSettings.sampleRate) <= 0.5) {
                    return;
                }
                qDebug() << "[FobosLifecycle] restarting reader for settled sample-rate change"
                         << "requested" << pendingSettings.sampleRate
                         << "apiKind" << fobosApiKindName(activeFobosApiKind);
                restartStreamForHardwareChange();
            });
        }
        sampleRateChangeTimer->start(300);
        return;
    }

    if (sampleRateChangeTimer) sampleRateChangeTimer->stop();

    if (hasActiveFobosDevice()) {
        const bool selectedRateMatchesOpenSession =
            appliedSampleRate > 0.0 &&
            std::abs(appliedSampleRate - selectedSampleRate) <= 0.5;

        sampleRateReopenRequired = !selectedRateMatchesOpenSession;
    } else {
        sampleRateReopenRequired = false;
    }

    qDebug() << "Sample rate will be applied on the next start.";
}

double YourClassName::fineTuneRangeHz() const {
    double visibleSpan = maxFrequency - minFrequency;
    if (!std::isfinite(visibleSpan) || visibleSpan <= 0.0) {
        visibleSpan = pendingSettings.sampleRate * (effectiveScalePercent() / 100.0);
    }
    if (!std::isfinite(visibleSpan) || visibleSpan <= 0.0) {
        visibleSpan = 100000.0;
    }
    return (std::clamp)(visibleSpan / FINE_TUNE_VISIBLE_RANGE_DIVISOR,
                        FINE_TUNE_MIN_RANGE_HZ,
                        FINE_TUNE_MAX_RANGE_HZ);
}

double YourClassName::fineTuneStepHz() const {
    return fineTuneRangeHz() /
           static_cast<double>((std::max)(std::abs(FINE_TUNE_DIAL_MIN),
                                          std::abs(FINE_TUNE_DIAL_MAX)));
}

void YourClassName::updateFineTuneLabel() {
    if (!fineTuneLabel) {
        return;
    }
    const double range = fineTuneRangeHz();
    if (fineTuneScaleWidget) {
        fineTuneScaleWidget->setRangeHz(range);
    }
    fineTuneLabel->setText(QStringLiteral("%1 +/- %2")
                               .arg(uiText(QStringLiteral("fine_tune"), QStringLiteral("Fine tune")),
                                    audioFilterFrequencyText(range)));
}

void YourClassName::updateFineTuneControlMode() {
    if (!fineTuneStack) {
        return;
    }
    fineTuneControlMode = fineTuneControlMode == FINE_TUNE_MODE_DIAL
                              ? FINE_TUNE_MODE_DIAL
                              : FINE_TUNE_MODE_SCALE;
    if (fineTuneScaleWidget && fineTuneScaleWidget->holdOffsetMode() != fineTuneScaleHoldMode) {
        QSignalBlocker blocker(fineTuneScaleWidget);
        fineTuneScaleWidget->setHoldOffsetMode(fineTuneScaleHoldMode);
    }
    fineTuneStack->setFixedHeight(fineTuneControlMode == FINE_TUNE_MODE_DIAL ? 78 : 58);
    fineTuneStack->setCurrentIndex(fineTuneControlMode == FINE_TUNE_MODE_DIAL ? 1 : 0);
    updateFineTuneScaleModeButton();
    onFineTuneDialReleased();
    updateFineTuneLabel();
}

void YourClassName::updateFineTuneScaleModeButton() {
    if (!fineTuneScaleModeButton) {
        return;
    }

    const bool holdMode = fineTuneScaleWidget ? fineTuneScaleWidget->holdOffsetMode()
                                              : fineTuneScaleHoldMode;
    fineTuneScaleHoldMode = holdMode;
    {
        QSignalBlocker blocker(fineTuneScaleModeButton);
        fineTuneScaleModeButton->setChecked(holdMode);
    }
    fineTuneScaleModeButton->setVisible(fineTuneControlMode == FINE_TUNE_MODE_SCALE);
    fineTuneScaleModeButton->setToolTip(holdMode
                                            ? QStringLiteral("Held fine tune offset. Double-click the scale or click here for temporary mode.")
                                            : QStringLiteral("Temporary fine tune. Double-click the scale or click here for held offset mode."));
    fineTuneScaleModeButton->setStyleSheet(holdMode
                                               ? QStringLiteral("QToolButton { background: #d65050; border: 1px solid #ff9a9a; border-radius: 8px; }"
                                                                "QToolButton:hover { background: #ec6969; }")
                                               : QStringLiteral("QToolButton { background: #3dbb68; border: 1px solid #8af0a8; border-radius: 8px; }"
                                                                "QToolButton:hover { background: #50d47a; }"));
}

void YourClassName::applyListeningFrequencyDelta(double deltaHz, int networkDelayMs) {
    if (!std::isfinite(deltaHz) || std::abs(deltaHz) < 0.01) {
        return;
    }

    const RadioSettings previousSettings = pendingSettings;
    pendingSettings.listeningFrequency += deltaHz;
    normalizeTuning(pendingSettings);
    applyCenterFrequencyToHardwareIfNeeded(previousSettings, "fine tune");
    publishSettingsToGlobals();

    if (frequencyControl) {
        QSignalBlocker blocker(frequencyControl);
        frequencyControl->setValueHz(pendingSettings.centerFrequency);
    }
    if (listeningFrequencyControl) {
        QSignalBlocker blocker(listeningFrequencyControl);
        listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
    }

    settingRange();
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand((std::min)(networkDelayMs, 20));
    } else if (networkMode == NetworkMode::Server) {
        sendServerStateToClients();
    }
}

void YourClassName::onFineTuneDialChanged(int value) {
    const int deltaSteps = value - fineTuneDialLastValue;
    fineTuneDialLastValue = value;
    if (deltaSteps == 0) {
        return;
    }

    applyListeningFrequencyDelta(deltaSteps * fineTuneStepHz(), 80);

    if (fineTuneDial && !fineTuneDial->isSliderDown()) {
        QTimer::singleShot(120, this, [this]() {
            onFineTuneDialReleased();
        });
    }
}

void YourClassName::onFineTuneDialReleased() {
    if (!fineTuneDial) {
        return;
    }
    QSignalBlocker blocker(fineTuneDial);
    fineTuneDial->setValue(0);
    fineTuneDialLastValue = 0;
}

void YourClassName::onListeningFrequencyEntered() {
    if (!listeningFrequencyControl) {
        return;
    }

    const RadioSettings previousSettings = pendingSettings;
    pendingSettings.listeningFrequency = listeningFrequencyControl->valueHz();
    normalizeTuning(pendingSettings);
    applyCenterFrequencyToHardwareIfNeeded(previousSettings, "listening control");
    publishSettingsToGlobals();
    if (frequencyControl) {
        QSignalBlocker blocker(frequencyControl);
        frequencyControl->setValueHz(pendingSettings.centerFrequency);
    }
    {
        QSignalBlocker blocker(listeningFrequencyControl);
        listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
    }
    qDebug() << "Frequency set to" << listeningFrequency << "Hz";
    settingRange();
    updateQuickHamPanel();
    savePersistentSettings();
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand(0);
    } else if (networkMode == NetworkMode::Server) {
        sendServerStateToClients();
    }
}

void YourClassName::onFrequencyEntered() {
    const RadioSettings previousSettings = pendingSettings;
    if (pendingSettings.inputMode == INPUT_RF) {
        if (frequencyControl) {
            const double requestedCenterFrequency = frequencyControl->valueHz();
            const double previousListeningFrequency = pendingSettings.listeningFrequency;
            pendingSettings.centerFrequency = requestedCenterFrequency;
            const double halfRate = pendingSettings.sampleRate > 0.0
                                        ? pendingSettings.sampleRate * 0.5
                                        : 0.0;
            const bool listeningOutsideNewWindow =
                halfRate > 0.0 &&
                std::isfinite(previousListeningFrequency) &&
                std::isfinite(requestedCenterFrequency) &&
                (previousListeningFrequency < requestedCenterFrequency - halfRate ||
                 previousListeningFrequency > requestedCenterFrequency + halfRate);
            if (listeningOutsideNewWindow) {
                qDebug() << "[Tune] center control reset listening after out-of-window center jump"
                         << "previousCenter" << previousSettings.centerFrequency
                         << "requestedCenter" << requestedCenterFrequency
                         << "previousListening" << previousListeningFrequency
                         << "sampleRate" << pendingSettings.sampleRate;
                pendingSettings.listeningFrequency = requestedCenterFrequency;
            }
            normalizeTuning(pendingSettings, true);
            applyCenterFrequencyToHardwareIfNeeded(previousSettings, "center control");
            publishSettingsToGlobals();
            QSignalBlocker frequencyBlocker(frequencyControl);
            frequencyControl->setValueHz(pendingSettings.centerFrequency);
            if (listeningFrequencyControl) {
                QSignalBlocker listeningBlocker(listeningFrequencyControl);
                listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
            }
            qDebug() << "Frequency set to" << globalFrequency << "Hz";
        }
    } else {
        pendingSettings.centerFrequency = 0;
        normalizeTuning(pendingSettings);
        publishSettingsToGlobals();
        if (frequencyControl) {
            QSignalBlocker frequencyBlocker(frequencyControl);
            frequencyControl->setValueHz(pendingSettings.centerFrequency);
        }
        if (listeningFrequencyControl) {
            QSignalBlocker listeningBlocker(listeningFrequencyControl);
            listeningFrequencyControl->setValueHz(pendingSettings.listeningFrequency);
        }
    }
    settingRange();
    updateQuickHamPanel();
    const bool deferSettingsSaveForLiveAgile =
        runState == RadioRunState::Running &&
        activeFobosApiKind == FobosApiKind::Agile &&
        pendingSettings.inputMode == INPUT_RF &&
        !agileScanEnabled;
    if (!deferSettingsSaveForLiveAgile) {
        savePersistentSettings();
    }
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand(0);
    } else if (networkMode == NetworkMode::Server) {
        sendServerStateToClients();
    }
}
