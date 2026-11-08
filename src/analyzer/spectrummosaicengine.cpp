#include "spectrummosaicengine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double kPi = 3.14159265358979323846;

double frameBinWidth(const AnalyzerSpectrumFrame &frame) {
    if (frame.levelsDb.size() < 2 || !(frame.maximumHz > frame.minimumHz)) return 0.0;
    return (frame.maximumHz - frame.minimumHz) /
           static_cast<double>(frame.levelsDb.size() - 1);
}

bool usableFrame(const AnalyzerSpectrumFrame &frame) {
    return frame.levelsDb.size() >= 2 && frame.maximumHz > frame.minimumHz &&
           std::isfinite(frame.minimumHz) && std::isfinite(frame.maximumHz) &&
           frameBinWidth(frame) > 0.0;
}

double interpolatePower(const AnalyzerSpectrumFrame &frame, double frequencyHz) {
    const double binWidth = frameBinWidth(frame);
    if (!(binWidth > 0.0) || frequencyHz < frame.minimumHz || frequencyHz > frame.maximumHz)
        return std::numeric_limits<double>::quiet_NaN();
    const double position = (frequencyHz - frame.minimumHz) / binWidth;
    const std::size_t first = std::min<std::size_t>(
        frame.levelsDb.size() - 1, static_cast<std::size_t>(std::floor(position)));
    const std::size_t second = std::min(frame.levelsDb.size() - 1, first + 1);
    const double a = frame.levelsDb[first];
    const double b = frame.levelsDb[second];
    if (!std::isfinite(a) || !std::isfinite(b)) return std::numeric_limits<double>::quiet_NaN();
    const double fraction = std::clamp(position - static_cast<double>(first), 0.0, 1.0);
    const double firstPower = std::pow(10.0, a / 10.0);
    const double secondPower = std::pow(10.0, b / 10.0);
    return firstPower + (secondPower - firstPower) * fraction;
}
}

void SpectrumMosaicEngine::setMaximumOutputBins(int bins) {
    maximumOutputBins = std::clamp(bins, 512, 262144);
}

void SpectrumMosaicEngine::setMaximumFrameAgeMs(qint64 ageMs) {
    maximumFrameAgeMs = std::clamp<qint64>(ageMs, 20, 10000);
}

SpectrumMosaicEngine::SourceState *SpectrumMosaicEngine::findOrCreate(int sourceId) {
    for (SourceState &source : sources) {
        if (source.sourceId == sourceId) return &source;
    }
    SourceState source;
    source.sourceId = sourceId;
    sources.append(source);
    return &sources.last();
}

const SpectrumMosaicEngine::SourceState *SpectrumMosaicEngine::find(int sourceId) const {
    for (const SourceState &source : sources) {
        if (source.sourceId == sourceId) return &source;
    }
    return nullptr;
}

void SpectrumMosaicEngine::setSourceSettings(
    int sourceId, const SpectrumMosaicSourceSettings &settings) {
    SourceState *source = findOrCreate(sourceId);
    source->settings = settings;
    source->settings.edgeDiscardFraction =
        std::clamp(source->settings.edgeDiscardFraction, 0.0, 0.45);
    source->settings.levelOffsetDb =
        std::clamp(source->settings.levelOffsetDb, -120.0, 120.0);
}

void SpectrumMosaicEngine::updateFrame(int sourceId, const AnalyzerSpectrumFrame &frame) {
    SourceState *source = findOrCreate(sourceId);
    source->frame = frame;
    source->hasFrame = usableFrame(frame);
}

void SpectrumMosaicEngine::clear() {
    for (SourceState &source : sources) {
        source.hasFrame = false;
        source.frame = AnalyzerSpectrumFrame{};
    }
}

std::optional<SpectrumMosaicResult> SpectrumMosaicEngine::build() const {
    qint64 newestUtcMs = std::numeric_limits<qint64>::min();
    for (const SourceState &source : sources) {
        if (source.settings.enabled && source.hasFrame && usableFrame(source.frame))
            newestUtcMs = std::max(newestUtcMs, source.frame.utcMs);
    }
    if (newestUtcMs == std::numeric_limits<qint64>::min()) return std::nullopt;

    struct ActiveSource {
        const SourceState *source = nullptr;
        double usableMinimumHz = 0.0;
        double usableMaximumHz = 0.0;
        double binWidthHz = 0.0;
    };
    QVector<ActiveSource> active;
    SpectrumMosaicResult result;
    double minimumHz = std::numeric_limits<double>::infinity();
    double maximumHz = -std::numeric_limits<double>::infinity();
    double outputBinWidthHz = 0.0;

    for (const SourceState &source : sources) {
        if (!source.settings.enabled || !source.hasFrame || !usableFrame(source.frame)) continue;
        if (newestUtcMs - source.frame.utcMs > maximumFrameAgeMs) {
            result.staleSources.append(source.sourceId);
            continue;
        }
        const double spanHz = source.frame.maximumHz - source.frame.minimumHz;
        const double trimHz = spanHz * source.settings.edgeDiscardFraction;
        const double usableMinimum = source.frame.minimumHz + trimHz;
        const double usableMaximum = source.frame.maximumHz - trimHz;
        if (!(usableMaximum > usableMinimum)) continue;
        const double binWidthHz = frameBinWidth(source.frame);
        active.append({&source, usableMinimum, usableMaximum, binWidthHz});
        minimumHz = std::min(minimumHz, usableMinimum);
        maximumHz = std::max(maximumHz, usableMaximum);
        outputBinWidthHz = std::max(outputBinWidthHz, binWidthHz);
        result.contributingSources.append(source.sourceId);
    }
    if (active.isEmpty() || !(maximumHz > minimumHz) || !(outputBinWidthHz > 0.0))
        return std::nullopt;

    const double totalSpanHz = maximumHz - minimumHz;
    outputBinWidthHz = std::max(
        outputBinWidthHz,
        totalSpanHz / static_cast<double>(std::max(1, maximumOutputBins - 1)));
    const int outputBins = std::clamp(
        static_cast<int>(std::floor(totalSpanHz / outputBinWidthHz)) + 1,
        2, maximumOutputBins);
    outputBinWidthHz = totalSpanHz / static_cast<double>(outputBins - 1);

    result.frame.sequence = 0;
    result.frame.utcMs = newestUtcMs;
    result.frame.minimumHz = minimumHz;
    result.frame.maximumHz = maximumHz;
    result.frame.centerHz = (minimumHz + maximumHz) * 0.5;
    result.frame.sourceFftLength = outputBins;
    result.frame.levelsDb.assign(static_cast<std::size_t>(outputBins),
                                 std::numeric_limits<float>::quiet_NaN());

    for (int bin = 0; bin < outputBins; ++bin) {
        const double frequencyHz = minimumHz + outputBinWidthHz * bin;
        double weightedPower = 0.0;
        double weightSum = 0.0;
        int contributors = 0;
        for (const ActiveSource &item : active) {
            if (frequencyHz < item.usableMinimumHz || frequencyHz > item.usableMaximumHz)
                continue;
            const double sourcePower = interpolatePower(item.source->frame, frequencyHz);
            if (!std::isfinite(sourcePower)) continue;
            const double normalized = std::clamp(
                (frequencyHz - item.usableMinimumHz) /
                    (item.usableMaximumHz - item.usableMinimumHz),
                0.0, 1.0);
            const double taper = std::sin(kPi * normalized);
            const double weight = 0.01 + taper * taper;
            const double correction = std::pow(10.0, item.source->settings.levelOffsetDb / 10.0);
            weightedPower += weight * sourcePower * correction;
            weightSum += weight;
            ++contributors;
        }
        if (contributors == 0) {
            ++result.uncoveredBins;
            continue;
        }
        if (contributors > 1) ++result.overlapBins;
        result.frame.levelsDb[static_cast<std::size_t>(bin)] = static_cast<float>(
            10.0 * std::log10(std::max(weightedPower / weightSum, 1.0e-30)));
    }
    return result;
}
