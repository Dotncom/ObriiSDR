#include "remoteaudioplayer.h"
#include "diagnosticlogging.h"

#include <QDebug>
#if !defined(_WIN32) && defined(FOBOSAPP_HAS_QT_AUDIO)
#include <QAudioDeviceInfo>
#include <QAudioFormat>
#endif
#include <cstring>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

RemoteAudioPlayer::RemoteAudioPlayer(QObject *parent)
    : QObject(parent) {
    cleanupTimer.setInterval(100);
    connect(&cleanupTimer, &QTimer::timeout, this, &RemoteAudioPlayer::cleanupCompletedBuffers);
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
    stop();
    outputDeviceIndex = index;
}

void RemoteAudioPlayer::setMaximumQueuedMs(int milliseconds) {
    const int value = milliseconds > 0 ? std::clamp(milliseconds, 50, 2000) : 0;
    if (value == maximumQueuedMs) return;
    stop();
    maximumQueuedMs = value;
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
            // A delayed optional DSP sink must never accumulate seconds of latency.
            waveOutReset(waveOut);
            cleanupCompletedBuffers();
        }
    }
    if (activeBlocks.size() >= MAX_ACTIVE_BLOCKS) {
        if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] dropping remote audio frame because playback queue is full"
                 << activeBlocks.size();
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

    QByteArray adjusted = pcmData;
    const float volume = outputVolume.load();
    auto *samples = reinterpret_cast<qint16 *>(adjusted.data());
    const int sampleCount = adjusted.size() / sizeof(qint16);
    for (int i = 0; i < sampleCount; ++i) {
        int sample = static_cast<int>(samples[i] * volume);
        sample = std::clamp(sample, -32768, 32767);
        samples[i] = static_cast<qint16>(sample);
    }

    if (qtAudioOutput->bytesFree() < adjusted.size()) {
        if (fobosVerboseLoggingEnabled()) qDebug() << "[NetworkAudio] dropping Qt audio frame because playback buffer is full"
                 << "bytesFree" << qtAudioOutput->bytesFree()
                 << "frameBytes" << adjusted.size();
        return;
    }
    qtAudioDevice->write(adjusted);
#else
    Q_UNUSED(pcmData);
#endif
}

void RemoteAudioPlayer::cleanupCompletedBuffers() {
#ifdef _WIN32
    if (!waveOut) {
        return;
    }

    for (int i = activeBlocks.size() - 1; i >= 0; --i) {
        AudioBlock *block = activeBlocks.at(i);
        if (block->header.dwFlags & WHDR_DONE) {
            activeBlocks.removeAt(i);
            releaseBlock(block);
        }
    }
#endif
}

#ifdef _WIN32
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
    cleanupTimer.stop();

#ifdef _WIN32
    if (waveOut) {
        waveOutReset(waveOut);
    }

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
