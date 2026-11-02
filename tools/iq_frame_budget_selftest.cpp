#include "iqframequeuebudget.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <atomic>
#include <thread>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        auto budget = std::make_shared<IqFrameQueueBudget>();
        constexpr auto mib = 1024LL * 1024LL;
        require(budget->reserve(20 * mib, 10), "First IQ frame was rejected");
        require(!budget->reserve(13 * mib, 7), "IQ queue exceeded its byte limit");
        require(budget->pending() == 20 * mib, "Rejected frame changed queue size");
        require(budget->takeDroppedFrames() == 1 && budget->takeDroppedSamples() == 7,
                "Rejected IQ frame was not counted");
        require(budget->reserve(12 * mib, 6), "Queue rejected a frame that fits exactly");
        require(!budget->reserve(1, 1), "Full IQ queue accepted another frame");
        budget->release(20 * mib);
        require(budget->reserve(1, 1), "IQ queue did not recover after release");
        budget->release(12 * mib + 1);
        require(budget->pending() == 0, "IQ queue did not drain");
        require(budget->takeDroppedFrames() == 1 && budget->takeDroppedSamples() == 1,
                "IQ drop counters did not reset");

        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        bool accepted[2] = {};
        std::thread first([&]() {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            accepted[0] = budget->reserve(20 * mib, 10);
        });
        std::thread second([&]() {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            accepted[1] = budget->reserve(20 * mib, 10);
        });
        while (ready.load() != 2) std::this_thread::yield();
        start.store(true);
        first.join();
        second.join();
        require(accepted[0] != accepted[1] && budget->pending() == 20 * mib,
                "Concurrent producers bypassed the IQ queue limit");
        budget->release(20 * mib);
        require(budget->pending() == 0 && budget->takeDroppedFrames() == 1,
                "Concurrent IQ queue accounting failed");
        std::cout << "IQ frame queue budget self-test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "IQ frame queue budget self-test failed: " << error.what() << '\n';
        return 1;
    }
}
