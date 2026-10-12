#ifndef DMRTRANSMITENCODER_H
#define DMRTRANSMITENCODER_H

#include "digital/dmrvocoder.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <array>
#include <complex>

struct DmrTxParameters {
    int outputSampleRate = 48000;
    float level = 0.50f;
    int colorCode = 1;
    int timeslot = 1;
    quint32 sourceId = 1;
    quint32 destinationId = 9;
    bool privateCall = false;
};

class DmrTransmitEncoder {
public:
    DmrTransmitEncoder();

    bool canEncodeVoice() const;
    bool configure(const DmrTxParameters &parameters, QString *error = nullptr);
    bool isActive() const;

    QVector<std::complex<float>> begin(QString *error = nullptr);
    QVector<std::complex<float>> pushAudio(const QVector<float> &audio,
                                           QString *error = nullptr);
    QVector<std::complex<float>> finish(QString *error = nullptr);
    void reset();

    static QByteArray buildLinkControlBurst(const DmrTxParameters &parameters,
                                            bool terminator);
    static QByteArray buildVoiceBurst(const DmrTxParameters &parameters,
                                      int burstIndex,
                                      const std::array<QByteArray, 3> &canonicalFrames);
    static QByteArray canonicalToOnAir(const QByteArray &canonicalFrame);
    static QVector<int> burstDibits(const QByteArray &burstWithFill);

private:
    QByteArray linkControlPayload() const;
    QVector<std::complex<float>> modulateBytes(const QByteArray &bytes);
    QVector<std::complex<float>> modulateBurst(const QByteArray &burst);
    QVector<std::complex<float>> encodeReadyAudio(QString *error);
    bool encodePcmFrame(const std::array<qint16, 160> &pcm, QString *error);
    void appendIq(QVector<std::complex<float>> &target,
                  const QVector<std::complex<float>> &source) const;
    void resetCallState();

    DmrVocoder vocoder;
    DmrTxParameters parameters;
    QVector<float> audioInput;
    QVector<qint16> pcm8k;
    QVector<QByteArray> pendingCanonicalFrames;
    double audioCursor = 0.0;
    double audioLowpass1 = 0.0;
    double audioLowpass2 = 0.0;
    std::array<float, 9> symbolHistory{};
    double phase = 0.0;
    int voiceBurstIndex = 0;
    bool configured = false;
    bool active = false;
};

#endif // DMRTRANSMITENCODER_H