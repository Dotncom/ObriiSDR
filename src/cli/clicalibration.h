#pragma once

#include "../research/receivercalibrationtable.h"
#include "../research/receivernoisebaseline.h"
#include "../research/receiveryfactorcalibration.h"

#include <QJsonObject>
#include <vector>

// On-demand, Qt Core only. Serialize command() and submitSpectrum() on one thread.
class CliCalibration {
public:
    QJsonObject command(const QJsonObject &request);
    // Frequencies are ascending, uniformly spaced Hz; levels are power dBFS,
    // before noise/Y-factor processing, with the settings described by key.
    // Empty when idle; otherwise returns a capture event (including rejection).
    QJsonObject submitSpectrum(const std::vector<float> &frequenciesHz,
                               const std::vector<float> &levelsDbfs,
                               const ReceiverNoiseBaselineKey &key);
    bool capturing() const { return noise.isCapturing() || yfactor.isCapturing(); }

private:
    QJsonObject status() const;
    void cancel();
    ReceiverCalibrationTable table;
    ReceiverNoiseBaseline noise;
    ReceiverYFactorCalibration yfactor;
    ReceiverNoiseBaselineKey selected;
    bool haveSelection = false;
    // Also retained between cold and hot, since the core checks only bin count
    // across the two stages, not their frequency grids.
    std::vector<float> captureGrid;
};
