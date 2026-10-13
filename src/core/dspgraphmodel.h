#pragma once

#include <QJsonObject>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QSet>

enum class DspLinkKind { Control, Settings, Vfo, Iq, Spectrum, Audio, Documentation, Invalid };
constexpr int DspMaxBranches = 8;
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
    int firTaps = 32;
    double transitionHz = 0.0;
    bool pfb = false;
    bool enabled = false;
    bool external = false;
    bool iqConsumer = false;
    int iqSamples = 4096;
    QVector<DspBranchFftConfig> ffts;
    QString demodulatorId;
    QString audioOutputId;
    int modulation = 1;
    double audioBandwidthHz = 12500.0;
    double audioLowPassHz = 3000.0;
    double squelchDb = -160.0;
    QString audioFilters;
    bool audioEnabled = false;
    int audioDevice = -1;
    double volume = 0.5;
};

struct DspGraphPlan {
    QVector<DspBranchConfig> branches;
    QHash<QString, QString> spectrumRoutes;
    QHash<QString, QString> researchRoutes;
    QHash<QString, QString> audioRoutes; // Audio block -> DDC branch.
    int pfbChannels = 0;
    int pfbTaps = 16;
    // Bindings to the one existing main FFT, not independent FFT workers.
    QHash<QString, QString> mainFftSources; // FFT block -> Receiver/IQ source block.
    QHash<QString, QString> mainSpectrumRoutes; // Workspace view -> connected main FFT block.
    QHash<QString, QString> mainDisplayControllers; // Workspace view -> settings block, by wire or binding ID.
    QSet<QString> explicitMainSpectrumViews; // Never fall back to implicit frames after disconnecting.
    QStringList errors;
    // DDC execution only; main FFT bindings must not restart branch workers.
    QByteArray executionKey;
};

DspGraphPlan compileDspGraph(const QJsonObject &graph);
// Explicit one-shot application; ordinary legacy wires remain descriptive.
QStringList compileDspAudioFilterOrder(const QJsonObject &graph, QString *error);
