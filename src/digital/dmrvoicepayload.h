#ifndef DMRVOICEPAYLOAD_H
#define DMRVOICEPAYLOAD_H

#include <QString>

#include <array>
#include <cstdint>

struct DmrAmbePayload {
    QString hex;
    int correctedErrors = 0;
    int rawCorrectedErrors = 0;
    int timeslot = 0;
    int burstIndex = -1;
    int frameIndex = -1;
    quint64 superframeSample = 0;
    quint64 superframeSamples = 0;
    int privacyIndicator = -1; // Trusted Voice LC: -1 unknown, 0 clear, 1 encrypted.
    int lateEntryNibble = -1;
};

struct DmrAmbeSoftFrame {
    QString hex;
    std::array<std::uint8_t, 72> reliability = {};
};

#endif // DMRVOICEPAYLOAD_H
