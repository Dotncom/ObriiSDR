#ifndef RECORDINGMANAGER_H
#define RECORDINGMANAGER_H

#include <QObject>
#include <QFile>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include "radiosettings.h"
#include "recordingformatcore.h"

class RecordingManager : public QObject {
    Q_OBJECT

public:
    enum class Mode {
        AudioWav = 0,
        ChannelIqWav = 1
    };

    explicit RecordingManager(QObject *parent = nullptr);
    ~RecordingManager();

    bool start(Mode mode, const RadioSettings &settings, QString *errorMessage = nullptr);
    void stop();
    bool isRecording() const;
    Mode mode() const;
    QString currentFilePath() const;
    void setDisplayScalePercent(double scalePercent);
    void setLabMetadata(const QJsonObject &metadata);

    void appendAudioFrame(const QByteArray &pcmData);
    void appendIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount);

signals:
    void statusChanged(const QString &status);

private:
    bool openWaveFile(int sampleRate, int channels, int bitsPerSample, QString *errorMessage = nullptr);
    QJsonObject makeMetadataObject() const;
    QByteArray makeMetadataChunk() const;
    bool patchWaveHeader(QString *errorMessage);
    bool writeSidecarMetadata(const QString &finishedFile, quint64 finishedBytes);
    QString makeRecordingPath(const QString &suffix) const;
    void updateStatus(const QString &status);

    QFile outputFile;
    Mode activeMode = Mode::AudioWav;
    RadioSettings recordingSettings;
    QJsonObject labMetadata;
    QString filePath;
    QDateTime recordingStartedAtUtc;
    bool recording = false;
    bool waveOpen = false;
    int waveSampleRate = 0;
    int waveChannels = 0;
    int waveBitsPerSample = 0;
    RecordingFormatCore::WaveLayout waveLayout;
    quint64 waveHeaderBytes = 0;
    double displayScalePercent = 100.0;
    quint64 dataBytesWritten = 0;
};

#endif // RECORDINGMANAGER_H
