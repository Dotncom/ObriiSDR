#include "analyzerinstrumentplan.h"
#include "analyzerinstrumentsimulation.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QTextStream>

#include <cmath>
#include <limits>

namespace {
int failures = 0;

void check(bool condition, const QString &message) {
    if (condition) return;
    QTextStream(stderr) << "FAIL: " << message << '\n';
    ++failures;
}

void testSimulation() {
    AnalyzerInstrumentSimulation simulation;
    AnalyzerInstrumentPlan plan;
    plan.mode = AnalyzerInstrumentMode::VectorNetworkAnalyzer;
    plan.startHz = 5e6;
    plan.stopHz = 15e6;
    plan.points = 3;
    plan.bidirectionalSweep = true;
    plan.settleMs = 1.0;
    plan.dwellMs = 1.0;
    plan.averages = 2;
    plan.useExternalGenerator = true;
    plan.scpi.enabled = true;
    plan.scpi.resource = QStringLiteral("fixture");
    plan.calibration.openMeasured = true;
    QVector<AnalyzerMeasurementPoint> points;
    QString completion;
    int completions = 0;
    QEventLoop loop;
    const auto run = [&](bool cancel) {
        points.clear();
        completion.clear();
        completions = 0;
        const auto report = [&](const AnalyzerMeasurementPoint &point) {
            points.append(point);
            if (cancel && points.size() == 2) simulation.cancel();
        };
        const auto done = [&](const QString &error) {
            ++completions;
            completion = error;
            loop.quit();
        };
        check(simulation.start(plan, report, done), QStringLiteral("simulation start"));
        check(points.isEmpty(), QStringLiteral("simulation start must return before measurements"));
        check(!simulation.start(plan, report, done), QStringLiteral("reject concurrent simulation"));
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(2000);
        loop.exec();
        check(completions == 1, QStringLiteral("simulation completes once"));
        simulation.cancel();
        check(completions == 1, QStringLiteral("cancel after completion is idempotent"));
    };
    run(false);
    QVector<double> frequencies;
    for (const auto &point : points) frequencies.append(point.frequencyHz);
    check(completion.isEmpty() && frequencies == QVector<double>{5e6, 10e6, 15e6, 10e6, 5e6},
          QStringLiteral("both passes visit the expected frequencies once per direction"));
    if (points.size() >= 2) {
        const auto &corner = points.at(1);
        const double expected = std::cos(0.002) / std::sqrt(2.0);
        check(corner.valid && std::abs(corner.s21Magnitude - expected) < 1e-12 &&
              std::abs(corner.s11Magnitude - expected) < 1e-12 &&
              std::abs(corner.phaseDegrees + 45.0) < 1e-12,
              QStringLiteral("averaged complex S-parameters at the 10 MHz corner"));
    }
    run(true);
    check(!completion.isEmpty() && points.size() == 2, QStringLiteral("stop keeps the measured prefix"));
    run(false);
    check(completion.isEmpty() && points.size() == 5, QStringLiteral("restart after cancellation"));

    const auto saved = AnalyzerInstrumentSimulation::simulationPlan(plan);
    check(!saved.useExternalGenerator && !saved.scpi.enabled && saved.scpi.resource.isEmpty() &&
          !saved.calibration.openMeasured && !simulation.capabilities().transmitterAvailable,
          QStringLiteral("simulation uses no SCPI, TX or claimed hardware calibration"));
    plan.points = std::numeric_limits<int>::max();
    check(!AnalyzerInstrumentSimulation::validationError(plan).isEmpty(), QStringLiteral("simulation rejects oversized plans"));
    plan.points = 8000;
    check(!AnalyzerInstrumentSimulation::validationError(plan).isEmpty(),
          QStringLiteral("both directions count toward the simulation limit"));
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    AnalyzerInstrumentPlan draft;
    draft.mode = AnalyzerInstrumentMode::VectorNetworkAnalyzer;
    AnalyzerPlanValidation validation = AnalyzerInstrumentPlanner::validate(draft);
    check(!validation.executable, QStringLiteral("unqualified VNA must be blocked"));
    check(!validation.calibratedResult, QStringLiteral("unqualified VNA must not be calibrated"));

    draft.capabilities.receiverAvailable = true;
    draft.capabilities.transmitterAvailable = true;
    draft.capabilities.fullDuplex = true;
    draft.capabilities.coherentReference = true;
    draft.capabilities.timestampedIq = true;
    draft.capabilities.receiverChannels = 2;
    draft.capabilities.transmitterChannels = 1;
    draft.capabilities.maximumSafeOutputDbm = 0.0;
    draft.calibration.openMeasured = true;
    draft.calibration.shortMeasured = true;
    draft.calibration.loadMeasured = true;
    draft.calibration.thruMeasured = true;
    draft.calibration.phaseReferenceMeasured = true;
    validation = AnalyzerInstrumentPlanner::validate(draft);
    check(validation.executable, QStringLiteral("qualified VNA plan should validate"));
    check(validation.calibratedResult, QStringLiteral("qualified VNA calibration should validate"));

    const QJsonDocument encoded(draft.toJson());
    const AnalyzerInstrumentPlan decoded = AnalyzerInstrumentPlan::fromJson(encoded.object());
    check(decoded.mode == draft.mode, QStringLiteral("mode JSON round-trip"));
    check(decoded.calibration.vectorComplete(), QStringLiteral("calibration JSON round-trip"));
    check(decoded.capabilities.receiverChannels == 2, QStringLiteral("capability JSON round-trip"));

    AnalyzerMeasurementSession session;
    session.sessionId = QStringLiteral("selftest");
    session.applicationVersion = QStringLiteral("test");
    session.startedUtc = QDateTime::currentDateTimeUtc();
    session.plan = draft;
    AnalyzerMeasurementPoint measured;
    measured.frequencyHz = 10.0e6;
    measured.s11Magnitude = 0.1;
    measured.valid = true;
    session.points.append(measured);
    session.eventLog.append(QStringLiteral("fixture"));
    session.backendId = QStringLiteral("simulation-ideal-low-pass");
    session.outcome = QStringLiteral("cancelled");
    const AnalyzerMeasurementSession restoredSession =
        AnalyzerMeasurementSession::fromJson(session.toJson());
    check(restoredSession.points.size() == 1 && restoredSession.points.first().valid,
          QStringLiteral("session point JSON round-trip"));
    check(restoredSession.eventLog.size() == 1 &&
              restoredSession.eventLog.first() == QStringLiteral("fixture"),
          QStringLiteral("session event-log JSON round-trip"));
    check(restoredSession.backendId == session.backendId && restoredSession.outcome == session.outcome,
          QStringLiteral("simulation provenance and outcome JSON round-trip"));

    const QVector<double> grid = AnalyzerInstrumentPlanner::frequencyGrid(draft);
    check(grid.size() == draft.points, QStringLiteral("frequency grid size"));
    check(!grid.isEmpty() && std::abs(grid.first() - draft.startHz) < 1.0e-6,
          QStringLiteral("frequency grid start"));
    check(!grid.isEmpty() && std::abs(grid.last() - draft.stopHz) < 1.0e-6,
          QStringLiteral("frequency grid stop"));

    auto invalid = draft;
    invalid.points = std::numeric_limits<int>::max();
    check(AnalyzerInstrumentPlanner::frequencyGrid(invalid).isEmpty(),
          QStringLiteral("oversized imported grid must be rejected before allocation"));
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()}) {
        invalid = draft;
        invalid.startHz = bad;
        check(AnalyzerInstrumentPlanner::frequencyGrid(invalid).isEmpty(),
              QStringLiteral("nonfinite grid start must be rejected"));
        invalid = draft;
        invalid.stopHz = bad;
        check(AnalyzerInstrumentPlanner::frequencyGrid(invalid).isEmpty(),
              QStringLiteral("nonfinite grid stop must be rejected"));
        invalid = draft;
        invalid.sourceLevelDbm = bad;
        check(!AnalyzerInstrumentPlanner::validate(invalid).executable,
              QStringLiteral("nonfinite source level must block execution"));
        invalid = draft;
        invalid.capabilities.maximumSafeOutputDbm = bad;
        check(!AnalyzerInstrumentPlanner::validate(invalid).executable,
              QStringLiteral("nonfinite safe output limit must block execution"));
    }
    invalid = draft;
    invalid.capabilities.transmitterChannels = 0;
    check(!AnalyzerInstrumentPlanner::validate(invalid).executable,
          QStringLiteral("TX availability without channels must block execution"));
    invalid = draft;
    invalid.calibration.referenceImpedanceOhm = 0.0;
    check(!AnalyzerInstrumentPlanner::validate(invalid).executable,
          QStringLiteral("zero reference impedance must block execution"));
    invalid = draft;
    invalid.capabilities.minimumFrequencyHz = 30.0e6;
    invalid.capabilities.maximumFrequencyHz = 1.0e6;
    check(!AnalyzerInstrumentPlanner::validate(invalid).executable,
          QStringLiteral("reversed hardware range must block execution"));
    invalid = draft;
    invalid.mode = AnalyzerInstrumentMode::ReturnLossSwr;
    invalid.capabilities.directionalCoupler = true;
    invalid.capabilities.fullDuplex = false;
    check(!AnalyzerInstrumentPlanner::validate(invalid).executable,
          QStringLiteral("return-loss requires simultaneous source and receiver"));
    invalid.useExternalGenerator = true;
    invalid.capabilities.externalGenerator = true;
    invalid.scpi.enabled = true;
    invalid.scpi.resource = QStringLiteral("TCPIP::fixture");
    check(AnalyzerInstrumentPlanner::validate(invalid).executable,
          QStringLiteral("external source permits return-loss with RX-only hardware"));

    const double magnitude = AnalyzerInstrumentPlanner::reflectionMagnitudeFromReturnLoss(20.0);
    check(std::abs(magnitude - 0.1) < 1.0e-12, QStringLiteral("return-loss inverse"));
    check(std::abs(AnalyzerInstrumentPlanner::returnLossDb(magnitude) - 20.0) < 1.0e-12,
          QStringLiteral("return-loss calculation"));
    check(std::abs(AnalyzerInstrumentPlanner::swrFromReflectionMagnitude(magnitude) -
                   1.2222222222222223) < 1.0e-12,
          QStringLiteral("SWR calculation"));

    using Complex = std::complex<double>;
    const std::array<Complex, 3> standards{{{0.99, 0.05}, {-0.94, -0.11}, {0.015, -0.02}}};
    const Complex expected{0.22, -0.34};
    const auto measure = [](Complex gamma) {
        return (Complex{0.08, -0.02} + Complex{0.62, 0.15} * gamma) /
               (1.0 + Complex{0.07, -0.13} * gamma);
    };
    std::array<Complex, 3> readings{{measure(standards[0]), measure(standards[1]), measure(standards[2])}};
    Complex corrected;
    check(AnalyzerInstrumentPlanner::correctOnePort(standards, readings, measure(expected), corrected) &&
              std::abs(corrected - expected) < 1e-12, QStringLiteral("complex Open/Short/Load correction"));
    readings.fill(readings[0]);
    check(!AnalyzerInstrumentPlanner::correctOnePort(standards, readings, measure(expected), corrected),
          QStringLiteral("singular Open/Short/Load calibration must be rejected"));

    testSimulation();
    QTextStream(stdout) << "Analyzer instrument plan and simulation self-test: "
                        << (failures == 0 ? "PASS" : "FAIL") << '\n';
    return failures == 0 ? 0 : 1;
}
