#pragma once
#include "spurcalibrationprofile.h"
#include "radiosettings.h"

inline void adaptSpurMaskToResolution(QVector<SpurMaskEntry> &mask, double rate, int fftLength, int window) {
    if (fftLength <= 0 || !std::isfinite(rate) || rate <= 0 || rate / fftLength * 3 > 20000) {
        // A very coarse FFT cannot separate a narrow line from a wide feature.
        // Preserve the stored profile, but do not erase unresolved screen bins.
        mask.clear(); return;
    }
    int bins = 3;
    switch (normalizedFftWindowType(window)) {
    case FFT_WINDOW_HANN: case FFT_WINDOW_HAMMING: bins = 6; break;
    case FFT_WINDOW_BLACKMAN_HARRIS: bins = 10; break;
    case FFT_WINDOW_FLAT_TOP: bins = 12; break;
    default: break;
    }
    const double minimumWidth = bins * rate / fftLength;
    for (auto &entry : mask) entry.widthHz = (std::max)(entry.widthHz, minimumWidth);
}
