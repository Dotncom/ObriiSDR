#ifndef SIGNALLABGENERATOR_H
#define SIGNALLABGENERATOR_H

#include <QString>
#include <QVector>

#include <complex>

enum class SignalLabWaveform {
    Carrier,
    Sine,
    Square,
    Triangle,
    Sawtooth,
    Pulse,
    MultiTone,
    LinearChirp,
    WhiteNoise,
    Bpsk,
    Qpsk,
    Psk8,
    Pi4Dqpsk,
    Qam16,
    Qam64,
    Qam256,
    Fsk2,
    Gfsk2,
    Ofdm
};

enum class SignalLabConstellation {
    Bpsk,
    Qpsk,
    Psk8,
    Qam16,
    Qam64,
    Qam256
};

struct SignalLabConfiguration {
    SignalLabWaveform waveform = SignalLabWaveform::Carrier;
    int sampleRate = 48000;
    double durationSeconds = 0.5;
    double amplitude = 0.7;
    double centerOffsetHz = 0.0;
    double toneFrequencyHz = 1000.0;
    double secondFrequencyHz = 3000.0;
    double dutyCycle = 0.5;
    QString multiToneFrequencies = QStringLiteral("-3000, -1000, 1000, 3000");
    double symbolRate = 1200.0;
    double rolloff = 0.35;
    double fskDeviationHz = 1200.0;
    quint32 randomSeed = 1;
    int ofdmFftSize = 256;
    int ofdmActiveCarriers = 128;
    int ofdmCyclicPrefix = 32;
    int ofdmSymbolCount = 40;
    int ofdmPilotSpacing = 16;
    SignalLabConstellation ofdmConstellation = SignalLabConstellation::Qpsk;
    bool ofdmNullDc = true;
    double ofdmEdgeWindowPercent = 2.0;
};

struct SignalLabGenerationResult {
    QVector<std::complex<float>> iq;
    QString error;
    QString summary;
    int sampleRate = 0;
    double durationSeconds = 0.0;
    double occupiedBandwidthHz = 0.0;
    double peak = 0.0;
    double rms = 0.0;
    double paprDb = 0.0;
    int samplesPerSymbol = 0;
    double pulseRolloff = 0.0;
    double centerOffsetHz = 0.0;
    double normalizationGain = 1.0;

    bool ok() const { return error.isEmpty() && !iq.isEmpty(); }
};

class SignalLabGenerator {
public:
    static SignalLabGenerationResult generate(const SignalLabConfiguration &configuration);
    static QVector<std::complex<float>> matchedSymbols(const SignalLabGenerationResult &result,
                                                       int maximumPoints = 2500,
                                                       QVector<int> *sampleIndices = nullptr);
    static QString waveformName(SignalLabWaveform waveform);
    static QString constellationName(SignalLabConstellation constellation);
};

#endif // SIGNALLABGENERATOR_H
