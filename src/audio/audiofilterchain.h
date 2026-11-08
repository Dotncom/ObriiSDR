#ifndef AUDIOFILTERCHAIN_H
#define AUDIOFILTERCHAIN_H

#include <QString>
#include <QVector>

#include <memory>
#include <vector>

enum class AudioFilterKind {
    LowPass,
    HighPass,
    BandPass,
    Notch,
    DcBlocker,
    Gain,
    Limiter,
    NoiseGate,
    DeEmphasis,
    ParametricEq,
    LowShelf,
    HighShelf,
    Compressor,
    AdaptiveNotch,
    NoiseBlanker,
    CwFilter,
    CtcssSuppressor,
    SpectralDenoise,
    CustomFir
};

struct AudioFilterPoint {
    double frequencyHz = 1000.0;
    double gainDb = 0.0;
};

struct AudioFilterStage {
    QString id;
    AudioFilterKind kind = AudioFilterKind::LowPass;
    bool enabled = true;
    QString customName;
    double frequencyHz = 3000.0;
    double frequency2Hz = 5000.0;
    double q = 0.707;
    double gainDb = 0.0;
    double thresholdDb = -6.0;
    double ratio = 3.0;
    double attackMs = 10.0;
    double releaseMs = 120.0;
    double timeConstantUs = 50.0;
    double bandwidthHz = 250.0;
    double amount = 0.8;
    QVector<AudioFilterPoint> curve;
};

QString audioFilterKindId(AudioFilterKind kind);
AudioFilterKind audioFilterKindFromId(const QString &id, bool *ok = nullptr);
QString audioFilterKindName(AudioFilterKind kind, bool ukrainian = false);
AudioFilterStage defaultAudioFilterStage(AudioFilterKind kind);
QVector<AudioFilterStage> decodeAudioFilterChain(const QString &json, bool *ok = nullptr);
QString encodeAudioFilterChain(const QVector<AudioFilterStage> &stages);
double audioFilterMagnitudeDb(const AudioFilterStage &stage,
                              double frequencyHz,
                              double sampleRate = 48000.0);

class AudioFilterChain {
public:
    AudioFilterChain();
    ~AudioFilterChain();

    AudioFilterChain(const AudioFilterChain &) = delete;
    AudioFilterChain &operator=(const AudioFilterChain &) = delete;

    void configure(const QString &json, double sampleRate = 48000.0);
    void reset();
    void process(std::vector<short> &samples);
    bool empty() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

#endif // AUDIOFILTERCHAIN_H
