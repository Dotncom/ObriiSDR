#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct FpgaPipelineStage {
    QString type;
    double frequencyHz = 0.0;
    double bandwidthHz = 1000000.0;
    double thresholdDbfs = -60.0;
    int factor = 2;
    QJsonObject customLogic;
};

struct FpgaPipelineContract {
    QString type;
    QString inputFormat = QStringLiteral("signed IQ16");
    QString outputFormat = QStringLiteral("signed IQ16");
    QString clockDomain = QStringLiteral("clk");
    double inputRateHz = 0.0;
    double outputRateHz = 0.0;
    bool eventOutput = false;
};

struct FpgaPipelineAnalysis {
    double outputRateHz = 0.0;
    QVector<FpgaPipelineContract> contracts;
    QStringList errors;
    QStringList rtlBlockers;
    QStringList warnings;

    bool canGenerateRtl() const { return errors.isEmpty() && rtlBlockers.isEmpty(); }
};

class FpgaPipelineCompiler {
public:
    static FpgaPipelineAnalysis analyze(const QVector<FpgaPipelineStage> &stages,
                                        double inputRateHz, double clockRateHz);
    static QByteArray generateVerilog(const QVector<FpgaPipelineStage> &stages,
                                     double inputRateHz, double clockRateHz, QString *error);
    static bool writeVerilog(const QVector<FpgaPipelineStage> &stages,
                             double inputRateHz, double clockRateHz,
                             const QString &path, QString *error);
};
