#include "scanvisualassembler.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr float SCAN_VISUAL_FLOOR_DB = -160.0f;
constexpr int SCAN_VISUAL_MIN_BINS = 1024;
constexpr int SCAN_VISUAL_MAX_BINS = 16384;
}

void ScanVisualAssembler::reset() {
    sectors.clear();
    outputFrequencies.clear();
    outputActualFrequencies.clear();
    outputLevels.clear();
    outputReferenceLevels.clear();
    passLevels.clear();
    passReferenceLevels.clear();
    completePassLevels.clear();
    completePassReferenceLevels.clear();
    passSectorSeen.clear();
    configuredSampleRateHz = 0.0;
    minFrequencyHz = 0.0;
    maxFrequencyHz = 0.0;
    outputBinCount = 0;
    configuredMode = ScanVisualMode::CompressedMosaic;
    lastPassSectorIndex = -1;
    haveCompletePass = false;
}

bool ScanVisualAssembler::configure(const QVector<double> &centerFrequenciesHz,
                                    double sampleRateHz,
                                    int targetBins,
                                    ScanVisualMode mode) {
    if (centerFrequenciesHz.size() < 2 ||
        !std::isfinite(sampleRateHz) ||
        sampleRateHz <= 0.0) {
        reset();
        return false;
    }

    std::vector<double> centers;
    centers.reserve(static_cast<std::size_t>(centerFrequenciesHz.size()));
    for (const double center : centerFrequenciesHz) {
        if (std::isfinite(center)) {
            centers.push_back(center);
        }
    }
    std::sort(centers.begin(), centers.end());
    centers.erase(std::unique(centers.begin(),
                              centers.end(),
                              [](double a, double b) {
                                  return std::abs(a - b) <= 0.5;
                              }),
                  centers.end());
    if (centers.size() < 2) {
        reset();
        return false;
    }

    std::vector<double> deltas;
    deltas.reserve(centers.size() - 1);
    for (std::size_t i = 1; i < centers.size(); ++i) {
        const double delta = centers[i] - centers[i - 1];
        if (std::isfinite(delta) && delta > 0.5) {
            deltas.push_back(delta);
        }
    }
    if (deltas.empty()) {
        reset();
        return false;
    }
    std::sort(deltas.begin(), deltas.end());
    const double nominalStepHz = deltas[deltas.size() / 2];
    const double gapThresholdHz = nominalStepHz * 3.0;
    const double halfSampleRateHz = sampleRateHz * 0.5;

    std::vector<Sector> nextSectors;
    nextSectors.reserve(centers.size());
    for (std::size_t i = 0; i < centers.size(); ++i) {
        const double center = centers[i];
        double leftHalf = halfSampleRateHz;
        double rightHalf = halfSampleRateHz;
        if (mode == ScanVisualMode::CompressedMosaic) {
            leftHalf = nominalStepHz * 0.5;
            rightHalf = nominalStepHz * 0.5;
            if (i > 0) {
                const double delta = center - centers[i - 1];
                leftHalf = delta <= gapThresholdHz ? delta * 0.5 : nominalStepHz * 0.5;
            }
            if (i + 1 < centers.size()) {
                const double delta = centers[i + 1] - center;
                rightHalf = delta <= gapThresholdHz ? delta * 0.5 : nominalStepHz * 0.5;
            }
        }
        leftHalf = (std::clamp)(leftHalf, 1.0, halfSampleRateHz);
        rightHalf = (std::clamp)(rightHalf, 1.0, halfSampleRateHz);
        nextSectors.push_back({center,
                               center - leftHalf,
                               center + rightHalf,
                               0.0,
                               0.0,
                               0.0});
    }

    if (mode == ScanVisualMode::CompressedMosaic) {
        double displayCursorHz = nextSectors.front().actualStartHz;
        for (Sector &sector : nextSectors) {
            const double widthHz = (std::max)(1.0, sector.actualEndHz - sector.actualStartHz);
            sector.displayStartHz = displayCursorHz;
            sector.displayEndHz = displayCursorHz + widthHz;
            sector.displayCenterHz = (sector.displayStartHz + sector.displayEndHz) * 0.5;
            displayCursorHz = sector.displayEndHz;
        }
    } else {
        for (Sector &sector : nextSectors) {
            sector.displayStartHz = sector.actualStartHz;
            sector.displayEndHz = sector.actualEndHz;
            sector.displayCenterHz = sector.actualCenterHz;
        }
    }

    const int nextOutputBinCount = (std::clamp)(targetBins,
                                                SCAN_VISUAL_MIN_BINS,
                                                SCAN_VISUAL_MAX_BINS);
    bool changed =
        sectors.size() != nextSectors.size() ||
        std::abs(configuredSampleRateHz - sampleRateHz) > 0.5 ||
        outputBinCount != nextOutputBinCount ||
        configuredMode != mode;
    if (!changed) {
        for (std::size_t i = 0; i < sectors.size(); ++i) {
            const Sector &current = sectors[i];
            const Sector &next = nextSectors[i];
            if (std::abs(current.actualCenterHz - next.actualCenterHz) > 0.5 ||
                std::abs(current.actualStartHz - next.actualStartHz) > 0.5 ||
                std::abs(current.actualEndHz - next.actualEndHz) > 0.5 ||
                std::abs(current.displayStartHz - next.displayStartHz) > 0.5 ||
                std::abs(current.displayEndHz - next.displayEndHz) > 0.5) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        sectors = std::move(nextSectors);
        configuredSampleRateHz = sampleRateHz;
        outputBinCount = nextOutputBinCount;
        configuredMode = mode;
        minFrequencyHz = sectors.front().displayStartHz;
        maxFrequencyHz = sectors.front().displayEndHz;
        for (const Sector &sector : sectors) {
            minFrequencyHz = (std::min)(minFrequencyHz, sector.displayStartHz);
            maxFrequencyHz = (std::max)(maxFrequencyHz, sector.displayEndHz);
        }
        rebuildOutputGrid();
        resetPassBuffers();
    }

    return true;
}

ScanVisualFrame ScanVisualAssembler::update(double frameCenterFrequencyHz,
                                            const std::vector<float> &frequencies,
                                            const std::vector<float> &magnitudes,
                                            const std::vector<float> &referenceMagnitudes) {
    if (!isConfigured() || frequencies.empty() || magnitudes.empty()) {
        return {};
    }

    const int dataCount = std::min(static_cast<int>(frequencies.size()),
                                   static_cast<int>(magnitudes.size()));
    if (dataCount <= 0) {
        return {};
    }

    const int sectorIndex = nearestSector(frameCenterFrequencyHz);
    if (sectorIndex < 0 || sectorIndex >= static_cast<int>(sectors.size())) {
        if (configuredMode == ScanVisualMode::PassComposite && haveCompletePass) {
            return composeFrame(!completePassReferenceLevels.empty(),
                                &completePassLevels,
                                &completePassReferenceLevels,
                                false);
        }
        return composeFrame(!outputReferenceLevels.empty());
    }

    const Sector &sector = sectors[static_cast<std::size_t>(sectorIndex)];
    const int firstBin =
        (std::clamp)(binForDisplayFrequency(sector.displayStartHz), 0, outputBinCount - 1);
    const int lastBin =
        (std::clamp)(binForDisplayFrequency(sector.displayEndHz), firstBin, outputBinCount - 1);
    std::vector<float> &targetLevels =
        configuredMode == ScanVisualMode::PassComposite ? passLevels : outputLevels;
    std::vector<float> &targetReferenceLevels =
        configuredMode == ScanVisualMode::PassComposite ? passReferenceLevels : outputReferenceLevels;

    if (targetLevels.size() != static_cast<std::size_t>(outputBinCount)) {
        targetLevels.assign(static_cast<std::size_t>(outputBinCount), SCAN_VISUAL_FLOOR_DB);
    }
    if (configuredMode == ScanVisualMode::FloatingTrueAxis) {
        std::fill(targetLevels.begin(), targetLevels.end(), SCAN_VISUAL_FLOOR_DB);
        if (!targetReferenceLevels.empty()) {
            std::fill(targetReferenceLevels.begin(),
                      targetReferenceLevels.end(),
                      SCAN_VISUAL_FLOOR_DB);
        }
    } else if (configuredMode != ScanVisualMode::PassComposite) {
        for (int bin = firstBin; bin <= lastBin; ++bin) {
            targetLevels[static_cast<std::size_t>(bin)] = SCAN_VISUAL_FLOOR_DB;
            if (!targetReferenceLevels.empty()) {
                targetReferenceLevels[static_cast<std::size_t>(bin)] = SCAN_VISUAL_FLOOR_DB;
            }
        }
    }

    const bool haveReference =
        static_cast<int>(referenceMagnitudes.size()) >= dataCount;
    if (haveReference && targetReferenceLevels.empty()) {
        targetReferenceLevels.assign(static_cast<std::size_t>(outputBinCount), SCAN_VISUAL_FLOOR_DB);
    }

    for (int i = 0; i < dataCount; ++i) {
        const double frequency = frequencies[static_cast<std::size_t>(i)];
        if (!std::isfinite(frequency) ||
            frequency < sector.actualStartHz ||
            frequency > sector.actualEndHz) {
            continue;
        }
        const int bin = binForSectorFrequency(sector, frequency);
        if (bin < 0 || bin >= outputBinCount) {
            continue;
        }
        const float level = shiftedValueAt(magnitudes, i, dataCount);
        if (std::isfinite(level)) {
            float &target = targetLevels[static_cast<std::size_t>(bin)];
            target = (std::max)(target, level);
        }
        if (haveReference) {
            const float referenceLevel = shiftedValueAt(referenceMagnitudes, i, dataCount);
            if (std::isfinite(referenceLevel)) {
                float &target = targetReferenceLevels[static_cast<std::size_t>(bin)];
                target = (std::max)(target, referenceLevel);
            }
        }
    }

    if (configuredMode != ScanVisualMode::PassComposite) {
        return composeFrame(haveReference || !outputReferenceLevels.empty());
    }

    if (passSectorSeen.size() != sectors.size()) {
        passSectorSeen.assign(sectors.size(), false);
    }
    passSectorSeen[static_cast<std::size_t>(sectorIndex)] = true;
    lastPassSectorIndex = sectorIndex;

    const bool passComplete =
        !passSectorSeen.empty() &&
        std::all_of(passSectorSeen.begin(), passSectorSeen.end(), [](bool seen) { return seen; });
    if (passComplete) {
        completePassLevels = passLevels;
        completePassReferenceLevels =
            passReferenceLevels.size() == static_cast<std::size_t>(outputBinCount)
                ? passReferenceLevels
                : std::vector<float>();
        haveCompletePass = true;
        passLevels.assign(static_cast<std::size_t>(outputBinCount), SCAN_VISUAL_FLOOR_DB);
        passReferenceLevels.clear();
        passSectorSeen.assign(sectors.size(), false);
        lastPassSectorIndex = -1;
        return composeFrame(!completePassReferenceLevels.empty(),
                            &completePassLevels,
                            &completePassReferenceLevels,
                            true);
    }

    if (haveCompletePass) {
        return composeFrame(!completePassReferenceLevels.empty(),
                            &completePassLevels,
                            &completePassReferenceLevels,
                            false);
    }

    return composeFrame(haveReference || !passReferenceLevels.empty(),
                        &passLevels,
                        &passReferenceLevels,
                        false);
}

bool ScanVisualAssembler::isConfigured() const {
    return sectors.size() >= 2 &&
           outputBinCount > 1 &&
           outputFrequencies.size() == static_cast<std::size_t>(outputBinCount) &&
           outputActualFrequencies.size() == static_cast<std::size_t>(outputBinCount) &&
           outputLevels.size() == static_cast<std::size_t>(outputBinCount) &&
           std::isfinite(minFrequencyHz) &&
           std::isfinite(maxFrequencyHz) &&
           maxFrequencyHz > minFrequencyHz;
}

int ScanVisualAssembler::nearestSector(double centerHz) const {
    if (sectors.empty() || !std::isfinite(centerHz)) {
        return -1;
    }

    int bestIndex = 0;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (int i = 0; i < static_cast<int>(sectors.size()); ++i) {
        const double distance = std::abs(sectors[static_cast<std::size_t>(i)].actualCenterHz - centerHz);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }
    return bestIndex;
}

void ScanVisualAssembler::rebuildOutputGrid() {
    outputFrequencies.assign(static_cast<std::size_t>(outputBinCount), 0.0f);
    outputActualFrequencies.assign(static_cast<std::size_t>(outputBinCount), 0.0f);
    outputLevels.assign(static_cast<std::size_t>(outputBinCount), SCAN_VISUAL_FLOOR_DB);
    outputReferenceLevels.clear();
    resetPassBuffers();

    const double spanHz = maxFrequencyHz - minFrequencyHz;
    const double stepHz = spanHz / static_cast<double>((std::max)(1, outputBinCount - 1));
    for (int i = 0; i < outputBinCount; ++i) {
        const double displayFrequency = minFrequencyHz + stepHz * static_cast<double>(i);
        outputFrequencies[static_cast<std::size_t>(i)] = static_cast<float>(displayFrequency);

        double actualFrequency = displayFrequency;
        for (const Sector &sector : sectors) {
            if (displayFrequency < sector.displayStartHz || displayFrequency > sector.displayEndHz) {
                continue;
            }
            const double displayWidth = (std::max)(1.0, sector.displayEndHz - sector.displayStartHz);
            const double ratio = (displayFrequency - sector.displayStartHz) / displayWidth;
            actualFrequency = sector.actualStartHz +
                              (std::clamp)(ratio, 0.0, 1.0) *
                                  (sector.actualEndHz - sector.actualStartHz);
            break;
        }
        outputActualFrequencies[static_cast<std::size_t>(i)] = static_cast<float>(actualFrequency);
    }
}

void ScanVisualAssembler::resetPassBuffers() {
    passLevels.assign(static_cast<std::size_t>((std::max)(0, outputBinCount)), SCAN_VISUAL_FLOOR_DB);
    passReferenceLevels.clear();
    completePassLevels.clear();
    completePassReferenceLevels.clear();
    passSectorSeen.assign(sectors.size(), false);
    lastPassSectorIndex = -1;
    haveCompletePass = false;
}

ScanVisualFrame ScanVisualAssembler::composeFrame(bool includeReference,
                                                  const std::vector<float> *levels,
                                                  const std::vector<float> *referenceLevels,
                                                  bool fresh) const {
    if (!isConfigured()) {
        return {};
    }

    const std::vector<float> &sourceLevels =
        (levels && levels->size() == static_cast<std::size_t>(outputBinCount)) ? *levels : outputLevels;
    const std::vector<float> *sourceReferenceLevels =
        (referenceLevels && referenceLevels->size() == static_cast<std::size_t>(outputBinCount))
            ? referenceLevels
            : &outputReferenceLevels;

    ScanVisualFrame frame;
    frame.valid = true;
    frame.mosaic = true;
    frame.fresh = fresh;
    frame.showSegmentMarkers = configuredMode == ScanVisualMode::CompressedMosaic;
    frame.minFrequency = minFrequencyHz;
    frame.maxFrequency = maxFrequencyHz;
    frame.centerFrequency = (minFrequencyHz + maxFrequencyHz) * 0.5;
    frame.fftLength = outputBinCount;
    frame.sectorCount = static_cast<int>(sectors.size());
    frame.frequencies = outputFrequencies;
    frame.actualFrequencies = outputActualFrequencies;
    frame.magnitudes.assign(static_cast<std::size_t>(outputBinCount), SCAN_VISUAL_FLOOR_DB);
    frame.segments.reserve(static_cast<int>(sectors.size()));
    for (const Sector &sector : sectors) {
        const QString label =
            QStringLiteral("%1-%2 MHz")
                .arg(sector.actualStartHz / 1000000.0, 0, 'f', 1)
                .arg(sector.actualEndHz / 1000000.0, 0, 'f', 1);
        frame.segments.push_back({sector.displayStartHz,
                                  sector.displayEndHz,
                                  sector.displayCenterHz,
                                  sector.actualStartHz,
                                  sector.actualEndHz,
                                  sector.actualCenterHz,
                                  label});
    }
    for (int i = 0; i < outputBinCount; ++i) {
        frame.magnitudes[static_cast<std::size_t>((i + outputBinCount / 2) % outputBinCount)] =
            sourceLevels[static_cast<std::size_t>(i)];
    }
    if (includeReference &&
        sourceReferenceLevels &&
        sourceReferenceLevels->size() == static_cast<std::size_t>(outputBinCount)) {
        frame.referenceMagnitudes.assign(static_cast<std::size_t>(outputBinCount), SCAN_VISUAL_FLOOR_DB);
        for (int i = 0; i < outputBinCount; ++i) {
            frame.referenceMagnitudes[static_cast<std::size_t>((i + outputBinCount / 2) % outputBinCount)] =
                (*sourceReferenceLevels)[static_cast<std::size_t>(i)];
        }
    }
    return frame;
}

int ScanVisualAssembler::binForDisplayFrequency(double frequencyHz) const {
    if (!isConfigured() || !std::isfinite(frequencyHz)) {
        return -1;
    }
    const double ratio = (frequencyHz - minFrequencyHz) / (maxFrequencyHz - minFrequencyHz);
    return static_cast<int>(std::llround((std::clamp)(ratio, 0.0, 1.0) *
                                         static_cast<double>(outputBinCount - 1)));
}

int ScanVisualAssembler::binForSectorFrequency(const Sector &sector, double frequencyHz) const {
    if (!isConfigured() ||
        !std::isfinite(frequencyHz) ||
        sector.actualEndHz <= sector.actualStartHz ||
        sector.displayEndHz <= sector.displayStartHz) {
        return -1;
    }

    const double ratio = (frequencyHz - sector.actualStartHz) /
                         (sector.actualEndHz - sector.actualStartHz);
    const double displayFrequency = sector.displayStartHz +
                                    (std::clamp)(ratio, 0.0, 1.0) *
                                        (sector.displayEndHz - sector.displayStartHz);
    return binForDisplayFrequency(displayFrequency);
}

float ScanVisualAssembler::shiftedValueAt(const std::vector<float> &values, int index, int count) {
    if (count <= 0 || static_cast<int>(values.size()) < count) {
        return SCAN_VISUAL_FLOOR_DB;
    }
    const int shiftedIndex = (index + count / 2) % count;
    return values[static_cast<std::size_t>(shiftedIndex)];
}
