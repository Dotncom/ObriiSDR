#ifndef FOBOSFREQUENCYLIMITS_H
#define FOBOSFREQUENCYLIMITS_H

#include <cstring>

inline constexpr double FOBOS_AGILE_LEGACY_MAX_RF_HZ = 6900000000.0;

inline bool fobosAgileHasKnown6900MhzLimit(const char *hardware, const char *firmware) {
    if (!hardware || !firmware ||
        (hardware[0] != '2' && hardware[0] != '3') || hardware[1] != '.') {
        return false;
    }
    // Verified in the official rev.2/rev.3 images. Unknown firmware and rev.4
    // retain their own capabilities rather than inheriting this legacy limit.
    const bool knownVersion = std::strncmp(firmware, "3.1.0", 5) == 0 ||
                              std::strncmp(firmware, "3.2.0", 5) == 0;
    return knownVersion && (firmware[5] == '\0' || firmware[5] == ' ');
}

#endif
