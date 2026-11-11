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
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

extern bool colorf;
extern bool secondGraph;
void YourClassName::sendNetworkAudioFrame(const QByteArray &pcmData) {
    if (networkMode != NetworkMode::Server ||
        isChannelIqProcessingMode() ||
        !networkController ||
        !networkController->isControlReady() ||
        pcmData.isEmpty()) {
        return;
    }

    QJsonObject frame;
    frame["type"] = "audiobin";
    frame["sampleRate"] = 48000;
    frame["channels"] = 1;
    frame["sampleFormat"] = "pcm_s16le";
    static quint64 sentAudioFrames = 0;
    static quint64 skippedAudioDeliveries = 0;
    frame["sequence"] = QString::number(++sentAudioFrames);
    frame["sourceTimeMs"] = QString::number(QDateTime::currentMSecsSinceEpoch());
    quint64 slowPeers = 0;
    QJsonObject legacyFrame;
    for (const QString &peerId : networkController->readyPeerIds()) {
        if (networkController->peerStreamProfile(peerId) == QStringLiteral("remote-vfo")) continue;
        if (networkController->pendingBytesForPeer(peerId) > 256 * 1024) {
            ++slowPeers;
            continue;
        }
        if (networkController->peerSupportsBinaryAudio(peerId)) {
            networkController->sendBinaryCommandToPeer(peerId, frame, pcmData);
        } else {
            if (legacyFrame.isEmpty()) {
                legacyFrame = frame;
                legacyFrame["type"] = "audio";
                legacyFrame["pcm"] = QString::fromLatin1(pcmData.toBase64());
            }
            networkController->sendControlCommandToPeer(peerId, legacyFrame);
        }
    }
    skippedAudioDeliveries += slowPeers;
    if (fobosVerboseLoggingEnabled() && sentAudioFrames % 200 == 1) {
        qDebug() << "[NetworkAudio] sent PCM frame"
                 << "frames" << static_cast<qulonglong>(sentAudioFrames)
                 << "bytes" << pcmData.size()
                 << "slowPeers" << slowPeers
                 << "skippedDeliveries" << skippedAudioDeliveries;
        skippedAudioDeliveries = 0;
    }
}

void YourClassName::playNetworkAudioFrame(const QJsonObject &frame) {
    playNetworkAudioPcmFrame(frame,
        QByteArray::fromBase64(frame.value("pcm").toString().toLatin1()));
}

void YourClassName::playNetworkAudioPcmFrame(const QJsonObject &frame,
                                             const QByteArray &pcmData) {
    if (networkMode != NetworkMode::Client || !remoteAudioPlayer) {
        return;
    }
    if (!networkController || !networkController->isControlReady()) {
        return;
    }

    if (frame.value("sampleRate").toInt(48000) != 48000 ||
        frame.value("channels").toInt(1) != 1 ||
        frame.value("sampleFormat").toString() != QStringLiteral("pcm_s16le")) {
        qDebug() << "[NetworkAudio] unsupported remote audio frame format";
        return;
    }

    if (pcmData.isEmpty() || pcmData.size() % int(sizeof(qint16)) != 0) return;
    if (!acceptNetworkReceiverFrame()) return;
    const bool wideFm = networkClientConfirmedSettingsJson.value(QStringLiteral("modulationType"))
                            .toInt(pendingSettings.modulationType) == MOD_WFM;
    remoteAudioPlayer->setWideFmNetworkBuffer(wideFm);
    static quint64 receivedAudioFrames = 0;
    static quint64 expectedSequence = 0;
    static quint64 missingFrames = 0;
    static qint64 lastArrivalMs = 0;
    static qint64 worstArrivalGapMs = 0;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const quint64 sequence = frame.value("sequence").toString().toULongLong();
    if (sequence > 0) {
        if (expectedSequence > 0 && sequence > expectedSequence) {
            const quint64 gap = sequence - expectedSequence;
            missingFrames += gap;
            if (gap <= 3) {
                const QByteArray silence(pcmData.size(), '\0');
                for (quint64 i = 0; i < gap; ++i)
                    remoteAudioPlayer->playNetworkPcmFrame(silence);
            }
        }
        expectedSequence = sequence + 1;
    }
    remoteAudioPlayer->playNetworkPcmFrame(pcmData);
    processDigitalAudioFrame(pcmData);
    processSstvAudioFrame(pcmData);
    processAptAudioFrame(pcmData);
    processWefaxAudioFrame(pcmData);
    sendAudioRelayFrame(pcmData);
    sendAudioHttpFrame(pcmData);
    if (lastArrivalMs > 0) worstArrivalGapMs = (std::max)(worstArrivalGapMs, nowMs - lastArrivalMs);
    lastArrivalMs = nowMs;
    ++receivedAudioFrames;
    if (fobosVerboseLoggingEnabled() && receivedAudioFrames % 200 == 1) {
        qDebug() << "[NetworkAudio] received PCM frame"
                 << "frames" << static_cast<qulonglong>(receivedAudioFrames)
                 << "bytes" << pcmData.size()
                 << "missing" << missingFrames
                 << "worstGapMs" << worstArrivalGapMs;
        worstArrivalGapMs = 0;
    }
}

void YourClassName::updateAudioRelaySocket() {
    if (!audioRelaySocket) {
        return;
    }

    if (!audioRelayReceiveEnabled) {
        if (audioRelaySocket->state() != QAbstractSocket::UnconnectedState) {
            audioRelaySocket->close();
        }
        return;
    }

    if (audioRelaySocket->state() != QAbstractSocket::UnconnectedState &&
        audioRelaySocket->localPort() == audioRelayListenPort) {
        return;
    }

    audioRelaySocket->close();
    const bool bound = audioRelaySocket->bind(QHostAddress::AnyIPv4,
                                              audioRelayListenPort,
                                              QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (bound) {
        qDebug() << "[AudioRelay] receive enabled on UDP port" << audioRelayListenPort;
    } else {
        qWarning() << "[AudioRelay] could not bind UDP port"
                   << audioRelayListenPort
                   << audioRelaySocket->errorString();
    }
}

void YourClassName::sendAudioRelayFrame(const QByteArray &pcmData) {
    if (!audioRelayTransmitEnabled ||
        !audioRelaySocket ||
        audioRelayHost.trimmed().isEmpty() ||
        audioRelayPort == 0 ||
        pcmData.isEmpty()) {
        return;
    }

    QHostAddress targetAddress;
    if (!targetAddress.setAddress(audioRelayHost.trimmed())) {
        static QString lastInvalidHost;
        if (lastInvalidHost != audioRelayHost) {
            lastInvalidHost = audioRelayHost;
            qWarning() << "[AudioRelay] target must be a numeric IP address:" << audioRelayHost;
        }
        return;
    }

    QByteArray datagram;
    datagram.reserve(AUDIO_RELAY_HEADER_BYTES + pcmData.size());
    datagram.append("FBA1", 4);
    appendLe32(datagram, ++audioRelaySequence);
    appendLe32(datagram, static_cast<quint32>(pcmData.size()));
    datagram.append(pcmData);

    audioRelaySocket->writeDatagram(datagram, targetAddress, audioRelayPort);
    static quint64 sentRelayFrames = 0;
    ++sentRelayFrames;
    if (fobosVerboseLoggingEnabled() &&
        (sentRelayFrames <= 8 || (sentRelayFrames % 200) == 0)) {
        qDebug() << "[AudioRelay] sent PCM frame"
                 << "frames" << static_cast<qulonglong>(sentRelayFrames)
                 << "bytes" << pcmData.size()
                 << "target" << audioRelayHost
                 << "port" << audioRelayPort;
    }
}

void YourClassName::receiveAudioRelayDatagrams() {
    if (!audioRelayReceiveEnabled || !audioRelaySocket) {
        return;
    }

    while (audioRelaySocket->hasPendingDatagrams()) {
        const qint64 pendingSize = audioRelaySocket->pendingDatagramSize();
        if (pendingSize < AUDIO_RELAY_HEADER_BYTES || pendingSize > 65507) {
            QByteArray discard;
            discard.resize(static_cast<int>((std::min)(pendingSize, qint64(65507))));
            audioRelaySocket->readDatagram(discard.data(), discard.size());
            continue;
        }

        QByteArray datagram;
        datagram.resize(static_cast<int>(pendingSize));
        audioRelaySocket->readDatagram(datagram.data(), datagram.size());

        if (!datagram.startsWith("FBA1")) {
            continue;
        }

        const quint32 payloadBytes = readLe32(datagram.constData() + 8);
        if (payloadBytes == 0 ||
            payloadBytes > static_cast<quint32>(datagram.size() - AUDIO_RELAY_HEADER_BYTES)) {
            continue;
        }

        const QByteArray pcmData = datagram.mid(AUDIO_RELAY_HEADER_BYTES, static_cast<int>(payloadBytes));
        static quint64 receivedRelayFrames = 0;
        ++receivedRelayFrames;
        if (fobosVerboseLoggingEnabled() &&
            (receivedRelayFrames <= 8 || (receivedRelayFrames % 200) == 0)) {
            qDebug() << "[AudioRelay] received PCM frame"
                     << "frames" << static_cast<qulonglong>(receivedRelayFrames)
                     << "bytes" << pcmData.size()
                     << "port" << audioRelayListenPort;
        }
        if (remoteAudioPlayer) {
            remoteAudioPlayer->playPcmFrame(pcmData);
        } else if (fobosVerboseLoggingEnabled()) {
            static bool loggedMissingPlayer = false;
            if (!loggedMissingPlayer) {
                loggedMissingPlayer = true;
                qDebug() << "[AudioRelay] received PCM but no remote audio player is available";
            }
        }
    }
}

void YourClassName::updateAudioHttpStreamServer() {
    if (!audioHttpServer) {
        return;
    }

    if (!audioHttpStreamEnabled) {
        if (audioHttpServer->isListening()) {
            audioHttpServer->close();
            qDebug() << "[AudioHTTP] stream server stopped";
        }
        for (QTcpSocket *client : std::as_const(audioHttpClients)) {
            if (client) {
                client->disconnectFromHost();
                client->deleteLater();
            }
        }
        audioHttpClients.clear();
        return;
    }

    if (audioHttpServer->isListening() && audioHttpServer->serverPort() == audioHttpStreamPort) {
        return;
    }

    for (QTcpSocket *client : std::as_const(audioHttpClients)) {
        if (client) {
            client->disconnectFromHost();
            client->deleteLater();
        }
    }
    audioHttpClients.clear();
    audioHttpServer->close();

    const bool listening = audioHttpServer->listen(QHostAddress::AnyIPv4, audioHttpStreamPort);
    if (listening) {
        qDebug() << "[AudioHTTP] VLC-compatible audio stream listening on"
                 << QString("http://0.0.0.0:%1/audio.wav").arg(audioHttpStreamPort);
    } else {
        qWarning() << "[AudioHTTP] could not listen on port"
                   << audioHttpStreamPort
                   << audioHttpServer->errorString();
    }
}

void YourClassName::acceptAudioHttpClient() {
    if (!audioHttpServer) {
        return;
    }

    while (QTcpSocket *client = audioHttpServer->nextPendingConnection()) {
        audioHttpClients.append(client);
        connect(client, &QTcpSocket::disconnected, this, [this, client]() {
            removeAudioHttpClient(client);
        });

        QByteArray response;
        response.append("HTTP/1.1 200 OK\r\n");
        response.append("Content-Type: audio/wav\r\n");
        response.append("Cache-Control: no-cache, no-store, must-revalidate\r\n");
        response.append("Pragma: no-cache\r\n");
        response.append("Connection: close\r\n");
        response.append("\r\n");
        response.append(streamingWavHeader());
        client->write(response);
        qDebug() << "[AudioHTTP] client connected"
                 << client->peerAddress().toString()
                 << "clients" << audioHttpClients.size();
    }
}

void YourClassName::removeAudioHttpClient(QTcpSocket *client) {
    if (!client) {
        return;
    }

    audioHttpClients.removeAll(client);
    client->deleteLater();
    qDebug() << "[AudioHTTP] client disconnected"
             << "clients" << audioHttpClients.size();
}

void YourClassName::sendAudioHttpFrame(const QByteArray &pcmData) {
    if (!audioHttpStreamEnabled || audioHttpClients.isEmpty() || pcmData.isEmpty()) {
        return;
    }

    for (int i = audioHttpClients.size() - 1; i >= 0; --i) {
        QTcpSocket *client = audioHttpClients.at(i);
        if (!client || client->state() != QAbstractSocket::ConnectedState) {
            if (client) {
                removeAudioHttpClient(client);
            } else {
                audioHttpClients.removeAt(i);
            }
            continue;
        }

        if (client->bytesToWrite() > AUDIO_HTTP_MAX_PENDING_BYTES) {
            continue;
        }
        client->write(pcmData);
    }
}
