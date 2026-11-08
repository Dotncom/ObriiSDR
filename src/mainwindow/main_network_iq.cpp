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
void YourClassName::sendNetworkIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount) {
    if (networkMode != NetworkMode::Server ||
        !isClientIqProcessingMode() ||
        !networkController ||
        !networkController->isControlReady() ||
        iqData.isEmpty()) {
        return;
    }

    const bool fullIqMode = isFullIqProcessingMode();
    const qint64 pendingLimit = fullIqMode
                                    ? NETWORK_IQ_MAX_PENDING_BYTES
                                    : NETWORK_CHANNEL_IQ_LOW_LATENCY_PENDING_BYTES;
    const qint64 pendingBytes = networkController->pendingBytes();
    if (pendingBytes > pendingLimit) {
        ++networkIqFramesDropped;
        if (networkIqFramesDropped == 1 ||
            (networkIqFramesDropped % NETWORK_IQ_DROP_LOG_INTERVAL) == 0) {
            qDebug() << "[NetworkIQ] dropping IQ frame because TCP queue is above low-latency limit"
                     << "dropped" << networkIqFramesDropped
                     << "pendingBytes" << pendingBytes
                     << "pendingLimit" << pendingLimit
                     << "frameBytes" << iqData.size();
        }
        return;
    }

    QJsonObject frame;
    frame["type"] = "iqbin";
    frame["sequence"] = QString::number(++networkIqFrameSequence);
    frame["sampleRate"] = sampleRate;
    frame["sourceSampleRate"] = pendingSettings.sampleRate;
    frame["sampleCount"] = sampleCount;
    frame["sampleFormat"] = isChannelIqProcessingMode() ? "channel_iq_s16le" : "iq_s8_interleaved";
    frame["channelized"] = isChannelIqProcessingMode();
    frame["centerFrequency"] = isChannelIqProcessingMode()
                                   ? pendingSettings.listeningFrequency
                                   : pendingSettings.centerFrequency;
    frame["actualFrequency"] = isChannelIqProcessingMode()
                                   ? pendingSettings.listeningFrequency
                                   : pendingSettings.actualFrequency;
    frame["listeningFrequency"] = pendingSettings.listeningFrequency;
    frame["bandwidth"] = pendingSettings.bandwidth;
    frame["modulationType"] = pendingSettings.modulationType;
    frame["sstvDemodulationMode"] =
        normalizedSstvDemodulationMode(pendingSettings.sstvDemodulationMode);
    frame["inputMode"] = isChannelIqProcessingMode() ? 0 : pendingSettings.inputMode;
    frame["payloadEncoding"] = "raw";

    networkController->sendBinaryCommand(frame, iqData);
}

void YourClassName::receiveNetworkIqFrame(const QJsonObject &frame) {
    QByteArray iqBytes = QByteArray::fromBase64(frame.value("iq").toString().toLatin1());
    handleNetworkIqPayload(frame, std::move(iqBytes));
}

void YourClassName::receiveNetworkIqFrameBinary(const QJsonObject &frame, const QByteArray &iqData) {
    handleNetworkIqPayload(frame, iqData);
}

void YourClassName::handleNetworkIqPayload(const QJsonObject &frame, QByteArray iqBytes) {
    if (networkMode != NetworkMode::Client || !isClientIqProcessingMode()) {
        return;
    }
    if (runState == RadioRunState::Idle &&
        networkController &&
        !networkController->clientHasControl()) {
        runState = RadioRunState::Running;
        updateUiForRunState();
        startNetworkClientProcessing();
    }
    if (runState == RadioRunState::Idle) {
        return;
    }

    const bool channelizedFrame = frame.value("channelized").toBool(false);
    const QString sampleFormat = frame.value("sampleFormat").toString();
    if ((!channelizedFrame && sampleFormat != QStringLiteral("iq_s8_interleaved")) ||
        (channelizedFrame && sampleFormat != QStringLiteral("channel_iq_s16le"))) {
        qDebug() << "[NetworkIQ] unsupported IQ frame format";
        return;
    }
    if (channelizedFrame != isChannelIqProcessingMode()) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[NetworkIQ] dropping IQ frame from previous processing mode"
                     << "frameChannelized" << channelizedFrame
                     << "currentProcessingMode" << static_cast<int>(networkProcessingMode);
        }
        return;
    }

    const int bytesPerFloat = channelizedFrame ? static_cast<int>(sizeof(qint16)) : 1;
    const int bytesPerIqSample = 2 * bytesPerFloat;
    if (iqBytes.size() < bytesPerIqSample) {
        return;
    }
    const int remainder = iqBytes.size() % bytesPerIqSample;
    if (remainder != 0) {
        iqBytes.chop(remainder);
    }

    const double frameSampleRate = frame.value("sampleRate").toDouble(pendingSettings.sampleRate);
    if (frameSampleRate <= 0.0) {
        return;
    }
    const int frameSampleCount = frame.value("sampleCount").toInt(iqBytes.size() / bytesPerIqSample);
    handleSpectrumEventIqFrame(iqBytes, frameSampleRate, frameSampleCount, channelizedFrame);
    processVideoIqFrame(iqBytes, frameSampleRate, frameSampleCount);
    if (channelizedFrame &&
        recordingManager &&
        recordingManager->isRecording() &&
        recordingManager->mode() == RecordingManager::Mode::ChannelIqWav) {
        recordingManager->appendIqFrame(iqBytes,
                                        frameSampleRate,
                                        frameSampleCount);
    }

    RadioSettings iqSettings = pendingSettings;
    iqSettings.sampleRate = frameSampleRate;
    iqSettings.centerFrequency = frame.value("centerFrequency").toDouble(iqSettings.centerFrequency);
    iqSettings.actualFrequency = frame.value("actualFrequency").toDouble(iqSettings.actualFrequency);
    iqSettings.listeningFrequency = frame.value("listeningFrequency").toDouble(iqSettings.listeningFrequency);
    iqSettings.bandwidth = frame.value("bandwidth").toDouble(iqSettings.bandwidth);
    iqSettings.modulationType = frame.value("modulationType").toInt(iqSettings.modulationType);
    iqSettings.sstvDemodulationMode = normalizedSstvDemodulationMode(
        frame.value("sstvDemodulationMode").toInt(iqSettings.sstvDemodulationMode));
    iqSettings.inputMode = frame.value("inputMode").toInt(iqSettings.inputMode);

    if (channelizedFrame) {
        iqSettings.inputMode = INPUT_RF;
        iqSettings.centerFrequency = iqSettings.listeningFrequency;
        iqSettings.actualFrequency = iqSettings.listeningFrequency;
        if (audioProcessor) {
            audioProcessor->configure(iqSettings);
        }
    }

    const bool streamShapeChanged =
        !networkIqStreamMetadataValid ||
        networkIqStreamWasChannelized != channelizedFrame ||
        std::abs(networkIqStreamSampleRate - frameSampleRate) > 0.5 ||
        networkIqStreamInputMode != iqSettings.inputMode ||
        (!channelizedFrame &&
         std::abs(networkIqStreamCenterFrequency - iqSettings.centerFrequency) > 0.5) ||
        (channelizedFrame &&
         std::abs(networkIqStreamListeningFrequency - iqSettings.listeningFrequency) > 0.5);

    if (streamShapeChanged) {
        qDebug() << "[NetworkIQ] receiver stream shape changed; resetting client IQ buffers"
                 << "channelized" << channelizedFrame
                 << "sampleRate" << frameSampleRate
                 << "center" << iqSettings.centerFrequency
                 << "listening" << iqSettings.listeningFrequency
                 << "inputMode" << iqSettings.inputMode;
        resetNetworkIqReceptionState(false, false, pendingSettings.audioEnabled && !isFullIqProcessingMode());
        networkIqStreamMetadataValid = true;
        networkIqStreamWasChannelized = channelizedFrame;
        networkIqStreamSampleRate = frameSampleRate;
        networkIqStreamCenterFrequency = iqSettings.centerFrequency;
        networkIqStreamListeningFrequency = iqSettings.listeningFrequency;
        networkIqStreamInputMode = iqSettings.inputMode;
    }

    std::vector<float> floatSamples(static_cast<std::size_t>(iqBytes.size() / bytesPerFloat));
    if (channelizedFrame) {
        const auto *src = reinterpret_cast<const uchar *>(iqBytes.constData());
        for (int i = 0, out = 0; i + 1 < iqBytes.size(); i += 2, ++out) {
            const qint16 value = static_cast<qint16>(src[i] | (src[i + 1] << 8));
            floatSamples[static_cast<std::size_t>(out)] = static_cast<float>(value) / 32767.0f;
        }
    } else {
        const auto *src = reinterpret_cast<const signed char *>(iqBytes.constData());
        for (int i = 0; i < iqBytes.size(); ++i) {
            floatSamples[static_cast<std::size_t>(i)] = static_cast<float>(src[i]) / 127.0f;
        }
    }

    const bool queueClientAudioIq = pendingSettings.audioEnabled && !isFullIqProcessingMode();
    IqBuffer::setSampleRateEstimate(frameSampleRate);
    IqBuffer::publish(floatSamples.data(), floatSamples.size(), queueClientAudioIq);
    if (isFullIqProcessingMode() && updateTimer && !updateTimer->isActive()) {
        qDebug() << "[NetworkIQ] restarting client spectrum timer after IQ frame";
        updateTimer->start();
    }

    if (pendingNetworkAudioStartAfterIqPrebuffer && pendingSettings.audioEnabled && audioProcessor) {
        const double queuedIqSamples = static_cast<double>(IqBuffer::queuedFloatCount()) / 2.0;
        const double queuedSeconds = queuedIqSamples / frameSampleRate;
        if (queuedSeconds >= NETWORK_AUDIO_PREBUFFER_SECONDS) {
            pendingNetworkAudioStartAfterIqPrebuffer = false;
            qDebug() << "[NetworkIQ] starting client demodulator after IQ prebuffer"
                     << "queuedSeconds" << queuedSeconds
                     << "queuedBlocks" << IqBuffer::queuedBlocks()
                     << "sampleRate" << frameSampleRate;
            audioProcessor->startDemodulation();
        }
    }
}
