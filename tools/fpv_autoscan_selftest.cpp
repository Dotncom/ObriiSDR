#include "fpvautoscancontroller.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FPV auto-scan self-test failed: " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    FpvAutoScanController controller;
    FpvAutoScanController::Settings settings;
    settings.verificationTimeoutMs = 1000;
    settings.signalLossTimeoutMs = 800;
    settings.minimumHsyncPercent = 45;
    controller.setSettings(settings);

    controller.start(0);
    require(controller.state() == FpvAutoScanController::State::Scanning,
            "start must enter scanning");
    require(controller.observeCandidate(5800000000.0, false, 100) ==
                FpvAutoScanController::Action::None,
            "unstable candidate must not stop scanning");
    require(controller.observeCandidate(5800000000.0, true, 200) ==
                FpvAutoScanController::Action::LockCandidate,
            "stable candidate must request tuning");
    require(controller.state() == FpvAutoScanController::State::VerifyingVideo,
            "candidate must enter video verification");

    controller.observeVideoLock(30, false, 350);
    require(controller.state() == FpvAutoScanController::State::VerifyingVideo,
            "weak HSync must not lock");
    controller.observeVideoLock(55, false, 450);
    require(controller.state() == FpvAutoScanController::State::Decoding,
            "good HSync must lock decoding");
    require(controller.tick(1100) == FpvAutoScanController::Action::None,
            "lock must be held inside loss timeout");
    require(controller.tick(1300) == FpvAutoScanController::Action::ResumeScan,
            "lost video must resume scanning");

    require(controller.observeCandidate(5800000000.0, true, 1350) ==
                FpvAutoScanController::Action::None,
            "recently rejected candidate must cool down");
    require(controller.observeCandidate(5740000000.0, true, 1400) ==
                FpvAutoScanController::Action::LockCandidate,
            "different stable candidate must still lock");
    require(controller.tick(2400) == FpvAutoScanController::Action::ResumeScan,
            "verification timeout must resume scanning");

    require(controller.observeCandidate(5660000000.0, true, 2500) ==
                FpvAutoScanController::Action::LockCandidate,
            "candidate must lock after resume");
    controller.observeVideoLock(0, true, 2600);
    require(controller.state() == FpvAutoScanController::State::Decoding,
            "VSync must lock even before HSync estimate");
    controller.stop();
    require(!controller.active(), "stop must return to idle");

    std::cout << "FPV auto-scan self-test passed\n";
    return 0;
}
