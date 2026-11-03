#pragma once
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <atomic>

struct FftPlanProfile {
    int threads = 0;
    QByteArray wisdom;
    double medianMs = 0.0;
};
class FftTransformCore;
// Caller holds fftwPlannerGlobalMutex; no benchmark or file access here.
bool restoreFftPlanProfile(const FftPlanProfile &profile, FftTransformCore &transform);
QString fftPlanMachineKey();
QString fftPlanCacheDirectory();
QString fftPlanProfilePath(int length, const QString &directory = {});
FftPlanProfile loadFftPlanProfile(int length, int maxThreads, const QString &directory = {});
bool removeFftPlanProfile(int length, const QString &directory = {});
bool publishFftPlanProfile(const QString &pendingPath, int length, const QString &directory = {});
inline std::atomic<unsigned> &fftPlanProfileGeneration() { static std::atomic<unsigned> value{1}; return value; }
int runFftTuningCommand(const QStringList &arguments);
