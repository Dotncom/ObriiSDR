#ifndef REMOTEAUDIOPLAYER_H
#define REMOTEAUDIOPLAYER_H

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QTimer>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#elif defined(FOBOSAPP_HAS_QT_AUDIO)
#include <QAudioOutput>
#include <QIODevice>
#endif
#include <atomic>
#include <algorithm>

class RemoteAudioPlayer : public QObject {
    Q_OBJECT

public:
    explicit RemoteAudioPlayer(QObject *parent = nullptr);
    ~RemoteAudioPlayer();

    void playPcmFrame(const QByteArray &pcmData);
    void stop();
    void setVolume(float volume);
    void setOutputDevice(int index);
    void setMaximumQueuedMs(int milliseconds);

private slots:
    void cleanupCompletedBuffers();

private:
#ifdef _WIN32
    struct AudioBlock {
        WAVEHDR header;
        QByteArray data;
    };
#endif
    std::atomic<float> outputVolume = 1.0f;
    int outputDeviceIndex = -1;
    int maximumQueuedMs = 0;
#ifdef _WIN32
    void releaseBlock(AudioBlock *block);
#endif
    bool openDevice();

#ifdef _WIN32
    HWAVEOUT waveOut = nullptr;
    QList<AudioBlock *> activeBlocks;
#elif defined(FOBOSAPP_HAS_QT_AUDIO)
    QAudioOutput *qtAudioOutput = nullptr;
    QIODevice *qtAudioDevice = nullptr;
#endif
    QTimer cleanupTimer;

    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int MAX_ACTIVE_BLOCKS = 24;
};

#endif // REMOTEAUDIOPLAYER_H
