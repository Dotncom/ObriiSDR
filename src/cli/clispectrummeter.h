#ifndef OBRII_CLI_SPECTRUM_METER_H
#define OBRII_CLI_SPECTRUM_METER_H

#include <QJsonObject>
#include <QString>

#include "../core/ffttransformcore.h"

#include <vector>

class CliSpectrumMeter {
public:
    // Instances are single-consumer; only FFTW planning is globally serialized.
    explicit CliSpectrumMeter(int fftSize);
    ~CliSpectrumMeter();
    CliSpectrumMeter(const CliSpectrumMeter &) = delete;
    CliSpectrumMeter &operator=(const CliSpectrumMeter &) = delete;

    bool valid() const;
    QJsonObject analyze(const float *interleavedIq, int complexSamples,
                        double sampleRateHz, double centerHz,
                        std::vector<float> *orderedTrace = nullptr) const;

private:
    int size = 0;
    FftTransformCore transform;
    double windowSum = 0.0;
};

#endif
