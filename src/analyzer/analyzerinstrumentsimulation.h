#pragma once

#include "analyzerinstrumentbackend.h"

#include <QTimer>

// Drives an ideal two-port DUT on the Qt event loop, without device I/O.
class AnalyzerInstrumentSimulation final : public AnalyzerInstrumentBackend {
public:
    AnalyzerInstrumentSimulation();

    QString backendId() const override;
    AnalyzerHardwareCapabilities capabilities() const override;
    bool start(const AnalyzerInstrumentPlan &plan, PointCallback pointReady,
               CompletionCallback completed) override;
    void cancel() override;

    static AnalyzerInstrumentPlan simulationPlan(AnalyzerInstrumentPlan plan);
    static QString validationError(const AnalyzerInstrumentPlan &plan);
    static qint64 pointCount(const AnalyzerInstrumentPlan &plan);
    static double durationMs(const AnalyzerInstrumentPlan &plan);

private:
    void startPoint();
    void sample();
    CompletionCallback stop();

    QTimer timer;
    QVector<double> frequencies;
    AnalyzerInstrumentPlan plan;
    PointCallback pointReady;
    CompletionCallback completed;
    std::complex<double> transmission{};
    std::complex<double> reflection{};
    int position = 0;
    int averages = 0;
    double frequencyHz = 0.0;
};
