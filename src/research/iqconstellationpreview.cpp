#include "iqconstellationpreview.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

SignalLabIqPreviewData buildResearchIqConstellation(const std::vector<float> &first,
                                                   const std::vector<float> &second,
                                                   double sampleRate,
                                                   double symbolRate,
                                                   double phaseFraction) {
    SignalLabIqPreviewData data;
    const std::size_t count = (std::min)(first.size(), second.size());
    if (!count || !std::isfinite(sampleRate) || sampleRate <= 0) return data;
    constexpr std::size_t maximumPoints = 2500;
    double maximum = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (std::isfinite(first[index])) maximum = (std::max)(maximum, std::abs(double(first[index])));
        if (std::isfinite(second[index])) maximum = (std::max)(maximum, std::abs(double(second[index])));
    }
    data.amplitudeScale = maximum > 0 ? maximum : 1;
    data.durationSeconds = count/sampleRate;
    data.continuousTrajectory = count <= maximumPoints;
    const auto finite = [](float value) { return std::isfinite(value) ? value : 0.0f; };
    const auto point = [&](std::size_t index) {
        return std::complex<float>(finite(first[index]), finite(second[index]));
    };
    const std::size_t points = (std::min)(count, maximumPoints);
    data.samples.reserve(int(points));
    data.sampleTimes.reserve(int(points));
    for (std::size_t bin = 0; bin < points; ++bin) {
        const std::size_t begin = bin*count/points, end = (bin+1)*count/points;
        // Stratified jitter avoids phase-locking a regular stride to a periodic signal.
        std::uint32_t hash = std::uint32_t(bin)+0x9e3779b9U;
        hash ^= hash >> 16; hash *= 0x85ebca6bU; hash ^= hash >> 13;
        const std::size_t index = bin == 0 ? 0 : bin+1 == points ? count-1 : begin+hash%(end-begin);
        data.samples.append(point(index));
        data.sampleTimes.append(index/sampleRate);
    }
    if (!std::isfinite(symbolRate) || symbolRate <= 0) return data;
    const double samplesPerSymbol = sampleRate/symbolRate;
    if (samplesPerSymbol < 2 || !std::isfinite(samplesPerSymbol)) return data;
    const double phase = std::isfinite(phaseFraction) ? std::clamp(phaseFraction, 0.0, 1.0) : 0.5;
    const double start = phase*samplesPerSymbol;
    if (start >= count-1) return data;
    const std::size_t symbolCount = std::size_t(std::floor((count-1-start)/samplesPerSymbol))+1;
    const std::size_t symbols = (std::min)(symbolCount, maximumPoints);
    data.symbols.reserve(int(symbols));
    data.symbolTimes.reserve(int(symbols));
    for (std::size_t index = 0; index < symbols; ++index) {
        const std::size_t symbol = symbols == 1 ? 0 : index*(symbolCount-1)/(symbols-1);
        const double position = (std::min)(start+symbol*samplesPerSymbol, double(count-1));
        const auto left = std::size_t(position);
        const auto right = (std::min)(left+1, count-1);
        const float fraction = float(position-left);
        data.symbols.append(point(left)+(point(right)-point(left))*fraction);
        data.symbolTimes.append(position/sampleRate);
    }
    return data;
}
