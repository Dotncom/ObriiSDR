#ifndef CWAUDIODECODER_H
#define CWAUDIODECODER_H

#include <QByteArray>
#include <QString>

class CwAudioDecoder {
public:
    struct Settings {
        int sampleRate = 48000;
        double toneHz = 700.0;
        int initialWpm = 18;
        bool adaptiveSpeed = true;
        int alphabet = 0; // 0: International/Latin, 1: Ukrainian, 2: both.
        int selectivity = 5; // 1: permissive, 10: narrow and noise-resistant.
    };

    struct Result {
        QString text;
        QString status;
        double estimatedWpm = 0.0;
        double confidence = 0.0;
    };

    void configure(const Settings &settings);
    void reset();
    Result processPcm16(const QByteArray &pcmData);

private:
    void finishKeyDown(qint64 durationSamples, QString &output);
    void finishCharacter(QString &output);
    void finishBilingualWord(QString &output);
    void updateGap(qint64 durationSamples, QString &output);
    QChar decodePattern(const QString &pattern) const;

    Settings currentSettings;
    double oscillatorPhase = 0.0;
    double mixedI = 0.0;
    double mixedQ = 0.0;
    double envelope = 0.0;
    double noiseFloor = 1.0e-8;
    double signalPeak = 1.0e-6;
    double dotSamples = 3200.0;
    double lastConfidence = 0.0;
    qint64 stateSamples = 0;
    bool keyDown = false;
    bool initialized = false;
    bool characterGapEmitted = false;
    bool wordGapEmitted = false;
    bool emittedText = false;
    QString currentPattern;
    QString bilingualLatinWord;
    QString bilingualUkrainianWord;
    qint64 processedSamples = 0;
    qint64 lastStatusSample = 0;
};

#endif // CWAUDIODECODER_H
