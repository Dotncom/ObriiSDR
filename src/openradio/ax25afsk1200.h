#ifndef AX25AFSK1200_H
#define AX25AFSK1200_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVector>

class Ax25Afsk1200 {
public:
    static QVector<float> modulate(const QByteArray &ax25WithoutFcs,
                                   int sampleRate = 48000,
                                   int preambleFlags = 64,
                                   QString *error = nullptr);
    static QList<QByteArray> demodulate(const QVector<float> &audio,
                                       int sampleRate,
                                       QString *error = nullptr);
    static QByteArray encodePcm16Wav(const QVector<float> &audio,
                                     int sampleRate,
                                     QString *error = nullptr);
    static bool decodePcm16Wav(const QByteArray &wav,
                               QVector<float> *audio,
                               int *sampleRate,
                               QString *error = nullptr);
};

#endif // AX25AFSK1200_H
