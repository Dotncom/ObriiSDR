#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audiolifecycle.h"
#include <cassert>
#include <iostream>

int main() {
    using State = RadioRunState;
    using Action = LocalAudioAction;
    assert(localAudioAction(State::Stopping, true, true, false) == Action::None);
    assert(localAudioAction(State::Idle, true, true, false) == Action::None);
    assert(localAudioAction(State::Starting, true, true, false) == Action::AwaitIq);
    assert(localAudioAction(State::Running, true, false, false) == Action::AwaitIq);
    assert(localAudioAction(State::Running, true, true, false) == Action::Start);
    assert(localAudioAction(State::Running, false, true, false) == Action::Stop);
    assert(localAudioAction(State::Running, true, true, true) == Action::None);
    // The failing log sequence: calibration started idle, its reader was still
    // alive in Stopping when audio preference was restored. No audio may start.
    assert(!resumeAudioAfterCalibration(false, State::Stopping, true, true));
    assert(!resumeAudioAfterCalibration(false, State::Running, true, true));
    assert(!resumeAudioAfterCalibration(true, State::Stopping, true, true));
    assert(!resumeAudioAfterCalibration(true, State::Running, true, false));
    assert(!resumeAudioAfterCalibration(true, State::Running, false, true));
    assert(resumeAudioAfterCalibration(true, State::Running, true, true));
    std::cout << "PASS: stopping-reader race, idle calibration, startup deferral, muted calibration and valid audio resume\n";
}
