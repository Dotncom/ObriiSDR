#pragma once
#include <atomic>
inline std::atomic_bool &fftTuningGuard() { static std::atomic_bool active{false}; return active; }
inline bool fftTuningActive() { return fftTuningGuard().load(std::memory_order_acquire); }
