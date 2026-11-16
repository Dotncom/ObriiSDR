#pragma once

inline bool fobosIfAutoRateEligible(double sampleRate) {
    return sampleRate >= 49000000.0 && sampleRate <= 51000000.0;
}

// Capabilities are obtained from the optional Obrii protocol, never inferred
// from the native firmware version or device name. No capability means native.
inline unsigned int fobosRequestedIfTestMode(int preference, unsigned int capabilities, double sampleRate) {
    if (!(capabilities & 4u)) return 0;
    if (preference == 3)
        return (capabilities & 2u) && fobosIfAutoRateEligible(sampleRate) ? 3u : 0u;
    return preference >= 0 && preference <= 2 ? static_cast<unsigned int>(preference) : 0u;
}
