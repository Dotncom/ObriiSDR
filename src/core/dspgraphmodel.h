#pragma once

#include <QJsonObject>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

enum class DspLinkKind { Control, Settings, Vfo, Iq, Spectrum, Documentation, Invalid };
DspLinkKind dspLinkKind(const QString &sourceType, const QString &targetType);
QString dspLinkDescription(DspLinkKind kind, bool ukrainian);
bool dspIsBranchType(const QString &type);

struct DspBranchFftConfig {
    QString id;
    int length = 2048;
    int window = 1;
    int refreshMs = 100;
};

struct DspBranchConfig {
    QString id;
    double frequencyHz = 0.0;
    double bandwidthHz = 12500.0;
    double outputRateHz = 192000.0;
    int decimation = 0;
    bool enabled = false;
    bool iqConsumer = false;
    int iqSamples = 4096;
    QVector<DspBranchFftConfig> ffts;
};

struct DspGraphPlan {
    QVector<DspBranchConfig> branches;
    QHash<QString, QString> spectrumRoutes;
    QHash<QString, QString> researchRoutes;
    QStringList errors;
    QByteArray executionKey;
};

DspGraphPlan compileDspGraph(const QJsonObject &graph);
