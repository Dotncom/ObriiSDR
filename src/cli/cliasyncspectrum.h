#pragma once

#include <QJsonObject>
#include <memory>
#include <vector>

struct CliSpectrumResult {
    QJsonObject summary;
    std::vector<float> trace;
    std::vector<float> display;
    double sampleRateHz = 0;
    double centerHz = 0;
    quint64 firstSample = 0;
    qint64 utcMs = 0;
    double computeMs = 0;
    QString error;
};

// One owner submits/polls; one optional low-priority worker executes FFTW.
// At most one full IQ window exists, shared by ownership transfer, not copying.
class CliAsyncSpectrum {
public:
    explicit CliAsyncSpectrum(int fftSize);
    ~CliAsyncSpectrum();
    CliAsyncSpectrum(const CliAsyncSpectrum &) = delete;
    CliAsyncSpectrum &operator=(const CliAsyncSpectrum &) = delete;
    bool valid() const;
    void submit(const float *iq, int count, double rate, double center,
                int intervalMs, bool fullTrace = true);
    bool poll(CliSpectrumResult &result);
    void reset();
    bool busy() const;
    QJsonObject status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
