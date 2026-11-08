#ifndef SPECTRUMIQEVENTRECORDER_H
#define SPECTRUMIQEVENTRECORDER_H

#include <QFile>
#include <QJsonObject>
#include <QString>

#include "radiosettings.h"
#include "recordingformatcore.h"

class SpectrumIqEventRecorder {
public:
    enum class Mode {
        ChannelIqS16,
        FullIqS8,
        FullIqS16
    };

    bool start(const QString &eventBasePath,
               Mode mode,
               const RadioSettings &settings,
               double sampleRate,
               qint64 firstFrameUtcMs = 0,
               qint64 spectrumFirstFrameUtcMs = 0,
               QString *errorMessage = nullptr);
    void stop();
    bool isRecording() const;
    QString currentFilePath() const;
    quint64 bytesWritten() const;
    Mode mode() const;
    void setExtendedMetadata(const QJsonObject &metadata);

    bool appendFrame(const QByteArray &iqData, double sampleRate, int sampleCount, qint64 frameUtcMs = 0);

private:
    bool openChannelWav(QString *errorMessage);
    bool openFullIqRaw(QString *errorMessage);
    void patchWavHeader();
    bool writeSidecar() const;
    bool writeSigMfMetadata() const;
    QJsonObject makeMetadataObject() const;

    QFile outputFile;
    QString filePath;
    QString sidecarPath;
    Mode activeMode = Mode::ChannelIqS16;
    RadioSettings recordingSettings;
    QJsonObject extendedMetadata;
    double recordingSampleRate = 0.0;
    qint64 firstFrameUtcMs = 0;
    qint64 lastFrameUtcMs = 0;
    qint64 spectrumFirstFrameUtcMs = 0;
    quint64 writtenBytes = 0;
    quint64 framesWritten = 0;
    quint64 writeErrors = 0;
    qint64 writeNsSum = 0;
    qint64 writeNsMax = 0;
    qint64 flushNs = 0;
    RecordingFormatCore::WaveLayout waveLayout;
    bool active = false;
};

#endif // SPECTRUMIQEVENTRECORDER_H
