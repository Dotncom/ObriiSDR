#include "main.h"

#include "appconstants.h"
#include "modulationutils.h"
#include "presethelpers.h"
#include "tuningutils.h"

#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <limits>
void YourClassName::updateDmrHunter(const std::vector<float> &frequencies,
                                    const std::vector<float> &magnitudes) {
    if (!dmrHunterControls) {
        return;
    }

    double previousSelectedCenterHz = std::numeric_limits<double>::quiet_NaN();
    if (dmrHunterCandidateIndex >= 0 &&
        dmrHunterCandidateIndex < static_cast<int>(dmrHunterCandidates.size())) {
        previousSelectedCenterHz = dmrHunterCandidates[static_cast<std::size_t>(dmrHunterCandidateIndex)].centerHz;
    }

    dmrHunterSettings = DmrHunterDetector::normalizedSettings(dmrHunterSettings);
    dmrHunterLastResult = DmrHunterDetector::analyze(frequencies,
                                                     magnitudes,
                                                     dmrHunterSettings);
    dmrHunterCandidates = dmrHunterLastResult.candidateList;
    dmrHunterCandidateIndex = -1;
    if (!dmrHunterCandidates.empty()) {
        int bestIndex = 0;
        if (std::isfinite(previousSelectedCenterHz)) {
            double bestDeltaHz = std::numeric_limits<double>::max();
            for (int i = 0; i < static_cast<int>(dmrHunterCandidates.size()); ++i) {
                const double centerHz = dmrHunterCandidates[static_cast<std::size_t>(i)].centerHz;
                if (!std::isfinite(centerHz)) {
                    continue;
                }
                const double deltaHz = std::abs(centerHz - previousSelectedCenterHz);
                if (deltaHz < bestDeltaHz) {
                    bestDeltaHz = deltaHz;
                    bestIndex = i;
                }
            }
        }
        dmrHunterCandidateIndex = bestIndex;
    }

    QString statusText = dmrHunterLastResult.statusText;
    if (dmrHunterCandidateIndex >= 0 &&
        dmrHunterCandidateIndex < static_cast<int>(dmrHunterCandidates.size())) {
        const DmrHunterCandidate &candidate =
            dmrHunterCandidates[static_cast<std::size_t>(dmrHunterCandidateIndex)];
        statusText += QStringLiteral("\nSelected %1/%2: %3 MHz, width %4 kHz, peak %5 dB, +%6 dB")
                          .arg(dmrHunterCandidateIndex + 1)
                          .arg(static_cast<int>(dmrHunterCandidates.size()))
                          .arg(candidate.centerHz / 1000000.0, 0, 'f', 6)
                          .arg(candidate.widthHz / 1000.0, 0, 'f', 1)
                          .arg(candidate.peakDb, 0, 'f', 1)
                          .arg(candidate.excessDb, 0, 'f', 1);
    }
    dmrHunterControls->setStatusText(statusText);
    updateDmrHunterControls();
}

void YourClassName::updateDmrHunterControls() {
    dmrHunterSettings = DmrHunterDetector::normalizedSettings(dmrHunterSettings);
    const bool enabled = dmrHunterSettings.enabled;
    if (dmrHunterControls) {
        dmrHunterControls->setDetectChecked(enabled);
        dmrHunterControls->setWidthValues(dmrHunterSettings.minWidthKhz,
                                          dmrHunterSettings.maxWidthKhz,
                                          dmrHunterSettings.thresholdDb);
        dmrHunterControls->setControlsEnabled(enabled);
        const bool hasCandidate = !dmrHunterCandidates.empty();
        dmrHunterControls->setCandidateNavigationEnabled(enabled && hasCandidate);
        dmrHunterControls->setCandidateIndex(enabled ? dmrHunterCandidateIndex : -1,
                                             enabled ? static_cast<int>(dmrHunterCandidates.size()) : 0);
        dmrHunterControls->setTuneEnabled(enabled && (hasCandidate || dmrHunterLastResult.best.valid));
        if (!enabled) {
            dmrHunterCandidates.clear();
            dmrHunterCandidateIndex = -1;
            dmrHunterControls->setCandidateNavigationEnabled(false);
            dmrHunterControls->setCandidateIndex(-1, 0);
            dmrHunterControls->setStatusText(uiText(QStringLiteral("dmr_hunter_off"),
                                                    QStringLiteral("DMR Hunter: off")));
        }
    }
}

void YourClassName::applyDmrHunterPresetToScan() {
    if (!dmrHunterControls || !agileScanRangesEdit || !agileScanStepSpin) {
        return;
    }

    const QString spec = dmrHunterControls->currentPresetSpec();
    const QString ranges = agileScanPresetRanges(spec);
    const double step = agileScanPresetStepMhz(spec, 0.0125);
    if (ranges.isEmpty()) {
        return;
    }

    agileScanRangesMhz = ranges;
    agileScanStepMhz = step;
    agileScanEnabled = true;
    if (agileScanCheckbox) {
        QSignalBlocker blocker(agileScanCheckbox);
        agileScanCheckbox->setChecked(true);
    }
    {
        QSignalBlocker blocker(agileScanRangesEdit);
        agileScanRangesEdit->setText(agileScanRangesMhz);
    }
    {
        QSignalBlocker blocker(agileScanStepSpin);
        agileScanStepSpin->setValue(agileScanStepMhz);
    }
    updateAgileScanControls();
    savePersistentSettings();
}

void YourClassName::tuneDmrHunterCandidate() {
    if (dmrHunterCandidateIndex >= 0 &&
        dmrHunterCandidateIndex < static_cast<int>(dmrHunterCandidates.size())) {
        tuneDmrHunterCandidateIndex(dmrHunterCandidateIndex);
        return;
    }

    if (!dmrHunterLastResult.best.valid || !std::isfinite(dmrHunterLastResult.best.centerHz)) {
        return;
    }

    DmrHunterCandidate candidate = dmrHunterLastResult.best;
    dmrHunterCandidates = {candidate};
    dmrHunterCandidateIndex = 0;
    tuneDmrHunterCandidateIndex(0);
}

void YourClassName::selectDmrHunterCandidate(int direction) {
    if (dmrHunterCandidates.empty()) {
        updateDmrHunterControls();
        return;
    }

    const int count = static_cast<int>(dmrHunterCandidates.size());
    int nextIndex = dmrHunterCandidateIndex;
    if (nextIndex < 0 || nextIndex >= count) {
        nextIndex = direction < 0 ? count - 1 : 0;
    } else {
        nextIndex = (nextIndex + direction) % count;
        if (nextIndex < 0) {
            nextIndex += count;
        }
    }

    dmrHunterCandidateIndex = nextIndex;
    updateDmrHunterControls();
    tuneDmrHunterCandidateIndex(dmrHunterCandidateIndex);
}

void YourClassName::tuneDmrHunterCandidateIndex(int index) {
    if (index < 0 || index >= static_cast<int>(dmrHunterCandidates.size())) {
        return;
    }

    const DmrHunterCandidate &candidate = dmrHunterCandidates[static_cast<std::size_t>(index)];
    if (!candidate.valid || !std::isfinite(candidate.centerHz)) {
        return;
    }

    if (pendingSettings.modulationType != MOD_DMR) {
        if (modulationButtonGroup) {
            if (QAbstractButton *button = modulationButtonGroup->button(MOD_DMR)) {
                modulationButtonGroup->blockSignals(true);
                button->setChecked(true);
                modulationButtonGroup->blockSignals(false);
            }
        }
        onModulationChanged(MOD_DMR);
    }

    pendingSettings.bandwidth = 12500.0;
    if (bandwidthControl) {
        QSignalBlocker blocker(bandwidthControl);
        bandwidthControl->setValueHz(pendingSettings.bandwidth);
    }
    publishSettingsToGlobals();
    updateIqFrameProducerSettings();
    settingRange();
    updateTuningFromScale(candidate.centerHz, candidate.centerHz);
    savePersistentSettings();
}

void YourClassName::updateFpvHunter(const std::vector<float> &frequencies,
                                    const std::vector<float> &magnitudes) {
    if (!fpvHunterControls) {
        return;
    }
    if (!fpvHunterClock.isValid()) {
        fpvHunterClock.start();
    }
    const qint64 fpvHunterNowMs = fpvHunterClock.elapsed();

    double previousSelectedCenterHz = std::numeric_limits<double>::quiet_NaN();
    if (fpvHunterCandidateIndex >= 0 &&
        fpvHunterCandidateIndex < static_cast<int>(fpvHunterCandidates.size())) {
        previousSelectedCenterHz = fpvHunterCandidates[static_cast<std::size_t>(fpvHunterCandidateIndex)].centerHz;
    }

    fpvHunterSettings = FpvHunterDetector::normalizedSettings(fpvHunterSettings);
    fpvHunterLastResult = FpvHunterDetector::analyze(frequencies,
                                                     magnitudes,
                                                     fpvHunterSettings);
    const FpvHunterCandidate currentDetectedCandidate = fpvHunterLastResult.best;
    fpvHunterCandidates = fpvHunterLastResult.candidateList;
    fpvHunterCandidateIndex = -1;
    if (!fpvHunterCandidates.empty()) {
        int bestIndex = 0;
        if (std::isfinite(previousSelectedCenterHz)) {
            double bestDeltaHz = std::numeric_limits<double>::max();
            for (int i = 0; i < static_cast<int>(fpvHunterCandidates.size()); ++i) {
                const double centerHz = fpvHunterCandidates[static_cast<std::size_t>(i)].centerHz;
                if (!std::isfinite(centerHz)) {
                    continue;
                }
                const double deltaHz = std::abs(centerHz - previousSelectedCenterHz);
                if (deltaHz < bestDeltaHz) {
                    bestDeltaHz = deltaHz;
                    bestIndex = i;
                }
            }
        }
        fpvHunterCandidateIndex = bestIndex;
    }

    ++fpvHunterFrameSequence;
    if (!fpvHunterSettings.enabled) {
        fpvHunterTrack = {};
    } else if (fpvHunterLastResult.best.valid &&
               std::isfinite(fpvHunterLastResult.best.centerHz)) {
        const FpvHunterCandidate &candidate = fpvHunterLastResult.best;
        const double matchWindowHz =
            (std::max)(2500000.0, (std::max)(candidate.widthHz, fpvHunterTrack.widthHz) * 0.65);
        const bool sameTrack =
            fpvHunterTrack.valid &&
            std::isfinite(fpvHunterTrack.centerHz) &&
            std::abs(candidate.centerHz - fpvHunterTrack.centerHz) <= matchWindowHz;

        if (!sameTrack) {
            fpvHunterTrack = {};
            fpvHunterTrack.valid = true;
            fpvHunterTrack.centerHz = candidate.centerHz;
            fpvHunterTrack.widthHz = candidate.widthHz;
            fpvHunterTrack.peakDb = candidate.peakDb;
            fpvHunterTrack.averageDb = candidate.averageDb;
            fpvHunterTrack.excessDb = candidate.excessDb;
            fpvHunterTrack.score = candidate.score;
            fpvHunterTrack.hits = 1;
            fpvHunterTrack.type = candidate.type;
            fpvHunterTrack.firstSeenMsec = fpvHunterNowMs;
        } else {
            constexpr double alpha = 0.35;
            fpvHunterTrack.centerHz =
                fpvHunterTrack.centerHz * (1.0 - alpha) + candidate.centerHz * alpha;
            fpvHunterTrack.widthHz =
                fpvHunterTrack.widthHz * (1.0 - alpha) + candidate.widthHz * alpha;
            fpvHunterTrack.peakDb = (std::max)(fpvHunterTrack.peakDb, candidate.peakDb);
            fpvHunterTrack.averageDb =
                static_cast<float>(fpvHunterTrack.averageDb * (1.0 - alpha) +
                                   candidate.averageDb * alpha);
            fpvHunterTrack.excessDb =
                static_cast<float>(fpvHunterTrack.excessDb * (1.0 - alpha) +
                                   candidate.excessDb * alpha);
            fpvHunterTrack.score =
                static_cast<float>(fpvHunterTrack.score * 0.75f + candidate.score * 0.25f);
            fpvHunterTrack.hits = (std::min)(fpvHunterTrack.hits + 1, 99);
            fpvHunterTrack.type = candidate.type.isEmpty() ? fpvHunterTrack.type : candidate.type;
        }
        rememberFpvHunterCandidate(candidate, !sameTrack, fpvHunterNowMs);
        fpvHunterTrack.misses = 0;
        fpvHunterTrack.lastSeenSequence = fpvHunterFrameSequence;
        fpvHunterTrack.lastSeenMsec = fpvHunterNowMs;
        if (fpvHunterTrack.firstSeenMsec < 0) {
            fpvHunterTrack.firstSeenMsec = fpvHunterNowMs;
        }
        fpvHunterTrack.stable = fpvHunterTrack.hits >= 3;
    } else if (fpvHunterTrack.valid) {
        ++fpvHunterTrack.misses;
        const bool staleByTime =
            fpvHunterTrack.lastSeenMsec >= 0 &&
            fpvHunterNowMs - fpvHunterTrack.lastSeenMsec > FPV_HUNTER_TRACK_HOLD_MS;
        if (staleByTime || fpvHunterTrack.misses > FPV_HUNTER_TRACK_HOLD_FRAMES) {
            fpvHunterTrack = {};
        } else {
            fpvHunterTrack.stable =
                fpvHunterTrack.hits >= 3 &&
                fpvHunterTrack.misses <= FPV_HUNTER_TRACK_STABLE_MISS_FRAMES;
        }
    }

    QString statusText = fpvHunterLastResult.statusText;
    if (fpvHunterTrack.valid) {
        FpvHunterCandidate tracked;
        tracked.valid = true;
        tracked.centerHz = fpvHunterTrack.centerHz;
        tracked.widthHz = fpvHunterTrack.widthHz;
        tracked.peakDb = fpvHunterTrack.peakDb;
        tracked.averageDb = fpvHunterTrack.averageDb;
        tracked.excessDb = fpvHunterTrack.excessDb;
        tracked.score = fpvHunterTrack.score;
        tracked.type = fpvHunterTrack.type;
        fpvHunterLastResult.best = tracked;

        const int confidence = (std::clamp)(fpvHunterTrack.hits * 18 - fpvHunterTrack.misses * 4,
                                            5,
                                            100);
        const QString state =
            fpvHunterTrack.misses > 0
                ? QStringLiteral("hold")
                : (fpvHunterTrack.stable ? QStringLiteral("stable") : QStringLiteral("tracking"));
        const qint64 ageMs =
            fpvHunterTrack.lastSeenMsec >= 0 ? fpvHunterNowMs - fpvHunterTrack.lastSeenMsec : -1;
        const qint64 durationMs =
            fpvHunterTrack.firstSeenMsec >= 0 && fpvHunterTrack.lastSeenMsec >= fpvHunterTrack.firstSeenMsec
                ? fpvHunterTrack.lastSeenMsec - fpvHunterTrack.firstSeenMsec
                : -1;
        const QString holdText =
            fpvHunterTrack.misses > 0
                ? QStringLiteral(", last seen %1 s ago")
                      .arg(ageMs >= 0 ? ageMs / 1000.0 : 0.0, 0, 'f', 1)
                : QString();
        const QString durationText =
            durationMs >= 1500
                ? QStringLiteral(", seen %1 s").arg(durationMs / 1000.0, 0, 'f', 1)
                : QString();
        statusText =
            QStringLiteral("%1\nFPV Hunter %2: %3 at %4 MHz, width %5 MHz, demod %6 MHz, confidence %7%, hits %8%9%10")
                .arg(fpvHunterLastResult.statusText)
                .arg(state)
                .arg(fpvHunterTrack.type.isEmpty() ? QStringLiteral("wide video") : fpvHunterTrack.type)
                .arg(fpvHunterTrack.centerHz / 1000000.0, 0, 'f', 3)
                .arg(fpvHunterTrack.widthHz / 1000000.0, 0, 'f', 2)
                .arg(recommendedFpvDemodBandwidthHz(fpvHunterTrack.widthHz) / 1000000.0, 0, 'f', 1)
                .arg(confidence)
                .arg(fpvHunterTrack.hits)
                .arg(durationText)
                .arg(holdText);
    }

    if (fpvHunterCandidateIndex >= 0 &&
        fpvHunterCandidateIndex < static_cast<int>(fpvHunterCandidates.size())) {
        const FpvHunterCandidate &candidate =
            fpvHunterCandidates[static_cast<std::size_t>(fpvHunterCandidateIndex)];
        statusText += QStringLiteral("\nSelected %1/%2: %3 at %4 MHz, width %5 MHz, demod %6 MHz, peak %7 dB")
                          .arg(fpvHunterCandidateIndex + 1)
                          .arg(static_cast<int>(fpvHunterCandidates.size()))
                          .arg(candidate.type.isEmpty() ? QStringLiteral("wide video") : candidate.type)
                          .arg(candidate.centerHz / 1000000.0, 0, 'f', 3)
                          .arg(candidate.widthHz / 1000000.0, 0, 'f', 2)
                          .arg(recommendedFpvDemodBandwidthHz(candidate.widthHz) / 1000000.0, 0, 'f', 1)
                          .arg(candidate.peakDb, 0, 'f', 1);
    }

    fpvHunterControls->setStatusText(statusText);
    updateFpvHunterControls();

    if (fpvAutoScanArmed &&
        runState == RadioRunState::Running &&
        !fpvAutoScanController.active()) {
        startFpvAutoScan();
    }
    if (fpvAutoScanController.active()) {
        if (currentDetectedCandidate.valid &&
            std::isfinite(currentDetectedCandidate.centerHz) &&
            fpvHunterTrack.stable) {
            const FpvAutoScanController::Action action =
                fpvAutoScanController.observeCandidate(currentDetectedCandidate.centerHz,
                                                        true,
                                                        fpvHunterNowMs);
            if (action == FpvAutoScanController::Action::LockCandidate) {
                lockFpvAutoScanCandidate(currentDetectedCandidate);
            }
        }
        if (fpvAutoScanController.tick(fpvHunterNowMs) ==
            FpvAutoScanController::Action::ResumeScan) {
            resumeFpvAutoScan();
        }
        updateFpvAutoScanStatus();
    }

    if (fpvHunterFollowEnabled &&
        !fpvAutoScanController.active() &&
        fpvHunterCandidateIndex >= 0 &&
        fpvHunterCandidateIndex < static_cast<int>(fpvHunterCandidates.size())) {
        const FpvHunterCandidate &candidate =
            fpvHunterCandidates[static_cast<std::size_t>(fpvHunterCandidateIndex)];
        const double targetBandwidthHz = recommendedFpvDemodBandwidthHz(candidate.widthHz);
        const double centerThresholdHz =
            fpvHunterTrack.stable
                ? (std::max)(75000.0, targetBandwidthHz * 0.025)
                : (std::max)(150000.0, targetBandwidthHz * 0.05);
        const bool centerChanged =
            !std::isfinite(fpvHunterLastFollowCenterHz) ||
            std::abs(candidate.centerHz - fpvHunterLastFollowCenterHz) > centerThresholdHz;
        const bool bandwidthChanged =
            !std::isfinite(fpvHunterLastFollowBandwidthHz) ||
            std::abs(targetBandwidthHz - fpvHunterLastFollowBandwidthHz) > 500000.0;
        if (centerChanged || bandwidthChanged) {
            tuneFpvHunterCandidateValue(candidate, false);
        }
    }
}

void YourClassName::updateFpvHunterControls() {
    fpvHunterSettings = FpvHunterDetector::normalizedSettings(fpvHunterSettings);
    const bool enabled = fpvHunterSettings.enabled;
    if (fpvHunterControls) {
        fpvHunterControls->setDetectChecked(enabled);
        fpvHunterControls->setWidthValues(fpvHunterSettings.minWidthMhz,
                                          fpvHunterSettings.maxWidthMhz,
                                          fpvHunterSettings.thresholdDb);
        fpvHunterControls->setControlsEnabled(enabled);
        const bool hasCandidate = !fpvHunterCandidates.empty();
        fpvHunterControls->setCandidateNavigationEnabled(enabled && hasCandidate);
        fpvHunterControls->setCandidateIndex(enabled ? fpvHunterCandidateIndex : -1,
                                             enabled ? static_cast<int>(fpvHunterCandidates.size()) : 0);
        fpvHunterControls->setFollowChecked(fpvHunterFollowEnabled);
        fpvHunterControls->setFollowEnabled(enabled && hasCandidate && !fpvAutoScanArmed);
        fpvHunterControls->setTuneEnabled(enabled && (hasCandidate ||
                                                      fpvHunterLastResult.best.valid ||
                                                      fpvHunterTrack.valid));
        fpvHunterControls->setAutoScanChecked(fpvAutoScanArmed);
        fpvHunterControls->setAutoScanValues(fpvAutoScanDwellMs,
                                             fpvAutoScanLossTimeoutMs,
                                             fpvAutoScanMinimumLockPercent);
        if (!enabled) {
            fpvHunterCandidates.clear();
            fpvHunterCandidateIndex = -1;
            fpvHunterLastFollowCenterHz = std::numeric_limits<double>::quiet_NaN();
            fpvHunterLastFollowBandwidthHz = std::numeric_limits<double>::quiet_NaN();
            fpvHunterControls->setCandidateNavigationEnabled(false);
            fpvHunterControls->setCandidateIndex(-1, 0);
            fpvHunterControls->setFollowEnabled(false);
            fpvHunterControls->setStatusText(uiText(QStringLiteral("fpv_hunter_off"),
                                                    QStringLiteral("FPV Hunter: off")));
        }
    }
    updateFpvHunterHistoryControls();
}

void YourClassName::rememberFpvHunterCandidate(const FpvHunterCandidate &candidate,
                                               bool startNewEvent,
                                               qint64 nowMs) {
    if (!candidate.valid || !std::isfinite(candidate.centerHz)) {
        return;
    }
    if (nowMs < 0) {
        nowMs = 0;
    }

    int eventIndex = fpvHunterActiveEventIndex;
    if (startNewEvent || eventIndex < 0 || eventIndex >= fpvHunterEvents.size()) {
        eventIndex = -1;
        for (int i = 0; i < fpvHunterEvents.size(); ++i) {
            const FpvHunterEvent &event = fpvHunterEvents.at(i);
            if (!event.valid || event.lastSeenMsec < 0 ||
                nowMs - event.lastSeenMsec > FPV_HUNTER_TRACK_HOLD_MS) {
                continue;
            }
            const double matchWindowHz =
                (std::max)(2500000.0, (std::max)(candidate.widthHz, event.widthHz) * 0.70);
            if (std::isfinite(event.centerHz) &&
                std::abs(candidate.centerHz - event.centerHz) <= matchWindowHz) {
                eventIndex = i;
                break;
            }
        }
    }

    FpvHunterEvent event;
    if (eventIndex >= 0 && eventIndex < fpvHunterEvents.size()) {
        event = fpvHunterEvents.at(eventIndex);
        constexpr double alpha = 0.30;
        event.centerHz = event.centerHz * (1.0 - alpha) + candidate.centerHz * alpha;
        event.widthHz = event.widthHz * (1.0 - alpha) + candidate.widthHz * alpha;
        event.peakDb = (std::max)(event.peakDb, candidate.peakDb);
        event.averageDb =
            static_cast<float>(event.averageDb * (1.0 - alpha) + candidate.averageDb * alpha);
        event.excessDb =
            static_cast<float>(event.excessDb * (1.0 - alpha) + candidate.excessDb * alpha);
        event.score = (std::max)(event.score, candidate.score);
        event.hits = (std::min)(event.hits + 1, 999);
        event.lastSeenMsec = nowMs;
        if (!candidate.type.isEmpty()) {
            event.type = candidate.type;
        }
        fpvHunterEvents.removeAt(eventIndex);
    } else {
        event.valid = true;
        event.id = fpvHunterNextEventId++;
        event.centerHz = candidate.centerHz;
        event.widthHz = candidate.widthHz;
        event.peakDb = candidate.peakDb;
        event.averageDb = candidate.averageDb;
        event.excessDb = candidate.excessDb;
        event.score = candidate.score;
        event.hits = 1;
        event.firstSeenMsec = nowMs;
        event.lastSeenMsec = nowMs;
        event.type = candidate.type;
    }

    fpvHunterEvents.prepend(event);
    while (fpvHunterEvents.size() > FPV_HUNTER_MAX_EVENTS) {
        fpvHunterEvents.removeLast();
    }
    fpvHunterActiveEventIndex = 0;
    updateFpvHunterHistoryControls();
}

void YourClassName::updateFpvHunterHistoryControls() {
    const bool hasEvents = !fpvHunterEvents.isEmpty();
    if (fpvHunterHistoryCombo) {
        QSignalBlocker blocker(fpvHunterHistoryCombo);
        fpvHunterHistoryCombo->clear();
        if (!hasEvents) {
            fpvHunterHistoryCombo->addItem(QStringLiteral("No FPV events yet"), -1);
        } else {
            const qint64 nowMs = fpvHunterClock.isValid() ? fpvHunterClock.elapsed() : -1;
            for (int i = 0; i < fpvHunterEvents.size(); ++i) {
                const FpvHunterEvent &event = fpvHunterEvents.at(i);
                const double durationSec =
                    event.firstSeenMsec >= 0 && event.lastSeenMsec >= event.firstSeenMsec
                        ? (event.lastSeenMsec - event.firstSeenMsec) / 1000.0
                        : 0.0;
                const double ageSec =
                    nowMs >= 0 && event.lastSeenMsec >= 0
                        ? (std::max)(static_cast<qint64>(0), nowMs - event.lastSeenMsec) / 1000.0
                        : 0.0;
                const QString item =
                    QStringLiteral("%1 MHz, W %2 MHz, pk %3 dB, %4, %5 s, age %6 s")
                        .arg(event.centerHz / 1000000.0, 0, 'f', 3)
                        .arg(event.widthHz / 1000000.0, 0, 'f', 2)
                        .arg(event.peakDb, 0, 'f', 1)
                        .arg(event.type.isEmpty() ? QStringLiteral("wide video") : event.type)
                        .arg(durationSec, 0, 'f', 1)
                        .arg(ageSec, 0, 'f', 1);
                fpvHunterHistoryCombo->addItem(item, i);
            }
        }
    }
    if (fpvHunterHistoryTuneButton) {
        fpvHunterHistoryTuneButton->setEnabled(hasEvents);
    }
    if (fpvHunterHistoryClearButton) {
        fpvHunterHistoryClearButton->setEnabled(hasEvents);
    }
}

void YourClassName::tuneFpvHunterHistorySelection() {
    if (!fpvHunterHistoryCombo || fpvHunterEvents.isEmpty()) {
        return;
    }
    bool ok = false;
    int index = fpvHunterHistoryCombo->currentData().toInt(&ok);
    if (!ok || index < 0 || index >= fpvHunterEvents.size()) {
        index = fpvHunterHistoryCombo->currentIndex();
    }
    if (index < 0 || index >= fpvHunterEvents.size()) {
        return;
    }

    const FpvHunterEvent &event = fpvHunterEvents.at(index);
    if (!event.valid || !std::isfinite(event.centerHz)) {
        return;
    }

    FpvHunterCandidate candidate;
    candidate.valid = true;
    candidate.centerHz = event.centerHz;
    candidate.widthHz = event.widthHz;
    candidate.peakDb = event.peakDb;
    candidate.averageDb = event.averageDb;
    candidate.excessDb = event.excessDb;
    candidate.score = event.score;
    candidate.type = event.type;

    fpvHunterActiveEventIndex = index;
    fpvHunterLastResult.best = candidate;
    fpvHunterCandidates = {candidate};
    fpvHunterCandidateIndex = 0;
    tuneFpvHunterCandidateValue(candidate, true);
    updateFpvHunterControls();
}

void YourClassName::clearFpvHunterHistory() {
    fpvHunterEvents.clear();
    fpvHunterActiveEventIndex = -1;
    updateFpvHunterHistoryControls();
}

void YourClassName::applyFpvHunterPresetToScan() {
    if (!fpvHunterControls) {
        return;
    }

    const QString spec = fpvHunterControls->currentPresetSpec();
    const QString ranges = agileScanPresetRanges(spec);
    const double step = agileScanPresetStepMhz(spec, 5.0);
    if (ranges.isEmpty()) {
        return;
    }

    const bool useNativeAgile = activeFobosApiKind == FobosApiKind::Agile &&
                                (isIdle() || agileScanRunning);
    if (useNativeAgile) {
        agileScanRangesMhz = ranges;
        agileScanStepMhz = step;
        agileScanAutoStepSampleRate = false;
        agileScanEnabled = true;
        standardScanEnabled = false;
        if (agileScanCheckbox) {
            const QSignalBlocker blocker(agileScanCheckbox);
            agileScanCheckbox->setChecked(true);
        }
        if (standardScanCheckbox) {
            const QSignalBlocker blocker(standardScanCheckbox);
            standardScanCheckbox->setChecked(false);
        }
        if (agileScanRangesEdit) {
            const QSignalBlocker blocker(agileScanRangesEdit);
            agileScanRangesEdit->setText(agileScanRangesMhz);
        }
        if (agileScanStepSpin) {
            const QSignalBlocker blocker(agileScanStepSpin);
            agileScanStepSpin->setValue(agileScanStepMhz);
        }
    } else {
        const double coverageStepMhz = (std::max)(step, pendingSettings.sampleRate / 1000000.0);
        QString error;
        const QVector<double> centers = parseAgileScanFrequenciesMhz(ranges, coverageStepMhz, &error);
        if (!error.isEmpty() || centers.size() < AGILE_SCAN_MIN_POINTS) {
            fpvHunterControls->setAutoScanStatusText(
                error.isEmpty() ? QStringLiteral("FPV scan needs at least two centers") : error);
            return;
        }
        standardScanCentersMhz = formatMhzList(centers);
        standardScanEnabled = true;
        agileScanEnabled = false;
        if (standardScanCheckbox) {
            const QSignalBlocker blocker(standardScanCheckbox);
            standardScanCheckbox->setChecked(true);
        }
        if (agileScanCheckbox) {
            const QSignalBlocker blocker(agileScanCheckbox);
            agileScanCheckbox->setChecked(false);
        }
        if (standardScanCentersEdit) {
            const QSignalBlocker blocker(standardScanCentersEdit);
            standardScanCentersEdit->setText(standardScanCentersMhz);
        }
    }
    updateAgileScanControls();
    if (runState == RadioRunState::Running) {
        if (useNativeAgile) {
            applyAgileScanSettings(false);
        } else {
            applyStandardScanSettings(false);
        }
    }
    savePersistentSettings();
}

void YourClassName::startFpvAutoScan() {
    if (!fpvAutoScanArmed || fpvAutoScanController.active() ||
        runState != RadioRunState::Running || pendingSettings.inputMode != INPUT_RF) {
        updateFpvAutoScanStatus();
        return;
    }

    if (!fpvHunterClock.isValid()) {
        fpvHunterClock.start();
    }
    fpvAutoScanSnapshot.valid = true;
    fpvAutoScanSnapshot.agileEnabled = agileScanEnabled;
    fpvAutoScanSnapshot.standardEnabled = standardScanEnabled;
    fpvAutoScanSnapshot.agileAutoStep = agileScanAutoStepSampleRate;
    fpvAutoScanSnapshot.agileRangesMhz = agileScanRangesMhz;
    fpvAutoScanSnapshot.agileStepMhz = agileScanStepMhz;
    fpvAutoScanSnapshot.standardCentersMhz = standardScanCentersMhz;
    fpvAutoScanSnapshot.standardDwellMs = standardScanDwellMs;

    FpvAutoScanController::Settings settings;
    settings.verificationTimeoutMs = (std::max)(3500, fpvAutoScanDwellMs * 3);
    settings.signalLossTimeoutMs = fpvAutoScanLossTimeoutMs;
    settings.minimumHsyncPercent = fpvAutoScanMinimumLockPercent;
    fpvAutoScanController.setSettings(settings);
    fpvAutoScanController.start(fpvHunterClock.elapsed());
    fpvAutoScanBackend = agileScanRunning
                             ? FpvAutoScanBackend::AgileNative
                             : FpvAutoScanBackend::StandardRetune;

    fpvHunterSettings.enabled = true;
    fpvHunterFollowEnabled = false;
    if (!configureFpvAutoScanBackend(true)) {
        stopFpvAutoScan(true);
        if (fpvHunterControls) {
            fpvHunterControls->setAutoScanStatusText(QStringLiteral("Auto scan could not start"));
        }
        return;
    }
    fpvHunterTrack = {};
    updateFpvHunterControls();
    updateFpvAutoScanStatus();
}

bool YourClassName::configureFpvAutoScanBackend(bool startScanning) {
    if (!fpvHunterControls || fpvAutoScanBackend == FpvAutoScanBackend::None) {
        return false;
    }
    const QString spec = fpvHunterControls->currentPresetSpec();
    const QString ranges = agileScanPresetRanges(spec);
    const double presetStepMhz = agileScanPresetStepMhz(spec, 5.0);
    if (ranges.isEmpty()) {
        return false;
    }

    if (fpvAutoScanBackend == FpvAutoScanBackend::AgileNative) {
        standardScanEnabled = false;
        resetStandardScanState(true);
        agileScanRangesMhz = ranges;
        agileScanStepMhz = (std::max)(presetStepMhz, pendingSettings.sampleRate / 1000000.0);
        agileScanAutoStepSampleRate = false;
        agileScanEnabled = startScanning;
        if (agileScanRangesEdit) {
            const QSignalBlocker blocker(agileScanRangesEdit);
            agileScanRangesEdit->setText(agileScanRangesMhz);
        }
        if (agileScanStepSpin) {
            const QSignalBlocker blocker(agileScanStepSpin);
            agileScanStepSpin->setValue(agileScanStepMhz);
        }
        if (agileScanCheckbox) {
            const QSignalBlocker blocker(agileScanCheckbox);
            agileScanCheckbox->setChecked(startScanning);
        }
        if (standardScanCheckbox) {
            const QSignalBlocker blocker(standardScanCheckbox);
            standardScanCheckbox->setChecked(false);
        }
        updateAgileScanControls();
        return startScanning ? applyAgileScanSettings(false) : applyAgileScanSettings(true);
    }

    if (agileScanRunning) {
        applyAgileScanSettings(true);
    }
    agileScanEnabled = false;
    const double coverageStepMhz =
        (std::max)(presetStepMhz, pendingSettings.sampleRate / 1000000.0);
    QString error;
    const QVector<double> centers = parseAgileScanFrequenciesMhz(ranges, coverageStepMhz, &error);
    if (!error.isEmpty() || centers.size() < AGILE_SCAN_MIN_POINTS) {
        if (fpvHunterControls) {
            fpvHunterControls->setAutoScanStatusText(
                error.isEmpty() ? QStringLiteral("FPV scan needs at least two centers") : error);
        }
        return false;
    }

    standardScanCentersMhz = formatMhzList(centers);
    standardScanDwellMs = fpvAutoScanDwellMs;
    standardScanEnabled = startScanning;
    if (standardScanCentersEdit) {
        const QSignalBlocker blocker(standardScanCentersEdit);
        standardScanCentersEdit->setText(standardScanCentersMhz);
    }
    if (standardScanDwellSpin) {
        const QSignalBlocker blocker(standardScanDwellSpin);
        standardScanDwellSpin->setValue(standardScanDwellMs);
    }
    if (standardScanCheckbox) {
        const QSignalBlocker blocker(standardScanCheckbox);
        standardScanCheckbox->setChecked(startScanning);
    }
    if (agileScanCheckbox) {
        const QSignalBlocker blocker(agileScanCheckbox);
        agileScanCheckbox->setChecked(false);
    }
    updateAgileScanControls();
    return startScanning ? applyStandardScanSettings(false) : applyStandardScanSettings(true);
}

void YourClassName::lockFpvAutoScanCandidate(const FpvHunterCandidate &candidate) {
    if (!fpvAutoScanController.active() || !candidate.valid) {
        return;
    }
    bool stopped = true;
    if (fpvAutoScanBackend == FpvAutoScanBackend::AgileNative) {
        stopped = applyAgileScanSettings(true);
        agileScanEnabled = false;
        if (agileScanCheckbox) {
            const QSignalBlocker blocker(agileScanCheckbox);
            agileScanCheckbox->setChecked(false);
        }
    } else if (fpvAutoScanBackend == FpvAutoScanBackend::StandardRetune) {
        standardScanEnabled = false;
        stopped = applyStandardScanSettings(true);
        if (standardScanCheckbox) {
            const QSignalBlocker blocker(standardScanCheckbox);
            standardScanCheckbox->setChecked(false);
        }
    }
    if (!stopped) {
        fpvAutoScanController.candidateTuneFailed(fpvHunterClock.elapsed());
        resumeFpvAutoScan();
        return;
    }
    if (videoHSyncCheckbox) {
        const QSignalBlocker blocker(videoHSyncCheckbox);
        videoHSyncCheckbox->setChecked(true);
    }
    if (videoVSyncCheckbox) {
        const QSignalBlocker blocker(videoVSyncCheckbox);
        videoVSyncCheckbox->setChecked(true);
    }
    tuneFpvHunterCandidateValue(candidate, false);
    updateFpvAutoScanStatus();
}

void YourClassName::resumeFpvAutoScan() {
    if (!fpvAutoScanArmed || !fpvAutoScanController.active() ||
        runState != RadioRunState::Running) {
        return;
    }
    fpvHunterTrack = {};
    fpvHunterCandidates.clear();
    fpvHunterCandidateIndex = -1;
    configureFpvAutoScanBackend(true);
    updateFpvAutoScanStatus();
}

void YourClassName::stopFpvAutoScan(bool restorePreviousScan) {
    if (fpvAutoScanBackend == FpvAutoScanBackend::AgileNative) {
        applyAgileScanSettings(true);
    } else if (fpvAutoScanBackend == FpvAutoScanBackend::StandardRetune) {
        applyStandardScanSettings(true);
    }
    fpvAutoScanController.stop();
    fpvAutoScanBackend = FpvAutoScanBackend::None;

    if (restorePreviousScan && fpvAutoScanSnapshot.valid) {
        agileScanEnabled = fpvAutoScanSnapshot.agileEnabled;
        standardScanEnabled = fpvAutoScanSnapshot.standardEnabled;
        agileScanAutoStepSampleRate = fpvAutoScanSnapshot.agileAutoStep;
        agileScanRangesMhz = fpvAutoScanSnapshot.agileRangesMhz;
        agileScanStepMhz = fpvAutoScanSnapshot.agileStepMhz;
        standardScanCentersMhz = fpvAutoScanSnapshot.standardCentersMhz;
        standardScanDwellMs = fpvAutoScanSnapshot.standardDwellMs;
        if (agileScanCheckbox) {
            const QSignalBlocker blocker(agileScanCheckbox);
            agileScanCheckbox->setChecked(agileScanEnabled);
        }
        if (standardScanCheckbox) {
            const QSignalBlocker blocker(standardScanCheckbox);
            standardScanCheckbox->setChecked(standardScanEnabled);
        }
        if (agileScanRangesEdit) {
            const QSignalBlocker blocker(agileScanRangesEdit);
            agileScanRangesEdit->setText(agileScanRangesMhz);
        }
        if (agileScanStepSpin) {
            const QSignalBlocker blocker(agileScanStepSpin);
            agileScanStepSpin->setValue(agileScanStepMhz);
        }
        if (standardScanCentersEdit) {
            const QSignalBlocker blocker(standardScanCentersEdit);
            standardScanCentersEdit->setText(standardScanCentersMhz);
        }
        if (standardScanDwellSpin) {
            const QSignalBlocker blocker(standardScanDwellSpin);
            standardScanDwellSpin->setValue(standardScanDwellMs);
        }
        if (runState == RadioRunState::Running) {
            if (agileScanEnabled) {
                applyAgileScanSettings(false);
            } else if (standardScanEnabled) {
                applyStandardScanSettings(false);
            }
        }
    }
    fpvAutoScanSnapshot = {};
    updateAgileScanControls();
    updateFpvAutoScanStatus();
}

void YourClassName::handleFpvAnalogLockMetrics(int hsyncPercent,
                                               bool verticalSync,
                                               double levelSpan) {
    Q_UNUSED(levelSpan)
    if (!fpvAutoScanController.active()) {
        return;
    }
    if (!fpvHunterClock.isValid()) {
        fpvHunterClock.start();
    }
    fpvAutoScanController.observeVideoLock(hsyncPercent,
                                           verticalSync,
                                           fpvHunterClock.elapsed());
    updateFpvAutoScanStatus();
}

void YourClassName::updateFpvAutoScanStatus() {
    if (!fpvHunterControls) {
        return;
    }
    if (!fpvAutoScanArmed) {
        fpvHunterControls->setAutoScanStatusText(QStringLiteral("Auto scan: off"));
        return;
    }
    if (runState != RadioRunState::Running && !fpvAutoScanController.active()) {
        fpvHunterControls->setAutoScanStatusText(QStringLiteral("Auto scan: armed; press Start"));
        return;
    }

    const QString backend = fpvAutoScanBackend == FpvAutoScanBackend::AgileNative
                                ? QStringLiteral("Agile scan")
                                : QStringLiteral("live retune");
    switch (fpvAutoScanController.state()) {
    case FpvAutoScanController::State::Scanning:
        fpvHunterControls->setAutoScanStatusText(
            QStringLiteral("Auto scan: searching (%1)").arg(backend));
        break;
    case FpvAutoScanController::State::VerifyingVideo:
        fpvHunterControls->setAutoScanStatusText(
            QStringLiteral("Auto scan: verifying %1 MHz, HSync %2%")
                .arg(fpvAutoScanController.lockedCenterHz() / 1000000.0, 0, 'f', 3)
                .arg(fpvAutoScanController.lastHsyncPercent()));
        break;
    case FpvAutoScanController::State::Decoding:
        fpvHunterControls->setAutoScanStatusText(
            QStringLiteral("Auto scan: video locked at %1 MHz, HSync %2%")
                .arg(fpvAutoScanController.lockedCenterHz() / 1000000.0, 0, 'f', 3)
                .arg(fpvAutoScanController.lastHsyncPercent()));
        break;
    case FpvAutoScanController::State::Idle:
        fpvHunterControls->setAutoScanStatusText(QStringLiteral("Auto scan: ready"));
        break;
    }
}

void YourClassName::tuneFpvHunterCandidate() {
    if (fpvHunterCandidateIndex >= 0 &&
        fpvHunterCandidateIndex < static_cast<int>(fpvHunterCandidates.size())) {
        tuneFpvHunterCandidateIndex(fpvHunterCandidateIndex);
        return;
    }

    if (!fpvHunterLastResult.best.valid || !std::isfinite(fpvHunterLastResult.best.centerHz)) {
        return;
    }

    FpvHunterCandidate candidate = fpvHunterLastResult.best;
    fpvHunterCandidates = {candidate};
    fpvHunterCandidateIndex = 0;
    tuneFpvHunterCandidateIndex(0);
}

void YourClassName::selectFpvHunterCandidate(int direction) {
    if (fpvHunterCandidates.empty()) {
        updateFpvHunterControls();
        return;
    }

    const int count = static_cast<int>(fpvHunterCandidates.size());
    int nextIndex = fpvHunterCandidateIndex;
    if (nextIndex < 0 || nextIndex >= count) {
        nextIndex = direction < 0 ? count - 1 : 0;
    } else {
        nextIndex = (nextIndex + direction) % count;
        if (nextIndex < 0) {
            nextIndex += count;
        }
    }

    fpvHunterCandidateIndex = nextIndex;
    updateFpvHunterControls();
    tuneFpvHunterCandidateIndex(fpvHunterCandidateIndex);
}

void YourClassName::tuneFpvHunterCandidateIndex(int index) {
    if (index < 0 || index >= static_cast<int>(fpvHunterCandidates.size())) {
        return;
    }

    const FpvHunterCandidate &candidate = fpvHunterCandidates[static_cast<std::size_t>(index)];
    tuneFpvHunterCandidateValue(candidate, true);
}

void YourClassName::tuneFpvHunterCandidateValue(const FpvHunterCandidate &candidate, bool saveSettings) {
    if (!candidate.valid || !std::isfinite(candidate.centerHz)) {
        return;
    }

    if (pendingSettings.modulationType != MOD_ATV) {
        if (modulationButtonGroup) {
            if (QAbstractButton *button = modulationButtonGroup->button(MOD_ATV)) {
                modulationButtonGroup->blockSignals(true);
                button->setChecked(true);
                modulationButtonGroup->blockSignals(false);
            }
        }
        onModulationChanged(MOD_ATV);
    }

    videoDecodeEnabled = true;
    if (videoDecodeCheckbox) {
        QSignalBlocker blocker(videoDecodeCheckbox);
        videoDecodeCheckbox->setChecked(true);
    }
    if (videoDemodCombo && videoDemodCombo->currentIndex() != 0) {
        QSignalBlocker blocker(videoDemodCombo);
        videoDemodCombo->setCurrentIndex(0);
    }
    if (videoToggleButton && !videoToggleButton->isChecked()) {
        QSignalBlocker blocker(videoToggleButton);
        videoToggleButton->setChecked(true);
    }
    if (videoDock && !videoDock->isVisible()) {
        videoDock->show();
    }

    const double videoBandwidthHz = recommendedFpvDemodBandwidthHz(candidate.widthHz);
    pendingSettings.bandwidth = videoBandwidthHz;
    if (bandwidthControl) {
        QSignalBlocker blocker(bandwidthControl);
        bandwidthControl->setValueHz(pendingSettings.bandwidth);
    }
    publishSettingsToGlobals();
    updateVideoProcessorMode();
    updateIqFrameProducerSettings();
    settingRange();
    updateTuningFromScale(candidate.centerHz, candidate.centerHz);
    fpvHunterLastFollowCenterHz = candidate.centerHz;
    fpvHunterLastFollowBandwidthHz = videoBandwidthHz;
    if (saveSettings) {
        savePersistentSettings();
    }
}

void YourClassName::updateDigitalVideoHunter(const std::vector<float> &frequencies,
                                             const std::vector<float> &magnitudes) {
    if (!digitalVideoHunterControls) {
        return;
    }

    double previousSelectedCenterHz = std::numeric_limits<double>::quiet_NaN();
    if (digitalVideoHunterCandidateIndex >= 0 &&
        digitalVideoHunterCandidateIndex < static_cast<int>(digitalVideoHunterCandidates.size())) {
        previousSelectedCenterHz =
            digitalVideoHunterCandidates[static_cast<std::size_t>(digitalVideoHunterCandidateIndex)].centerHz;
    }

    digitalVideoHunterSettings = DigitalVideoHunterDetector::normalizedSettings(digitalVideoHunterSettings);
    digitalVideoHunterLastResult = DigitalVideoHunterDetector::analyze(frequencies,
                                                                       magnitudes,
                                                                       digitalVideoHunterSettings);
    digitalVideoHunterCandidates = digitalVideoHunterLastResult.candidateList;
    digitalVideoHunterCandidateIndex = -1;
    if (!digitalVideoHunterCandidates.empty()) {
        int bestIndex = 0;
        if (std::isfinite(previousSelectedCenterHz)) {
            double bestDeltaHz = std::numeric_limits<double>::max();
            for (int i = 0; i < static_cast<int>(digitalVideoHunterCandidates.size()); ++i) {
                const double centerHz = digitalVideoHunterCandidates[static_cast<std::size_t>(i)].centerHz;
                if (!std::isfinite(centerHz)) {
                    continue;
                }
                const double deltaHz = std::abs(centerHz - previousSelectedCenterHz);
                if (deltaHz < bestDeltaHz) {
                    bestDeltaHz = deltaHz;
                    bestIndex = i;
                }
            }
        }
        digitalVideoHunterCandidateIndex = bestIndex;
    }

    QString statusText = digitalVideoHunterLastResult.statusText;
    if (digitalVideoHunterCandidateIndex >= 0 &&
        digitalVideoHunterCandidateIndex < static_cast<int>(digitalVideoHunterCandidates.size())) {
        const DigitalVideoHunterCandidate &candidate =
            digitalVideoHunterCandidates[static_cast<std::size_t>(digitalVideoHunterCandidateIndex)];
        statusText += QStringLiteral("\nSelected %1/%2: %3 at %4 MHz, width %5 MHz, BW %6 MHz, flat %7 dB, occ %8%, peak %9 dB")
                          .arg(digitalVideoHunterCandidateIndex + 1)
                          .arg(static_cast<int>(digitalVideoHunterCandidates.size()))
                          .arg(candidate.type.isEmpty() ? QStringLiteral("wide digital") : candidate.type)
                          .arg(candidate.centerHz / 1000000.0, 0, 'f', 3)
                          .arg(candidate.widthHz / 1000000.0, 0, 'f', 2)
                          .arg(recommendedDigitalVideoBandwidthHz(candidate.widthHz) / 1000000.0, 0, 'f', 1)
                          .arg(candidate.flatnessDb, 0, 'f', 1)
                          .arg(candidate.occupancy * 100.0f, 0, 'f', 0)
                          .arg(candidate.peakDb, 0, 'f', 1);
    }
    digitalVideoHunterControls->setStatusText(statusText);
    updateDigitalVideoHunterControls();
}

void YourClassName::updateDigitalVideoHunterControls() {
    digitalVideoHunterSettings = DigitalVideoHunterDetector::normalizedSettings(digitalVideoHunterSettings);
    const bool enabled = digitalVideoHunterSettings.enabled;
    if (digitalVideoHunterControls) {
        digitalVideoHunterControls->setDetectChecked(enabled);
        digitalVideoHunterControls->setWidthValues(digitalVideoHunterSettings.minWidthMhz,
                                                   digitalVideoHunterSettings.maxWidthMhz,
                                                   digitalVideoHunterSettings.thresholdDb);
        digitalVideoHunterControls->setControlsEnabled(enabled);
        const bool hasCandidate = !digitalVideoHunterCandidates.empty();
        digitalVideoHunterControls->setCandidateNavigationEnabled(enabled && hasCandidate);
        digitalVideoHunterControls->setCandidateIndex(enabled ? digitalVideoHunterCandidateIndex : -1,
                                                      enabled ? static_cast<int>(digitalVideoHunterCandidates.size()) : 0);
        digitalVideoHunterControls->setTuneEnabled(enabled && (hasCandidate || digitalVideoHunterLastResult.best.valid));
        if (!enabled) {
            digitalVideoHunterCandidates.clear();
            digitalVideoHunterCandidateIndex = -1;
            digitalVideoHunterControls->setCandidateNavigationEnabled(false);
            digitalVideoHunterControls->setCandidateIndex(-1, 0);
            digitalVideoHunterControls->setStatusText(uiText(QStringLiteral("digital_video_hunter_off"),
                                                             QStringLiteral("Digital Video Hunter: off")));
        }
    }
}

void YourClassName::applyDigitalVideoHunterPresetToScan() {
    if (!digitalVideoHunterControls || !agileScanRangesEdit || !agileScanStepSpin) {
        return;
    }

    const QString spec = digitalVideoHunterControls->currentPresetSpec();
    const QString ranges = agileScanPresetRanges(spec);
    const double step = agileScanPresetStepMhz(spec, 5.0);
    if (ranges.isEmpty()) {
        return;
    }

    agileScanRangesMhz = ranges;
    agileScanStepMhz = step;
    agileScanEnabled = true;
    if (agileScanCheckbox) {
        QSignalBlocker blocker(agileScanCheckbox);
        agileScanCheckbox->setChecked(true);
    }
    {
        QSignalBlocker blocker(agileScanRangesEdit);
        agileScanRangesEdit->setText(agileScanRangesMhz);
    }
    {
        QSignalBlocker blocker(agileScanStepSpin);
        agileScanStepSpin->setValue(agileScanStepMhz);
    }
    updateAgileScanControls();
    savePersistentSettings();
}

void YourClassName::tuneDigitalVideoHunterCandidate() {
    if (digitalVideoHunterCandidateIndex >= 0 &&
        digitalVideoHunterCandidateIndex < static_cast<int>(digitalVideoHunterCandidates.size())) {
        tuneDigitalVideoHunterCandidateIndex(digitalVideoHunterCandidateIndex);
        return;
    }

    if (!digitalVideoHunterLastResult.best.valid ||
        !std::isfinite(digitalVideoHunterLastResult.best.centerHz)) {
        return;
    }

    DigitalVideoHunterCandidate candidate = digitalVideoHunterLastResult.best;
    digitalVideoHunterCandidates = {candidate};
    digitalVideoHunterCandidateIndex = 0;
    tuneDigitalVideoHunterCandidateIndex(0);
}

void YourClassName::selectDigitalVideoHunterCandidate(int direction) {
    if (digitalVideoHunterCandidates.empty()) {
        updateDigitalVideoHunterControls();
        return;
    }

    const int count = static_cast<int>(digitalVideoHunterCandidates.size());
    int nextIndex = digitalVideoHunterCandidateIndex;
    if (nextIndex < 0 || nextIndex >= count) {
        nextIndex = direction < 0 ? count - 1 : 0;
    } else {
        nextIndex = (nextIndex + direction) % count;
        if (nextIndex < 0) {
            nextIndex += count;
        }
    }

    digitalVideoHunterCandidateIndex = nextIndex;
    updateDigitalVideoHunterControls();
    tuneDigitalVideoHunterCandidateIndex(digitalVideoHunterCandidateIndex);
}

void YourClassName::tuneDigitalVideoHunterCandidateIndex(int index) {
    if (index < 0 || index >= static_cast<int>(digitalVideoHunterCandidates.size())) {
        return;
    }

    const DigitalVideoHunterCandidate &candidate =
        digitalVideoHunterCandidates[static_cast<std::size_t>(index)];
    tuneDigitalVideoHunterCandidateValue(candidate, true);
}

void YourClassName::tuneDigitalVideoHunterCandidateValue(const DigitalVideoHunterCandidate &candidate,
                                                        bool saveSettings) {
    if (!candidate.valid || !std::isfinite(candidate.centerHz)) {
        return;
    }

    pendingSettings.bandwidth = recommendedDigitalVideoBandwidthHz(candidate.widthHz);
    if (bandwidthControl) {
        QSignalBlocker blocker(bandwidthControl);
        bandwidthControl->setValueHz(pendingSettings.bandwidth);
    }

    publishSettingsToGlobals();
    updateIqFrameProducerSettings();
    settingRange();
    updateTuningFromScale(candidate.centerHz, candidate.centerHz);
    if (saveSettings) {
        savePersistentSettings();
    }
}
