#include "main.h"
#include "appconstants.h"
#include "iqbuffer.h"
#include "spectrumframereplaydialog.h"
#include "spectrumfftworker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileInfoList>
#include <QMessageBox>
#include <QMetaObject>
#include <QSignalBlocker>
#include <QSysInfo>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#ifndef FOBOSAPP_VERSION
#define FOBOSAPP_VERSION "unknown"
#endif

namespace {

QString recordingModulationName(int modulationType) {
    switch (modulationType) {
    case MOD_AM: return QStringLiteral("AM");
    case MOD_NFM: return QStringLiteral("NFM");
    case MOD_SAM: return QStringLiteral("SAM");
    case MOD_USB: return QStringLiteral("USB");
    case MOD_LSB: return QStringLiteral("LSB");
    case MOD_DSB: return QStringLiteral("DSB");
    case MOD_CW: return QStringLiteral("CW");
    case MOD_WFM: return QStringLiteral("WFM");
    case MOD_FT8: return QStringLiteral("FT8");
    case MOD_RTTY: return QStringLiteral("RTTY");
    case MOD_FSK: return QStringLiteral("FSK");
    case MOD_PSK: return QStringLiteral("PSK");
    case MOD_ATV: return QStringLiteral("ATV");
    case MOD_SSTV: return QStringLiteral("SSTV");
    case MOD_APT: return QStringLiteral("APT");
    case MOD_WEFAX: return QStringLiteral("WEFAX");
    case MOD_LRPT: return QStringLiteral("LRPT");
    case MOD_DMR: return QStringLiteral("DMR");
    default: return QStringLiteral("unknown");
    }
}

} // namespace

RecordingManager::Mode YourClassName::selectedRecordingMode() const {
    if (!recordingModeCombo) {
        return RecordingManager::Mode::AudioWav;
    }
    bool ok = false;
    const int value = recordingModeCombo->currentData().toInt(&ok);
    return ok ? static_cast<RecordingManager::Mode>(value) : RecordingManager::Mode::AudioWav;
}

QJsonObject YourClassName::recordingLabMetadata() const {
    QJsonObject lab;
    if (pendingSettings.modulationType != MOD_DMR) {
        return lab;
    }

    bool hasMetadata = false;
    lab["schema"] = QStringLiteral("dmr-profile");
    lab["schemaVersion"] = 2;
    lab["mode"] = QStringLiteral("DMR");
    lab["locked"] = dmrLabCaptureCheckbox && dmrLabCaptureCheckbox->isChecked();
    lab["basebandSampleRate"] =
        dmrBasebandRateCombo
            ? normalizedDmrBasebandSampleRate(dmrBasebandRateCombo->currentData().toInt())
            : normalizedDmrBasebandSampleRate(pendingSettings.dmrBasebandSampleRate);
    lab["manualTiming"] = dmrManualTimingCheckbox && dmrManualTimingCheckbox->isChecked();
    lab["timingOffset"] = dmrTimingOffsetSpin ? dmrTimingOffsetSpin->value() : 0;
    lab["slicerRatio"] = dmrSlicerRatioSpin ? dmrSlicerRatioSpin->value() : pendingSettings.dmrSlicerRatio;
    lab["adaptiveSlicer"] = !dmrAdaptiveSlicerCheckbox || dmrAdaptiveSlicerCheckbox->isChecked();
    lab["ambeLayout"] =
        QString::fromLatin1(dmrAmbeLayoutName(dmrAmbeLayoutCombo
                                                  ? dmrAmbeLayoutCombo->currentData().toInt()
                                                  : pendingSettings.dmrAmbeLayout));
    lab["privacyMode"] = dmrPrivacyModeId(pendingSettings.dmrPrivacyMode);
    lab["privacyKeyId"] = pendingSettings.dmrPrivacyKeyId;
    lab["privacyKeySet"] = !pendingSettings.dmrPrivacyKeyHex.isEmpty();
    lab["privacyKeyHexLen"] = pendingSettings.dmrPrivacyKeyHex.size();
    lab["privacyEnabled"] = pendingSettings.dmrPrivacyMode != DMR_PRIVACY_NONE;
    lab["privacyVariant"] = pendingSettings.dmrPrivacyVariant;
    lab["privacyLayout"] = pendingSettings.dmrPrivacyLayout;
    lab["privacyFrameOffset"] = pendingSettings.dmrPrivacyFrameOffset;
    lab["description"] = QStringLiteral("DMR metadata learned from the signal or locked by the user.");

    if (dmrLabColorCodeCombo) {
        const int colorCode = dmrLabColorCodeCombo->currentData().toInt();
        if (colorCode >= 0) {
            lab["colorCode"] = colorCode;
            hasMetadata = true;
        }
    }
    if (dmrLabSlotCombo) {
        const int slot = dmrLabSlotCombo->currentData().toInt();
        if (slot == 1 || slot == 2) {
            lab["timeslot"] = slot;
            hasMetadata = true;
        }
    }
    if (dmrLabCallTypeCombo) {
        const QString callType = dmrLabCallTypeCombo->currentData().toString();
        if (!callType.isEmpty() && callType != QStringLiteral("unknown")) {
            lab["callType"] = callType;
            hasMetadata = true;
        }
    }

    auto addText = [&lab, &hasMetadata](const QString &key, const QLineEdit *edit) {
        if (!edit) {
            return;
        }
        const QString text = edit->text().trimmed();
        if (!text.isEmpty()) {
            lab[key] = text;
            hasMetadata = true;
        }
    };
    addText(QStringLiteral("sourceId"), dmrLabSourceIdEdit);
    addText(QStringLiteral("targetId"), dmrLabTargetIdEdit);
    addText(QStringLiteral("radio"), dmrLabRadioEdit);
    addText(QStringLiteral("notes"), dmrLabNotesEdit);
    if (!hasMetadata && !(dmrLabCaptureCheckbox && dmrLabCaptureCheckbox->isChecked())) {
        return QJsonObject();
    }
    return lab;
}

QJsonObject YourClassName::recordingScientificSnapshot(const QString &phase) const {
    QJsonObject snapshot;
    snapshot["phase"] = phase;
    snapshot["capturedAtUtc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    QJsonObject software;
    software["name"] = QStringLiteral("Obrii SDR");
    software["version"] = QStringLiteral(FOBOSAPP_VERSION);
    software["buildAbi"] = QSysInfo::buildAbi();
    software["cpuArchitecture"] = QSysInfo::currentCpuArchitecture();
    software["kernelType"] = QSysInfo::kernelType();
    software["kernelVersion"] = QSysInfo::kernelVersion();
    snapshot["software"] = software;

    QString backend = QStringLiteral("fobos");
    QString receiverVariant;
    if (isNetworkClientMode()) {
        backend = QStringLiteral("network-client");
    } else if (isRtlTcpSelected()) {
        backend = QStringLiteral("rtl_tcp");
    } else if (isRtlSdrNativeSelected()) {
        backend = QStringLiteral("rtlsdr-native");
    } else if (isSoapySdrSelected()) {
        backend = QStringLiteral("soapysdr");
    } else if (isBladeRfNativeSelected()) {
        backend = QStringLiteral("bladerf-native");
    } else if (isHackRfNativeSelected()) {
        backend = QStringLiteral("hackrf-native");
    } else {
        receiverVariant = selectedFobosDeviceInfo().apiKind == FobosApiKind::Agile
                              ? QStringLiteral("agile")
                              : QStringLiteral("standard");
    }
    QJsonObject receiver;
    receiver["backend"] = backend;
    if (!receiverVariant.isEmpty()) {
        receiver["variant"] = receiverVariant;
    }
    receiver["runState"] = static_cast<int>(runState);
    receiver["networkMode"] = static_cast<int>(networkMode);
    receiver["deviceIndex"] = pendingSettings.deviceIndex;
    snapshot["receiver"] = receiver;

    QJsonObject radio;
    radio["centerFrequencyHz"] = pendingSettings.centerFrequency;
    radio["actualFrequencyHz"] = pendingSettings.actualFrequency;
    radio["listeningFrequencyHz"] = pendingSettings.listeningFrequency;
    radio["sampleRateHz"] = pendingSettings.sampleRate;
    radio["bandwidthHz"] = pendingSettings.bandwidth;
    radio["modulationType"] = pendingSettings.modulationType;
    radio["modulationName"] = recordingModulationName(pendingSettings.modulationType);
    radio["clockSource"] = pendingSettings.clockSource;
    radio["inputMode"] = pendingSettings.inputMode;
    radio["lnaGain"] = pendingSettings.lnaGain;
    radio["vgaGain"] = pendingSettings.vgaGain;
    radio["rtlAgc"] = pendingSettings.rtlAgc;
    radio["rtlTunerGainTenthsDb"] = pendingSettings.rtlTunerGainTenthsDb;
    snapshot["radio"] = radio;

    const double fftBinWidthHz = pendingSettings.fftLength > 0
                                     ? pendingSettings.sampleRate /
                                           static_cast<double>(pendingSettings.fftLength)
                                     : 0.0;
    QJsonObject analysis;
    analysis["fftLength"] = pendingSettings.fftLength;
    analysis["fftWindowType"] = normalizedFftWindowType(pendingSettings.fftWindowType);
    analysis["fftWindowName"] = QString::fromLatin1(fftWindowTypeName(pendingSettings.fftWindowType));
    analysis["fftBinWidthHz"] = fftBinWidthHz;
    analysis["rbwHz"] = fftBinWidthHz * fftWindowEnbwBins(pendingSettings.fftWindowType);
    analysis["displayScalePercent"] = effectiveScalePercent();
    analysis["displayLevelMinDb"] = displayLevelMin;
    analysis["displayLevelMaxDb"] = displayLevelMax;
    analysis["spectrumUpdateIntervalMs"] = spectrumUpdateIntervalMs;
    analysis["frequencyCalibrationOffsetHz"] = frequencyCalibrationOffsetHz;
    analysis["amplitudeCalibrationOffsetDb"] = amplitudeCalibrationOffsetDb;
    analysis["effectiveFrequencyCalibrationOffsetHz"] =
        effectiveFrequencyCalibrationOffsetHz(pendingSettings.centerFrequency);
    analysis["effectiveAmplitudeCalibrationOffsetDb"] =
        effectiveAmplitudeCalibrationOffsetDb(pendingSettings.centerFrequency);
    analysis["calibrationTableEnabled"] = calibrationTableEnabled;
    QJsonArray calibrationPoints;
    if (calibrationTableEnabled) {
        for (const ReceiverCalibrationPoint &point : receiverCalibrationTable.points()) {
            QJsonObject object;
            object["frequencyHz"] = point.frequencyHz;
            object["frequencyOffsetHz"] = point.frequencyOffsetHz;
            object["amplitudeOffsetDb"] = point.amplitudeOffsetDb;
            object["uncertaintyDb"] = point.uncertaintyDb;
            object["note"] = point.note;
            calibrationPoints.append(object);
        }
    }
    analysis["calibrationTable"] = calibrationPoints;
    analysis["maxHoldEnabled"] = spectrumScienceMaxHoldEnabled;
    analysis["minHoldEnabled"] = spectrumScienceMinHoldEnabled;
    analysis["averageEnabled"] = spectrumScienceAverageEnabled;
    analysis["averageTimeSeconds"] = spectrumScienceAverageSeconds;
    analysis["detectorMode"] = spectrumDetectorMode;
    analysis["detectorFrames"] = spectrumDetectorFrames;
    analysis["vbwHz"] = spectrumVbwHz;
    analysis["fftOverlapPercent"] = spectrumFftOverlapPercent;
    analysis["averageFrameCount"] = spectrumAverageFrameCount;
    analysis["percentile50Enabled"] = spectrumPercentile50Enabled;
    analysis["percentile90Enabled"] = spectrumPercentile90Enabled;
    analysis["percentile99Enabled"] = spectrumPercentile99Enabled;
    analysis["amplitudeUnit"] = spectrumAmplitudeUnit;
    analysis["amplitudeUnitName"] = spectrumAmplitudeUnit == 1
                                         ? QStringLiteral("dBm")
                                         : spectrumAmplitudeUnit == 2
                                               ? QStringLiteral("dBuV_50ohm")
                                               : spectrumAmplitudeUnit == 3
                                                     ? QStringLiteral("uV_50ohm")
                                                     : QStringLiteral("dBFS");
    snapshot["analysis"] = analysis;

    auto markerJson = [](const SpectrumScienceMarker &marker) {
        QJsonObject object;
        object["enabled"] = marker.enabled;
        object["label"] = marker.label;
        if (marker.enabled) {
            object["frequencyHz"] = marker.frequencyHz;
            object["levelDb"] = marker.levelDb;
        }
        return object;
    };
    QJsonObject markers;
    markers["a"] = markerJson(spectrumScienceAnalyzer.marker(0));
    markers["b"] = markerJson(spectrumScienceAnalyzer.marker(1));
    snapshot["markers"] = markers;

    const SpectrumScienceMetrics &metrics = spectrumScienceAnalyzer.metrics();
    QJsonObject measurement;
    measurement["valid"] = metrics.valid;
    if (metrics.valid) {
        measurement["rangeStartHz"] = metrics.rangeStartHz;
        measurement["rangeEndHz"] = metrics.rangeEndHz;
        measurement["peakFrequencyHz"] = metrics.peakFrequencyHz;
        measurement["centroidFrequencyHz"] = metrics.centroidFrequencyHz;
        measurement["occupiedBandwidth99Hz"] = metrics.occupiedBandwidthHz;
        measurement["occupiedBandwidth90Hz"] = metrics.occupiedBandwidth90Hz;
        measurement["occupiedBandwidth95Hz"] = metrics.occupiedBandwidth95Hz;
        measurement["width3DbHz"] = metrics.width3DbHz;
        measurement["width6DbHz"] = metrics.width6DbHz;
        measurement["width20DbHz"] = metrics.width20DbHz;
        measurement["peakDb"] = metrics.peakDb;
        measurement["noiseFloorDb"] = metrics.noiseFloorDb;
        measurement["snrDb"] = metrics.snrDb;
        measurement["channelPowerDb"] = metrics.channelPowerDb;
        measurement["adjacentPowerLowerDb"] = metrics.adjacentPowerLowerDb;
        measurement["adjacentPowerUpperDb"] = metrics.adjacentPowerUpperDb;
        measurement["acprLowerDb"] = metrics.acprLowerDb;
        measurement["acprUpperDb"] = metrics.acprUpperDb;
        measurement["binCount"] = metrics.binCount;
    }
    snapshot["measurement"] = measurement;
    return snapshot;
}

QJsonObject YourClassName::recordingScientificMetadata(const QString &captureKind) const {
    QJsonObject metadata;
    metadata["schema"] = QStringLiteral("fobosapp-scientific-recording");
    metadata["schemaVersion"] = 1;
    metadata["captureKind"] = captureKind;
    metadata["start"] = recordingScientificSnapshot(QStringLiteral("start"));

    QJsonObject privacy;
    privacy["coordinatesIncluded"] = false;
    privacy["deviceSerialIncluded"] = false;
    privacy["decryptionKeysIncluded"] = false;
    privacy["apiTokensIncluded"] = false;
    privacy["note"] = QStringLiteral("Sensitive location, receiver serial, keys and API tokens are intentionally excluded.");
    metadata["privacy"] = privacy;
    return metadata;
}

bool YourClassName::isChannelIqRecordingActive() const {
    return recordingManager &&
           recordingManager->isRecording() &&
           recordingManager->mode() == RecordingManager::Mode::ChannelIqWav;
}

bool YourClassName::spectrumEventIqProducerNeeded() const {
    return spectrumEventCaptureMode > 0 &&
           (spectrumFrameBufferEnabled || spectrumFrameRecorder.isRecording());
}

bool YourClassName::spectrumEventIqChannelized() const {
    return spectrumEventCaptureMode == 1;
}

bool YourClassName::dmrAudioIqProducerNeeded() const {
    return audioProcessor && pendingSettings.audioEnabled &&
           pendingSettings.modulationType == MOD_DMR && pendingSettings.inputMode == INPUT_RF &&
           !isNetworkClientMode() &&
           !(networkMode == NetworkMode::Server && isClientIqProcessingMode()) &&
           (isChannelIqRecordingActive() ||
            (spectrumEventIqProducerNeeded() && spectrumEventIqChannelized())) &&
           !(spectrumEventIqProducerNeeded() && spectrumEventCaptureMode == 2);
}

void YourClassName::handleLocalIqFrame(const QByteArray &iqData, double sampleRate,
                                      int sampleCount, bool channelized) {
    if (channelized && isChannelIqRecordingActive()) {
        recordingManager->appendIqFrame(iqData, sampleRate, sampleCount);
    }
    handleSpectrumEventIqFrame(iqData, sampleRate, sampleCount, channelized);
    if (channelized) processVideoIqFrame(iqData, sampleRate, sampleCount);
    if (channelized == isChannelIqProcessingMode()) {
        sendNetworkIqFrame(iqData, sampleRate, sampleCount);
    }
}

void YourClassName::updateIqFrameProducerSettings() {
    const bool dmrAudioIq = dmrAudioIqProducerNeeded();
    if (audioProcessor) audioProcessor->setDmrChannelIqEnabled(dmrAudioIq);
    if (!processor) {
        return;
    }

    const bool serverIqStreaming = networkMode == NetworkMode::Server && isClientIqProcessingMode();
    const bool serverFullIqStreaming = networkMode == NetworkMode::Server && isFullIqProcessingMode();
    const bool serverChannelIqStreaming = networkMode == NetworkMode::Server && isChannelIqProcessingMode();
    const bool channelIqRecording = isChannelIqRecordingActive();
    const bool eventIq = spectrumEventIqProducerNeeded();
    const bool eventFullIq = eventIq && spectrumEventCaptureMode == 2;
    const bool eventChannelIq = eventIq && spectrumEventIqChannelized();
    const bool atvChannelIq = isVideoDecodeActive() &&
                              pendingSettings.modulationType == MOD_ATV;
    processor->configureNetworkIqStreaming(pendingSettings,
                                           serverIqStreaming || atvChannelIq ||
                                               (!dmrAudioIq && (channelIqRecording || eventIq)),
                                           !serverFullIqStreaming &&
                                               !eventFullIq &&
                                               (serverChannelIqStreaming || channelIqRecording ||
                                                eventChannelIq || atvChannelIq));
}

void YourClassName::updateRecordingStatus(const QString &status) {
    if (recordingStatusLabel) {
        recordingStatusLabel->setProperty("statusRawText", status);
        const QString localized = localizedStatusText(status);
        recordingStatusLabel->setText(localized);
        recordingStatusLabel->setToolTip(localized);
    }
    if (recordingManager && !recordingManager->isRecording() &&
        recordButton && recordButton->isChecked()) {
        QSignalBlocker blocker(recordButton);
        recordButton->setChecked(false);
        recordButton->setText(uiText(QStringLiteral("record"), QStringLiteral("Record")));
        if (recordingModeCombo) recordingModeCombo->setEnabled(true);
        momentaryRecordingActive = false;
        updateIqFrameProducerSettings();
    }
}

void YourClassName::updateSpectrumFrameRecordingStatus(const QString &status) {
    if (spectrumFrameRecordingStatusLabel) {
        spectrumFrameRecordingStatusLabel->setProperty("statusRawText", status);
        const QString localized = localizedStatusText(status);
        spectrumFrameRecordingStatusLabel->setText(localized);
        spectrumFrameRecordingStatusLabel->setToolTip(localized);
    }
}

void YourClassName::startSpectrumFrameRecording() {
    if (spectrumFrameRecorder.isRecording()) {
        return;
    }

    QString errorMessage;
    const int bins = spectrumFrameBinsCombo ? spectrumFrameBinsCombo->currentData().toInt() : 4096;
    const bool wantEventIq = spectrumEventCaptureMode > 0;
    const bool wantChannelized = spectrumEventIqChannelized();
    const qint64 firstSpectrumUtcMs = (!spectrumFramePrebuffer.empty() && spectrumFrameBufferEnabled)
                                          ? spectrumFramePrebuffer.front().utcMs
                                          : 0;
    qint64 firstIqUtcMs = 0;
    if (wantEventIq && spectrumFrameBufferEnabled) {
        for (const SpectrumEventIqFrame &frame : spectrumIqPrebuffer) {
            if (frame.channelized == wantChannelized) {
                firstIqUtcMs = frame.utcMs;
                break;
            }
        }
    }

    qint64 firstUtcMs = firstSpectrumUtcMs;
    if (wantEventIq && spectrumFrameBufferEnabled) {
        if (firstSpectrumUtcMs > 0 && firstIqUtcMs > 0) {
            firstUtcMs = (std::max)(firstSpectrumUtcMs, firstIqUtcMs);
        } else if (firstIqUtcMs > 0) {
            firstUtcMs = firstIqUtcMs;
        } else {
            firstUtcMs = 0;
        }
    }
    const QJsonObject extendedMetadata = extendedRecordingMetadataEnabled
                                             ? recordingScientificMetadata(QStringLiteral("spectrum_event"))
                                             : QJsonObject();
    spectrumFrameRecorder.setExtendedMetadata(extendedMetadata);
    spectrumIqEventRecorder.setExtendedMetadata(extendedMetadata);
    if (!spectrumFrameRecorder.start(pendingSettings, effectiveScalePercent(), bins, firstUtcMs, &errorMessage)) {
        updateSpectrumFrameRecordingStatus(QStringLiteral("Spectrum recording failed: %1").arg(errorMessage));
        if (spectrumFrameRecordButton) {
            QSignalBlocker blocker(spectrumFrameRecordButton);
            spectrumFrameRecordButton->setChecked(false);
        }
        return;
    }

    spectrumEventBasePath = spectrumFrameRecorder.currentFilePath();
    if (spectrumEventBasePath.endsWith(QStringLiteral("_spectrum.fbspec"))) {
        spectrumEventBasePath.chop(QStringLiteral("_spectrum.fbspec").size());
    }

    if (spectrumFrameBufferEnabled && firstUtcMs > 0 && !spectrumFramePrebuffer.empty()) {
        for (const SpectrumFrameRecord &frame : spectrumFramePrebuffer) {
            if (frame.utcMs < firstUtcMs) {
                continue;
            }
            if (!spectrumFrameRecorder.appendFrameRecord(frame)) {
                spectrumFrameRecorder.stop();
                updateSpectrumFrameRecordingStatus(QStringLiteral("Spectrum recording failed: pre-buffer write error"));
                if (spectrumFrameRecordButton) {
                    QSignalBlocker blocker(spectrumFrameRecordButton);
                    spectrumFrameRecordButton->setChecked(false);
                }
                return;
            }
        }
    }

    if (spectrumEventCaptureMode > 0 && !spectrumIqPrebuffer.empty()) {
        auto firstIqIt = std::find_if(spectrumIqPrebuffer.begin(),
                                      spectrumIqPrebuffer.end(),
                                      [wantChannelized, firstUtcMs](const SpectrumEventIqFrame &frame) {
                                          return frame.channelized == wantChannelized &&
                                                 (firstUtcMs <= 0 || frame.utcMs >= firstUtcMs);
                                      });
        if (firstIqIt != spectrumIqPrebuffer.end()) {
            const SpectrumEventIqFrame &firstIq = *firstIqIt;
            const SpectrumIqEventRecorder::Mode iqMode =
                wantChannelized
                    ? SpectrumIqEventRecorder::Mode::ChannelIqS16
                    : SpectrumIqEventRecorder::Mode::FullIqS8;
            if (!spectrumIqEventRecorder.start(spectrumEventBasePath,
                                               iqMode,
                                               pendingSettings,
                                               firstIq.sampleRate,
                                               firstIq.utcMs,
                                               spectrumFrameRecorder.firstFrameUtcMs(),
                                               &errorMessage)) {
                qDebug() << "[SpectrumEventIQ] failed to start prebuffer IQ recorder" << errorMessage;
            } else {
                for (auto it = firstIqIt; it != spectrumIqPrebuffer.end(); ++it) {
                    const SpectrumEventIqFrame &frame = *it;
                    if (frame.channelized == wantChannelized) {
                        spectrumIqEventRecorder.appendFrame(frame.data, frame.sampleRate, frame.sampleCount, frame.utcMs);
                    }
                }
            }
        }
    }

    if (spectrumFrameBinsCombo) {
        spectrumFrameBinsCombo->setEnabled(false);
    }
    if (spectrumFramePrebufferSpin) {
        spectrumFramePrebufferSpin->setEnabled(false);
    }
    if (spectrumFrameRecordButton) {
        QSignalBlocker blocker(spectrumFrameRecordButton);
        spectrumFrameRecordButton->setChecked(true);
        spectrumFrameRecordButton->setText(uiText(QStringLiteral("stop_rec"), QStringLiteral("Stop Rec")));
    }
    updateSpectrumFrameRecordingStatus(
        QStringLiteral("Spectrum recording: %1 (%2 pre)")
            .arg(QFileInfo(spectrumFrameRecorder.currentFilePath()).fileName())
            .arg(spectrumFrameRecorder.frameCount()));
    qDebug() << "[SpectrumRecorder] started"
             << spectrumFrameRecorder.currentFilePath()
             << "bins" << spectrumFrameRecorder.targetBins()
             << "eventMode" << spectrumEventCaptureMode
             << "spectrumFirstUtc" << firstSpectrumUtcMs
             << "iqFirstUtc" << firstIqUtcMs
             << "commonFirstUtc" << spectrumFrameRecorder.firstFrameUtcMs();
    updateIqFrameProducerSettings();
}

void YourClassName::stopSpectrumFrameRecording() {
    if (!spectrumFrameRecorder.isRecording()) {
        if (spectrumFrameRecordButton && spectrumFrameRecordButton->isChecked()) {
            QSignalBlocker blocker(spectrumFrameRecordButton);
            spectrumFrameRecordButton->setChecked(false);
            spectrumFrameRecordButton->setText(uiText(QStringLiteral("spectrum_rec"), QStringLiteral("Spectrum Rec")));
        }
        return;
    }

    const QString finishedFile = spectrumFrameRecorder.currentFilePath();
    const quint64 frames = spectrumFrameRecorder.frameCount();
    const quint64 iqBytes = spectrumIqEventRecorder.bytesWritten();
    if (spectrumIqEventRecorder.isRecording()) {
        spectrumIqEventRecorder.stop();
    }
    spectrumFrameRecorder.stop();
    if (spectrumFrameBinsCombo) {
        spectrumFrameBinsCombo->setEnabled(true);
    }
    if (spectrumFramePrebufferSpin) {
        spectrumFramePrebufferSpin->setEnabled(true);
    }
    if (spectrumFrameRecordButton) {
        QSignalBlocker blocker(spectrumFrameRecordButton);
        spectrumFrameRecordButton->setChecked(false);
        spectrumFrameRecordButton->setText(uiText(QStringLiteral("spectrum_rec"), QStringLiteral("Spectrum Rec")));
    }
    updateSpectrumFrameRecordingStatus(
        frames > 0
            ? QStringLiteral("Spectrum saved: %1 (%2 frames, IQ %3 MB)")
                  .arg(QFileInfo(finishedFile).fileName())
                  .arg(frames)
                  .arg(static_cast<double>(iqBytes) / (1024.0 * 1024.0), 0, 'f', 1)
            : QStringLiteral("Spectrum recording stopped: no frames"));
    qDebug() << "[SpectrumRecorder] stopped"
             << finishedFile
             << "frames" << static_cast<qulonglong>(frames)
             << "iqBytes" << static_cast<qulonglong>(iqBytes);
    updateIqFrameProducerSettings();
}

void YourClassName::openSpectrumFrameReplay() {
    const QString startDir = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("recordings/spectrum"));
    const QString path =
        QFileDialog::getOpenFileName(this,
                                     uiText(QStringLiteral("spectrum_replay"), QStringLiteral("Spectrum replay")),
                                     startDir,
                                     QStringLiteral("Fobos spectrum frames (*.fbspec);;All files (*.*)"));
    if (path.isEmpty()) {
        return;
    }

    QString errorMessage;
    auto *dialog = new SpectrumFrameReplayDialog(this);
    if (!dialog->loadRecording(path, &errorMessage)) {
        dialog->deleteLater();
        QMessageBox::warning(this,
                             uiText(QStringLiteral("spectrum_replay"), QStringLiteral("Spectrum replay")),
                             QStringLiteral("Could not open spectrum recording:\n%1").arg(errorMessage));
        return;
    }
    connect(dialog,
            &SpectrumFrameReplayDialog::replayAudioFrameReady,
            this,
            [this](const QByteArray &pcmData) {
                processDigitalAudioFrame(pcmData);
                processSstvAudioFrame(pcmData);
                processAptAudioFrame(pcmData);
                processWefaxAudioFrame(pcmData);
                sendNetworkAudioFrame(pcmData);
                sendAudioRelayFrame(pcmData);
                sendAudioHttpFrame(pcmData);
#if !defined(_WIN32) && defined(FOBOSAPP_HAS_QT_AUDIO)
                const bool suppressServerLocalOutput =
                    networkMode == NetworkMode::Server &&
                    serverDisableLocalVisualAudio &&
                    networkController &&
                    networkController->isControlReady();
                if (remoteAudioPlayer && !suppressServerLocalOutput) {
                    remoteAudioPlayer->playPcmFrame(pcmData);
                }
#endif
            },
            Qt::QueuedConnection);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->show();
}

void YourClassName::handleSpectrumEventIqFrame(const QByteArray &iqData,
                                               double sampleRate,
                                               int sampleCount,
                                               bool channelized) {
    if (iqData.isEmpty() || sampleRate <= 0.0 || sampleCount <= 0 || spectrumEventCaptureMode <= 0) {
        return;
    }
    const bool wantChannelized = spectrumEventIqChannelized();
    if (channelized != wantChannelized) {
        return;
    }

    SpectrumEventIqFrame frame;
    frame.data = iqData;
    frame.sampleRate = sampleRate;
    frame.sampleCount = sampleCount;
    frame.channelized = channelized;
    frame.utcMs = QDateTime::currentMSecsSinceEpoch();

    if (spectrumFrameBufferEnabled) {
        spectrumIqPrebuffer.push_back(frame);
        const qint64 keepMs = static_cast<qint64>((std::max)(1, spectrumFramePrebufferSeconds)) * 1000;
        while (!spectrumIqPrebuffer.empty() &&
               frame.utcMs - spectrumIqPrebuffer.front().utcMs > keepMs) {
            spectrumIqPrebuffer.pop_front();
        }
    }

    if (!spectrumFrameRecorder.isRecording()) {
        return;
    }

    if (!spectrumIqEventRecorder.isRecording()) {
        QString errorMessage;
        if (spectrumEventBasePath.isEmpty()) {
            spectrumEventBasePath = spectrumFrameRecorder.currentFilePath();
            if (spectrumEventBasePath.endsWith(QStringLiteral("_spectrum.fbspec"))) {
                spectrumEventBasePath.chop(QStringLiteral("_spectrum.fbspec").size());
            }
        }
        const SpectrumIqEventRecorder::Mode iqMode =
            channelized
                ? SpectrumIqEventRecorder::Mode::ChannelIqS16
                : SpectrumIqEventRecorder::Mode::FullIqS8;
        if (!spectrumIqEventRecorder.start(spectrumEventBasePath,
                                           iqMode,
                                           pendingSettings,
                                           sampleRate,
                                           frame.utcMs,
                                           spectrumFrameRecorder.firstFrameUtcMs(),
                                           &errorMessage)) {
            qDebug() << "[SpectrumEventIQ] failed to start IQ event recorder" << errorMessage;
            return;
        }
    }
    if (!spectrumIqEventRecorder.appendFrame(iqData, sampleRate, sampleCount, frame.utcMs)) {
        qDebug() << "[SpectrumEventIQ] IQ write failed";
    }
}

QString YourClassName::selectedPlaybackFilePath() const {
    if (!playbackFileCombo) {
        return QString();
    }
    return playbackFileCombo->currentData().toString();
}

void YourClassName::startRecording(bool momentary) {
    if (!recordingManager) {
        return;
    }
    if (recordingManager->isRecording()) {
        if (momentary) {
            momentaryRecordingActive = false;
        }
        return;
    }

    const RecordingManager::Mode mode = selectedRecordingMode();
    if (mode == RecordingManager::Mode::ChannelIqWav &&
        ((networkMode != NetworkMode::Disabled && isFullIqProcessingMode()) ||
         (spectrumEventIqProducerNeeded() && spectrumEventCaptureMode == 2))) {
        updateRecordingStatus(QStringLiteral("Recording blocked: Channel IQ cannot run during Full IQ streaming"));
        if (recordButton) {
            QSignalBlocker blocker(recordButton);
            recordButton->setChecked(false);
            recordButton->setText(uiText(QStringLiteral("record"), QStringLiteral("Record")));
        }
        momentaryRecordingActive = false;
        return;
    }

    QString errorMessage;
    recordingManager->setDisplayScalePercent(effectiveScalePercent());
    activeRecordingLabMetadata = recordingLabMetadata();
    if (extendedRecordingMetadataEnabled) {
        activeRecordingLabMetadata["scientificMetadata"] = recordingScientificMetadata(
            mode == RecordingManager::Mode::ChannelIqWav
                ? QStringLiteral("channel_iq_wav")
                : QStringLiteral("audio_wav"));
    }
    recordingManager->setLabMetadata(activeRecordingLabMetadata);
    if (!recordingManager->start(mode, pendingSettings, &errorMessage)) {
        activeRecordingLabMetadata = QJsonObject();
        updateRecordingStatus(QStringLiteral("Recording failed: %1").arg(errorMessage));
        if (recordButton) {
            QSignalBlocker blocker(recordButton);
            recordButton->setChecked(false);
            recordButton->setText(uiText(QStringLiteral("record"), QStringLiteral("Record")));
        }
        momentaryRecordingActive = false;
        return;
    }

    momentaryRecordingActive = momentary;
    if (recordButton) {
        QSignalBlocker blocker(recordButton);
        recordButton->setChecked(true);
        recordButton->setText(momentary
                                  ? uiText(QStringLiteral("hold_f9"), QStringLiteral("Hold F9"))
                                  : uiText(QStringLiteral("stop_rec"), QStringLiteral("Stop Rec")));
    }
    if (recordingModeCombo) {
        recordingModeCombo->setEnabled(false);
    }
    if (mode == RecordingManager::Mode::ChannelIqWav) {
        updateIqFrameProducerSettings();
    }
}

void YourClassName::stopRecording(bool momentaryRelease) {
    if (!recordingManager) {
        return;
    }
    if (momentaryRelease && !momentaryRecordingActive) {
        return;
    }

    const bool wasChannelIqRecording = isChannelIqRecordingActive();
    momentaryRecordingActive = false;
    if (activeRecordingLabMetadata.contains(QStringLiteral("scientificMetadata"))) {
        QJsonObject scientific = activeRecordingLabMetadata
                                     .value(QStringLiteral("scientificMetadata"))
                                     .toObject();
        scientific["end"] = recordingScientificSnapshot(QStringLiteral("end"));
        activeRecordingLabMetadata["scientificMetadata"] = scientific;
        recordingManager->setLabMetadata(activeRecordingLabMetadata);
    }
    recordingManager->stop();
    activeRecordingLabMetadata = QJsonObject();
    if (recordButton) {
        QSignalBlocker blocker(recordButton);
        recordButton->setChecked(false);
        recordButton->setText(uiText(QStringLiteral("record"), QStringLiteral("Record")));
    }
    if (recordingModeCombo) {
        recordingModeCombo->setEnabled(true);
    }
    if (wasChannelIqRecording) {
        updateIqFrameProducerSettings();
    }
}

void YourClassName::refreshPlaybackFiles() {
    if (!playbackFileCombo) {
        return;
    }

    const QString previousPath = selectedPlaybackFilePath();
    playbackFileCombo->blockSignals(true);
    playbackFileCombo->clear();

    QDir recordingsDir(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("recordings")));
    const QFileInfoList files = recordingsDir.entryInfoList(QStringList() << QStringLiteral("*.wav"),
                                                            QDir::Files,
                                                            QDir::Time);
    for (const QFileInfo &fileInfo : files) {
        PlaybackManager::WavInfo info;
        if (!PlaybackManager::readWavInfo(fileInfo.absoluteFilePath(), info)) {
            continue;
        }

        const double seconds = info.dataSize / static_cast<double>(
            info.sampleRate * info.channels * (info.bitsPerSample / 8));
        const QString type = info.mode == PlaybackManager::Mode::AudioWav
                                 ? QStringLiteral("Audio")
                                 : QStringLiteral("Ch IQ");
        const QString label = QStringLiteral("%1  %2  %3 Hz  %4 s")
                                  .arg(fileInfo.fileName(),
                                       type,
                                       QString::number(info.sampleRate),
                                       QString::number(seconds, 'f', 1));
        playbackFileCombo->addItem(label, fileInfo.absoluteFilePath());
    }

    if (playbackFileCombo->count() == 0) {
        playbackFileCombo->addItem(uiText(QStringLiteral("no_wav_recordings_found"),
                                          QStringLiteral("No WAV recordings found")),
                                   QString());
        playbackFileCombo->setEnabled(false);
        if (playbackButton) {
            playbackButton->setEnabled(false);
        }
    } else {
        playbackFileCombo->setEnabled(true);
        if (playbackButton) {
            playbackButton->setEnabled(true);
        }
        const int previousIndex = playbackFileCombo->findData(previousPath);
        if (previousIndex >= 0) {
            playbackFileCombo->setCurrentIndex(previousIndex);
        }
    }

    playbackFileCombo->blockSignals(false);
}

void YourClassName::startPlayback() {
    if (!playbackManager) {
        return;
    }
    if (playbackManager->isPlaying()) {
        return;
    }
    if (runState != RadioRunState::Idle || deviceOpened || (processor && processor->isRunning())) {
        onPlaybackStatusChanged(QStringLiteral("Playback blocked: stop receiver first"));
        if (playbackButton) {
            QSignalBlocker blocker(playbackButton);
            playbackButton->setChecked(false);
        }
        return;
    }
    if (recordingManager && recordingManager->isRecording()) {
        onPlaybackStatusChanged(QStringLiteral("Playback blocked: stop recording first"));
        if (playbackButton) {
            QSignalBlocker blocker(playbackButton);
            playbackButton->setChecked(false);
        }
        return;
    }

    const QString path = selectedPlaybackFilePath();
    if (path.isEmpty()) {
        onPlaybackStatusChanged(QStringLiteral("Playback: no file selected"));
        if (playbackButton) {
            QSignalBlocker blocker(playbackButton);
            playbackButton->setChecked(false);
        }
        return;
    }

    PlaybackManager::WavInfo info;
    QString errorMessage;
    if (!PlaybackManager::readWavInfo(path, info, &errorMessage)) {
        onPlaybackStatusChanged(QStringLiteral("Playback failed: %1").arg(errorMessage));
        if (playbackButton) {
            QSignalBlocker blocker(playbackButton);
            playbackButton->setChecked(false);
        }
        return;
    }
    if (info.mode == PlaybackManager::Mode::AudioWav && (info.channels != 1 || info.sampleRate != 48000)) {
        onPlaybackStatusChanged(QStringLiteral("Playback failed: audio WAV must be mono 48 kHz"));
        if (playbackButton) {
            QSignalBlocker blocker(playbackButton);
            playbackButton->setChecked(false);
        }
        return;
    }

    if (!playbackManager->start(path, &errorMessage)) {
        onPlaybackStatusChanged(QStringLiteral("Playback failed: %1").arg(errorMessage));
        if (playbackButton) {
            QSignalBlocker blocker(playbackButton);
            playbackButton->setChecked(false);
        }
    }
}

void YourClassName::stopPlayback() {
    if (playbackManager && playbackManager->isPlaying()) {
        playbackManager->stop();
        return;
    }
    if (playbackButton && playbackButton->isChecked()) {
        QSignalBlocker blocker(playbackButton);
        playbackButton->setChecked(false);
        playbackButton->setText(uiText(QStringLiteral("play"), QStringLiteral("Play")));
    }
}

void YourClassName::onPlaybackStarted(const QString &filePath, PlaybackManager::WavInfo info) {
    Q_UNUSED(filePath);
    if (playbackButton) {
        QSignalBlocker blocker(playbackButton);
        playbackButton->setChecked(true);
        playbackButton->setText(uiText(QStringLiteral("stop_play"), QStringLiteral("Stop Play")));
    }
    if (playbackFileCombo) {
        playbackFileCombo->setEnabled(false);
    }
    if (playbackRefreshButton) {
        playbackRefreshButton->setEnabled(false);
    }
    if (startButton) {
        startButton->setEnabled(false);
    }

    if (info.mode == PlaybackManager::Mode::ChannelIqWav) {
        if (!playbackSettingsSaved) {
            settingsBeforePlayback = pendingSettings;
            playbackSettingsSaved = true;
        }
        offlineIqPlaybackActive = true;
        offlineIqPlaybackHasMetadata = info.hasRadioSettings;
        offlineIqPlaybackSampleRate = info.sampleRate;
        IqBuffer::clear();
        IqBuffer::setSampleRateEstimate(info.sampleRate);
        const bool audioEnabledBeforePlayback = pendingSettings.audioEnabled;
        const int audioDeviceBeforePlayback = pendingSettings.audioDeviceId;
        if (info.hasRadioSettings) {
            pendingSettings.deviceIndex = info.radioSettings.deviceIndex;
            pendingSettings.clockSource = info.radioSettings.clockSource;
            pendingSettings.inputMode = info.radioSettings.inputMode;
            pendingSettings.centerFrequency = info.radioSettings.centerFrequency;
            pendingSettings.actualFrequency = info.radioSettings.actualFrequency;
            pendingSettings.listeningFrequency = info.radioSettings.listeningFrequency;
            pendingSettings.sampleRate = info.radioSettings.sampleRate;
            pendingSettings.bandwidth = info.radioSettings.bandwidth;
            pendingSettings.modulationType = info.radioSettings.modulationType;
            pendingSettings.fftLength = info.radioSettings.fftLength;
            if (info.radioSettings.lnaGain >= 0) {
                pendingSettings.lnaGain = info.radioSettings.lnaGain;
            }
            if (info.radioSettings.vgaGain >= 0) {
                pendingSettings.vgaGain = info.radioSettings.vgaGain;
            }
            const double playbackCenter = info.radioSettings.listeningFrequency > 0.0
                                              ? info.radioSettings.listeningFrequency
                                              : info.radioSettings.centerFrequency;
            if (playbackCenter > 0.0) {
                pendingSettings.listeningFrequency = playbackCenter;
            }
        } else {
            pendingSettings.sampleRate = info.sampleRate;
            pendingSettings.centerFrequency = pendingSettings.listeningFrequency;
            pendingSettings.actualFrequency = pendingSettings.listeningFrequency;
            pendingSettings.inputMode = INPUT_RF;
        }
        pendingSettings.audioEnabled = audioEnabledBeforePlayback;
        pendingSettings.audioDeviceId = audioDeviceBeforePlayback;
        if (info.hasScalePercent) {
            const double recordedScale = (std::max)(MIN_SCALE_PERCENT / 20.0,
                                                     info.scalePercent);
            additionalScaleDivisor = recordedScale < MIN_SCALE_PERCENT
                                         ? ADDITIONAL_SCALE_DIVISOR_MAX
                                         : 1;
            currentScale = std::clamp(recordedScale * additionalScaleDivisor,
                                      static_cast<double>(minScale) / 10.0,
                                      static_cast<double>(maxScale) / 10.0);
        } else if (info.hasRadioSettings &&
                   info.radioSettings.sampleRate > 0.0 &&
                   info.sampleRate > 0) {
            const double channelScalePercent =
                (static_cast<double>(info.sampleRate) / info.radioSettings.sampleRate) * 100.0;
            const double recordedScale = (std::max)(MIN_SCALE_PERCENT / 20.0,
                                                     channelScalePercent);
            additionalScaleDivisor = recordedScale < MIN_SCALE_PERCENT
                                         ? ADDITIONAL_SCALE_DIVISOR_MAX
                                         : 1;
            currentScale = std::clamp(recordedScale * additionalScaleDivisor,
                                      static_cast<double>(minScale) / 10.0,
                                      static_cast<double>(maxScale) / 10.0);
        }
        publishSettingsToGlobals();
        updateUiFromPendingSettings();
        fftResult = std::make_unique<FFTResult>();
        if (spectrumFftWorker) {
            spectrumFftWorker->resetHfNoiseCancelState();
        }
        updateSpectrumTimerInterval();
        if (audioProcessor) {
            audioProcessor->configure(audioProcessorSettings());
        }
        pendingPlaybackAudioStartAfterIqPrebuffer = pendingSettings.audioEnabled;
        if (updateTimer) {
            updateTimer->start();
        }
        settingRange();
    } else {
        offlineIqPlaybackActive = false;
        offlineIqPlaybackHasMetadata = false;
        offlineIqPlaybackSampleRate = 0.0;
    }
}

void YourClassName::onPlaybackStopped() {
    if (offlineIqPlaybackActive) {
        offlineIqPlaybackActive = false;
        offlineIqPlaybackHasMetadata = false;
        offlineIqPlaybackSampleRate = 0.0;
        pendingPlaybackAudioStartAfterIqPrebuffer = false;
        if (audioProcessor) {
            audioProcessor->stopDemodulation();
        }
        if (updateTimer && runState == RadioRunState::Idle) {
            updateTimer->stop();
        }
    }
    if (playbackSettingsSaved) {
        pendingSettings = settingsBeforePlayback;
        playbackSettingsSaved = false;
        publishSettingsToGlobals();
        updateUiFromPendingSettings();
    }
    if (remoteAudioPlayer && runState == RadioRunState::Idle) {
        remoteAudioPlayer->stop();
    }
    if (playbackButton) {
        QSignalBlocker blocker(playbackButton);
        playbackButton->setChecked(false);
        playbackButton->setText(uiText(QStringLiteral("play"), QStringLiteral("Play")));
    }
    if (playbackFileCombo) {
        playbackFileCombo->setEnabled(playbackFileCombo->count() > 0 && !selectedPlaybackFilePath().isEmpty());
    }
    if (playbackRefreshButton) {
        playbackRefreshButton->setEnabled(true);
    }
    updateUiForRunState();
}

void YourClassName::onPlaybackStatusChanged(const QString &status) {
    if (playbackStatusLabel) {
        playbackStatusLabel->setProperty("statusRawText", status);
        const QString localized = localizedStatusText(status);
        playbackStatusLabel->setText(localized);
        playbackStatusLabel->setToolTip(localized);
    }
}

void YourClassName::handlePlaybackAudioFrame(const QByteArray &pcmData) {
    processDigitalAudioFrame(pcmData);
    processSstvAudioFrame(pcmData);
    processAptAudioFrame(pcmData);
    processWefaxAudioFrame(pcmData);
    sendAudioRelayFrame(pcmData);
    sendAudioHttpFrame(pcmData);
    if (remoteAudioPlayer) {
        remoteAudioPlayer->playPcmFrame(pcmData);
    }
}

void YourClassName::handlePlaybackIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount) {
    if (iqData.size() < static_cast<int>(2 * sizeof(qint16))) {
        return;
    }
    processGnssPackedIqFrame(iqData,
                             sampleRate,
                             sampleCount > 0 ? sampleCount : iqData.size() / (2 * static_cast<int>(sizeof(qint16))));
    processVideoIqFrame(iqData,
                        sampleRate,
                        sampleCount > 0 ? sampleCount : iqData.size() / (2 * static_cast<int>(sizeof(qint16))));
    const int bytesPerIq = 2 * static_cast<int>(sizeof(qint16));
    std::vector<float> floatSamples(static_cast<std::size_t>(iqData.size() / sizeof(qint16)));
    const auto *src = reinterpret_cast<const uchar *>(iqData.constData());
    for (int i = 0, out = 0; i + bytesPerIq - 1 < iqData.size(); i += bytesPerIq) {
        const qint16 iSample = static_cast<qint16>(src[i] | (src[i + 1] << 8));
        const qint16 qSample = static_cast<qint16>(src[i + 2] | (src[i + 3] << 8));
        floatSamples[static_cast<std::size_t>(out++)] = iSample / 32768.0f;
        floatSamples[static_cast<std::size_t>(out++)] = qSample / 32768.0f;
    }
    IqBuffer::setSampleRateEstimate(sampleRate);
    IqBuffer::publish(floatSamples.data(), floatSamples.size(), pendingSettings.audioEnabled);
    if (pendingPlaybackAudioStartAfterIqPrebuffer && pendingSettings.audioEnabled && audioProcessor && sampleRate > 0.0) {
        const double queuedIqSamples = static_cast<double>(IqBuffer::queuedFloatCount()) / 2.0;
        const double queuedSeconds = queuedIqSamples / sampleRate;
        if (queuedSeconds >= NETWORK_AUDIO_PREBUFFER_SECONDS) {
            pendingPlaybackAudioStartAfterIqPrebuffer = false;
            qDebug() << "[PlaybackIQ] starting demodulator after IQ prebuffer"
                     << "queuedSeconds" << queuedSeconds
                     << "queuedBlocks" << IqBuffer::queuedBlocks()
                     << "sampleRate" << sampleRate;
            audioProcessor->startDemodulation();
        }
    }
}
