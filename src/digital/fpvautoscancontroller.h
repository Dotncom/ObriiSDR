#ifndef FPVAUTOSCANCONTROLLER_H
#define FPVAUTOSCANCONTROLLER_H

#include <cstdint>

class FpvAutoScanController {
public:
    enum class State {
        Idle,
        Scanning,
        VerifyingVideo,
        Decoding
    };

    enum class Action {
        None,
        LockCandidate,
        ResumeScan
    };

    struct Settings {
        int verificationTimeoutMs = 3500;
        int signalLossTimeoutMs = 2500;
        int minimumHsyncPercent = 45;
    };

    void setSettings(const Settings &settings);
    const Settings &settings() const { return settings_; }

    void start(std::int64_t nowMs);
    void stop();
    Action observeCandidate(double centerHz, bool stable, std::int64_t nowMs);
    void candidateTuneFailed(std::int64_t nowMs);
    void observeVideoLock(int hsyncPercent, bool verticalSync, std::int64_t nowMs);
    Action tick(std::int64_t nowMs);

    State state() const { return state_; }
    bool active() const { return state_ != State::Idle; }
    double lockedCenterHz() const { return lockedCenterHz_; }
    int lastHsyncPercent() const { return lastHsyncPercent_; }

private:
    static Settings normalizedSettings(Settings settings);

    Settings settings_;
    State state_ = State::Idle;
    double lockedCenterHz_ = 0.0;
    std::int64_t stateSinceMs_ = 0;
    std::int64_t lastGoodVideoMs_ = -1;
    std::int64_t rejectedCenterUntilMs_ = -1;
    double rejectedCenterHz_ = 0.0;
    int lastHsyncPercent_ = 0;
};

#endif // FPVAUTOSCANCONTROLLER_H
