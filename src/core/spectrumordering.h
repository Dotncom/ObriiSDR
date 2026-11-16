#pragma once
#include <vector>
#include <algorithm>

// FFT magnitudes in the processing pipeline are unshifted; the frequency axis
// and receiver-noise calibration API are ascending. Do not associate IQ bin0
// (DC) with the left edge of the RF window.
inline std::vector<float> spectrumInFrequencyOrder(const std::vector<float> &unshifted) {
    std::vector<float> result = unshifted;
    if (!result.empty()) std::rotate(result.begin(), result.begin() + result.size() / 2, result.end());
    return result;
}
