#ifndef SPECTRUMFRAMERECORDER_H
#define SPECTRUMFRAMERECORDER_H

#include <QDateTime>
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <vector>

#include "radiosettings.h"

struct SpectrumFrameRecord {
    qint64 elapsedMs = 0;
    qint64 utcMs = 0;
    double centerFrequency = 0.0;
    double minFrequency = 0.0;
    double maxFrequency = 0.0;
    int fftLength = 0;
    std::vector<float> magnitudes;
};

struct SpectrumFrameRecording {
    QJsonObject metadata;
    std::vector<SpectrumFrameRecord> frames;
};

class SpectrumFrameReader {
public:
    bool open(const QString &path, QString *errorMessage);
    bool readNext(SpectrumFrameRecord &frame, QString *errorMessage);
    bool atEnd() const { return file.atEnd(); }
    qint64 position() const { return file.pos(); }
    qint64 size() const { return file.size(); }
    const QJsonObject &metadata() const { return header; }
private:
    QFile file;
    QJsonObject header;
};

class SpectrumFrameRecorder {
public:
    bool start(const RadioSettings &settings,
               double displayScalePercent,
               int targetBins,
               qint64 firstFrameUtcMs = 0,
               QString *errorMessage = nullptr);
    void stop();
    bool isRecording() const;
    QString currentFilePath() const;
    quint64 frameCount() const;
    int targetBins() const;
    qint64 firstFrameUtcMs() const;
    void setExtendedMetadata(const QJsonObject &metadata);

    bool appendFrame(const std::vector<float> &frequencies,
                     const std::vector<float> &magnitudes,
                     double centerFrequency,
                     double minFrequency,
                     double maxFrequency,
                     int fftLength);
    bool appendFrameRecord(const SpectrumFrameRecord &frame);

    static bool loadFile(const QString &path,
                         SpectrumFrameRecording &recording,
                         QString *errorMessage = nullptr);
    static SpectrumFrameRecord makeFrame(const std::vector<float> &frequencies,
                                         const std::vector<float> &magnitudes,
                                         double centerFrequency,
                                         double minFrequency,
                                         double maxFrequency,
                                         int fftLength,
                                         int targetBins,
                                         qint64 utcMs);

private:
    static std::vector<float> reduceMagnitudes(const std::vector<float> &magnitudes, int targetBins);
    static QString makeRecordingPath();
    QJsonObject makeMetadataObject() const;
    bool writeHeader(QString *errorMessage);

    QFile outputFile;
    RadioSettings recordingSettings;
    QJsonObject extendedMetadata;
    QDateTime startedAtUtc;
    QString filePath;
    double scalePercent = 100.0;
    int binLimit = 4096;
    qint64 firstUtcMs = 0;
    quint64 framesWritten = 0;
    bool active = false;
};

#endif // SPECTRUMFRAMERECORDER_H
