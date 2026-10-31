#pragma once

#include "dspsourceidentity.h"

#include <QSet>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <vector>

struct DspIqSourceSnapshot {
    DspSourceIdentity source;
    std::vector<float> iq;
    std::uint64_t epoch = 0;
    std::uint64_t endFloatCount = 0;
    double sampleRateHz = 0.0;
    double centerHz = 0.0;
    bool gap = false;
    std::uint64_t startComplexSample = 0;
    std::uint64_t endComplexSample = 0;
    qint64 hostTimestampNs = 0;
    QString clockId;
    bool sampleClockValid = false;
    bool hardwareTimestamp = false;
    bool clockLocked = false;
    int nativeAdcBits = 0;
    QString nativeSampleFormat;
    double normalizedFullScale = 1.0;
};

struct DspIqTimingMetadata {
    std::uint64_t epoch = 0;
    std::uint64_t firstComplexSample = 0;
    qint64 hostTimestampNs = 0;
    std::uint64_t hardwareTimestampTicks = 0;
    QString clockId;
    bool sampleClockValid = false;
    bool hardwareTimestamp = false;
    bool clockLocked = false;
    int nativeAdcBits = 0;
    QString nativeSampleFormat;
    double normalizedFullScale = 1.0;
};

struct DspIqSourceRegistryStats {
    int subscribedSources = 0;
    int bufferedSources = 0;
    std::uint64_t publishedBlocks = 0;
    std::uint64_t ignoredBlocks = 0;
    std::uint64_t droppedFloats = 0;
};

// Bounded opt-in source registry. The main receiver continues to use IqBuffer
// for its proven hot path. Any source is copied here only while a DSP/coherent
// consumer explicitly subscribes to its receiver/channel identity.
class DspIqSourceRegistry final {
public:
    static DspIqSourceRegistry &instance();

    void setSubscriptions(const void *owner, const QSet<QString> &sourceKeys);
    void removeSubscriptions(const void *owner);
    bool publish(const DspSourceIdentity &source,
                 const float *samples,
                 std::size_t floatCount,
                 double sampleRateHz,
                 double centerHz,
                 bool discontinuity = false,
                 const DspIqTimingMetadata *timing = nullptr);
    bool snapshotSince(const DspSourceIdentity &source,
                       std::uint64_t previousEpoch,
                       std::uint64_t previousEndFloatCount,
                       std::size_t maximumFloatCount,
                       DspIqSourceSnapshot &snapshot) const;
    DspIqSourceRegistryStats stats() const;
    void clear(const DspSourceIdentity &source);
    void clearAll();

private:
    DspIqSourceRegistry() = default;
    struct Impl;
    static Impl &impl();
};
