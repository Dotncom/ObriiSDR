#include "clinetworkbridge.h"
#include "displayspectrumcodec.h"
#include "iq8quantizer.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <cstring>

CliNetworkBridge::CliNetworkBridge(QObject *parent) : network(parent) {
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

bool CliNetworkBridge::startServer(const QString &bindAddress, quint16 port,
                                   ServerCallbacks callbacks) {
    serverCallbacks = std::move(callbacks);
    network.startServer(bindAddress, port);
    return network.mode() == NetworkMode::Server;
}

void CliNetworkBridge::connectClient(const QString &address, quint16 port) {
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
        iqChannelizer.reset();
        previousIqRate = 0.0;
        broadcastState();
    }
    return true;
}

void CliNetworkBridge::broadcastState() {
    if (network.mode() != NetworkMode::Server || !serverCallbacks.settings) return;
    QJsonObject state{{QStringLiteral("type"), QStringLiteral("control")},
                      {QStringLiteral("action"), QStringLiteral("serverState")},
                      {QStringLiteral("processingMode"), mode},
                      {QStringLiteral("serverDisableLocalVisualAudio"), true},
                      {QStringLiteral("fullResolutionSpectrumFrames"), false},
                      {QStringLiteral("settings"), serverCallbacks.settings()}};
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
    if (action == QStringLiteral("setViewport") ||
        action == QStringLiteral("setServerLocalOutput")) return;
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
        QString reason;
        const bool applied = !serverCallbacks.applySettings ||
            serverCallbacks.applySettings(command.value(QStringLiteral("settings")).toObject(), &reason);
        if (!applied) {
            sendAck(peerId, command, false, reason);
            return;
        }
        bool running = true;
        if (action == QStringLiteral("start") && serverCallbacks.setRunning)
            running = serverCallbacks.setRunning(true, &reason);
        sendAck(peerId, command, running, reason);
        broadcastState();
        return;
    }
    if (action == QStringLiteral("stop")) {
        QString reason;
        if (serverCallbacks.setRunning) serverCallbacks.setRunning(false, &reason);
        broadcastState();
    }
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

void CliNetworkBridge::sendAudio(const QByteArray &pcm) {
    if (network.mode() != NetworkMode::Server || mode != 0 || peerCount() == 0 || pcm.isEmpty()) return;
    QJsonObject frame{{QStringLiteral("type"), QStringLiteral("audiobin")},
                      {QStringLiteral("sampleRate"), 48000},
                      {QStringLiteral("channels"), 1},
                      {QStringLiteral("sampleFormat"), QStringLiteral("pcm_s16le")},
                      {QStringLiteral("sequence"), QString::number(++audioSequence)},
                      {QStringLiteral("sourceTimeMs"), QString::number(QDateTime::currentMSecsSinceEpoch())}};
    for (const QString &peerId : network.readyPeerIds()) {
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
