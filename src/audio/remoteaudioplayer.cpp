#include "remoteaudioplayer.h"
#include "appconstants.h"
#include "diagnosticlogging.h"

#include <QDebug>
#ifdef _WIN32
#include <QDateTime>
#endif
#if !defined(_WIN32) && defined(FOBOSAPP_HAS_QT_AUDIO)
#include <QAudioDeviceInfo>
#include <QAudioFormat>
#endif
#include <cstring>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

namespace {
constexpr int RelayOutputBlockMs = 20;
constexpr int RelayBurstHeadroomMs = 80;
constexpr int RelayIdleTimeoutMs = 500;
}

RemoteAudioPlayer::RemoteAudioPlayer(QObject *parent)
    : QObject(parent) {
    cleanupTimer.setInterval(100);
#if !defined(_WIN32) && defined(FOBOSAPP_HAS_QT_AUDIO)
    connect(&cleanupTimer, &QTimer::timeout, this, &RemoteAudioPlayer::drainRelayQtAudio);
#else
    connect(&cleanupTimer, &QTimer::timeout, this, &RemoteAudioPlayer::cleanupCompletedBuffers);
#endif
}

RemoteAudioPlayer::~RemoteAudioPlayer() {
    stop();
}

bool RemoteAudioPlayer::openDevice() {
#ifndef _WIN32
#ifdef FOBOSAPP_HAS_QT_AUDIO
    if (qtAudioOutput && qtAudioDevice) {
        return true;
    }

    QAudioFormat format;
    format.setSampleRate(SAMPLE_RATE);
    format.setChannelCount(1);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    const auto devices = QAudioDeviceInfo::availableDevices(QAudio::AudioOutput);
    if (outputDeviceIndex >= devices.size()) return false;
    const QAudioDeviceInfo deviceInfo = outputDeviceIndex < 0
        ? QAudioDeviceInfo::defaultOutputDevice() : devices[outputDeviceIndex];
    if (deviceInfo.isNull()) {
        if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] no default Qt audio output device";
        return false;
    }
    if (!deviceInfo.isFormatSupported(format)) {
        const QAudioFormat nearest = deviceInfo.nearestFormat(format);
        if (nearest.sampleRate() != SAMPLE_RATE ||
            nearest.channelCount() != 1 ||
            nearest.sampleSize() != 16 ||
            nearest.sampleType() != QAudioFormat::SignedInt) {
            if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] default Qt audio output does not support 48 kHz mono s16 PCM";
            return false;
        }
        format = nearest;
    }

    qtAudioOutput = new QAudioOutput(deviceInfo, format, this);
    qtAudioOutput->setBufferSize((maximumQueuedMs > 0 ? maximumQueuedMs : 2000)
                                * SAMPLE_RATE / 1000 * static_cast<int>(sizeof(qint16)));
    qtAudioDevice = qtAudioOutput->start();
    if (!qtAudioDevice) {
        if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] QAudioOutput start failed";
        delete qtAudioOutput;
        qtAudioOutput = nullptr;
        return false;
    }

    if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] Qt audio output opened" << deviceInfo.deviceName();
    return true;
#else
    static bool logged = false;
    if (!logged) {
        qDebug() << "[NetworkAudio] remote PCM playback is not implemented on this platform yet";
        logged = true;
    }
    return false;
#endif
#else
    if (waveOut) {
        return true;
    }

    WAVEFORMATEX format;
    std::memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = SAMPLE_RATE;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>((format.nChannels * format.wBitsPerSample) / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    const MMRESULT result = waveOutOpen(&waveOut,
                                        outputDeviceIndex < 0 ? WAVE_MAPPER : UINT(outputDeviceIndex),
                                        &format,
                                        0,
                                        0,
                                        CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        waveOut = nullptr;
        if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] waveOutOpen failed" << result;
        return false;
    }

    waveOutSubmittedBytes = 0;
    cleanupTimer.start();
    if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] remote audio output opened";
    return true;
#endif
}

void RemoteAudioPlayer::setVolume(float volume) {
    outputVolume.store(std::clamp(volume, 0.0f, 3.0f));
}

void RemoteAudioPlayer::setOutputDevice(int index) {
    if (index == outputDeviceIndex) return;
    stopPlayback("outputDeviceChange");
    outputDeviceIndex = index;
}

void RemoteAudioPlayer::setMaximumQueuedMs(int milliseconds) {
    const int value = milliseconds > 0 ? std::clamp(milliseconds, 50, 2000) : 0;
    if (value == maximumQueuedMs) return;
    stopPlayback("queueLimitChange");
    maximumQueuedMs = value;
}

void RemoteAudioPlayer::setWideFmNetworkBuffer(bool enabled) {
    if (enabled == networkWideFm) return;
    stopPlayback("modulationChange");
    networkWideFm = enabled;
}

void RemoteAudioPlayer::playNetworkPcmFrame(const QByteArray &pcmData) {
    playBufferedPcmFrame(pcmData, networkWideFm ? WFM_NETWORK_PREBUFFER_MS : NETWORK_PREBUFFER_MS);
}

void RemoteAudioPlayer::setRelayBufferMs(int milliseconds) {
    relayBufferMs = std::clamp(milliseconds, AUDIO_RELAY_BUFFER_MIN_MS, AUDIO_RELAY_BUFFER_MAX_MS);
    setMaximumQueuedMs(relayBufferMs + RelayBurstHeadroomMs);
}

void RemoteAudioPlayer::playRelayPcmFrame(const QByteArray &pcmData) {
    if (relayBufferMs == 0 || pcmData.isEmpty() || pcmData.size() % int(sizeof(qint16)) != 0) return;

#ifdef _WIN32
    // A cold waveOutOpen can take longer than the burst headroom. Open the
    // device before accumulating the prebuffer, so queued arrivals can fill it.
    if (!waveOut && !openDevice()) return;
#endif

    if (relayArrivalTimer.isValid() && relayArrivalTimer.elapsed() >= RelayIdleTimeoutMs) {
        // A new transmission should not start with samples left by the previous one.
        relayPartialBlock.clear();
        networkPendingFrames.clear();
        networkPendingBytes = 0;
        networkPrimed = false;
    }
    relayArrivalTimer.start();

    // Small UDP packets otherwise consume a WinMM header each, regardless of duration.
    constexpr int blockBytes = RelayOutputBlockMs * SAMPLE_RATE * int(sizeof(qint16)) / 1000;
    int offset = 0;
    while (offset < pcmData.size()) {
        const int bytes = (std::min)(blockBytes - relayPartialBlock.size(), pcmData.size() - offset);
        relayPartialBlock.append(pcmData.constData() + offset, bytes);
        offset += bytes;
        if (relayPartialBlock.size() == blockBytes) {
            playBufferedPcmFrame(relayPartialBlock, relayBufferMs);
            relayPartialBlock.clear();
        }
    }
}

void RemoteAudioPlayer::playBufferedPcmFrame(const QByteArray &pcmData, int prebufferMs) {
    constexpr qint64 bytesPerMs = SAMPLE_RATE * qint64(sizeof(qint16)) / 1000;
    const int maxPendingMs = relayBufferMs > 0 ? maximumQueuedMs : NETWORK_MAX_PENDING_MS;
    if (pcmData.isEmpty() || pcmData.size() % int(sizeof(qint16)) != 0 ||
        pcmData.size() > maxPendingMs * bytesPerMs) return;

#ifdef _WIN32
    cleanupCompletedBuffers();
    const bool outputDrained = networkPrimed && waveOut && activeBlocks.isEmpty();
#elif defined(FOBOSAPP_HAS_QT_AUDIO)
    const bool outputDrained = networkPrimed && qtAudioOutput &&
                               networkPendingFrames.isEmpty() &&
                               qtAudioOutput->state() == QAudio::IdleState;
#else
    const bool outputDrained = false;
#endif
    if (outputDrained) networkPrimed = false;

    networkPendingFrames.append(pcmData);
    networkPendingBytes += pcmData.size();
    while (networkPendingBytes > maxPendingMs * bytesPerMs &&
           networkPendingFrames.size() > 1) {
        networkPendingBytes -= networkPendingFrames.takeFirst().size();
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << (relayBufferMs ? "[AudioRelay] dropped stale prebuffer frame"
                                      : "[NetworkAudio] dropped stale prebuffer frame");
        }
    }
    const bool ready = networkPendingBytes >= prebufferMs * bytesPerMs;
    const bool priming = !networkPrimed && ready;
#ifdef _WIN32
    if ((!relayBufferMs || outputDrained || priming) && fobosVerboseLoggingEnabled()) {
        logWaveOutQueue(outputDrained ? "rebuffer" : "receive", pcmData.size(), prebufferMs);
        if (priming && !relayBufferMs) logWaveOutQueue("primed", pcmData.size(), prebufferMs);
    }
#endif
    if (!networkPrimed && !ready) return;
    networkPrimed = true;
#if !defined(_WIN32) && defined(FOBOSAPP_HAS_QT_AUDIO)
    if (relayBufferMs) {
        if (!openDevice()) {
            networkPrimed = false;
            return;
        }
        // Qt may report no writable space until its audio backend services the stream.
        drainRelayQtAudio();
        if (priming && fobosVerboseLoggingEnabled())
            qDebug() << "[AudioRelay] playback primed" << "bufferMs" << prebufferMs;
        return;
    }
#endif
    while (!networkPendingFrames.isEmpty()) {
        QByteArray frame = networkPendingFrames.takeFirst();
        networkPendingBytes -= frame.size();
        playPcmFrame(frame);
    }
    if (priming && relayBufferMs && fobosVerboseLoggingEnabled()) {
        qDebug() << "[AudioRelay] playback primed" << "bufferMs" << prebufferMs;
    }
#ifdef _WIN32
    if (priming && relayBufferMs && fobosVerboseLoggingEnabled())
        logWaveOutQueue("primed", pcmData.size(), prebufferMs);
#endif
}

void RemoteAudioPlayer::playPcmFrame(const QByteArray &pcmData) {
    if (pcmData.isEmpty()) {
        return;
    }
    if (!openDevice()) {
        return;
    }

#ifdef _WIN32
    cleanupCompletedBuffers();
    if (maximumQueuedMs > 0) {
        std::size_t queuedBytes = std::size_t(pcmData.size());
        for (const auto *block : activeBlocks) queuedBytes += block->data.size();
        if (queuedBytes > std::size_t(maximumQueuedMs) * SAMPLE_RATE * sizeof(qint16) / 1000) {
            if (fobosVerboseLoggingEnabled()) logWaveOutQueue("drop", pcmData.size(), 0, "queueLimit");
            return;
        }
    }
    if (activeBlocks.size() >= MAX_ACTIVE_BLOCKS) {
        if (fobosVerboseLoggingEnabled()) logWaveOutQueue("drop", pcmData.size(), 0, "blockLimit");
        return;
    }

    auto *block = new AudioBlock();
    std::memset(&block->header, 0, sizeof(WAVEHDR));
    const float volume = outputVolume.load();
    QByteArray adjusted = pcmData;

    auto *samples = reinterpret_cast<qint16 *>(adjusted.data());
    const int sampleCount = adjusted.size() / sizeof(qint16);

    for (int i = 0; i < sampleCount; ++i) {
        int sample = static_cast<int>(samples[i] * volume);
        sample = std::clamp(sample, -32768, 32767);
        samples[i] = static_cast<qint16>(sample);
    }

    block->data = adjusted;
    block->header.lpData = block->data.data();
    block->header.dwBufferLength = static_cast<DWORD>(block->data.size());

    MMRESULT result = waveOutPrepareHeader(waveOut, &block->header, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] waveOutPrepareHeader failed" << result;
        delete block;
        return;
    }

    result = waveOutWrite(waveOut, &block->header, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] waveOutWrite failed" << result;
        releaseBlock(block);
        return;
    }

    waveOutSubmittedBytes += static_cast<quint64>(block->data.size());
    activeBlocks.append(block);
#elif defined(FOBOSAPP_HAS_QT_AUDIO)
    if (!qtAudioOutput || !qtAudioDevice) {
        return;
    }
    if (qtAudioOutput->state() == QAudio::StoppedState) {
        qtAudioDevice = qtAudioOutput->start();
        if (!qtAudioDevice) {
            if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] QAudioOutput restart failed";
            return;
        }
    }

    if (qtAudioOutput->bytesFree() < pcmData.size()) {
        if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] dropping Qt audio frame because playback buffer is full"
                 << "bytesFree" << qtAudioOutput->bytesFree()
                 << "frameBytes" << pcmData.size();
        return;
    }
    writeQtPcmFrame(pcmData);
#else
    Q_UNUSED(pcmData);
#endif
}

#if !defined(_WIN32) && defined(FOBOSAPP_HAS_QT_AUDIO)
qint64 RemoteAudioPlayer::writeQtPcmFrame(const QByteArray &pcmData) {
    QByteArray adjusted = pcmData;
    const float volume = outputVolume.load();
    auto *samples = reinterpret_cast<qint16 *>(adjusted.data());
    const int sampleCount = adjusted.size() / sizeof(qint16);
    for (int i = 0; i < sampleCount; ++i) {
        const int sample = static_cast<int>(samples[i] * volume);
        samples[i] = static_cast<qint16>(std::clamp(sample, -32768, 32767));
    }
    return qtAudioDevice->write(adjusted);
}

void RemoteAudioPlayer::drainRelayQtAudio() {
    if (qtAudioOutput->state() == QAudio::StoppedState) {
        qtAudioDevice = qtAudioOutput->start();
        if (!qtAudioDevice) {
            cleanupTimer.stop();
            networkPrimed = false;
            return;
        }
    }

    while (!networkPendingFrames.isEmpty()) {
        auto &frame = networkPendingFrames.first();
        const int writable = (std::min)(qtAudioOutput->bytesFree(), frame.size());
        const int alignedBytes = writable - writable % int(sizeof(qint16));
        if (alignedBytes <= 0) break;
        const qint64 written = writeQtPcmFrame(frame.left(alignedBytes));
        if (written <= 0) break;
        networkPendingBytes -= written;
        if (written == frame.size()) networkPendingFrames.removeFirst();
        else frame.remove(0, static_cast<int>(written));
    }

    if (networkPendingFrames.isEmpty()) cleanupTimer.stop();
    else if (!cleanupTimer.isActive()) cleanupTimer.start(10);
}
#endif

void RemoteAudioPlayer::cleanupCompletedBuffers() {
#ifdef _WIN32
    if (!waveOut) {
        return;
    }

    const bool hadBlocks = !activeBlocks.isEmpty();
    for (int i = activeBlocks.size() - 1; i >= 0; --i) {
        AudioBlock *block = activeBlocks.at(i);
        if (block->header.dwFlags & WHDR_DONE) {
            activeBlocks.removeAt(i);
            releaseBlock(block);
        }
    }
    if (hadBlocks && activeBlocks.isEmpty() && networkPrimed && waveOutSubmittedBytes != 0 &&
        fobosVerboseLoggingEnabled())
        logWaveOutQueue("drained");
#endif
}

#ifdef _WIN32
void RemoteAudioPlayer::logWaveOutQueue(const char *event, qint64 incomingBytes,
                                        int prebufferMs, const char *reason) const {
    qint64 headerBytes = 0;
    for (const auto *block : activeBlocks) headerBytes += block->data.size();
    qint64 reserveBytes = 0;
    if (waveOut) {
        reserveBytes = -1;
        MMTIME position{};
        position.wType = TIME_BYTES;
        if (waveOutGetPosition(waveOut, &position, sizeof(position)) == MMSYSERR_NOERROR) {
            qint64 remaining = -1;
            // WinMM reports a 32-bit cursor. Subtract in the same units so a
            // wrap does not look like hours of queued audio.
            if (position.wType == TIME_BYTES)
                remaining = quint32(quint32(waveOutSubmittedBytes) - position.u.cb);
            else if (position.wType == TIME_SAMPLES)
                remaining = qint64(quint32(quint32(waveOutSubmittedBytes / sizeof(qint16)) -
                                           position.u.sample)) * sizeof(qint16);
            if (remaining >= 0 && remaining <= headerBytes + SAMPLE_RATE * qint64(sizeof(qint16)))
                reserveBytes = remaining;
        }
    }
    qDebug().nospace().noquote() << "[WinMMAudioQueue] " << event
        << " wallMs=" << QDateTime::currentMSecsSinceEpoch()
        << " headerBytes=" << headerBytes << " reserveBytes=" << reserveBytes
        << " pendingBytes=" << networkPendingBytes << " incomingBytes=" << incomingBytes
        << " blocks=" << activeBlocks.size() << " primed=" << int(networkPrimed)
        << " prebufferMs=" << prebufferMs << " limitMs=" << maximumQueuedMs
        << " submittedBytes=" << waveOutSubmittedBytes << " reason=" << reason
        << (relayBufferMs > 0 ? " source=udpRelay" : "");
}

void RemoteAudioPlayer::resetWaveOut(const char *reason) {
    const bool log = fobosVerboseLoggingEnabled();
    if (log) logWaveOutQueue("waveOutReset", 0, 0, reason);
    const auto result = waveOutReset(waveOut);
    if (result == MMSYSERR_NOERROR) waveOutSubmittedBytes = 0;
    else if (log) qDebug() << "[WinMMAudioQueue] waveOutReset failed" << result;
}

void RemoteAudioPlayer::releaseBlock(AudioBlock *block) {
    if (!block) {
        return;
    }
    if (waveOut && (block->header.dwFlags & WHDR_PREPARED)) {
        waveOutUnprepareHeader(waveOut, &block->header, sizeof(WAVEHDR));
    }
    delete block;
}
#endif

void RemoteAudioPlayer::stop() {
    stopPlayback("stop");
}

void RemoteAudioPlayer::stopPlayback(const char *reason) {
    cleanupTimer.stop();
#ifdef _WIN32
    if (waveOut) resetWaveOut(reason);
#else
    Q_UNUSED(reason);
#endif
    networkPendingFrames.clear();
    networkPendingBytes = 0;
    networkPrimed = false;
    relayPartialBlock.clear();
    relayArrivalTimer.invalidate();

#ifdef _WIN32
    while (!activeBlocks.isEmpty()) {
        releaseBlock(activeBlocks.takeLast());
    }

    if (waveOut) {
        waveOutClose(waveOut);
        waveOut = nullptr;
        if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] remote audio output closed";
    }
#elif defined(FOBOSAPP_HAS_QT_AUDIO)
    qtAudioDevice = nullptr;
    if (qtAudioOutput) {
        qtAudioOutput->stop();
        delete qtAudioOutput;
        qtAudioOutput = nullptr;
        if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] Qt audio output closed";
    }
#endif
}
