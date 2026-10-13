#pragma once

// Remote-main-FFT-only retry policy, independent of wall clock for testing.
struct RemoteFftRetryPolicy {
    int transientMs = 1000;
    void reset() { transientMs = 1000; }
    int failed(bool unavailable) {
        if (!unavailable) return 30000;
        const int delay = transientMs;
        if (transientMs < 8000) transientMs *= 2;
        return delay;
    }
};
