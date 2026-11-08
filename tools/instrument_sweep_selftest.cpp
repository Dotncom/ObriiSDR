#include "instrumentsweepengine.h"

#include <cmath>
#include <iostream>
#include <vector>

int main() {
    InstrumentSweepSettings settings;
    settings.startHz = 100.0e6;
    settings.stopHz = 140.0e6;
    settings.captureBandwidthHz = 10.0e6;
    settings.overlapFraction = 0.2;
    settings.edgeDiscardFraction = 0.1;
    settings.outputBinWidthHz = 10000.0;
    const InstrumentSweepPlan plan = InstrumentSweepEngine::createPlan(settings);
    if (!plan.isValid() || plan.segments.size() < 5) {
        std::cerr << "invalid sweep plan\n";
        return 1;
    }
    if (std::abs(plan.segments.front().usableMinimumHz - settings.startHz) > 1.0 ||
        std::abs(plan.segments.back().usableMaximumHz - settings.stopHz) > 1.0) {
        std::cerr << "sweep endpoints are not covered\n";
        return 2;
    }

    InstrumentSweepEngine engine;
    if (!engine.begin(plan)) return 3;
    std::vector<float> liveFrequencies;
    std::vector<float> liveLevels;
    for (const InstrumentSweepSegment &segment : plan.segments) {
        const int count = 1001;
        const double minimum = segment.centerHz - settings.captureBandwidthHz * 0.5;
        const double maximum = segment.centerHz + settings.captureBandwidthHz * 0.5;
        std::vector<float> levels(static_cast<std::size_t>(count), -100.0f);
        for (int index = 0; index < count; ++index) {
            const double frequency = minimum + (maximum - minimum) * index / (count - 1);
            if (std::abs(frequency - 123.45e6) < 30000.0) levels[static_cast<std::size_t>(index)] = -20.0f;
        }
        if (!engine.addFrame(segment.index, minimum, maximum, levels)) return 4;
        if (!engine.updateResultForSegment(segment.index, liveFrequencies, liveLevels, -160.0f))
            return 7;
    }
    const InstrumentSweepResult result = engine.finish();
    if (result.levelsDb.empty() || result.missingBins > 10) {
        std::cerr << "incomplete stitched sweep: " << result.missingBins << " missing bins\n";
        return 5;
    }
    float peak = -200.0f;
    for (float level : result.levelsDb) if (std::isfinite(level)) peak = std::max(peak, level);
    if (peak < -25.0f || peak > -15.0f) {
        std::cerr << "unexpected stitched peak: " << peak << '\n';
        return 6;
    }
    if (liveFrequencies.size() != result.frequenciesHz.size() ||
        liveLevels.size() != result.levelsDb.size()) {
        std::cerr << "live sweep snapshot geometry mismatch\n";
        return 8;
    }
    float livePeak = -200.0f;
    for (float level : liveLevels) if (std::isfinite(level)) livePeak = std::max(livePeak, level);
    if (livePeak < -25.0f || livePeak > -15.0f) {
        std::cerr << "unexpected live stitched peak: " << livePeak << '\n';
        return 9;
    }
    std::cout << "instrument sweep self-test: ok; " << plan.segments.size()
              << " segments, " << result.levelsDb.size() << " bins\n";
    return 0;
}
