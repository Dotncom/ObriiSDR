#ifndef GNSSSIGNALMONITOR_H
#define GNSSSIGNALMONITOR_H

#include "radiosettings.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

struct GnssSignalReport {
    bool valid = false;
    bool channelized = false;
    bool fromSnapshot = false;
    QString sampleFormat;
    quint64 frames = 0;
    quint64 samples = 0;
    double seconds = 0.0;
    double sampleRate = 0.0;
    double centerFrequency = 0.0;
    double listeningFrequency = 0.0;
    int sampleCount = 0;
    double rmsDbfs = -160.0;
    double peakDbfs = -160.0;
    double dcDbfs = -160.0;
    double dcToRmsDb = -160.0;
    double clippingPercent = 0.0;
    double crestDb = 0.0;
    double iqBalanceDb = 0.0;
    double meanI = 0.0;
    double meanQ = 0.0;
};

class GnssSignalMonitor {
public:
    void reset();
    GnssSignalReport analyzeFloatSnapshot(const std::vector<float> &samples,
                                          double sampleRate,
                                          const RadioSettings &settings,
                                          std::uint64_t sequence);
    GnssSignalReport analyzePackedIqFrame(const QByteArray &iqData,
                                          double sampleRate,
                                          int sampleCount,
                                          bool channelized,
                                          const RadioSettings &settings);
    GnssSignalReport lastReport() const { return last; }
    std::uint64_t lastSnapshotSequence() const { return lastSequence; }

private:
    template <typename Reader>
    GnssSignalReport analyze(int sampleCount,
                             double sampleRate,
                             bool channelized,
                             bool fromSnapshot,
                             const QString &sampleFormat,
                             const RadioSettings &settings,
                             Reader reader);

    GnssSignalReport last;
    std::uint64_t lastSequence = 0;
};

#endif // GNSSSIGNALMONITOR_H
