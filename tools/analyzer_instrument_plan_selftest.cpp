#include "analyzerinstrumentplan.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>

#include <cmath>

namespace {
int failures = 0;

void check(bool condition, const QString &message) {
    if (condition) return;
    QTextStream(stderr) << "FAIL: " << message << '\n';
    ++failures;
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
    const AnalyzerMeasurementSession restoredSession =
        AnalyzerMeasurementSession::fromJson(session.toJson());
    check(restoredSession.points.size() == 1 && restoredSession.points.first().valid,
          QStringLiteral("session point JSON round-trip"));
    check(restoredSession.eventLog.size() == 1 &&
              restoredSession.eventLog.first() == QStringLiteral("fixture"),
          QStringLiteral("session event-log JSON round-trip"));

    const QVector<double> grid = AnalyzerInstrumentPlanner::frequencyGrid(draft);
    check(grid.size() == draft.points, QStringLiteral("frequency grid size"));
    check(!grid.isEmpty() && std::abs(grid.first() - draft.startHz) < 1.0e-6,
          QStringLiteral("frequency grid start"));
    check(!grid.isEmpty() && std::abs(grid.last() - draft.stopHz) < 1.0e-6,
          QStringLiteral("frequency grid stop"));

    const double magnitude = AnalyzerInstrumentPlanner::reflectionMagnitudeFromReturnLoss(20.0);
    check(std::abs(magnitude - 0.1) < 1.0e-12, QStringLiteral("return-loss inverse"));
    check(std::abs(AnalyzerInstrumentPlanner::returnLossDb(magnitude) - 20.0) < 1.0e-12,
          QStringLiteral("return-loss calculation"));
    check(std::abs(AnalyzerInstrumentPlanner::swrFromReflectionMagnitude(magnitude) -
                   1.2222222222222223) < 1.0e-12,
          QStringLiteral("SWR calculation"));

    QTextStream(stdout) << "Analyzer instrument plan self-test: "
                        << (failures == 0 ? "PASS" : "FAIL") << '\n';
    return failures == 0 ? 0 : 1;
}
