#include "channelizerutils.h"

#include <algorithm>
#include <cmath>

namespace {

double clampDouble(double value, double low, double high) {
    return (std::max)(low, (std::min)(value, high));
}

} // namespace

double channelizerTargetRate(const RadioSettings &settings) {
    switch (settings.modulationType) {
    case MOD_ATV:
        return clampDouble((std::max)(2400000.0, settings.bandwidth * 1.4),
                           2400000.0,
                           8000000.0);
    case MOD_WFM:
        return clampDouble((std::max)(384000.0, settings.bandwidth * 3.0),
                           384000.0,
                           768000.0);
    case MOD_NFM:
    case MOD_M17:
    case MOD_RTTY:
    case MOD_FSK:
        return clampDouble((std::max)(240000.0, settings.bandwidth * 4.0),
                           240000.0,
                           384000.0);
    case MOD_DMR:
        return (std::max)(192000.0,
                          static_cast<double>(
                              normalizedDmrBasebandSampleRate(settings.dmrBasebandSampleRate)));
    case MOD_CW:
    case MOD_USB:
    case MOD_LSB:
    case MOD_FT8:
    case MOD_FT4:
    case MOD_PSK:
    case MOD_AM:
    case MOD_SAM:
    case MOD_DSB:
    default:
        return 192000.0;
    }
}

double channelizerOutputRate(const RadioSettings &settings) {
    if (settings.sampleRate <= 0.0 || !std::isfinite(settings.sampleRate)) {
        return 0.0;
    }
    const double targetRate = channelizerTargetRate(settings);
    const int decimationFactor =
        (std::max)(1, static_cast<int>(std::floor(settings.sampleRate / targetRate)));
    const double coarseRate =
        settings.sampleRate / static_cast<double>(decimationFactor);
    return (std::min)(targetRate, coarseRate);
}

double channelizerCutoff(const RadioSettings &settings, double outputRate) {
    double requestedCutoff = settings.bandwidth * 0.6;
    switch (settings.modulationType) {
    case MOD_ATV:
        requestedCutoff = (std::max)(1000000.0, settings.bandwidth * 0.9);
        break;
    case MOD_WFM:
        requestedCutoff = (std::max)(120000.0, settings.bandwidth * 0.6);
        break;
    case MOD_NFM:
    case MOD_M17:
    case MOD_RTTY:
    case MOD_FSK:
        requestedCutoff = (std::max)(12000.0, settings.bandwidth * 0.6);
        break;
    case MOD_DMR:
        requestedCutoff = (std::min)(9500.0, (std::max)(6000.0, settings.bandwidth * 0.75));
        break;
    case MOD_USB:
    case MOD_LSB:
    case MOD_FT8:
    case MOD_FT4:
    case MOD_PSK:
        requestedCutoff = (std::max)(3600.0, settings.bandwidth);
        break;
    case MOD_CW:
        requestedCutoff = (std::max)(1200.0, settings.bandwidth);
        break;
    case MOD_AM:
    case MOD_SAM:
    case MOD_DSB:
    default:
        requestedCutoff = (std::max)(10000.0, settings.bandwidth * 0.6);
        break;
    }
    return (std::min)(requestedCutoff, outputRate * 0.45);
}

int channelizerFrameSamplesForRate(double outputRate, int baseFrameSamples) {
    if (outputRate >= 4000000.0) {
        return baseFrameSamples * 32;
    }
    if (outputRate >= 1000000.0) {
        return baseFrameSamples * 16;
    }
    return baseFrameSamples;
}
