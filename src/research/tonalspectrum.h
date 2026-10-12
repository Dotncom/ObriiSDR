#ifndef TONALSPECTRUM_H
#define TONALSPECTRUM_H

#include <limits>
#include <vector>

struct TonalSpectrumResult {
    bool valid = false;
    bool completeHarmonicSpan = false;
    int harmonicsMeasured = 0;
    int harmonicsRequested = 0;
    double fundamentalHz = 0;
    double binWidthHz = 0;
    double signalPower = 0;
    double harmonicPower = 0;
    double noisePower = 0;
    double snrDb = std::numeric_limits<double>::quiet_NaN();
    double sinadDb = std::numeric_limits<double>::quiet_NaN();
    double thdDb = std::numeric_limits<double>::quiet_NaN();
    double thdnDb = std::numeric_limits<double>::quiet_NaN();
    double thdPercent = 0;
    double thdnPercent = 0;
};

// Input is an ascending, contiguous single-tone spectrum, not a scan mosaic.
TonalSpectrumResult analyzeTonalSpectrum(const std::vector<float> &frequencies,
                                        const std::vector<float> &levels,
                                        double fundamentalHz, double integrationHalfWidthHz,
                                        int lastHarmonic = 5);
#endif
