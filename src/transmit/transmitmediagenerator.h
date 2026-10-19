#ifndef TRANSMITMEDIAGENERATOR_H
#define TRANSMITMEDIAGENERATOR_H

#include "transmitterbackend.h"

#include <QImage>
#include <QString>
#include <QVector>

#include <complex>

struct TxAudioMedia {
    QVector<float> samples;
    int sampleRate = 0;
    QString summary;
    QString error;
    bool ok() const { return error.isEmpty() && !samples.isEmpty() && sampleRate > 0; }
};

class TransmitMediaGenerator {
public:
    static QVector<float> resampleAudio(const QVector<float> &input,
                                        int sourceRate,
                                        int targetRate);
    static TxAudioMedia loadWav(const QString &path, int targetSampleRate);
    static TxAudioMedia generateSstv(const QImage &image,
                                     const QString &modeId,
                                     int sampleRate);
    static QVector<std::complex<float>> generateAtvFrame(const QImage &image,
                                                          const TxConfiguration &configuration,
                                                          bool fmVideo,
                                                          int width = 384,
                                                          int height = 288,
                                                          double lineRate = 15625.0);
};

#endif // TRANSMITMEDIAGENERATOR_H
