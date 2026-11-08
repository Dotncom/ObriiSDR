#ifndef PLAYBACKMANAGER_H
#define PLAYBACKMANAGER_H

#include <QObject>
#include <QByteArray>
#include <QFile>
#include <QTimer>

#include "radiosettings.h"

class PlaybackManager : public QObject {
    Q_OBJECT

public:
    enum class Mode {
        AudioWav,
        ChannelIqWav
    };

    struct WavInfo {
        bool valid = false;
        Mode mode = Mode::AudioWav;
        int sampleRate = 0;
        int channels = 0;
        int bitsPerSample = 0;
        quint64 dataOffset = 0;
        quint64 dataSize = 0;
        bool hasRadioSettings = false;
        RadioSettings radioSettings;
        bool hasScalePercent = false;
        double scalePercent = 100.0;
    };

    explicit PlaybackManager(QObject *parent = nullptr);
    ~PlaybackManager();

    bool start(const QString &filePath, QString *errorMessage = nullptr);
    void stop();
    bool isPlaying() const;
    QString currentFilePath() const;
    WavInfo currentInfo() const;

    static bool readWavInfo(const QString &filePath, WavInfo &info, QString *errorMessage = nullptr);

signals:
    void audioFrameReady(const QByteArray &pcmData);
    void iqFrameReady(const QByteArray &iqData, double sampleRate, int sampleCount);
    void started(const QString &filePath, PlaybackManager::WavInfo info);
    void stopped();
    void statusChanged(const QString &status);

private slots:
    void pump();

private:
    void scheduleNext(int sampleCount);
    void updateStatus(const QString &status);

    QFile inputFile;
    QString activeFilePath;
    WavInfo activeInfo;
    QTimer pumpTimer;
    bool playing = false;
    quint64 bytesRead = 0;
};

#endif // PLAYBACKMANAGER_H
