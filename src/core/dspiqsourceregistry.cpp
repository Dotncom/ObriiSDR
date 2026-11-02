#include "dspiqsourceregistry.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>

namespace {
constexpr std::size_t MaximumSourceFloats = 2U * 1024U * 1024U;
constexpr int MaximumBufferedSources = 8;

struct SourceBuffer {
    std::deque<float> history;
    std::uint64_t epoch = 1;
    std::uint64_t totalFloats = 0;
    double sampleRateHz = 0.0;
    double centerHz = 0.0;
    bool pendingGap = true;
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
}

struct DspIqSourceRegistry::Impl {
    mutable std::mutex mutex;
    QHash<quintptr, QSet<QString>> subscriptions;
    QHash<QString, int> subscriberCounts;
    QHash<QString, SourceBuffer> sources;
    DspIqSourceRegistryStats statistics;

    void rebuildSubscribers() {
        subscriberCounts.clear();
        for (auto it = subscriptions.cbegin(); it != subscriptions.cend(); ++it)
            for (const QString &key : it.value()) ++subscriberCounts[key];
        statistics.subscribedSources = subscriberCounts.size();
        for (auto it = sources.begin(); it != sources.end();) {
            if (!subscriberCounts.contains(it.key())) it = sources.erase(it);
            else ++it;
        }
        statistics.bufferedSources = sources.size();
    }
};

DspIqSourceRegistry::Impl &DspIqSourceRegistry::impl() {
    static Impl value;
    return value;
}

DspIqSourceRegistry &DspIqSourceRegistry::instance() {
    static DspIqSourceRegistry value;
    return value;
}

void DspIqSourceRegistry::setSubscriptions(const void *owner,
                                           const QSet<QString> &sourceKeys) {
    Impl &state = impl();
    std::lock_guard<std::mutex> lock(state.mutex);
    const quintptr key = reinterpret_cast<quintptr>(owner);
    if (sourceKeys.isEmpty()) state.subscriptions.remove(key);
    else state.subscriptions.insert(key, sourceKeys);
    state.rebuildSubscribers();
}

void DspIqSourceRegistry::removeSubscriptions(const void *owner) {
    setSubscriptions(owner, {});
}

bool DspIqSourceRegistry::publish(const DspSourceIdentity &source,
                                  const float *samples,
                                  std::size_t floatCount,
                                  double sampleRateHz,
                                  double centerHz,
                                  bool discontinuity,
                                  const DspIqTimingMetadata *timing) {
    Impl &state = impl();
    if (!source.isValid() || !samples || floatCount < 2 ||
        floatCount % 2U || !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0 ||
        !std::isfinite(centerHz))
        return false;
    std::lock_guard<std::mutex> lock(state.mutex);
    const QString key = source.key();
    if (state.subscriberCounts.value(key) <= 0) {
        ++state.statistics.ignoredBlocks;
        return false;
    }
    if (!state.sources.contains(key) && state.sources.size() >= MaximumBufferedSources) {
        ++state.statistics.ignoredBlocks;
        return false;
    }
    SourceBuffer &buffer = state.sources[key];
    const bool timingEpochChanged = timing && timing->epoch && buffer.epoch != timing->epoch;
    const bool timingDiscontinuity = timing && timing->sampleClockValid && buffer.sampleClockValid &&
        timing->firstComplexSample != buffer.endComplexSample;
    const bool formatChanged = buffer.sampleRateHz > 0.0 &&
        (std::abs(buffer.sampleRateHz - sampleRateHz) > 0.5 ||
         std::abs(buffer.centerHz - centerHz) > 0.5);
    if (formatChanged || discontinuity || timingEpochChanged || timingDiscontinuity) {
        buffer.history.clear();
        buffer.totalFloats = 0;
        buffer.epoch = timing && timing->epoch ? timing->epoch : buffer.epoch + 1;
        buffer.endComplexSample = timing && timing->sampleClockValid
                                      ? timing->firstComplexSample : 0;
        buffer.pendingGap = true;
    }
    buffer.sampleRateHz = sampleRateHz;
    buffer.centerHz = centerHz;
    const std::size_t keepStart = floatCount > MaximumSourceFloats
                                      ? floatCount - MaximumSourceFloats
                                      : 0;
    if (keepStart) {
        state.statistics.droppedFloats += keepStart;
        buffer.pendingGap = true;
    }
    for (std::size_t index = keepStart; index < floatCount; ++index)
        buffer.history.push_back(samples[index]);
    buffer.totalFloats += floatCount - keepStart;
    if (timing) {
        buffer.hostTimestampNs = timing->hostTimestampNs;
        buffer.clockId = timing->clockId;
        buffer.sampleClockValid = timing->sampleClockValid;
        buffer.hardwareTimestamp = timing->hardwareTimestamp;
        buffer.clockLocked = timing->clockLocked;
        buffer.nativeAdcBits = std::max(0, timing->nativeAdcBits);
        buffer.nativeSampleFormat = timing->nativeSampleFormat;
        buffer.normalizedFullScale = std::isfinite(timing->normalizedFullScale) &&
                                     timing->normalizedFullScale > 0.0
                                         ? timing->normalizedFullScale : 1.0;
        if (timing->sampleClockValid)
            buffer.endComplexSample = timing->firstComplexSample + floatCount / 2U;
        else
            buffer.endComplexSample += floatCount / 2U;
    } else {
        buffer.endComplexSample += floatCount / 2U;
    }
    while (buffer.history.size() > MaximumSourceFloats) {
        buffer.history.pop_front();
        ++state.statistics.droppedFloats;
    }
    ++state.statistics.publishedBlocks;
    state.statistics.bufferedSources = state.sources.size();
    return true;
}

bool DspIqSourceRegistry::snapshotSince(const DspSourceIdentity &source,
                                        std::uint64_t previousEpoch,
                                        std::uint64_t previousEndFloatCount,
                                        std::size_t maximumFloatCount,
                                        DspIqSourceSnapshot &snapshot) const {
    snapshot = {};
    maximumFloatCount &= ~std::size_t(1);
    if (!source.isValid() || maximumFloatCount < 2) return false;
    Impl &state = impl();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.sources.find(source.key());
    if (it == state.sources.end() || it->history.size() < 2) return false;
    SourceBuffer &buffer = it.value();
    const std::uint64_t earliest = buffer.totalFloats - buffer.history.size();
    const bool reset = previousEpoch != buffer.epoch || previousEndFloatCount < earliest ||
                       previousEndFloatCount > buffer.totalFloats;
    std::uint64_t requested = reset ? buffer.history.size()
                                    : buffer.totalFloats - previousEndFloatCount;
    if (requested < 2) return false;
    const bool truncated = requested > maximumFloatCount;
    requested = std::min<std::uint64_t>(requested, maximumFloatCount);
    requested &= ~std::uint64_t(1);
    snapshot.source = source;
    snapshot.epoch = buffer.epoch;
    snapshot.endFloatCount = buffer.totalFloats;
    snapshot.sampleRateHz = buffer.sampleRateHz;
    snapshot.centerHz = buffer.centerHz;
    snapshot.gap = reset || truncated || buffer.pendingGap;
    snapshot.endComplexSample = buffer.endComplexSample;
    snapshot.startComplexSample = snapshot.endComplexSample - requested / 2U;
    snapshot.hostTimestampNs = buffer.hostTimestampNs;
    snapshot.clockId = buffer.clockId;
    snapshot.sampleClockValid = buffer.sampleClockValid;
    snapshot.hardwareTimestamp = buffer.hardwareTimestamp;
    snapshot.clockLocked = buffer.clockLocked;
    snapshot.nativeAdcBits = buffer.nativeAdcBits;
    snapshot.nativeSampleFormat = buffer.nativeSampleFormat;
    snapshot.normalizedFullScale = buffer.normalizedFullScale;
    buffer.pendingGap = false;
    snapshot.iq.resize(static_cast<std::size_t>(requested));
    const auto begin = buffer.history.end() - static_cast<std::ptrdiff_t>(requested);
    std::copy(begin, buffer.history.end(), snapshot.iq.begin());
    return true;
}

DspIqSourceRegistryStats DspIqSourceRegistry::stats() const {
    Impl &state = impl();
    std::lock_guard<std::mutex> lock(state.mutex);
    DspIqSourceRegistryStats result = state.statistics;
    result.bufferedSources = state.sources.size();
    result.subscribedSources = state.subscriberCounts.size();
    return result;
}

void DspIqSourceRegistry::clear(const DspSourceIdentity &source) {
    Impl &state = impl();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.sources.remove(source.key());
    state.statistics.bufferedSources = state.sources.size();
}

void DspIqSourceRegistry::clearAll() {
    Impl &state = impl();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.sources.clear();
    state.statistics.bufferedSources = 0;
}
