#pragma once

#include "coherentiqaligner.h"
#include "dspiqsourceregistry.h"

#include <QString>
#include <QStringList>

#include <cstddef>
#include <vector>

enum class CoherentIqCombineMode {
    ReferenceOnly = 0,
    BestSnr,
    EqualAverage,
    MaximumRatio
};
struct CoherentIqCombinedResult {
    DspSourceIdentity reference;
    std::vector<float> iq;
    std::vector<DspSourceIdentity> sources;
    std::vector<double> weights;
    std::uint64_t referenceFirstComplexSample = 0;
    double sampleRateHz = 0.0;
    double centerHz = 0.0;
    bool valid = false;
    QString error;
};

class CoherentIqSession final {
public:
    // Creates a unique child directory and commits manifest.json last. A
    // manifest therefore identifies a complete bounded multi-source snapshot.
    static bool write(const QString &parentDirectory,
                      const std::vector<DspIqSourceSnapshot> &sources,
                      const std::vector<CoherentIqCalibration> &calibrations,
                      QString &manifestPath,
                      QString &error);

    static bool read(const QString &manifestPath,
                     std::vector<DspIqSourceSnapshot> &sources,
                     std::vector<CoherentIqCalibration> &calibrations,
                     QString &error);

    // Combines same-center, same-rate recordings in reference sample space.
    // Different RF spans belong to the spectrum mosaic/crossfade path instead.
    static CoherentIqCombinedResult combine(
        const std::vector<DspIqSourceSnapshot> &sources,
        const std::vector<CoherentIqCalibration> &calibrations,
        const DspSourceIdentity &reference,
        CoherentIqCombineMode mode,
        std::size_t maximumComplexSamples = 0);

    static double estimateNoisePower(const std::vector<float> &iq);
};

// A recording indexes sequential snapshot files containing separately aligned
// receiver channels. An interrupted recording can still expose its valid prefix.
class CoherentIqRecording final {
public:
    bool create(const QString &parentDirectory, QString &error);
    bool append(const std::vector<DspIqSourceSnapshot> &channels, QString &error);
    QString finish(const QString &interruption, QString &error) const;

    bool open(const QString &manifestPath, QString &error);
    // An empty block on success marks the end of the recording.
    bool readNext(std::vector<DspIqSourceSnapshot> &channels, QString &error);
    QString interruptionReason() const { return interruption; }

private:
    bool contiguous(const std::vector<DspIqSourceSnapshot> &channels, QString &error) const;
    void advance(const std::vector<DspIqSourceSnapshot> &channels);

    QString directory;
    QString interruption;
    QStringList segments;
    int position = 0;
    std::vector<DspSourceIdentity> sources;
    QString clock;
    double rate = 0;
    double center = 0;
    std::uint64_t epoch = 0;
    std::uint64_t next = 0;
};
