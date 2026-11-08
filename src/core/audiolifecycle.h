#pragma once
#include "radiosettings.h"

enum class LocalAudioAction { None, Stop, Start, AwaitIq };

inline LocalAudioAction localAudioAction(RadioRunState state, bool enabled, bool readerAlive, bool calibrating) {
    if (state == RadioRunState::Idle) return LocalAudioAction::None;
    if (!enabled) return LocalAudioAction::Stop;
    // A live QThread is not evidence of an active receiver: it can still be
    // unwinding after cancel_async. A restored checkbox is only a preference.
    if (state == RadioRunState::Stopping || calibrating) return LocalAudioAction::None;
    if (state == RadioRunState::Starting || !readerAlive) return LocalAudioAction::AwaitIq;
    return LocalAudioAction::Start;
}

inline bool resumeAudioAfterCalibration(bool receiverWasRunning, RadioRunState currentState,
                                        bool enabled, bool readerAlive) {
    return receiverWasRunning && localAudioAction(currentState, enabled, readerAlive, false) == LocalAudioAction::Start;
}
