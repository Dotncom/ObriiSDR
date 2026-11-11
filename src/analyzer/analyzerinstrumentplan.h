#ifndef ANALYZERINSTRUMENTPLAN_H
#define ANALYZERINSTRUMENTPLAN_H

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <complex>

enum class AnalyzerInstrumentMode {
    TrackingGenerator,
    ScalarNetworkAnalyzer,
    ReturnLossSwr,
    VectorNetworkAnalyzer
};

struct AnalyzerHardwareCapabilities {
    bool receiverAvailable = false;
    bool transmitterAvailable = false;
    bool fullDuplex = false;
    bool coherentReference = false;
    bool timestampedIq = false;
    bool externalGenerator = false;
    bool directionalCoupler = false;
    int receiverChannels = 0;
    int transmitterChannels = 0;
    double minimumFrequencyHz = 0.0;
    double maximumFrequencyHz = 0.0;
    double maximumSafeOutputDbm = -100.0;

    QJsonObject toJson() const;
    static AnalyzerHardwareCapabilities fromJson(const QJsonObject &object);
};

struct AnalyzerScpiProfile {
    bool enabled = false;
    QString resource;
    QString identityQuery = QStringLiteral("*IDN?");
    QString frequencyCommand = QStringLiteral("FREQ %1 HZ");
    QString powerCommand = QStringLiteral("POW %1 DBM");
    QString outputCommand = QStringLiteral("OUTP %1");
    QString triggerCommand = QStringLiteral("*TRG");
    QString readCommand;
    int timeoutMs = 2000;

    QJsonObject toJson() const;
    static AnalyzerScpiProfile fromJson(const QJsonObject &object);
};

struct AnalyzerCalibrationState {
    QString calibrationId;
    QString notes;
    QDateTime capturedUtc;
    double referenceImpedanceOhm = 50.0;
    bool scalarNormalization = false;
    bool openMeasured = false;
    bool shortMeasured = false;
    bool loadMeasured = false;
    bool thruMeasured = false;
    bool phaseReferenceMeasured = false;

    bool oslComplete() const { return openMeasured && shortMeasured && loadMeasured; }
    bool vectorComplete() const {
        return oslComplete() && thruMeasured && phaseReferenceMeasured;
    }
    QJsonObject toJson() const;
    static AnalyzerCalibrationState fromJson(const QJsonObject &object);
};

struct AnalyzerInstrumentPlan {
    AnalyzerInstrumentMode mode = AnalyzerInstrumentMode::TrackingGenerator;
    QString name = QStringLiteral("Untitled measurement");
    QString transmitDeviceId;
    QString receiveDeviceId;
    bool useExternalGenerator = false;
    double startHz = 1.0e6;
    double stopHz = 30.0e6;
    int points = 401;
    double sourceLevelDbm = -30.0;
    double dwellMs = 10.0;
    double settleMs = 5.0;
    int averages = 1;
    bool bidirectionalSweep = false;
    AnalyzerHardwareCapabilities capabilities;
    AnalyzerCalibrationState calibration;
    AnalyzerScpiProfile scpi;
    QString operatorName;
    QString deviceUnderTest;
    QString comment;

    QJsonObject toJson() const;
    static AnalyzerInstrumentPlan fromJson(const QJsonObject &object);
};

struct AnalyzerPlanValidation {
    bool executable = false;
    bool calibratedResult = false;
    QStringList errors;
    QStringList warnings;
    QStringList requirements;

    QString summary() const;
};

struct AnalyzerMeasurementPoint {
    double frequencyHz = 0.0;
    double stimulusDbm = 0.0;
    double referenceDb = 0.0;
    double forwardDb = 0.0;
    double reflectedDb = 0.0;
    double phaseDegrees = 0.0;
    double s11Magnitude = 0.0;
    double s21Magnitude = 0.0;
    bool valid = false;

    QJsonObject toJson() const;
    static AnalyzerMeasurementPoint fromJson(const QJsonObject &object);
};

struct AnalyzerMeasurementSession {
    QString sessionId;
    QString applicationVersion;
    QDateTime startedUtc;
    QDateTime completedUtc;
    AnalyzerInstrumentPlan plan;
    QVector<AnalyzerMeasurementPoint> points;
    QStringList eventLog;
    QString backendId;
    QString outcome;

    QJsonObject toJson() const;
    static AnalyzerMeasurementSession fromJson(const QJsonObject &object);
};

class AnalyzerInstrumentPlanner final {
public:
    static QString modeId(AnalyzerInstrumentMode mode);
    static QString modeName(AnalyzerInstrumentMode mode);
    static AnalyzerInstrumentMode modeFromId(const QString &id);
    static AnalyzerPlanValidation validate(const AnalyzerInstrumentPlan &plan);
    static QVector<double> frequencyGrid(const AnalyzerInstrumentPlan &plan);

    static double returnLossDb(double reflectionMagnitude);
    static double swrFromReflectionMagnitude(double reflectionMagnitude);
    static double reflectionMagnitudeFromReturnLoss(double returnLossDb);
    static bool correctOnePort(const std::array<std::complex<double>, 3> &standards,
                               const std::array<std::complex<double>, 3> &measuredStandards,
                               std::complex<double> measuredDevice, std::complex<double> &corrected);
};

#endif
