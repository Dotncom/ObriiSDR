#include "computebudgetmanager.h"

#include <QDir>
#include <QByteArray>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QTextStream>

#include <algorithm>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
int clampThreads(int value, int maximum) {
    return std::clamp(value, 1, std::max(1, maximum));
}

}

ComputeTopology detectComputeTopology() {
    ComputeTopology topology;
    topology.logicalThreads = std::max(1U, std::thread::hardware_concurrency());
    topology.physicalCores = topology.logicalThreads;
#ifdef _WIN32
    topology.processorGroups = std::max<int>(1, GetActiveProcessorGroupCount());
    topology.logicalThreads = 0;
    for (WORD group = 0; group < GetActiveProcessorGroupCount(); ++group)
        topology.logicalThreads += GetActiveProcessorCount(group);
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length) {
        QByteArray storage(int(length), Qt::Uninitialized);
        auto *cursor = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, cursor, &length)) {
            int cores = 0;
            DWORD offset = 0;
            while (offset < length) {
                auto *entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data() + offset);
                if (entry->Relationship == RelationProcessorCore) ++cores;
                if (!entry->Size) break;
                offset += entry->Size;
            }
            topology.physicalCores = std::max(1, cores);
        }
    }
    ULONG highestNode = 0;
    if (GetNumaHighestNodeNumber(&highestNode)) topology.numaNodes = int(highestNode) + 1;
#elif defined(__linux__)
    const QDir nodes(QStringLiteral("/sys/devices/system/node"));
    const auto nodeEntries = nodes.entryList({QStringLiteral("node[0-9]*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    topology.numaNodes = std::max(1, nodeEntries.size());
    const QDir cpus(QStringLiteral("/sys/devices/system/cpu"));
    const auto cpuEntries = cpus.entryList({QStringLiteral("cpu[0-9]*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    if (!cpuEntries.isEmpty()) topology.logicalThreads = cpuEntries.size();
#endif
    topology.logicalThreads = std::max(1, topology.logicalThreads);
    topology.physicalCores = std::clamp(topology.physicalCores, 1, topology.logicalThreads);
    topology.description = QStringLiteral("%1 logical / %2 cores / %3 NUMA / %4 groups")
                               .arg(topology.logicalThreads)
                               .arg(topology.physicalCores)
                               .arg(topology.numaNodes)
                               .arg(topology.processorGroups);
    return topology;
}

ComputeBudget calculateComputeBudget(const ComputeTopology &topology,
                                     const ComputeBudgetSettings &settings,
                                     bool simplifiedMode) {
    const int logical = std::max(1, topology.logicalThreads);
    ComputeBudget result;
    if (simplifiedMode || settings.profile == ComputeProfile::Conservative) {
        result.reservedRealtimeThreads = std::min(logical, logical <= 2 ? 1 : 2);
        result.totalWorkerThreads = std::max(1, logical - result.reservedRealtimeThreads);
        result.mainFftThreads = std::min(2, result.totalWorkerThreads);
        result.auxiliaryFftThreads = 1;
        result.dspBranchThreads = 1;
        result.researchThreads = 1;
        return result;
    }
    result.reservedRealtimeThreads = logical <= 4 ? 1 : std::max(2, logical / 6);
    int available = std::max(1, logical - result.reservedRealtimeThreads);
    if (settings.profile == ComputeProfile::Manual)
        available = std::min(available, std::max(1, settings.manualThreadLimit));
    result.totalWorkerThreads = available;
    if (settings.profile == ComputeProfile::Throughput) {
        result.mainFftThreads = clampThreads((available * 2 + 2) / 3, 16);
        result.auxiliaryFftThreads = clampThreads(std::max(1, available / 3), 8);
        result.dspBranchThreads = clampThreads(std::max(1, available / 4), 4);
        result.researchThreads = clampThreads(std::max(1, available / 3), 8);
    } else {
        result.mainFftThreads = clampThreads(std::min(4, std::max(1, available / 2)), available);
        result.auxiliaryFftThreads = clampThreads(std::min(4, std::max(1, available - result.mainFftThreads)), available);
        result.dspBranchThreads = clampThreads(std::min(2, std::max(1, available / 4)), available);
        result.researchThreads = clampThreads(std::min(4, std::max(1, available / 3)), available);
    }
    return result;
}

struct ComputeBudgetManager::Impl {
    mutable QMutex mutex;
    ComputeTopology topology = detectComputeTopology();
    ComputeBudgetSettings settings;
};

ComputeBudgetManager::ComputeBudgetManager() : impl(new Impl) {
    QSettings saved;
    impl->settings.profile = static_cast<ComputeProfile>(std::clamp(
        saved.value(QStringLiteral("computeBudget/profile"), int(ComputeProfile::Balanced)).toInt(), 0, 3));
    impl->settings.manualThreadLimit = std::clamp(
        saved.value(QStringLiteral("computeBudget/manualThreadLimit"), 4).toInt(), 1, 256);
    impl->settings.topologyAware = saved.value(
        QStringLiteral("computeBudget/topologyAware"), impl->topology.numaNodes > 1 || impl->topology.processorGroups > 1).toBool();
}

ComputeBudgetManager::~ComputeBudgetManager() { delete impl; }

ComputeBudgetManager &ComputeBudgetManager::instance() {
    static ComputeBudgetManager manager;
    return manager;
}

ComputeTopology ComputeBudgetManager::topology() const {
    QMutexLocker lock(&impl->mutex);
    return impl->topology;
}

ComputeBudgetSettings ComputeBudgetManager::settings() const {
    QMutexLocker lock(&impl->mutex);
    return impl->settings;
}

void ComputeBudgetManager::setSettings(const ComputeBudgetSettings &settings) {
    QMutexLocker lock(&impl->mutex);
    impl->settings.profile = static_cast<ComputeProfile>(std::clamp(int(settings.profile), 0, 3));
    impl->settings.manualThreadLimit = std::clamp(settings.manualThreadLimit, 1, 256);
    impl->settings.topologyAware = settings.topologyAware;
    QSettings saved;
    saved.setValue(QStringLiteral("computeBudget/profile"), int(impl->settings.profile));
    saved.setValue(QStringLiteral("computeBudget/manualThreadLimit"), impl->settings.manualThreadLimit);
    saved.setValue(QStringLiteral("computeBudget/topologyAware"), impl->settings.topologyAware);
}

ComputeBudget ComputeBudgetManager::budget(bool simplifiedMode) const {
    QMutexLocker lock(&impl->mutex);
    return calculateComputeBudget(impl->topology, impl->settings, simplifiedMode);
}

int ComputeBudgetManager::recommendedThreads(ComputeWorkClass work,
                                             bool simplifiedMode,
                                             int concurrentJobs) const {
    const ComputeBudget value = budget(simplifiedMode);
    int threads = 1;
    switch (work) {
    case ComputeWorkClass::MainFft: threads = value.mainFftThreads; break;
    case ComputeWorkClass::AuxiliaryFft: threads = value.auxiliaryFftThreads; break;
    case ComputeWorkClass::DspBranch: threads = value.dspBranchThreads; break;
    case ComputeWorkClass::Research: threads = value.researchThreads; break;
    case ComputeWorkClass::Network: threads = 1; break;
    }
    return std::max(1, threads / std::max(1, concurrentJobs));
}

void ComputeBudgetManager::applyCurrentThreadRole(ComputeThreadRole role, int sourceOrdinal) const {
    QMutexLocker lock(&impl->mutex);
    if (!impl->settings.topologyAware ||
        (impl->topology.numaNodes <= 1 && impl->topology.processorGroups <= 1))
        return;
    const int ordinal = std::max(0, sourceOrdinal) + int(role);
#ifdef _WIN32
    const WORD groups = GetActiveProcessorGroupCount();
    if (!groups) return;
    PROCESSOR_NUMBER target{};
    target.Group = WORD(ordinal % groups);
    const DWORD count = GetActiveProcessorCount(target.Group);
    if (!count) return;
    target.Number = BYTE((ordinal / groups) % count);
    SetThreadIdealProcessorEx(GetCurrentThread(), &target, nullptr);
#elif defined(__linux__)
    const int node = ordinal % std::max(1, impl->topology.numaNodes);
    QFile file(QStringLiteral("/sys/devices/system/node/node%1/cpulist").arg(node));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const QString text = QString::fromLatin1(file.readAll()).trimmed();
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList range = part.split(QLatin1Char('-'));
        bool ok = false;
        const int first = range.value(0).toInt(&ok);
        if (!ok) continue;
        const int last = range.size() > 1 ? range.value(1).toInt(&ok) : first;
        if (!ok) continue;
        for (int cpu = first; cpu <= last && cpu < CPU_SETSIZE; ++cpu) CPU_SET(cpu, &set);
    }
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    Q_UNUSED(role);
    Q_UNUSED(sourceOrdinal);
#endif
}
