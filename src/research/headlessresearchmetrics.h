#pragma once

#include <QString>
#include <QtGlobal>

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

// Pure, synchronous computations extracted from the research widgets. No GUI,
// clocks, device state, I/O, or implicit sampling. Callers supply all observations.
namespace HeadlessResearchMetrics {
constexpr int MaximumSamples = 65536;
constexpr int MaximumDensityColumns = 1024;
constexpr int DensityRows = 256;
constexpr int MaximumHistoryFrames = 4096;
constexpr int MaximumHistoryCells = 1048576;
constexpr int MaximumConstellationPoints = 3000;
constexpr int EyePoints = 192;
constexpr int MaximumEyeTraces = 128;

struct DensityConfig {
    int columns = 512;
    int historyFrames = 256;
    double retainPercent = 98;
    double minimumDb = -140;
    double maximumDb = -20;
    bool linearPower = false;
};

class DensityHistogram {
public:
    bool configure(const DensityConfig &config, QString *error = nullptr);
    bool append(const std::vector<float> &levels, double firstHz, double lastHz,
                bool *rangeReset = nullptr, QString *error = nullptr);
    const DensityConfig &configuration() const { return config_; }
    const std::vector<float> &histogram() const { return histogram_; } // row-major
    int retainedFrames() const { return static_cast<int>(contributions_.size()); }
    double firstHz() const { return firstHz_; }
    double lastHz() const { return lastHz_; }
    bool hasRange() const { return haveRange_; }
private:
    struct Contribution {
        std::vector<std::uint16_t> rows;
        std::vector<float> weights;
    };
    DensityConfig config_;
    bool configured_ = false;
    bool haveRange_ = false;
    double firstHz_ = 0, lastHz_ = 0;
    std::vector<float> histogram_;
    std::deque<Contribution> contributions_;
};

struct PulseSample { qint64 timeMs = 0; float levelDb = 0; };
struct PulseConfig {
    double thresholdDb = -80;
    double hysteresisDb = 3;
    bool relativeToNoise = false; // ordered[n / 5] + thresholdDb
};
struct PulseEvent {
    qint64 startMs = 0, endMs = 0;
    double riseMs = 0, fallMs = 0;
    float peakDb = 0;
    bool leftCensored = false;
};
struct PulseResult {
    bool valid = false;
    QString error;
    double thresholdDb = 0, releaseDb = 0, observedMs = 0;
    double averageWidthMs = 0, averagePriMs = 0, repetitionHz = 0;
    double dutyPercent = 0, riseMs = 0, fallMs = 0;
    bool trailingActive = false;
    std::vector<PulseEvent> events;
};
PulseResult analyzePulses(const std::vector<PulseSample> &samples, const PulseConfig &config);

struct IqPoint { double samplePosition = 0; float i = 0, q = 0; };
struct IqViews {
    bool valid = false;
    QString error;
    double dcI = 0, dcQ = 0, samplesPerSymbol = 0;
    int eyeTraces = 0;
    bool constellationLimited = false, eyeLimited = false;
    std::vector<IqPoint> constellation;
    std::vector<IqPoint> eye; // trace-major, 192 points per complete trace
};
IqViews buildIqViews(const std::vector<float> &interleavedIq, double sampleRateHz,
                     double symbolRateHz, double phaseFraction, int requestedEyeTraces);

struct IqPhaseResult {
    bool valid = false, frequencyValid = false, imageValid = false, imageSaturated = false;
    QString error;
    double frequencyHz = 0, frequencyStdHz = 0, imageEstimateDb = 0;
    std::array<float, 129> autocorrelation{};
    bool autocorrelationValid = false;
};
IqPhaseResult analyzeIqPhase(const std::vector<float> &interleavedIq, double sampleRateHz);

struct CombPeak {
    int bin = 0, family = 0, harmonic = 0;
    double frequencyHz = 0;
    float levelDb = 0, prominenceDb = 0, referenceDeltaDb = 0;
};
struct CombFamily { double stepHz = 0, anchorHz = 0; int members = 0; };
struct CombResult {
    bool valid = false, referenceValid = false;
    QString error;
    float noiseDb = 0;
    std::vector<CombPeak> peaks; // at most 96, ascending frequency
    std::vector<CombFamily> families; // at most 3
};
CombResult analyzeComb(const std::vector<float> &frequenciesHz, const std::vector<float> &levelsDb,
                       double prominenceDb, double minimumSpacingHz, double maximumSpacingHz,
                       const std::vector<float> &referenceLevelsDb = {});
}
