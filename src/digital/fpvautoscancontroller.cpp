#include "fpvautoscancontroller.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double SAME_CANDIDATE_WINDOW_HZ = 2000000.0;
constexpr int REJECTED_CANDIDATE_COOLDOWN_MS = 1500;
}

FpvAutoScanController::Settings
FpvAutoScanController::normalizedSettings(Settings settings) {
    settings.verificationTimeoutMs =
        (std::clamp)(settings.verificationTimeoutMs, 500, 15000);
    settings.signalLossTimeoutMs =
        (std::clamp)(settings.signalLossTimeoutMs, 500, 30000);
    settings.minimumHsyncPercent =
        (std::clamp)(settings.minimumHsyncPercent, 1, 100);
    return settings;
}

void FpvAutoScanController::setSettings(const Settings &settings) {
    settings_ = normalizedSettings(settings);
}

void FpvAutoScanController::start(std::int64_t nowMs) {
    state_ = State::Scanning;
    stateSinceMs_ = nowMs;
    lockedCenterHz_ = 0.0;
    lastGoodVideoMs_ = -1;
    lastHsyncPercent_ = 0;
}

void FpvAutoScanController::stop() {
    state_ = State::Idle;
    stateSinceMs_ = 0;
    lockedCenterHz_ = 0.0;
    lastGoodVideoMs_ = -1;
    rejectedCenterUntilMs_ = -1;
    rejectedCenterHz_ = 0.0;
    lastHsyncPercent_ = 0;
}

FpvAutoScanController::Action
FpvAutoScanController::observeCandidate(double centerHz,
                                        bool stable,
                                        std::int64_t nowMs) {
    if (state_ != State::Scanning || !stable || !std::isfinite(centerHz) || centerHz <= 0.0) {
        return Action::None;
    }
    if (nowMs < rejectedCenterUntilMs_ &&
        std::abs(centerHz - rejectedCenterHz_) <= SAME_CANDIDATE_WINDOW_HZ) {
        return Action::None;
    }

    lockedCenterHz_ = centerHz;
    state_ = State::VerifyingVideo;
    stateSinceMs_ = nowMs;
    lastGoodVideoMs_ = -1;
    lastHsyncPercent_ = 0;
    return Action::LockCandidate;
}

void FpvAutoScanController::candidateTuneFailed(std::int64_t nowMs) {
    rejectedCenterHz_ = lockedCenterHz_;
    rejectedCenterUntilMs_ = nowMs + REJECTED_CANDIDATE_COOLDOWN_MS;
    state_ = State::Scanning;
    stateSinceMs_ = nowMs;
    lockedCenterHz_ = 0.0;
    lastGoodVideoMs_ = -1;
    lastHsyncPercent_ = 0;
}

void FpvAutoScanController::observeVideoLock(int hsyncPercent,
                                             bool verticalSync,
                                             std::int64_t nowMs) {
    if (state_ != State::VerifyingVideo && state_ != State::Decoding) {
        return;
    }
    lastHsyncPercent_ = (std::clamp)(hsyncPercent, 0, 100);
    if (!verticalSync && lastHsyncPercent_ < settings_.minimumHsyncPercent) {
        return;
    }

    lastGoodVideoMs_ = nowMs;
    if (state_ == State::VerifyingVideo) {
        state_ = State::Decoding;
        stateSinceMs_ = nowMs;
    }
}

FpvAutoScanController::Action FpvAutoScanController::tick(std::int64_t nowMs) {
    if (state_ == State::VerifyingVideo &&
        nowMs - stateSinceMs_ >= settings_.verificationTimeoutMs) {
        rejectedCenterHz_ = lockedCenterHz_;
        rejectedCenterUntilMs_ = nowMs + REJECTED_CANDIDATE_COOLDOWN_MS;
        state_ = State::Scanning;
        stateSinceMs_ = nowMs;
        lockedCenterHz_ = 0.0;
        lastGoodVideoMs_ = -1;
        lastHsyncPercent_ = 0;
        return Action::ResumeScan;
    }

    if (state_ == State::Decoding &&
        lastGoodVideoMs_ >= 0 &&
        nowMs - lastGoodVideoMs_ >= settings_.signalLossTimeoutMs) {
        rejectedCenterHz_ = lockedCenterHz_;
        rejectedCenterUntilMs_ = nowMs + REJECTED_CANDIDATE_COOLDOWN_MS;
        state_ = State::Scanning;
        stateSinceMs_ = nowMs;
        lockedCenterHz_ = 0.0;
        lastGoodVideoMs_ = -1;
        lastHsyncPercent_ = 0;
        return Action::ResumeScan;
    }
    return Action::None;
}
