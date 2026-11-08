#ifndef SCIENTIFICSESSIONTOOLS_H
#define SCIENTIFICSESSIONTOOLS_H

#include "spectrumscienceanalyzer.h"

#include <QWidget>
#include <QJsonObject>

#include <functional>
#include <memory>
#include <vector>

struct ScientificSessionContext {
    double sampleRateHz = 0.0;
    double centerFrequencyHz = 0.0;
    double listeningFrequencyHz = 0.0;
    double bandwidthHz = 0.0;
    int inputMode = 0;
    int modulationType = 0;
    int fftLength = 0;
    int fftWindowType = 0;
};

class PulseAnalysisWidget : public QWidget {
public:
    using Translator = std::function<QString(const QString &, const QString &)>;

    explicit PulseAnalysisWidget(Translator translator, QWidget *parent = nullptr);
    ~PulseAnalysisWidget() override;

    void appendSpectrumFrame(const std::vector<float> &frequencies,
                             const std::vector<float> &levels,
                             const ScientificSessionContext &context,
                             const SpectrumScienceMetrics &metrics,
                             int amplitudeUnit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class MeasurementSessionWidget : public QWidget {
public:
    using Translator = std::function<QString(const QString &, const QString &)>;
    using FrequencySetter = std::function<void(double)>;

    explicit MeasurementSessionWidget(Translator translator,
                                      FrequencySetter frequencySetter,
                                      QWidget *parent = nullptr);
    ~MeasurementSessionWidget() override;

    bool isRunning() const;
    void setExperimentBundleCallbacks(std::function<QJsonObject()> snapshot,
                                      std::function<bool(const QJsonObject &, QString *)> applyWorkspace);
    void appendSpectrumFrame(const std::vector<float> &frequencies,
                             const std::vector<float> &levels,
                             const ScientificSessionContext &context,
                             const SpectrumScienceMetrics &metrics,
                             int amplitudeUnit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // SCIENTIFICSESSIONTOOLS_H
