#include "main.h"
#include "appconstants.h"
#include "appruntimeutils.h"
#include "diagnosticlogging.h"
#include "iqbuffer.h"
#include "modulationutils.h"
#include "samplefileutils.h"
#include "scanvisualutils.h"
#include "spectrumfftworker.h"
#include "tuningutils.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

extern bool colorf;
extern bool secondGraph;
bool YourClassName::isNetworkClientMode() const {
    return networkMode == NetworkMode::Client;
}

bool YourClassName::isClientIqProcessingMode() const {
    return isChannelIqProcessingMode() || isFullIqProcessingMode();
}

bool YourClassName::isChannelIqProcessingMode() const {
    return networkProcessingMode == NetworkProcessingMode::ChannelIqClientSide;
}

bool YourClassName::isFullIqProcessingMode() const {
    return networkProcessingMode == NetworkProcessingMode::FullIqClientSide;
}

bool YourClassName::isClientIqProcessingMode(NetworkProcessingMode mode) const {
    return mode == NetworkProcessingMode::ChannelIqClientSide ||
           mode == NetworkProcessingMode::FullIqClientSide;
}

void YourClassName::appendNetworkState(QJsonObject &command) const {
    command["processingMode"] = static_cast<int>(networkProcessingMode);
    command["serverDisableLocalVisualAudio"] = serverDisableLocalVisualAudio;
    command["fullResolutionSpectrumFrames"] = networkFullResolutionSpectrumFrames;
}

void YourClassName::applyNetworkStateFromCommand(const QJsonObject &command) {
    if (command.contains("processingMode")) {
        networkProcessingMode = static_cast<NetworkProcessingMode>(command.value("processingMode").toInt(
            static_cast<int>(networkProcessingMode)));
    }
    if (command.contains("serverDisableLocalVisualAudio")) {
        serverDisableLocalVisualAudio = command.value("serverDisableLocalVisualAudio").toBool(serverDisableLocalVisualAudio);
    }
    if (command.contains("fullResolutionSpectrumFrames")) {
        networkFullResolutionSpectrumFrames =
            command.value("fullResolutionSpectrumFrames").toBool(networkFullResolutionSpectrumFrames);
    }
    onNetworkStatusChanged(networkController ? networkController->statusText() : QString());
}

QJsonObject YourClassName::networkSettingsPatch(const QJsonObject &settings) const {
    if (!networkClientConfirmedSettingsValid) {
        return settings;
    }

    QJsonObject patch;
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
        const QJsonValue confirmedValue = networkClientConfirmedSettingsJson.value(it.key());
        if (!networkClientConfirmedSettingsJson.contains(it.key()) ||
            !jsonValuesEquivalent(it.value(), confirmedValue)) {
            patch.insert(it.key(), it.value());
        }
    }
    return patch;
}

bool YourClassName::isRemoteReceiverDeviceValue(int deviceIndex) const {
    return remoteReceiverDeviceListValid && remoteReceiverDeviceValues.contains(deviceIndex);
}

QJsonObject YourClassName::networkClientSettingsForCommand(const QJsonObject &settings,
                                                           const QString &action) const {
    QJsonObject sanitized = settings;
    if (!isNetworkClientMode()) {
        return sanitized;
    }

    // Calibration belongs to the physical receiver host.  A client may learn
    // the server table from authoritative state for local IQ rendering, but
    // must not overwrite it when sending ordinary tuning commands.
    sanitized.remove(QStringLiteral("calibrationTableEnabled"));
    sanitized.remove(QStringLiteral("calibrationTable"));

    const bool maySendReceiverChoice =
        remoteReceiverDeviceListValid &&
        isRemoteReceiverDeviceValue(pendingSettings.deviceIndex);
    if (!maySendReceiverChoice) {
        sanitized.remove(QStringLiteral("deviceIndex"));
        qDebug() << "[Network] client settings omit receiver device until server device list is known"
                 << "action" << action
                 << "remoteListValid" << remoteReceiverDeviceListValid
                 << "deviceIndex" << pendingSettings.deviceIndex;
    }
    return sanitized;
}

void YourClassName::applyAuthoritativeNetworkState(const QJsonObject &command) {
    const RadioSettings previousSettings = pendingSettings;
    const int previousFftLength = pendingSettings.fftLength;
    const int previousScanVisualMode = scanVisualMode;
    const QJsonObject settings = command.value(QStringLiteral("settings")).toObject();

    applyNetworkStateFromCommand(command);
    applyReceiverDeviceListFromJson(command.value(QStringLiteral("receiverDevices")).toArray());
    applySettingsFromJson(settings, false);
    publishSettingsToGlobals();
    if (previousFftLength != pendingSettings.fftLength) {
        applyFftLengthChange(pendingSettings.fftLength, false);
    }
    if (normalizedScanVisualMode(previousScanVisualMode) != normalizedScanVisualMode(scanVisualMode)) {
        scanVisualAssembler.reset();
    }
    updateUiFromPendingSettings();
    updateUiForRunState();

    const bool demodSettingsChanged =
        previousSettings.modulationType != pendingSettings.modulationType ||
        previousSettings.sstvDemodulationMode != pendingSettings.sstvDemodulationMode ||
        std::abs(previousSettings.bandwidth - pendingSettings.bandwidth) > 0.5 ||
        std::abs(previousSettings.audioLowPassHz - pendingSettings.audioLowPassHz) > 0.5 ||
        std::abs(previousSettings.audioHighPassHz - pendingSettings.audioHighPassHz) > 0.5 ||
        previousSettings.audioFilterChainJson != pendingSettings.audioFilterChainJson ||
        previousSettings.simplifiedAudioChannelizer != pendingSettings.simplifiedAudioChannelizer ||
        previousSettings.audioEnabled != pendingSettings.audioEnabled;
    if (demodSettingsChanged) {
        updateDigitalDecoderMode();
        updateVideoProcessorMode();
        if (isClientIqProcessingMode() && audioProcessor) {
            audioProcessor->configure(audioProcessorSettings());
        }
    }

    if (!settings.isEmpty()) {
        networkClientConfirmedSettingsJson = settings;
        networkClientConfirmedSettingsValid = true;
    }
    networkClientAwaitingSettingsAck = false;
    if (networkSettingsAckTimer) {
        networkSettingsAckTimer->stop();
    }
}

void YourClassName::sendSettingsAckToPeer(const QString &peerId,
                                          bool ok,
                                          const QString &requestId,
                                          const QString &reason) {
    if (networkMode != NetworkMode::Server || !networkController || peerId.isEmpty()) {
        return;
    }

    QJsonObject ack;
    ack["type"] = "control";
    ack["action"] = "settingsAck";
    ack["ok"] = ok;
    if (!requestId.isEmpty()) {
        ack["requestId"] = requestId;
    }
    if (!reason.isEmpty()) {
        ack["reason"] = reason;
    }
    appendNetworkState(ack);
    ack["settings"] = settingsToJson();
    ack["receiverDevices"] = receiverDeviceListToJson();
    networkController->sendControlCommandToPeer(peerId, ack);
}

void YourClassName::startNetworkSettingsAckWait() {
    if (!isNetworkClientMode()) {
        return;
    }

    networkClientAwaitingSettingsAck = true;
    if (networkSettingsAckTimer) {
        networkSettingsAckTimer->start();
    }
}

void YourClassName::handleNetworkSettingsAckTimeout() {
    if (!isNetworkClientMode() || !networkClientAwaitingSettingsAck) {
        return;
    }

    networkClientAwaitingSettingsAck = false;
    qDebug() << "[Network] settings ack timeout; restoring last confirmed server state";
    if (networkClientConfirmedSettingsValid) {
        const int previousFftLength = pendingSettings.fftLength;
        applySettingsFromJson(networkClientConfirmedSettingsJson, false);
        publishSettingsToGlobals();
        if (previousFftLength != pendingSettings.fftLength) {
            applyFftLengthChange(pendingSettings.fftLength, false);
        }
        updateUiFromPendingSettings();
        updateUiForRunState();
    }
    sendRemoteControlCommand(QStringLiteral("requestServerState"));
}

void YourClassName::sendServerStateToClients() {
    if (networkMode != NetworkMode::Server || !networkController || !networkController->isControlReady()) {
        return;
    }

    QJsonObject state;
    state["type"] = "control";
    state["action"] = "serverState";
    appendNetworkState(state);
    state["settings"] = settingsToJson();
    state["receiverDevices"] = receiverDeviceListToJson();
    networkController->sendControlCommand(state);
}

void YourClassName::resetNetworkIqReceptionState(bool clearGraph, bool clearWaterfall, bool restartAudioPrebuffer) {
    IqBuffer::clear();
    networkIqStreamMetadataValid = false;
    networkIqStreamWasChannelized = false;
    networkIqStreamSampleRate = 0.0;
    networkIqStreamCenterFrequency = 0.0;
    networkIqStreamListeningFrequency = 0.0;
    networkIqStreamInputMode = 0;
    networkSpectrumFrameMetadataValid = false;
    networkSpectrumFrameMinFrequency = 0.0;
    networkSpectrumFrameMaxFrequency = 0.0;
    networkSpectrumFrameFftLength = 0;

    if (audioProcessor) {
        audioProcessor->stopDemodulation();
    }
    pendingNetworkAudioStartAfterIqPrebuffer = restartAudioPrebuffer && pendingSettings.audioEnabled;

    if (clearGraph && graphWidget) {
        graphWidget->clearData();
    }
    if (clearWaterfall && waterfallWidget) {
        waterfallWidget->clearData();
    }

    fftResult = std::make_unique<FFTResult>();
    if (spectrumFftWorker) {
        spectrumFftWorker->resetHfNoiseCancelState();
    }
    spectrumDebugFramesRemaining = fobosVerboseLoggingEnabled() ? 8 : 0;
    updateSpectrumTimerInterval();
}

void YourClassName::startNetworkClientProcessing() {
    if (!isNetworkClientMode() || !isClientIqProcessingMode()) {
        return;
    }

    if (networkClientIqProcessingActive &&
        activeNetworkClientProcessingMode == networkProcessingMode &&
        runState == RadioRunState::Running) {
        if (audioProcessor) {
            audioProcessor->setLocalPlaybackEnabled(true);
        }
        if (updateTimer && isFullIqProcessingMode() && !updateTimer->isActive()) {
            updateTimer->start();
        }
        return;
    }

    if (remoteAudioPlayer) {
        remoteAudioPlayer->stop();
    }
    if (updateTimer) {
        updateTimer->stop();
    }
    if (audioProcessor) {
        audioProcessor->setLocalPlaybackEnabled(true);
    }

    const bool switchingBetweenClientIqModes =
        networkClientIqProcessingActive &&
        activeNetworkClientProcessingMode != networkProcessingMode;
    resetNetworkIqReceptionState(false,
                                 switchingBetweenClientIqModes,
                                 pendingSettings.audioEnabled && !isFullIqProcessingMode());
    IqBuffer::setSampleRateEstimate(pendingSettings.sampleRate);
    publishSettingsToGlobals();

    if (updateTimer && isFullIqProcessingMode()) {
        updateTimer->start();
    }
    networkClientIqProcessingActive = true;
    activeNetworkClientProcessingMode = networkProcessingMode;

    qDebug() << "[NetworkIQ] client-side IQ processing started"
             << "sampleRate" << pendingSettings.sampleRate
             << "audio" << pendingSettings.audioEnabled
             << "audioPrebuffer" << pendingNetworkAudioStartAfterIqPrebuffer;
}

void YourClassName::stopNetworkClientProcessing() {
    if (spectrumFftWorker) spectrumFftWorker->resetHfNoiseCancelState();
    if (updateTimer) {
        updateTimer->stop();
    }
    if (audioProcessor) {
        audioProcessor->stopDemodulation();
    }
    if (remoteAudioPlayer) {
        remoteAudioPlayer->stop();
    }
    resetNetworkIqReceptionState(false, false, false);
    networkClientIqProcessingActive = false;
    activeNetworkClientProcessingMode = NetworkProcessingMode::ServerSide;
    qDebug() << "[NetworkIQ] client-side IQ processing stopped";
}

bool YourClassName::sendRemoteControlCommand(const QString &action, const QJsonObject &extra) {
    if (!networkController || networkMode != NetworkMode::Client) {
        return false;
    }

    if (action != QStringLiteral("setParameters") &&
        action != QStringLiteral("settings")) {
        cancelPendingRemoteSettingsCommand();
    }

    const bool controlActionAllowed =
        action == QStringLiteral("requestPriority") ||
        action == QStringLiteral("priorityResponse") ||
        action == QStringLiteral("requestServerState");
    if (!networkController->clientHasControl() && !controlActionAllowed) {
        qDebug() << "[Network] remote command blocked because this client is observer" << action;
        return false;
    }

    const bool carriesSettings =
        action == QStringLiteral("setParameters") ||
        action == QStringLiteral("settings") ||
        action == QStringLiteral("start");
    QJsonObject currentSettings;
    if (carriesSettings) {
        refreshSettingsFromUi();
        currentSettings = networkClientSettingsForCommand(settingsToJson(), action);
    }

    QJsonObject command;
    command["type"] = "control";
    command["action"] = action;
    appendNetworkState(command);
    if (carriesSettings) {
        command["requestId"] = QString::number(++networkClientSettingsRequestCounter);
        command["settings"] =
            action == QStringLiteral("setParameters")
                ? networkSettingsPatch(currentSettings)
                : currentSettings;
    }
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
        command[it.key()] = it.value();
    }

    const bool sent = networkController->sendControlCommand(command);
    if (!sent) {
        qDebug() << "[Network] remote command could not be sent" << action;
    } else {
        if (carriesSettings) {
            startNetworkSettingsAckWait();
        }
        qDebug() << "[Network] remote command sent" << action;
    }
    return sent;
}

void YourClassName::scheduleRemoteSettingsCommand(int delayMs) {
    if (!isNetworkClientMode() ||
        !networkController ||
        !networkController->clientHasControl()) {
        return;
    }

    if (!networkSettingsDebounceTimer) {
        sendRemoteControlCommand("setParameters");
        return;
    }

    const int clampedDelayMs = (std::clamp)(delayMs, 0, 1000);
    if (clampedDelayMs == 0) {
        networkSettingsDebounceTimer->stop();
        sendRemoteControlCommand("setParameters");
        return;
    }

    if (!networkSettingsDebounceTimer->isActive()) {
        networkSettingsDebounceTimer->start(clampedDelayMs);
    }
}

void YourClassName::cancelPendingRemoteSettingsCommand() {
    if (networkSettingsDebounceTimer) {
        networkSettingsDebounceTimer->stop();
    }
}

void YourClassName::applyLiveRemoteSettings(const RadioSettings &previousSettings) {
    if (!hasActiveFobosDevice() || isIdle()) {
        return;
    }

    applyCenterFrequencyToHardwareIfNeeded(previousSettings, "remote settings");

    if (hardwareSettingsApplied && previousSettings.lnaGain != pendingSettings.lnaGain) {
        const int result = setActiveLnaGainSafely(static_cast<unsigned int>(pendingSettings.lnaGain));
        qDebug() << "[Network] remote LNA apply result" << result;
        if (result == FOBOS_ERR_OK) {
            appliedHardwareSettings.lnaGain = pendingSettings.lnaGain;
        }
    }
    if (hardwareSettingsApplied && previousSettings.vgaGain != pendingSettings.vgaGain) {
        const int result = setActiveVgaGainSafely(static_cast<unsigned int>(pendingSettings.vgaGain));
        qDebug() << "[Network] remote VGA apply result" << result;
        if (result == FOBOS_ERR_OK) {
            appliedHardwareSettings.vgaGain = pendingSettings.vgaGain;
        }
    }
    if (hardwareSettingsApplied && previousSettings.gpoValue != pendingSettings.gpoValue) {
        const int result = setActiveGpoSafely(pendingSettings.gpoValue);
        qDebug() << "[Network] remote GPO apply result" << result;
        if (result == FOBOS_ERR_OK) {
            appliedHardwareSettings.gpoValue = pendingSettings.gpoValue;
        }
    }

    const bool demodSettingsChanged =
        previousSettings.modulationType != pendingSettings.modulationType ||
        previousSettings.sstvDemodulationMode != pendingSettings.sstvDemodulationMode ||
        std::abs(previousSettings.bandwidth - pendingSettings.bandwidth) > 0.5 ||
        std::abs(previousSettings.audioLowPassHz - pendingSettings.audioLowPassHz) > 0.5 ||
        std::abs(previousSettings.audioHighPassHz - pendingSettings.audioHighPassHz) > 0.5 ||
        previousSettings.audioFilterChainJson != pendingSettings.audioFilterChainJson ||
        previousSettings.simplifiedAudioChannelizer != pendingSettings.simplifiedAudioChannelizer;
    if (demodSettingsChanged) {
        qDebug() << "[Network] applying live demod settings"
                 << "modulation" << pendingSettings.modulationType
                 << "bandwidth" << pendingSettings.bandwidth
                 << "audioLPF" << pendingSettings.audioLowPassHz
                 << "audioHPF" << pendingSettings.audioHighPassHz;
        updateDigitalDecoderMode();
        updateVideoProcessorMode();
        if (audioProcessor) {
            audioProcessor->configure(audioProcessorSettings());
        }
    }

    if (std::abs(previousSettings.sampleRate - pendingSettings.sampleRate) > 0.5 ||
        previousSettings.inputMode != pendingSettings.inputMode ||
        previousSettings.clockSource != pendingSettings.clockSource) {
        qDebug() << "[Network] remote settings include restart-only changes; they will be applied on next server start";
    }
}

void YourClassName::onNetworkControlCommandReceived(const QJsonObject &command) {
    if (networkMode == NetworkMode::Client && command.value("type").toString() == "iq") {
        receiveNetworkIqFrame(command);
        return;
    }

    if (networkMode == NetworkMode::Client && !isChannelIqProcessingMode() && command.value("type").toString() == "audio") {
        playNetworkAudioFrame(command);
        return;
    }

    if (networkMode == NetworkMode::Client && !isFullIqProcessingMode() && command.value("type").toString() == "spectrum") {
        displayNetworkSpectrumFrame(command);
        return;
    }

    if (networkMode == NetworkMode::Client && command.value("type").toString() == "control") {
        const QString action = command.value("action").toString();
        if (action == QStringLiteral("serverState") ||
            action == QStringLiteral("settingsAck")) {
            const bool ok = command.value(QStringLiteral("ok")).toBool(true);
            if (!ok) {
                qDebug() << "[Network] settings command rejected by server"
                         << command.value(QStringLiteral("reason")).toString();
            }
            applyAuthoritativeNetworkState(command);
            return;
        }

        if (action == QStringLiteral("role")) {
            const bool canControl = command.value("canControl").toBool(true);
            const QString peerLabel = command.value("peerLabel").toString();
            qDebug() << "[Network] client role update"
                     << "canControl" << canControl
                     << "peer" << peerLabel
                     << "controllerPeerId" << command.value("controllerPeerId").toString();
            updateUiForRunState();
            updateNetworkButtonText();
            return;
        }

        if (action == QStringLiteral("priorityRequest")) {
            if (!networkController || !networkController->clientHasControl()) {
                return;
            }

            const QString requesterId = command.value("requesterId").toString();
            const QString requesterLabel = command.value("requesterLabel").toString(QStringLiteral("unknown client"));
            QMessageBox box(this);
            box.setWindowTitle("Control Request");
            box.setText(QString("Client %1 requests control of the receiver.").arg(requesterLabel));
            box.setInformativeText("If you allow it, this client will become observer.");
            box.setIcon(QMessageBox::Question);
            QCheckBox *blockRequesterCheck = new QCheckBox("Block further requests from this client", &box);
            box.setCheckBox(blockRequesterCheck);
            QPushButton *allowButton = box.addButton("Allow", QMessageBox::AcceptRole);
            box.addButton("Deny", QMessageBox::RejectRole);
            box.exec();

            QJsonObject response;
            response["requesterId"] = requesterId;
            response["accepted"] = box.clickedButton() == allowButton;
            response["blocked"] = blockRequesterCheck && blockRequesterCheck->isChecked();
            sendRemoteControlCommand("priorityResponse", response);
            return;
        }

        if (action == QStringLiteral("priorityDenied") ||
            action == QStringLiteral("controlRejected")) {
            const QString reason = command.value("reason").toString("Request denied");
            qDebug() << "[Network]" << reason;
            QMessageBox::information(this, "Network Control", reason);
            return;
        }

        return;
    }

    if (networkMode != NetworkMode::Server || command.value("type").toString() != "control") {
        return;
    }

    const QString action = command.value("action").toString();
    const QJsonObject settingsJson = command.value("settings").toObject();
    const QString peerId = command.value("_networkPeerId").toString();
    const QString peerLabel = command.value("_networkPeerLabel").toString(QStringLiteral("unknown client"));
    const bool peerIsController = command.value("_networkPeerIsController").toBool(true);

    if (action == QStringLiteral("requestServerState")) {
        sendServerStateToClients();
        return;
    }

    if (action == QStringLiteral("requestPriority")) {
        if (!networkController || peerId.isEmpty()) {
            return;
        }

        if (peerIsController) {
            QJsonObject role;
            role["type"] = "control";
            role["action"] = "role";
            role["canControl"] = true;
            networkController->sendControlCommandToPeer(peerId, role);
            return;
        }

        if (networkController->isPriorityRequestBlocked(peerId)) {
            QJsonObject denied;
            denied["type"] = "control";
            denied["action"] = "priorityDenied";
            denied["reason"] = "Control request is blocked by current controller";
            networkController->sendControlCommandToPeer(peerId, denied);
            qDebug() << "[Network] blocked control request from" << peerLabel;
            return;
        }

        if (networkController->controllerPeerId().isEmpty()) {
            networkController->setControllerPeer(peerId);
            return;
        }

        QJsonObject request;
        request["type"] = "control";
        request["action"] = "priorityRequest";
        request["requesterId"] = peerId;
        request["requesterLabel"] = peerLabel;
        if (!networkController->sendControlCommandToController(request)) {
            qDebug() << "[Network] controller unavailable; granting control to requester" << peerLabel;
            networkController->setControllerPeer(peerId);
        }
        return;
    }

    if (action == QStringLiteral("priorityResponse")) {
        if (!networkController || !peerIsController) {
            return;
        }

        const QString requesterId = command.value("requesterId").toString();
        const bool accepted = command.value("accepted").toBool(false);
        const bool blocked = command.value("blocked").toBool(false);
        if (blocked && !accepted) {
            networkController->blockPriorityRequestsFromPeer(requesterId);
        }

        if (accepted && networkController->setControllerPeer(requesterId)) {
            qDebug() << "[Network] control transferred to" << requesterId;
        } else {
            QJsonObject denied;
            denied["type"] = "control";
            denied["action"] = "priorityDenied";
            denied["reason"] = blocked
                                    ? "Control request denied and further requests were blocked"
                                    : "Control request denied";
            networkController->sendControlCommandToPeer(requesterId, denied);
        }
        return;
    }

    if (!peerIsController) {
        QJsonObject rejected;
        rejected["type"] = "control";
        rejected["action"] = "controlRejected";
        rejected["reason"] = "This client is observer. Request control in Network Settings first.";
        if (networkController && !peerId.isEmpty()) {
            networkController->sendControlCommandToPeer(peerId, rejected);
        }
        qDebug() << "[Network] observer command rejected"
                 << "action" << action
                 << "peer" << peerLabel;
        return;
    }

    if (action == QStringLiteral("refreshDevices")) {
        qDebug() << "[Network] remote refreshDevices returns cached server receiver list";
        sendServerStateToClients();
        return;
    }

    const NetworkProcessingMode previousProcessingMode = networkProcessingMode;
    const bool previousServerDisableLocalVisualAudio = serverDisableLocalVisualAudio;
    applyNetworkStateFromCommand(command);
    const bool serverLocalOutputChanged =
        previousServerDisableLocalVisualAudio != serverDisableLocalVisualAudio;
    qDebug() << "[Network] received remote control command" << action;

    if (action == "setParameters" || action == "settings" || action == "start") {
        const RadioSettings previousSettings = pendingSettings;
        const bool previousAgileScanEnabled = agileScanEnabled;
        const QString previousAgileScanRangesMhz = agileScanRangesMhz;
        const double previousAgileScanStepMhz = agileScanStepMhz;
        const int previousScanVisualMode = scanVisualMode;
        const bool previousStandardScanEnabled = standardScanEnabled;
        const QString previousStandardScanCentersMhz = standardScanCentersMhz;
        const int previousStandardScanDwellMs = standardScanDwellMs;
        const int previousStandardScanSettleMs = standardScanSettleMs;
        const QString previousStandardScanRangeStartMhz = standardScanRangeStartMhz;
        const QString previousStandardScanRangeEndMhz = standardScanRangeEndMhz;
        const bool previousListeningScanEnabled = listeningScanEnabled;
        const QString previousListeningScanTargetsMhz = listeningScanTargetsMhz;
        const int previousListeningScanDwellMs = listeningScanDwellMs;
        const int previousListeningScanSettleMs = listeningScanSettleMs;
        if (settingsJson.contains(QStringLiteral("deviceIndex"))) {
            QVector<int> serverReceiverValues;
            const QJsonArray serverReceivers = receiverDeviceListToJson();
            for (const QJsonValue &value : serverReceivers) {
                serverReceiverValues.append(value.toObject().value(QStringLiteral("deviceIndex")).toInt());
            }
            const int requestedDeviceIndex =
                settingsJson.value(QStringLiteral("deviceIndex")).toInt(pendingSettings.deviceIndex);
            if (!serverReceiverValues.contains(requestedDeviceIndex)) {
                const QString reason =
                    QStringLiteral("Requested receiver is not available on the server");
                qDebug() << "[Network] rejecting unknown remote receiver choice"
                         << "peer" << peerLabel
                         << "requestedDeviceIndex" << requestedDeviceIndex
                         << "serverValues" << serverReceiverValues;
                sendSettingsAckToPeer(peerId,
                                      false,
                                      command.value(QStringLiteral("requestId")).toString(),
                                      reason);
                sendServerStateToClients();
                return;
            }
        }
        applySettingsFromJson(settingsJson);
        publishSettingsToGlobals();
        updateUiFromPendingSettings();
        applyLiveRemoteSettings(previousSettings);
        const bool audioChanged =
            previousSettings.audioEnabled != pendingSettings.audioEnabled;
        const bool fftChanged =
            previousSettings.fftLength != pendingSettings.fftLength;
        const bool streamModeChanged =
            previousProcessingMode != networkProcessingMode;
        const bool fullIqServerAudioPathChanged =
            audioChanged && isFullIqProcessingMode();
        const bool agileScanChanged =
            previousAgileScanEnabled != agileScanEnabled ||
            previousAgileScanRangesMhz != agileScanRangesMhz ||
            std::abs(previousAgileScanStepMhz - agileScanStepMhz) > 0.0000001;
        const bool standardScanChanged =
            previousStandardScanEnabled != standardScanEnabled ||
            previousStandardScanCentersMhz != standardScanCentersMhz ||
            previousStandardScanDwellMs != standardScanDwellMs ||
            previousStandardScanSettleMs != standardScanSettleMs ||
            previousStandardScanRangeStartMhz != standardScanRangeStartMhz ||
            previousStandardScanRangeEndMhz != standardScanRangeEndMhz;
        const bool listeningScanChanged =
            previousListeningScanEnabled != listeningScanEnabled ||
            previousListeningScanTargetsMhz != listeningScanTargetsMhz ||
            previousListeningScanDwellMs != listeningScanDwellMs ||
            previousListeningScanSettleMs != listeningScanSettleMs;
        const bool scanVisualModeChanged =
            normalizedScanVisualMode(previousScanVisualMode) != normalizedScanVisualMode(scanVisualMode);
        if (scanVisualModeChanged) {
            scanVisualAssembler.reset();
        }

        if (fftChanged) {
            applyFftLengthChange(pendingSettings.fftLength, false);
        }

        if (audioChanged && !isIdle()) {
            if (pendingSettings.audioEnabled) {
                if (audioProcessor) {
                    audioProcessor->startDemodulation();
                }
            } else {
                if (audioProcessor) {
                    audioProcessor->stopDemodulation();
                }
                pendingAudioStartAfterStreamReady = false;
            }
        }
        const bool restartRequired =
            std::abs(previousSettings.sampleRate - pendingSettings.sampleRate) > 0.5 ||
            previousSettings.inputMode != pendingSettings.inputMode ||
            previousSettings.clockSource != pendingSettings.clockSource ||
            agileScanChanged ||
            standardScanChanged ||
            streamModeChanged ||
            fullIqServerAudioPathChanged;

        if (restartRequired && !isIdle()) {
            restartStreamForHardwareChange();
        } else if (serverLocalOutputChanged && !isIdle()) {
            applyServerLocalOutputPolicy();
        }
        if (listeningScanChanged && !isIdle() && !restartRequired) {
            applyListeningScanSettings(false);
        }

        if (processor && processor->isRunning()) {
            updateIqFrameProducerSettings();
        }
        sendSettingsAckToPeer(peerId, true, command.value(QStringLiteral("requestId")).toString());
        sendServerStateToClients();
    }

    if (action == "start") {
        if (isIdle()) {
            startFobosProcessing();
        } else {
            qDebug() << "[Network] remote start ignored because server is not idle";
        }
    } else if (action == "stop") {
        stopFobosProcessing();
    }
}

void YourClassName::updateNetworkButtonText() {
    if (!networkButton) {
        return;
    }

    const QString base = uiText(QStringLiteral("network"), QStringLiteral("Network"));
    switch (networkMode) {
    case NetworkMode::Server:
        networkButton->setText(isChannelIqProcessingMode()
                                   ? QStringLiteral("%1: Server ChIQ").arg(base)
                                   : (isFullIqProcessingMode() ? QStringLiteral("%1: Server IQ").arg(base)
                                                               : QStringLiteral("%1: Server").arg(base)));
        break;
    case NetworkMode::Client: {
        const QString roleSuffix =
            networkController && networkController->isControlReady()
                ? (networkController->clientHasControl() ? QStringLiteral(" Ctrl") : QStringLiteral(" Obs"))
                : QString();
        networkButton->setText((isChannelIqProcessingMode()
                                    ? QStringLiteral("%1: Client ChIQ").arg(base)
                                    : (isFullIqProcessingMode() ? QStringLiteral("%1: Client IQ").arg(base)
                                                                : QStringLiteral("%1: Client").arg(base))) + roleSuffix);
        break;
    }
    case NetworkMode::Disabled:
    default:
        networkButton->setText(base);
        break;
    }
}

void YourClassName::onNetworkStatusChanged(const QString &status) {
    qDebug() << "[Network]" << status;
    updateNetworkButtonText();
}
