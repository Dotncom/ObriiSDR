#include "spectrummosaicengine.h"

#include <cmath>
#include <iostream>

namespace {
AnalyzerSpectrumFrame constantFrame(double minimumHz, double maximumHz,
                                    float levelDb, qint64 utcMs) {
    AnalyzerSpectrumFrame frame;
    frame.utcMs = utcMs;
    frame.minimumHz = minimumHz;
    frame.maximumHz = maximumHz;
    frame.centerHz = (minimumHz + maximumHz) * 0.5;
    frame.sourceFftLength = 1024;
    frame.levelsDb.assign(1024, levelDb);
    return frame;
}

bool nearly(double value, double expected, double tolerance = 0.25) {
    return std::abs(value - expected) <= tolerance;
}
}

int main() {
    SpectrumMosaicEngine engine;
    engine.setMaximumOutputBins(4096);
    engine.setMaximumFrameAgeMs(100);

    SpectrumMosaicSourceSettings first;
    first.edgeDiscardFraction = 0.0;
    SpectrumMosaicSourceSettings second = first;
    second.levelOffsetDb = -10.0;
    engine.setSourceSettings(0, first);
    engine.setSourceSettings(1, second);
    engine.updateFrame(0, constantFrame(0.0, 1000.0, -20.0f, 1000));
    engine.updateFrame(1, constantFrame(500.0, 1500.0, -10.0f, 1000));

    const auto result = engine.build();
    if (!result || result->contributingSources.size() != 2 || result->overlapBins == 0) {
        std::cerr << "mosaic did not combine both sources\n";
        return 1;
    }
    for (float value : result->frame.levelsDb) {
        if (std::isfinite(value) && !nearly(value, -20.0)) {
            std::cerr << "linear-power overlap or level correction is wrong: " << value << '\n';
            return 2;
        }
    }

    engine.updateFrame(1, constantFrame(500.0, 1500.0, -10.0f, 1201));
    const auto stale = engine.build();
    if (!stale || stale->contributingSources.size() != 1 || !stale->staleSources.contains(0)) {
        std::cerr << "stale source rejection failed\n";
        return 3;
    }
    std::cout << "Spectrum mosaic self-test passed\n";
    return 0;
}
