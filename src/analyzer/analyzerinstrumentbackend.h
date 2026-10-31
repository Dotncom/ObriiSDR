#ifndef ANALYZERINSTRUMENTBACKEND_H
#define ANALYZERINSTRUMENTBACKEND_H

#include "analyzerinstrumentplan.h"

#include <functional>

// Hardware adapters implement this boundary. The UI and plan serializer never
// access a receiver/TX handle directly, which keeps an eventual UHD service out
// of the analyzer event loop and makes safety/capability checks unavoidable.
class AnalyzerInstrumentBackend {
public:
    using PointCallback = std::function<void(const AnalyzerMeasurementPoint &)>;
    using CompletionCallback = std::function<void(const QString &error)>;

    virtual ~AnalyzerInstrumentBackend() = default;
    virtual QString backendId() const = 0;
    virtual AnalyzerHardwareCapabilities capabilities() const = 0;
    virtual bool start(const AnalyzerInstrumentPlan &plan,
                       PointCallback pointReady,
                       CompletionCallback completed) = 0;
    virtual void cancel() = 0;
};

#endif
