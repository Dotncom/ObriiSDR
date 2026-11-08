#ifndef INSTRUMENTSWEEPENGINE_H
#define INSTRUMENTSWEEPENGINE_H

#include <QString>
#include <QVector>

#include <vector>

struct InstrumentSweepSettings {
    double startHz = 0.0;
    double stopHz = 0.0;
    double captureBandwidthHz = 0.0;
    double overlapFraction = 0.15;
    double edgeDiscardFraction = 0.08;
    double outputBinWidthHz = 0.0;
    int maximumSegments = 10000;
    int maximumOutputBins = 4194304;
};

struct InstrumentSweepSegment {
    int index = 0;
    double centerHz = 0.0;
    double usableMinimumHz = 0.0;
    double usableMaximumHz = 0.0;
};

struct InstrumentSweepPlan {
    InstrumentSweepSettings settings;
    QVector<InstrumentSweepSegment> segments;
    QString error;

    bool isValid() const { return error.isEmpty() && !segments.isEmpty(); }
};

struct InstrumentSweepResult {
    std::vector<float> frequenciesHz;
    std::vector<float> levelsDb;
    std::vector<quint32> contributionCounts;
    int missingBins = 0;
};

class InstrumentSweepEngine final {
public:
    static InstrumentSweepPlan createPlan(const InstrumentSweepSettings &settings);

    bool begin(const InstrumentSweepPlan &plan);
    bool addFrame(int segmentIndex,
                  double frameMinimumHz,
                  double frameMaximumHz,
                  const std::vector<float> &levelsDb);
    bool updateResultForSegment(int segmentIndex,
                                std::vector<float> &frequenciesHz,
                                std::vector<float> &levelsDb,
                                float initialLevelDb) const;
    InstrumentSweepResult finish() const;
    void clear();

    const InstrumentSweepPlan &plan() const { return currentPlan; }

private:
    InstrumentSweepPlan currentPlan;
    std::vector<double> weightedLinearPower;
    std::vector<double> weightSums;
    std::vector<quint32> counts;
    double outputBinWidthHz = 0.0;
};

#endif
