#include "main.h"

#include "appconstants.h"
#include "diagnosticlogging.h"
#include "dmrbackendpaths.h"
#include "dmrprivacyutils.h"
#include "dsdneobridge.h"
#include "gophertrunkbridge.h"
#include "tuningutils.h"

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

int YourClassName::selectedDmrBackend() const {
    if (!dmrBackendCombo) {
        return DMR_BACKEND_FOBOS_MBELIB;
    }
    const int backend = dmrBackendCombo->currentData().toInt();
    switch (backend) {
    case DMR_BACKEND_FOBOS_MBELIB:
    case DMR_BACKEND_FOBOS_OPENDMR:
    case DMR_BACKEND_DSD_NEO:
    case DMR_BACKEND_GOPHERTRUNK:
        return backend;
    default:
        return DMR_BACKEND_FOBOS_MBELIB;
    }
}

QStringList YourClassName::dsdNeoProcessArguments() const {
    const int sampleRate =
        dmrBasebandRateCombo
            ? normalizedDmrBasebandSampleRate(dmrBasebandRateCombo->currentData().toInt())
            : normalizedDmrBasebandSampleRate(pendingSettings.dmrBasebandSampleRate);
    const quint16 outputPort =
        dsdNeoUdpOutputPortSpin ? static_cast<quint16>(dsdNeoUdpOutputPortSpin->value()) : quint16(23456);

    return DsdNeoBridge::dmrProcessArguments(sampleRate, outputPort);
}

void YourClassName::updateDsdNeoBridgeSettings() {
    if (!dsdNeoBridge) {
        return;
    }

    const bool enabled = selectedDmrBackend() == DMR_BACKEND_DSD_NEO;
    bool autoStart = dsdNeoAutoStartCheckbox && dsdNeoAutoStartCheckbox->isChecked();
    const quint16 inputPort =
        dsdNeoInputPortSpin ? static_cast<quint16>(dsdNeoInputPortSpin->value()) : quint16(7355);
    const quint16 outputPort =
        dsdNeoUdpOutputPortSpin ? static_cast<quint16>(dsdNeoUdpOutputPortSpin->value()) : quint16(23456);
    const QString rawProgram =
        dsdNeoProgramEdit ? dsdNeoProgramEdit->text().trimmed() : QString();
    QString program = rawProgram.isEmpty() ? defaultDsdNeoProgramPath() : rawProgram;
    const QString bundledProgram =
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(defaultDsdNeoProgramPath());
    if ((rawProgram.isEmpty() || isLegacyDsdNeoProgramName(rawProgram)) &&
        QFileInfo::exists(bundledProgram)) {
        program = bundledProgram;
        if (dsdNeoProgramEdit && dsdNeoProgramEdit->text().trimmed() != defaultDsdNeoProgramPath()) {
            const QSignalBlocker blocker(dsdNeoProgramEdit);
            dsdNeoProgramEdit->setText(defaultDsdNeoProgramPath());
        }
        if (enabled && !autoStart && dsdNeoAutoStartCheckbox) {
            const QSignalBlocker blocker(dsdNeoAutoStartCheckbox);
            dsdNeoAutoStartCheckbox->setChecked(true);
            autoStart = true;
        }
    }
    QFileInfo programInfo(program);
    if (!programInfo.isAbsolute() &&
        (program.contains(QLatin1Char('/')) || program.contains(QLatin1Char('\\')))) {
        program = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(program);
        programInfo.setFile(program);
    }
    const QString workingDirectory =
        programInfo.isAbsolute() ? programInfo.absolutePath() : QString();

    if (dsdNeoAutoStartCheckbox) {
        dsdNeoAutoStartCheckbox->setEnabled(enabled);
    }
    if (dsdNeoProgramEdit) {
        dsdNeoProgramEdit->setEnabled(enabled && autoStart);
    }
    if (dsdNeoInputPortSpin) {
        dsdNeoInputPortSpin->setEnabled(enabled && !autoStart);
    }
    if (dsdNeoUdpOutputPortSpin) {
        dsdNeoUdpOutputPortSpin->setEnabled(enabled);
    }

    const QString privacyMode = dmrPrivacyModeId(pendingSettings.dmrPrivacyMode);
    const int keyId = pendingSettings.dmrPrivacyKeyId;
    const QString keyHex = pendingSettings.dmrPrivacyKeyHex;
    const bool forward = pendingSettings.dmrPrivacyMode != DMR_PRIVACY_NONE;
    const QStringList arguments = dsdNeoProcessArguments();
    QMetaObject::invokeMethod(dsdNeoBridge,
        [bridge = dsdNeoBridge, inputPort, outputPort, privacyMode, keyId, keyHex,
         forward, autoStart, program, arguments, workingDirectory, enabled]() {
            bridge->configureInputServer(true, inputPort);
            bridge->configureUdpOutput(true, outputPort, 2);
            bridge->configurePrivacy(privacyMode, keyId, keyHex, forward);
            bridge->configureProcess(autoStart, program, arguments, workingDirectory);
            bridge->setEnabled(enabled);
        }, Qt::QueuedConnection);

    if (dsdNeoStatusLabel && !enabled) {
        dsdNeoStatusLabel->setText(uiText(QStringLiteral("dsd_neo_idle"),
                                          QStringLiteral("DSD-neo bridge idle")));
    }
}

void YourClassName::updateGopherTrunkBridgeSettings() {
    if (!gopherTrunkBridge) {
        return;
    }

    const bool enabled = selectedDmrBackend() == DMR_BACKEND_GOPHERTRUNK;
    const quint16 outputPort =
        dsdNeoUdpOutputPortSpin ? static_cast<quint16>(dsdNeoUdpOutputPortSpin->value()) : quint16(23456);
    QString program = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(defaultGopherTrunkProgramPath());
    QFileInfo programInfo(program);
    const QString workingDirectory = programInfo.absolutePath();
    gopherTrunkBridge->configure(true, program, quint16(7460), outputPort, workingDirectory);
    gopherTrunkBridge->configurePrivacy(dmrPrivacyModeId(pendingSettings.dmrPrivacyMode),
                                        pendingSettings.dmrPrivacyKeyId,
                                        pendingSettings.dmrPrivacyKeyHex,
                                        pendingSettings.dmrPrivacyMode != DMR_PRIVACY_NONE,
                                        pendingSettings.dmrPrivacyVariant,
                                        pendingSettings.dmrPrivacyLayout,
                                        pendingSettings.dmrPrivacyFrameOffset);
    gopherTrunkBridge->setEnabled(enabled);
    if (dsdNeoStatusLabel && !enabled && selectedDmrBackend() != DMR_BACKEND_DSD_NEO) {
        dsdNeoStatusLabel->setText(uiText(QStringLiteral("dsd_neo_idle"),
                                          QStringLiteral("DSD-neo bridge idle")));
    }
}

void YourClassName::processDigitalAudioFrame(const QByteArray &pcmData, int sampleRate) {
    if (!digitalDecoder ||
        !digitalDecoderThread ||
        !digitalDecodeEnabled ||
        !digitalDecodeCheckbox ||
        !digitalDecodeCheckbox->isChecked()) {
        return;
    }
    if (sampleRate <= 0) {
        sampleRate = 48000;
    }

    RadioSettings settings = pendingSettings;
    settings.cwDecoderToneHz = cwDecoderToneSpin ? cwDecoderToneSpin->value()
                                                 : settings.cwDecoderToneHz;
    settings.cwDecoderWpm = cwDecoderWpmSpin ? cwDecoderWpmSpin->value()
                                             : settings.cwDecoderWpm;
    settings.cwDecoderAdaptiveSpeed =
        !cwDecoderAdaptiveCheckbox || cwDecoderAdaptiveCheckbox->isChecked();
    settings.cwDecoderSelectivity = cwDecoderSelectivitySpin
                                        ? cwDecoderSelectivitySpin->value()
                                        : settings.cwDecoderSelectivity;
    settings.cwDecoderAlphabet =
        cwDecoderAlphabetCombo ? cwDecoderAlphabetCombo->currentData().toInt()
                               : settings.cwDecoderAlphabet;
    settings.dmrLabEnabled = dmrLabCaptureCheckbox && dmrLabCaptureCheckbox->isChecked();
    settings.dmrLabColorCode = dmrLabColorCodeCombo
                                   ? dmrLabColorCodeCombo->currentData().toInt()
                                   : -1;
    settings.dmrLabTimeslot = dmrLabSlotCombo
                                  ? dmrLabSlotCombo->currentData().toInt()
                                  : 0;
    settings.dmrBasebandSampleRate =
        dmrBasebandRateCombo
            ? normalizedDmrBasebandSampleRate(dmrBasebandRateCombo->currentData().toInt())
            : normalizedDmrBasebandSampleRate(settings.dmrBasebandSampleRate);
    if (dmrChannelRateCombo) {
        settings.dmrChannelSampleRate = dmrChannelRateCombo->currentData().toInt();
    }
    settings.dmrAmbeLayout =
        dmrAmbeLayoutCombo
            ? normalizedDmrAmbeLayout(dmrAmbeLayoutCombo->currentData().toInt())
            : normalizedDmrAmbeLayout(settings.dmrAmbeLayout);
    settings.dmrManualTimingEnabled =
        dmrManualTimingCheckbox && dmrManualTimingCheckbox->isChecked();
    settings.dmrManualTimingOffset =
        dmrTimingOffsetSpin ? dmrTimingOffsetSpin->value() : settings.dmrManualTimingOffset;
    settings.dmrSlicerRatio =
        dmrSlicerRatioSpin ? dmrSlicerRatioSpin->value() : settings.dmrSlicerRatio;
    settings.dmrAdaptiveSlicer =
        !dmrAdaptiveSlicerCheckbox || dmrAdaptiveSlicerCheckbox->isChecked();
    settings.dmrPrivacyMode = pendingSettings.dmrPrivacyMode;
    settings.dmrPrivacyKeyId = pendingSettings.dmrPrivacyKeyId;
    settings.dmrPrivacyKeyHex = pendingSettings.dmrPrivacyKeyHex;
    settings.dmrPrivacyVariant = pendingSettings.dmrPrivacyVariant;
    settings.dmrPrivacyLayout = pendingSettings.dmrPrivacyLayout;
    settings.dmrPrivacyFrameOffset = pendingSettings.dmrPrivacyFrameOffset;
    const auto parseDmrLabId = [](const QLineEdit *edit) {
        if (!edit) {
            return 0;
        }
        bool ok = false;
        const int value = edit->text().trimmed().toInt(&ok);
        return ok && value > 0 ? value : 0;
    };
    settings.dmrLabSourceId = parseDmrLabId(dmrLabSourceIdEdit);
    settings.dmrLabTargetId = parseDmrLabId(dmrLabTargetIdEdit);
    const bool isDmr = settings.modulationType == MOD_DMR;
    const uint64_t decoderGeneration =
        digitalDecoderGeneration.load(std::memory_order_relaxed);
    QByteArray decoderPcmData = pcmData;
    if (isDmr) {
        constexpr int dmrPcmBytesPerSample = static_cast<int>(sizeof(qint16));
        constexpr int dmrPcmChunkMs = 60;
        const int dmrPcmChunkBytes =
            (std::max)(1, sampleRate * dmrPcmBytesPerSample * dmrPcmChunkMs / 1000);
        const int dmrPcmMaxBufferedBytes = dmrPcmChunkBytes * 3;

        if (pendingDmrDecoderSampleRate != sampleRate) {
            qDebug() << "[Digital] DMR input sample-rate changed"
                     << "oldRate" << pendingDmrDecoderSampleRate
                     << "newRate" << sampleRate
                     << "chunkBytes" << dmrPcmChunkBytes;
            if (!pendingDmrDecoderPcm.isEmpty()) {
                qDebug() << "[Digital] clearing DMR input buffer after sample-rate change"
                         << "oldRate" << pendingDmrDecoderSampleRate
                         << "newRate" << sampleRate
                         << "droppedBytes" << pendingDmrDecoderPcm.size();
            }
            pendingDmrDecoderPcm.clear();
            pendingDmrDecoderSampleRate = sampleRate;
        }

        pendingDmrDecoderPcm.append(pcmData);
        if (pendingDmrDecoderPcm.size() > dmrPcmMaxBufferedBytes) {
            const int bytesToDrop = pendingDmrDecoderPcm.size() - dmrPcmChunkBytes;
            pendingDmrDecoderPcm.remove(0, bytesToDrop);
            if (fobosVerboseLoggingEnabled()) {
                qDebug() << "[Digital] trimming DMR PCM input buffer"
                         << "droppedBytes" << bytesToDrop
                         << "keptBytes" << pendingDmrDecoderPcm.size();
            }
        }
        if (pendingDmrDecoderPcm.size() < dmrPcmChunkBytes) {
            return;
        }
        decoderPcmData = pendingDmrDecoderPcm.left(dmrPcmChunkBytes);
        pendingDmrDecoderPcm.remove(0, dmrPcmChunkBytes);
    } else if (!pendingDmrDecoderPcm.isEmpty()) {
        pendingDmrDecoderPcm.clear();
        pendingDmrDecoderSampleRate = 48000;
    }

    const int maxQueuedFrames = isDmr ? 6 : 32;
    const int queuedBefore = pendingDigitalDecoderFrames.fetch_add(1, std::memory_order_relaxed);
    if (queuedBefore >= maxQueuedFrames) {
        pendingDigitalDecoderFrames.fetch_sub(1, std::memory_order_relaxed);
        const int dropped = droppedDigitalDecoderFramesSinceLog.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fobosVerboseLoggingEnabled() && (dropped == 1 || dropped % 50 == 0)) {
            qDebug() << "[Digital] dropping stale PCM frame"
                     << "mode" << settings.modulationType
                     << "queued" << queuedBefore
                     << "limit" << maxQueuedFrames
                     << "dropped" << dropped
                     << "bytes" << decoderPcmData.size();
        }
        return;
    }

    QMetaObject::invokeMethod(digitalDecoder,
                              [this,
                               decoder = digitalDecoder,
                               pcmData = decoderPcmData,
                               settings,
                               sampleRate,
                               decoderGeneration]() {
                                  const auto releaseQueuedFrame = qScopeGuard([this]() {
                                      pendingDigitalDecoderFrames.fetch_sub(1, std::memory_order_relaxed);
                                  });
                                  if (decoderGeneration !=
                                      digitalDecoderGeneration.load(std::memory_order_relaxed)) {
                                      return;
                                  }
                                  decoder->processPcmFrame(pcmData, settings, sampleRate);
                              },
                              Qt::QueuedConnection);
}

void YourClassName::updateDigitalDecoderMode() {
    if (!digitalDecoder || !digitalDecoderThread) {
        return;
    }
    const bool enabled = digitalDecodeEnabled;
    QString preferredDmrVoiceBackendId;
    const int dmrBackend = selectedDmrBackend();
    if (dmrBackend == DMR_BACKEND_FOBOS_MBELIB) {
        preferredDmrVoiceBackendId = QStringLiteral("fobos.dmr.voice.mbelib");
    } else if (dmrBackend == DMR_BACKEND_FOBOS_OPENDMR) {
        preferredDmrVoiceBackendId = QStringLiteral("fobos.dmr.voice.opendmr");
    }
    const bool internalDmrVoiceOutput =
        dmrBackend != DMR_BACKEND_DSD_NEO &&
        dmrBackend != DMR_BACKEND_GOPHERTRUNK;
    RadioSettings settings = pendingSettings;
    settings.cwDecoderToneHz = cwDecoderToneSpin ? cwDecoderToneSpin->value()
                                                 : settings.cwDecoderToneHz;
    settings.cwDecoderWpm = cwDecoderWpmSpin ? cwDecoderWpmSpin->value()
                                             : settings.cwDecoderWpm;
    settings.cwDecoderAdaptiveSpeed =
        !cwDecoderAdaptiveCheckbox || cwDecoderAdaptiveCheckbox->isChecked();
    settings.cwDecoderSelectivity = cwDecoderSelectivitySpin
                                        ? cwDecoderSelectivitySpin->value()
                                        : settings.cwDecoderSelectivity;
    settings.cwDecoderAlphabet =
        cwDecoderAlphabetCombo ? cwDecoderAlphabetCombo->currentData().toInt()
                               : settings.cwDecoderAlphabet;
    settings.dmrBasebandSampleRate =
        dmrBasebandRateCombo
            ? normalizedDmrBasebandSampleRate(dmrBasebandRateCombo->currentData().toInt())
            : normalizedDmrBasebandSampleRate(settings.dmrBasebandSampleRate);
    settings.dmrAmbeLayout =
        dmrAmbeLayoutCombo
            ? normalizedDmrAmbeLayout(dmrAmbeLayoutCombo->currentData().toInt())
            : normalizedDmrAmbeLayout(settings.dmrAmbeLayout);
    settings.dmrManualTimingEnabled =
        dmrManualTimingCheckbox && dmrManualTimingCheckbox->isChecked();
    settings.dmrManualTimingOffset =
        dmrTimingOffsetSpin ? dmrTimingOffsetSpin->value() : settings.dmrManualTimingOffset;
    settings.dmrSlicerRatio =
        dmrSlicerRatioSpin ? dmrSlicerRatioSpin->value() : settings.dmrSlicerRatio;
    settings.dmrAdaptiveSlicer =
        !dmrAdaptiveSlicerCheckbox || dmrAdaptiveSlicerCheckbox->isChecked();
    settings.dmrPrivacyMode = pendingSettings.dmrPrivacyMode;
    settings.dmrPrivacyKeyId = pendingSettings.dmrPrivacyKeyId;
    settings.dmrPrivacyKeyHex = pendingSettings.dmrPrivacyKeyHex;
    settings.dmrPrivacyVariant = pendingSettings.dmrPrivacyVariant;
    settings.dmrPrivacyLayout = pendingSettings.dmrPrivacyLayout;
    settings.dmrPrivacyFrameOffset = pendingSettings.dmrPrivacyFrameOffset;
    const int decoderSampleRate =
        settings.modulationType == MOD_DMR ? settings.dmrBasebandSampleRate : 48000;
    QString privacyWarning;
    if (settings.modulationType == MOD_DMR &&
        settings.dmrPrivacyMode == DMR_PRIVACY_HYTERA_BASIC &&
        dmrBackend == DMR_BACKEND_GOPHERTRUNK) {
        privacyWarning = uiText(QStringLiteral("dmr_hytera_backend_required"),
            QStringLiteral("GopherTrunk does not support Hytera Basic. Select DSD-neo or native mbelib/OpenDMR. Full is not supported."));
    } else if (settings.modulationType == MOD_DMR &&
        settings.dmrPrivacyMode == DMR_PRIVACY_AES256 && dmrBackend == DMR_BACKEND_GOPHERTRUNK) {
        privacyWarning = uiText(QStringLiteral("dmr_gopher_aes_unsupported"),
            QStringLiteral("GopherTrunk does not decrypt DMR AES-256. Select DSD-neo or a native decoder."));
    } else if (settings.modulationType == MOD_DMR && internalDmrVoiceOutput &&
        (settings.dmrPrivacyMode == DMR_PRIVACY_ARC4 || settings.dmrPrivacyMode == DMR_PRIVACY_AES256)) {
        privacyWarning = uiText(QStringLiteral("dmr_native_privacy_beta"),
            QStringLiteral("Native Motorola ARC4 is RF-tested; AES-256 is beta. Uses the saved key and PI/late-entry MI."));
    }
    QString capability;
    if (internalDmrVoiceOutput) {
        capability = uiText(QStringLiteral("dmr_capability_native"),
            QStringLiteral("Own decryption: ARC4, AES-256 (beta). mbelib/OpenDMR converts voice to audio."));
    } else if (dmrBackend == DMR_BACKEND_DSD_NEO) {
        capability = uiText(QStringLiteral("dmr_capability_dsd"),
            QStringLiteral("DSD-neo decryption: ARC4 and AES-256."));
    } else {
        capability = uiText(QStringLiteral("dmr_capability_gopher"),
            QStringLiteral("GopherTrunk decryption: ARC4 only; DMR AES-256 is not supported."));
    }
    if (dmrPrivacyCapabilityLabel && dmrPrivacyCapabilityLabel->text() != capability)
        dmrPrivacyCapabilityLabel->setText(capability);
    if (privacyWarning != lastDmrPrivacyCapabilityWarning) {
        lastDmrPrivacyCapabilityWarning = privacyWarning;
        if (!privacyWarning.isEmpty()) {
            qWarning().noquote() << "[DMR voice backend]" << privacyWarning;
        }
        if (dsdNeoStatusLabel && (internalDmrVoiceOutput || !privacyWarning.isEmpty())) {
            dsdNeoStatusLabel->setText(privacyWarning.isEmpty()
                ? uiText(QStringLiteral("dsd_neo_idle"), QStringLiteral("Idle")) : privacyWarning);
        }
    }
    if (dmrPrivacyKeyIdCombo) {
        dmrPrivacyKeyIdCombo->setToolTip(!privacyWarning.isEmpty() ? privacyWarning :
            settings.dmrPrivacyMode == DMR_PRIVACY_HYTERA_BASIC
                ? uiText(QStringLiteral("dmr_hytera_beta_hint"),
                         QStringLiteral("Hytera Basic beta: native mbelib/OpenDMR or DSD-neo; 10/32/64 HEX characters. Full is not supported."))
                : uiText(QStringLiteral("dmr_privacy_key_id_tooltip"),
                         QStringLiteral("Select a saved key from the active privacy table.")));
    }
    QMetaObject::invokeMethod(digitalDecoder,
                              [decoder = digitalDecoder,
                               enabled,
                               settings,
                               decoderSampleRate,
                               preferredDmrVoiceBackendId,
                               dmrBackend,
                               internalDmrVoiceOutput]() {
                                  decoder->setEnabled(enabled);
                                  decoder->setDmrVoiceBackendId(preferredDmrVoiceBackendId);
                                  decoder->setDmrVoiceOutputEnabled(internalDmrVoiceOutput);
                                  decoder->setDmrVoicePayloadEnabled(dmrBackend != DMR_BACKEND_DSD_NEO);
                                  decoder->configure(settings, decoderSampleRate);
                              },
                              Qt::QueuedConnection);
}
