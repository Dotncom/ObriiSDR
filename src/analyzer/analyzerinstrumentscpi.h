#ifndef ANALYZERINSTRUMENTSCPI_H
#define ANALYZERINSTRUMENTSCPI_H

#include "analyzerinstrumentbackend.h"

#include <QObject>
#include <atomic>

class QThread;

class AnalyzerInstrumentScpi final : public QObject, public AnalyzerInstrumentBackend {
public:
    ~AnalyzerInstrumentScpi() override;
    QString backendId() const override;
    AnalyzerHardwareCapabilities capabilities() const override;
    bool start(const AnalyzerInstrumentPlan &, PointCallback, CompletionCallback) override;
    void cancel() override;
    bool running() const { return worker != nullptr; }
    static QString validationError(const AnalyzerInstrumentPlan &);

private:
    QThread *worker = nullptr;
    std::atomic_bool cancelled{false};
    PointCallback pointReady;
};

#endif
