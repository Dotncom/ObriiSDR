#pragma once

#include <QString>

enum class ComputeProfile { Conservative = 0, Balanced = 1, Throughput = 2, Manual = 3 };
enum class ComputeWorkClass { MainFft, AuxiliaryFft, DspBranch, Research, Network };
enum class ComputeThreadRole { MainFft, AuxiliaryFft, DspLocal, DspAuxiliary, DspRemote, Research };

struct ComputeTopology {
    int logicalThreads = 1;
    int physicalCores = 1;
    int numaNodes = 1;
    int processorGroups = 1;
    QString description;
};

struct ComputeBudgetSettings {
    ComputeProfile profile = ComputeProfile::Balanced;
    int manualThreadLimit = 4;
    bool topologyAware = true;
};

struct ComputeBudget {
    int totalWorkerThreads = 1;
    int reservedRealtimeThreads = 1;
    int mainFftThreads = 1;
    int auxiliaryFftThreads = 1;
    int dspBranchThreads = 1;
    int researchThreads = 1;
};

ComputeTopology detectComputeTopology();
ComputeBudget calculateComputeBudget(const ComputeTopology &topology,
                                     const ComputeBudgetSettings &settings,
                                     bool simplifiedMode);

class ComputeBudgetManager final {
public:
    static ComputeBudgetManager &instance();
    ~ComputeBudgetManager();

    ComputeTopology topology() const;
    ComputeBudgetSettings settings() const;
    void setSettings(const ComputeBudgetSettings &settings);
    ComputeBudget budget(bool simplifiedMode = false) const;
    int recommendedThreads(ComputeWorkClass work,
                           bool simplifiedMode = false,
                           int concurrentJobs = 1) const;
    void applyCurrentThreadRole(ComputeThreadRole role, int sourceOrdinal = 0) const;

private:
    ComputeBudgetManager();
    struct Impl;
    Impl *impl;
};
