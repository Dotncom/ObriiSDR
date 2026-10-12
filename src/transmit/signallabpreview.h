#ifndef SIGNALLABPREVIEW_H
#define SIGNALLABPREVIEW_H

#include "signallabgenerator.h"

#include <QImage>
#include <QVector>
#include <QWidget>

#include <complex>
#include <vector>

struct SignalLabSpectralPreviewData {
    QImage image;
    std::vector<std::vector<float>> levels;
    std::vector<std::vector<unsigned char>> colors;
    QString error;
    int fftSize = 0;
    int rowCount = 0;
    int sampleRate = 0;
    int windowSamples = 0;
    double rbwHz = 0.0;
    double durationSeconds = 0.0;
    float minimumDb = -120.0f;
    float maximumDb = 0.0f;

    bool ok() const {
        return error.isEmpty() && !image.isNull() && !levels.empty();
    }
};

SignalLabSpectralPreviewData buildSignalLabSpectralPreview(
    const SignalLabGenerationResult &result,
    int fftSize,
    int requestedRows);

struct SignalLabIqPreviewData {
    QVector<std::complex<float>> timePoints;
    QVector<std::complex<float>> samples;
    QVector<double> sampleTimes;
    QVector<std::complex<float>> symbols;
    QVector<double> symbolTimes;
    double timeSpanSeconds = 0.0;
    double durationSeconds = 0.0;
    bool continuousTrajectory = false;
    double amplitudeScale = 1.0;
};

SignalLabIqPreviewData buildSignalLabIqPreview(const SignalLabGenerationResult &result);

class SignalLabPreviewWidget final : public QWidget {
public:
    explicit SignalLabPreviewWidget(QWidget *parent = nullptr);

    void setResult(const SignalLabGenerationResult &result);
    void setData(const SignalLabIqPreviewData &data);
    void clear();
    void setLanguage(const QString &language);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<std::complex<float>> timePoints;
    QVector<std::complex<float>> constellation;
    QVector<std::complex<float>> symbols;
    double timeSpanSeconds = 0.0;
    bool ukrainian = false;
};

class SignalLabWaterfall2DWidget final : public QWidget {
public:
    explicit SignalLabWaterfall2DWidget(QWidget *parent = nullptr);

    void setData(const SignalLabSpectralPreviewData &data);
    void clear();
    void setLanguage(const QString &language);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString formatFrequency(double frequencyHz) const;

    QImage waterfall;
    int fftSize = 0;
    int rowCount = 0;
    int sampleRate = 0;
    double durationSeconds = 0.0;
    int windowSamples = 0;
    double rbwHz = 0.0;
    float minimumDb = -120.0f;
    float maximumDb = 0.0f;
    bool ukrainian = false;
};

#endif // SIGNALLABPREVIEW_H
