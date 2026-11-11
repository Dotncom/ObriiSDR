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
    void playNetworkPcmFrame(const QByteArray &pcmData);
    void stop();
    void setVolume(float volume);
    void setOutputDevice(int index);
    void setMaximumQueuedMs(int milliseconds);
    void setWideFmNetworkBuffer(bool enabled);

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
    QList<QByteArray> networkPendingFrames;
    qint64 networkPendingBytes = 0;
    bool networkPrimed = false;
    bool networkWideFm = false;
#ifdef _WIN32
    void releaseBlock(AudioBlock *block);
    void logWaveOutQueue(const char *event, qint64 incomingBytes = 0, int prebufferMs = 0,
                         const char *reason = "-") const;
    void resetWaveOut(const char *reason);
#endif
    void stopPlayback(const char *reason);
    bool openDevice();

#ifdef _WIN32
    HWAVEOUT waveOut = nullptr;
    QList<AudioBlock *> activeBlocks;
    quint64 waveOutSubmittedBytes = 0;
#elif defined(FOBOSAPP_HAS_QT_AUDIO)
    QAudioOutput *qtAudioOutput = nullptr;
    QIODevice *qtAudioDevice = nullptr;
#endif
    QTimer cleanupTimer;

    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int MAX_ACTIVE_BLOCKS = 24;
    static constexpr int NETWORK_PREBUFFER_MS = 80;
    static constexpr int WFM_NETWORK_PREBUFFER_MS = 160;
    static constexpr int NETWORK_MAX_PENDING_MS = 250;
};

#endif // REMOTEAUDIOPLAYER_H
