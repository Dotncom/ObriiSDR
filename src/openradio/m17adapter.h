#ifndef M17ADAPTER_H
#define M17ADAPTER_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include <complex>
#include <memory>

struct M17PacketSymbols {
    QByteArray linkSetupFrame;
    QVector<qint8> symbols;
    int packetFrameCount = 0;
    int symbolRate = 4800;
};

struct M17DecodedPacket {
    QString source;
    QString destination;
    int channelAccessNumber = -1;
    QByteArray payload;
    int packetFrameCount = 0;
};

class M17Adapter {
public:
    static constexpr int VoiceSampleRate = 8000;
    static constexpr int MaximumVoiceSamples = 60 * VoiceSampleRate;

    static bool available();
    static QString libraryVersion();
    static bool encodeCallsign(const QString &callsign, QByteArray *encoded,
                               QString *error = nullptr);
    static QString decodeCallsign(const QByteArray &encoded, QString *error = nullptr);
    static M17PacketSymbols buildPacket(const QString &source,
                                        const QString &destination,
                                        const QByteArray &payload,
                                        int channelAccessNumber,
                                        QString *error = nullptr);
    static QVector<std::complex<float>> modulatePacket(const M17PacketSymbols &packet, float level,
                                                       int sampleRate = 48000);
    // Finite, clear Codec2 3200 voice. The input is normalized mono PCM at 8 kHz.
    static QVector<std::complex<float>> modulateVoice(const QString &source, const QString &destination,
        int channelAccessNumber, const QVector<float> &pcm, float level, int sampleRate, QString *error = nullptr);
    static bool decodePacketSymbols(const QByteArray &symbols,
                                    M17DecodedPacket *packet,
                                    QString *error = nullptr);
};

// One clear Codec2 3200 call. IQ is emitted at 48 kS/s.
class M17VoiceStream {
public:
    static constexpr int IqRate = 48000;
    static constexpr int FrameSamples = 320; // 40 ms at 8 kHz

    M17VoiceStream();
    ~M17VoiceStream();
    QVector<std::complex<float>> begin(const QString &source, const QString &destination, int can,
                                      float level, QString *error);
    QVector<std::complex<float>> pushAudio(const QVector<float> &mono8k, QString *error);
    QVector<std::complex<float>> finish(QString *error);
    void reset();
    bool isActive() const;

private:
    struct State;
    std::unique_ptr<State> state;
};

#endif // M17ADAPTER_H
