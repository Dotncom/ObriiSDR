#include "clinetworkbridge.h"
#include "displayspectrumcodec.h"
#include "iq8quantizer.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <cstring>

CliNetworkBridge::CliNetworkBridge(QObject *parent) : network(parent) {
    centerChangeTimer.setSingleShot(true);
    QObject::connect(&centerChangeTimer, &QTimer::timeout, &network,
                     [this] { flushCenterChange(); });
    QObject::connect(&network, &NetworkController::peerDisconnected, &network, [this] {
        if (peerCount() == 0) resetPendingAudio();
    });
    QObject::connect(&network, &NetworkController::channelReady, &network, [this](const QString &) {
        if (network.mode() == NetworkMode::Server) broadcastState();
        else if (network.mode() == NetworkMode::Client)
            sendClientControl(QStringLiteral("requestServerState"));
    });
    QObject::connect(&network, &NetworkController::controlCommandReceived, &network,
                     [this](const QJsonObject &command) {
        if (network.mode() == NetworkMode::Server) {
            handleServerCommand(command);
        } else if (network.mode() == NetworkMode::Client) {
            const QString type = command.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("control") &&
                (command.value(QStringLiteral("action")).toString() == QStringLiteral("serverState") ||
                 command.value(QStringLiteral("action")).toString() == QStringLiteral("settingsAck"))) {
                receivedState = command;
                mode = command.value(QStringLiteral("processingMode")).toInt(mode);
                if (onClientState) onClientState(command);
            } else if (type == QStringLiteral("spectrum") && onClientSpectrum) {
                onClientSpectrum(command);
            } else if (type == QStringLiteral("audio") && onClientAudio) {
                onClientAudio(QByteArray::fromBase64(command.value(QStringLiteral("pcm")).toString().toLatin1()));
            }
        }
    });
    QObject::connect(&network, &NetworkController::binaryCommandReceived, &network,
                     [this](const QJsonObject &command, const QByteArray &payload) {
        if (network.mode() != NetworkMode::Client) return;
        const QString type = command.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("audiobin") && onClientAudio) {
            onClientAudio(payload);
        } else if (type == QStringLiteral("iqbin") && onClientIq) {
            onClientIq(command, payload);
        } else if (type == QStringLiteral("uhdRxSpectrum") && onClientUhdRxSpectrum) {
            onClientUhdRxSpectrum(command, payload);
        } else if (type == QStringLiteral("spectrumbin") && onClientSpectrum) {
            const int count = command.value(QStringLiteral("fftLength")).toInt();
            if (count < 2 || count > 65536) return;
            std::vector<float> magnitudes;
            const QString format = command.value(QStringLiteral("binFormat")).toString();
            if (format == QStringLiteral("s16le-db10")) {
                std::vector<float> reference;
                if (!DisplaySpectrumCodec::decodeDb10(payload, count,
                    command.value(QStringLiteral("hasReferenceMagnitudes")).toBool(),
                    &magnitudes, &reference)) return;
            } else if (format == QStringLiteral("f32le") && payload.size() >= count * 4) {
                magnitudes.resize(static_cast<std::size_t>(count));
                std::memcpy(magnitudes.data(), payload.constData(), static_cast<std::size_t>(count) * 4);
            } else return;
            const double minimum = command.value(QStringLiteral("minFrequency")).toDouble();
            const double maximum = command.value(QStringLiteral("maxFrequency")).toDouble();
            if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum) return;
            QJsonArray frequencies, levels;
            for (int i = 0; i < count; ++i) {
                frequencies.append(minimum + (maximum - minimum) * i / (count - 1));
                levels.append(magnitudes[static_cast<std::size_t>(i)]);
            }
            QJsonObject decoded = command;
            decoded.insert(QStringLiteral("frequencies"), frequencies);
            decoded.insert(QStringLiteral("magnitudes"), levels);
            onClientSpectrum(decoded);
        }
    });
    QObject::connect(&network, &NetworkController::statusChanged, &network,
                     [this](const QString &status) { if (onStatus) onStatus(status); });
}

CliNetworkBridge::~CliNetworkBridge() {
    // Peer-disconnect handlers capture the bridge's audio buffers.
    network.blockSignals(true);
    network.stop();
}

bool CliNetworkBridge::startServer(const QString &bindAddress, quint16 port,
                                   ServerCallbacks callbacks) {
    centerChangeTimer.stop();
    pendingCenterChange = {};
    pendingCenterRequestIds.clear();
    resetPendingAudio();
    serverCallbacks = std::move(callbacks);
    network.startServer(bindAddress, port);
    return network.mode() == NetworkMode::Server;
}

void CliNetworkBridge::connectClient(const QString &address, quint16 port) {
    network.enableUhdRxSpectrum(bool(onClientUhdRxSpectrum));
    network.testClientConnection(address, port);
}

QJsonObject CliNetworkBridge::metrics() const {
    QJsonObject result{{QStringLiteral("connectedPeers"), peerCount()},
                       {QStringLiteral("pendingNetworkBytes"), static_cast<double>(network.pendingBytes())},
                       {QStringLiteral("droppedSpectrumFrames"), static_cast<double>(droppedSpectrum)},
                       {QStringLiteral("droppedAudioFrames"), static_cast<double>(droppedAudio)},
                       {QStringLiteral("droppedIqFrames"), static_cast<double>(droppedIq)},
                       {QStringLiteral("processingMode"), mode}};
    if (network.mode() == NetworkMode::Client) {
        result.insert(QStringLiteral("connected"), network.isControlReady());
        result.insert(QStringLiteral("canControl"), network.isControlReady() && network.clientHasControl());
    }
    return result;
}

bool CliNetworkBridge::sendClientControl(const QString &action, const QJsonObject &settings) {
    QJsonObject command{{QStringLiteral("type"), QStringLiteral("control")},
                        {QStringLiteral("action"), action}};
    if (!settings.isEmpty()) command.insert(QStringLiteral("settings"), settings);
    return network.sendControlCommand(command);
}

bool CliNetworkBridge::requestClientProcessingMode(int requested) {
    if (requested < 0 || requested > 2 || !clientHasControl()) return false;
    return network.sendControlCommand({{QStringLiteral("type"), QStringLiteral("control")},
                                       {QStringLiteral("action"), QStringLiteral("setProcessingMode")},
                                       {QStringLiteral("processingMode"), requested}});
}

bool CliNetworkBridge::setServerProcessingMode(int requested) {
    if (network.mode() != NetworkMode::Server || requested < 0 || requested > 2) return false;
    if (mode != requested) {
        mode = requested;
        resetPendingAudio();
        iqChannelizer.reset();
        previousIqRate = 0.0;
        broadcastState();
    }
    return true;
}

void CliNetworkBridge::publishUhdClockStatus(const QJsonObject &status) {
    network.sendControlCommand({{"type", "control"}, {"action", "uhdClockStatus"}, {"status", status}});
}

void CliNetworkBridge::broadcastState() {
    if (network.mode() != NetworkMode::Server || !serverCallbacks.settings) return;
    QJsonObject state{{QStringLiteral("type"), QStringLiteral("control")},
                      {QStringLiteral("action"), QStringLiteral("serverState")},
                      {QStringLiteral("processingMode"), mode},
                      {QStringLiteral("serverDisableLocalVisualAudio"), true},
                      {QStringLiteral("fullResolutionSpectrumFrames"), false},
                      {QStringLiteral("settings"), serverCallbacks.settings()}};
    if (serverCallbacks.isRunning)
        state.insert(QStringLiteral("receiverRunning"), serverCallbacks.isRunning());
    if (serverCallbacks.receivers)
        state.insert(QStringLiteral("receiverDevices"), serverCallbacks.receivers());
    network.sendControlCommand(state);
}

void CliNetworkBridge::sendAck(const QString &peerId, const QJsonObject &request,
                               bool okay, const QString &reason) {
    QJsonObject ack{{QStringLiteral("type"), QStringLiteral("control")},
                    {QStringLiteral("action"), QStringLiteral("settingsAck")},
                    {QStringLiteral("ok"), okay},
                    {QStringLiteral("processingMode"), mode},
                    {QStringLiteral("serverDisableLocalVisualAudio"), true},
                    {QStringLiteral("settings"), serverCallbacks.settings()}};
    if (serverCallbacks.isRunning)
        ack.insert(QStringLiteral("receiverRunning"), serverCallbacks.isRunning());
    if (serverCallbacks.receivers)
        ack.insert(QStringLiteral("receiverDevices"), serverCallbacks.receivers());
    if (request.contains(QStringLiteral("requestId")))
        ack.insert(QStringLiteral("requestId"), request.value(QStringLiteral("requestId")));
    if (!reason.isEmpty()) ack.insert(QStringLiteral("reason"), reason);
    network.sendControlCommandToPeer(peerId, ack);
}

void CliNetworkBridge::handleServerCommand(const QJsonObject &command) {
    if (command.value(QStringLiteral("type")).toString() != QStringLiteral("control")) return;
    const QString action = command.value(QStringLiteral("action")).toString();
    const QString peerId = command.value(QStringLiteral("_networkPeerId")).toString();
    if (action == QStringLiteral("requestServerState") || action == QStringLiteral("refreshDevices")) {
        broadcastState();
        return;
    }
    if (!command.value(QStringLiteral("_networkPeerIsController")).toBool(false)) {
        network.sendControlCommandToPeer(peerId,
            {{QStringLiteral("type"), QStringLiteral("control")},
             {QStringLiteral("action"), QStringLiteral("controlRejected")},
             {QStringLiteral("reason"), QStringLiteral("Observer cannot control the CLI receiver")}});
        return;
    }
    if (action == QStringLiteral("getUhdGnss")) {
        const QJsonObject status = serverCallbacks.uhdGnssStatus
            ? serverCallbacks.uhdGnssStatus() : QJsonObject{{"state", "disabled"}};
        QJsonObject reply{{"type", "control"}, {"action", "uhdGnssStatus"}, {"status", status}};
        const QString requestId = command.value(QStringLiteral("requestId")).toString();
        if (!requestId.isEmpty() && requestId.size() <= 128)
            reply.insert(QStringLiteral("requestId"), requestId);
        network.sendControlCommandToPeer(peerId, reply);
        return;
    }
    if (action == QStringLiteral("setViewport") ||
        action == QStringLiteral("setServerLocalOutput")) return;
    const auto settings = command.value(QStringLiteral("settings")).toObject();
    const QString requestId = command.value(QStringLiteral("requestId")).toString();
    if (serverCallbacks.coalesceCenterChanges && action == QStringLiteral("setParameters") &&
        settings.value(QStringLiteral("centerFrequency")).isDouble() && !requestId.isEmpty()) {
        // Only replace patches for the same fields. A change to another
        // setting must retain its position relative to a pending retune.
        if (!pendingCenterChange.isEmpty() &&
            (pendingCenterChange.value(QStringLiteral("_networkPeerId")).toString() != peerId ||
             pendingCenterChange.value(QStringLiteral("settings")).toObject().keys() != settings.keys()))
            flushCenterChange();
        pendingCenterChange = command;
        pendingCenterRequestIds.append(requestId);
        if (pendingCenterRequestIds.size() >= 32) flushCenterChange();
        else centerChangeTimer.start(120);
        return;
    }
    flushCenterChange();
    if (action == QStringLiteral("setProcessingMode")) {
        const int requested = command.value(QStringLiteral("processingMode")).toInt(-1);
        if (requested < 0 || requested > 2) {
            network.sendControlCommandToPeer(peerId,
                {{QStringLiteral("type"), QStringLiteral("control")},
                 {QStringLiteral("action"), QStringLiteral("controlRejected")},
                 {QStringLiteral("reason"), QStringLiteral("Invalid CLI processing mode")}});
            return;
        }
        setServerProcessingMode(requested);
        return;
    }
    if (action == QStringLiteral("setParameters") || action == QStringLiteral("settings") ||
        action == QStringLiteral("start")) {
        applyServerSettings(command);
        return;
    }
    if (action == QStringLiteral("stop")) {
        QString reason;
        if (serverCallbacks.setRunning) serverCallbacks.setRunning(false, &reason);
        publishState();
    }
}

void CliNetworkBridge::flushCenterChange() {
    if (pendingCenterChange.isEmpty()) return;
    centerChangeTimer.stop();
    QJsonObject command;
    QStringList requestIds;
    command.swap(pendingCenterChange);
    requestIds.swap(pendingCenterRequestIds);
    if (command.value(QStringLiteral("_networkPeerId")).toString() == network.controllerPeerId())
        applyServerSettings(command, requestIds);
}

void CliNetworkBridge::applyServerSettings(const QJsonObject &command, const QStringList &requestIds) {
    const QString peerId = command.value(QStringLiteral("_networkPeerId")).toString();
    const auto settings = command.value(QStringLiteral("settings")).toObject();
    const bool txOnly = settings.size() == 1 && settings.contains(QStringLiteral("uhdTx"));
    const bool wasRunning = serverCallbacks.isRunning && serverCallbacks.isRunning();
    QString reason;
    const bool applied = !serverCallbacks.applySettings || serverCallbacks.applySettings(settings, &reason);
    bool okay = applied;
    if (applied && command.value(QStringLiteral("action")).toString() == QStringLiteral("start") &&
        serverCallbacks.setRunning)
        okay = serverCallbacks.setRunning(true, &reason);
    if (applied && (!txOnly || command.value("action") == QLatin1String("start")))
        resetPendingAudio();
    if (requestIds.isEmpty()) {
        sendAck(peerId, command, okay, reason);
    } else {
        for (const QString &requestId : requestIds)
            sendAck(peerId, {{QStringLiteral("requestId"), requestId}}, okay, reason);
    }
    if (applied || (serverCallbacks.isRunning && serverCallbacks.isRunning() != wasRunning))
        broadcastState();
}

void CliNetworkBridge::sendSpectrum(const std::vector<float> &orderedDbfs,
                                    double centerHz, double sampleRateHz,
                                    double listeningHz, double bandwidthHz,
                                    int modulation, int inputMode, int sourceFftLength) {
    if (network.mode() != NetworkMode::Server || mode == 2 || peerCount() == 0 || orderedDbfs.empty()) return;
    const int sourceCount = static_cast<int>(orderedDbfs.size());
    const bool reduced = sourceFftLength > sourceCount;
    const int originalCount = reduced ? sourceFftLength : sourceCount;
    const int targetCount = (std::min)(sourceCount, 512);
    const double binHz = sampleRateHz / originalCount;
    QJsonArray frequencies, magnitudes;
    for (int i = 0; i < targetCount; ++i) {
        const int first = static_cast<int>(static_cast<qint64>(i) * sourceCount / targetCount);
        const int last = static_cast<int>(static_cast<qint64>(i + 1) * sourceCount / targetCount);
        const float peak = *std::max_element(orderedDbfs.begin() + first,
                                             orderedDbfs.begin() + last);
        const qint64 frequencyFirst = static_cast<qint64>(i) * originalCount / targetCount;
        const qint64 frequencyLast = static_cast<qint64>(i + 1) * originalCount / targetCount;
        frequencies.append(centerHz - (originalCount / 2) * binHz +
                           (frequencyFirst + frequencyLast - 1) * 0.5 * binHz);
        magnitudes.append(peak);
    }
    QJsonArray nativeMagnitudes;
    // Match GUI/Android's established contract: ascending frequencies but
    // FFT-native levels, shifted by the display exactly once.
    for (int i = 0; i < targetCount; ++i)
        nativeMagnitudes.append(magnitudes[(i + targetCount / 2) % targetCount]);
    QJsonObject frame{{QStringLiteral("type"), QStringLiteral("spectrum")},
                      {QStringLiteral("sequence"), QString::number(++spectrumSequence)},
                      {QStringLiteral("centerFrequency"), centerHz},
                      {QStringLiteral("listeningFrequency"), listeningHz},
                      {QStringLiteral("sampleRate"), sampleRateHz},
                      {QStringLiteral("bandwidth"), bandwidthHz},
                      {QStringLiteral("modulationType"), modulation},
                      {QStringLiteral("inputMode"), inputMode},
                      {QStringLiteral("fftLength"), targetCount},
                      {QStringLiteral("sourceFftLength"), originalCount},
                      {QStringLiteral("magnitudeOrder"), QStringLiteral("fft-native")},
                      {QStringLiteral("minFrequency"), centerHz - sampleRateHz * 0.5},
                      {QStringLiteral("maxFrequency"), centerHz + sampleRateHz * 0.5},
                      {QStringLiteral("fresh"), true},
                      {QStringLiteral("frequencies"), frequencies},
                      {QStringLiteral("magnitudes"), nativeMagnitudes}};
    for (const QString &peerId : network.readyPeerIds()) {
        if (network.pendingBytesForPeer(peerId) > 256 * 1024) {
            ++droppedSpectrum;
            continue;
        }
        network.sendControlCommandToPeer(peerId, frame);
    }
}

bool CliNetworkBridge::hasUhdRxSpectrumPeers() const {
    if (network.mode() != NetworkMode::Server) return false;
    for (const QString &peerId : network.readyPeerIds()) {
        if (network.peerSupportsUhdRxSpectrum(peerId) &&
            network.peerStreamProfile(peerId) != QStringLiteral("remote-vfo")) return true;
    }
    return false;
}

void CliNetworkBridge::sendUhdRxSpectrum(int channel, const std::vector<float> &orderedDbfs,
                                        double centerHz, double sampleRateHz, int sourceFftLength) {
    if (network.mode() != NetworkMode::Server || channel < 0 || channel > 1 ||
        orderedDbfs.size() < 2 || !std::isfinite(centerHz) || centerHz <= 0.0 ||
        !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0) return;
    const int sourceCount = static_cast<int>(orderedDbfs.size());
    const int count = (std::min)(sourceCount, 512);
    std::vector<float> levels;
    levels.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const auto first = static_cast<qint64>(i) * sourceCount / count;
        const auto last = static_cast<qint64>(i + 1) * sourceCount / count;
        levels.push_back(*std::max_element(orderedDbfs.begin() + first, orderedDbfs.begin() + last));
    }
    const QJsonObject frame{
        {QStringLiteral("type"), QStringLiteral("uhdRxSpectrum")},
        {QStringLiteral("channel"), channel},
        {QStringLiteral("sequence"), QString::number(++uhdRxSpectrumSequence)},
        {QStringLiteral("centerFrequency"), centerHz},
        {QStringLiteral("sampleRate"), sampleRateHz},
        {QStringLiteral("fftLength"), count},
        {QStringLiteral("sourceFftLength"), (std::max)(sourceFftLength, sourceCount)},
        {QStringLiteral("binFormat"), QStringLiteral("s16le-db10")}};
    const QByteArray payload = DisplaySpectrumCodec::encodeDb10(levels);
    for (const QString &peerId : network.readyPeerIds()) {
        if (!network.peerSupportsUhdRxSpectrum(peerId) ||
            network.peerStreamProfile(peerId) == QStringLiteral("remote-vfo")) continue;
        if (network.pendingBytesForPeer(peerId) > 256 * 1024) {
            ++droppedSpectrum;
            continue;
        }
        network.sendBinaryCommandToPeer(peerId, frame, payload);
    }
}

void CliNetworkBridge::sendAudio(const QByteArray &pcm) {
    if (network.mode() != NetworkMode::Server || mode != 0 || !network.isControlReady() ||
        pcm.isEmpty() || pcm.size() % sizeof(qint16) != 0) return;
    // 20 ms of mono PCM16 at 48 kHz. The decoder can produce only a few samples
    // per IQ read at high sample rates, so send complete playback blocks.
    constexpr int packetBytes = 48000 * 20 / 1000 * sizeof(qint16);
    pendingAudio.append(pcm);
    if (pendingAudio.size() < packetBytes) return;
    const QStringList peers = network.readyPeerIds();
    if (peers.isEmpty()) {
        pendingAudio.clear();
        return;
    }
    int offset = 0;
    while (pendingAudio.size() - offset >= packetBytes) {
        sendAudioPacket(pendingAudio.mid(offset, packetBytes), peers);
        offset += packetBytes;
    }
    pendingAudio.remove(0, offset);
}

void CliNetworkBridge::sendAudioPacket(const QByteArray &pcm, const QStringList &peers) {
    QJsonObject frame{{QStringLiteral("type"), QStringLiteral("audiobin")},
                      {QStringLiteral("sampleRate"), 48000},
                      {QStringLiteral("channels"), 1},
                      {QStringLiteral("sampleFormat"), QStringLiteral("pcm_s16le")},
                      {QStringLiteral("sequence"), QString::number(++audioSequence)},
                      {QStringLiteral("sourceTimeMs"), QString::number(QDateTime::currentMSecsSinceEpoch())}};
    for (const QString &peerId : peers) {
        if (network.pendingBytesForPeer(peerId) > 256 * 1024) {
            ++droppedAudio;
            continue;
        }
        if (network.peerSupportsBinaryAudio(peerId))
            network.sendBinaryCommandToPeer(peerId, frame, pcm);
        else {
            QJsonObject legacy = frame;
            legacy.insert(QStringLiteral("type"), QStringLiteral("audio"));
            legacy.insert(QStringLiteral("pcm"), QString::fromLatin1(pcm.toBase64()));
            network.sendControlCommandToPeer(peerId, legacy);
        }
    }
}

void CliNetworkBridge::sendIq(const float *samples, int sampleCount, const RadioSettings &settings) {
    if (network.mode() != NetworkMode::Server || mode == 0 || peerCount() == 0 ||
        !samples || sampleCount <= 0) return;
    QByteArray payload;
    double frameRate = settings.sampleRate;
    double frameCenter = settings.centerFrequency;
    const bool channelized = mode == 1;
    if (channelized) {
        if (std::abs(previousIqCenter - settings.centerFrequency) > 0.5 ||
            std::abs(previousIqListen - settings.listeningFrequency) > 0.5 ||
            std::abs(previousIqRate - settings.sampleRate) > 0.5) {
            iqChannelizer.reset();
        }
        previousIqCenter = settings.centerFrequency;
        previousIqListen = settings.listeningFrequency;
        previousIqRate = settings.sampleRate;
        const auto result = iqChannelizer.processFloatIq(samples, static_cast<std::size_t>(sampleCount) * 2,
                                                          settings, channelIq);
        if (!result.valid || result.outputSamples <= 0) return;
        frameRate = result.outputRate;
        frameCenter = settings.listeningFrequency;
        sampleCount = result.outputSamples;
        payload.resize(sampleCount * 4);
        auto *bytes = reinterpret_cast<unsigned char *>(payload.data());
        for (int i = 0; i < sampleCount * 2; ++i) {
            const float value = std::isfinite(channelIq[static_cast<std::size_t>(i)])
                                    ? channelIq[static_cast<std::size_t>(i)] : 0.0f;
            const auto quantized = static_cast<qint16>(std::lrint(
                (std::clamp)(value, -1.0f, 1.0f) * 32767.0f));
            bytes[i * 2] = static_cast<unsigned char>(quantized & 0xff);
            bytes[i * 2 + 1] = static_cast<unsigned char>((quantized >> 8) & 0xff);
        }
    } else {
        payload.resize(sampleCount * 2);
        ObriiIq8Quantizer::quantize(samples, payload.data(), static_cast<std::size_t>(sampleCount) * 2);
    }
    const QJsonObject frame{{QStringLiteral("type"), QStringLiteral("iqbin")},
                            {QStringLiteral("sequence"), QString::number(++iqSequence)},
                            {QStringLiteral("sampleRate"), frameRate},
                            {QStringLiteral("sourceSampleRate"), settings.sampleRate},
                            {QStringLiteral("sampleCount"), sampleCount},
                            {QStringLiteral("sampleFormat"), channelized
                                ? QStringLiteral("channel_iq_s16le") : QStringLiteral("iq_s8_interleaved")},
                            {QStringLiteral("channelized"), channelized},
                            {QStringLiteral("centerFrequency"), frameCenter},
                            {QStringLiteral("actualFrequency"), frameCenter},
                            {QStringLiteral("listeningFrequency"), settings.listeningFrequency},
                            {QStringLiteral("bandwidth"), settings.bandwidth},
                            {QStringLiteral("modulationType"), settings.modulationType},
                            {QStringLiteral("inputMode"), 0},
                            {QStringLiteral("payloadEncoding"), QStringLiteral("raw")}};
    const qint64 limit = channelized ? 2 * 1024 * 1024 : 8 * 1024 * 1024;
    for (const QString &peerId : network.readyPeerIds()) {
        if (network.pendingBytesForPeer(peerId) > limit) {
            ++droppedIq;
            continue;
        }
        network.sendBinaryCommandToPeer(peerId, frame, payload);
    }
}
