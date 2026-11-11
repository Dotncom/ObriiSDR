#include "analyzerinstrumentsimulation.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr int MaximumPoints = 10000;
constexpr int MaximumSamples = 100000;
constexpr double MaximumDurationMs = 15 * 60 * 1000;
constexpr double CornerHz = 10.0e6;
constexpr double RadiansToDegrees = 180.0 / 3.14159265358979323846;

double magnitudeDb(double magnitude) {
    return 20.0 * std::log10(std::max(magnitude, 1e-12));
}
}

AnalyzerInstrumentSimulation::AnalyzerInstrumentSimulation() {
    timer.setSingleShot(true);
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &timer, [this]() { sample(); });
}

QString AnalyzerInstrumentSimulation::backendId() const {
    return QStringLiteral("simulation-ideal-low-pass");
}

AnalyzerHardwareCapabilities AnalyzerInstrumentSimulation::capabilities() const {
    return {};
}

AnalyzerInstrumentPlan AnalyzerInstrumentSimulation::simulationPlan(AnalyzerInstrumentPlan plan) {
    plan.useExternalGenerator = false;
    plan.transmitDeviceId.clear();
    plan.receiveDeviceId.clear();
    plan.capabilities = {};
    plan.scpi = {};
    const double impedance = plan.calibration.referenceImpedanceOhm;
    plan.calibration = {};
    plan.calibration.referenceImpedanceOhm = impedance;
    return plan;
}

qint64 AnalyzerInstrumentSimulation::pointCount(const AnalyzerInstrumentPlan &plan) {
    return plan.bidirectionalSweep ? 2LL * plan.points - 1 : plan.points;
}

double AnalyzerInstrumentSimulation::durationMs(const AnalyzerInstrumentPlan &plan) {
    return (plan.settleMs + plan.dwellMs * plan.averages) * pointCount(plan);
}

QString AnalyzerInstrumentSimulation::validationError(const AnalyzerInstrumentPlan &requested) {
    auto plan = simulationPlan(requested);
    plan.capabilities.maximumSafeOutputDbm = 30;
    plan.capabilities.maximumFrequencyHz = 100.0e9;
    const auto validation = AnalyzerInstrumentPlanner::validate(plan);
    if (!validation.errors.isEmpty()) return validation.errors.join(QLatin1Char('\n'));
    if (pointCount(plan) > MaximumPoints ||
        pointCount(plan) * plan.averages > MaximumSamples ||
        durationMs(plan) > MaximumDurationMs)
        return QStringLiteral("Simulation is limited to 10,000 points, 100,000 samples and 15 minutes.");
    return {};
}

bool AnalyzerInstrumentSimulation::start(const AnalyzerInstrumentPlan &requested,
                                        PointCallback report, CompletionCallback done) {
    if (completed || !report || !done || !validationError(requested).isEmpty()) return false;
    plan = simulationPlan(requested);
    frequencies = AnalyzerInstrumentPlanner::frequencyGrid(plan);
    position = 0;
    pointReady = std::move(report);
    completed = std::move(done);
    startPoint();
    return true;
}

void AnalyzerInstrumentSimulation::startPoint() {
    const int index = position < frequencies.size() ? position : 2 * (frequencies.size() - 1) - position;
    frequencyHz = frequencies.at(index);
    averages = 0;
    transmission = reflection = {};
    timer.start(int(std::ceil(plan.settleMs + plan.dwellMs)));
}

void AnalyzerInstrumentSimulation::sample() {
    // Ideal series reactance between equal reference impedances. The fixed
    // corner lets separate frequency sweeps recover the same DUT response.
    const std::complex<double> reactance{0.0, frequencyHz / CornerHz};
    const auto s21 = 1.0 / (1.0 + reactance);
    const auto s11 = reactance * s21;
    // Alternate a small coherent error to exercise averaging reproducibly.
    const auto phaseOffset = std::polar(1.0, averages % 2 == 0 ? 0.002 : -0.002);
    transmission += s21 * phaseOffset;
    reflection += s11 * phaseOffset;
    if (++averages < plan.averages) {
        timer.start(int(std::ceil(plan.dwellMs)));
        return;
    }

    const auto meanS21 = transmission / double(averages);
    const auto meanS11 = reflection / double(averages);
    AnalyzerMeasurementPoint point;
    point.frequencyHz = frequencyHz;
    point.stimulusDbm = point.referenceDb = plan.sourceLevelDbm;
    point.s21Magnitude = std::abs(meanS21);
    point.s11Magnitude = std::abs(meanS11);
    point.forwardDb = point.referenceDb + magnitudeDb(point.s21Magnitude);
    point.reflectedDb = point.referenceDb + magnitudeDb(point.s11Magnitude);
    point.phaseDegrees = std::arg(meanS21) * RadiansToDegrees;
    point.valid = true;

    const auto report = pointReady;
    if (++position == pointCount(plan)) {
        const auto done = stop();
        report(point);
        done({});
    } else {
        startPoint();
        report(point);
    }
}

AnalyzerInstrumentBackend::CompletionCallback AnalyzerInstrumentSimulation::stop() {
    timer.stop();
    pointReady = {};
    return std::exchange(completed, {});
}

void AnalyzerInstrumentSimulation::cancel() {
    if (const auto done = stop()) done(QStringLiteral("Simulation stopped by the operator"));
}
