#ifndef TRANSMITWAVEFORMGENERATOR_H
#define TRANSMITWAVEFORMGENERATOR_H

#include "transmitterbackend.h"

#include <QString>
#include <QVector>

#include <array>
#include <complex>

struct TxGenerationResult {
    QVector<std::complex<float>> iq;
    QString error;
    QString summary;
    double durationSeconds = 0.0;

    bool ok() const { return error.isEmpty() && !iq.isEmpty(); }
};

struct TxModulatorState {
    double phase = 0.0;
    std::array<float, 63> hilbertHistory{};
    int hilbertWriteIndex = 0;
    int audioFilterSampleRate = 0;
    double audioFilterCutoffHz = 0.0;
    std::array<double, 2> audioFilterZ1{};
    std::array<double, 2> audioFilterZ2{};

    void resetAudioFilter();
};

struct TxBandwidthLimiterState {
    int sampleRate = 0;
    double bandwidthHz = 0.0;
    double centerHz = 0.0;
    std::complex<double> shiftOscillator{1.0, 0.0};
    std::array<double, 2> iZ1{};
    std::array<double, 2> iZ2{};
    std::array<double, 2> qZ1{};
    std::array<double, 2> qZ2{};

    void reset();
};

class TransmitWaveformGenerator {
public:
    static bool isTextMode(TxModulation modulation);
    static bool isDigitalTextMode(TxModulation modulation);
    static double effectiveFmDeviationHz(const TxConfiguration &configuration);
    static double effectiveFskDeviationHz(const TxConfiguration &configuration);
    static double signalCenterHz(const TxConfiguration &configuration);
    static double audioLowPassCutoffHz(const TxConfiguration &configuration);
    static QString modulationName(TxModulation modulation);
    static TxGenerationResult generateText(const TxConfiguration &configuration,
                                           const QString &text);
    static QVector<std::complex<float>> modulateAudio(const TxConfiguration &configuration,
                                                      const QVector<float> &audio,
                                                      TxModulatorState *state = nullptr);
    static bool limitSignalBandwidth(const TxConfiguration &configuration,
                                     const std::complex<float> *input,
                                     int count,
                                     QVector<std::complex<float>> *output,
                                     TxBandwidthLimiterState *state);

private:
    static TxGenerationResult generateCw(const TxConfiguration &configuration,
                                         const QString &text);
    static TxGenerationResult generateFt8(const TxConfiguration &configuration,
                                          const QString &text);
    static TxGenerationResult generateDigitalText(const TxConfiguration &configuration,
                                                   const QString &text);
};

#endif // TRANSMITWAVEFORMGENERATOR_H
