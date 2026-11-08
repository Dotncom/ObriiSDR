#pragma once

#include "dspiqcodec.h"

enum class SpectrumComputeMode { Local = 0, Windows = 1, Continuous = 2 };

struct SpectrumComputeOptions {
    SpectrumComputeMode mode = SpectrumComputeMode::Local;
    int backend = 1; // CPU FFTW; values match FftBackendPreference.
    DspIqFormat iqFormat = DspIqFormat::Auto;
    int uploadLimitMBps = 100;
    bool fallbackToLocal = false;
    bool operator==(const SpectrumComputeOptions &other) const {
        return mode == other.mode && backend == other.backend && iqFormat == other.iqFormat &&
               uploadLimitMBps == other.uploadLimitMBps && fallbackToLocal == other.fallbackToLocal;
    }
};
