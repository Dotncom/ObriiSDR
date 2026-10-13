#include "dspbranchaudioqueue_p.h"

#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

DspBranchPcmFrame frame(std::uint64_t generation, bool reset = false, int bytes = 8) {
    DspBranchPcmFrame value;
    value.sinkId = QStringLiteral("audio");
    value.generation = generation;
    value.reset = reset;
    value.pcm = QByteArray(bytes, 'a');
    return value;
}

void postDrainReset() {
    DspBranchAudioQueue queue;
    queue.setActiveSinks({QStringLiteral("audio")});
    queue.enqueue(frame(1));
    std::promise<void> drained, resume;
    auto drainedSignal = drained.get_future();
    auto resumeSignal = resume.get_future();
    bool delivered = true, consumerCalled = false;
    std::thread worker([&]() {
        const auto batch = queue.drain();
        drained.set_value();
        resumeSignal.wait();
        delivered = queue.deliver(batch.begin()->second, [&](const auto &) { consumerCalled = true; });
    });
    drainedSignal.wait();
    queue.enqueue(frame(2, true, 0));
    resume.set_value();
    worker.join();
    require(!delivered && !consumerCalled, "A newer reset failed to reject an already-drained old frame");

    const auto reset = queue.drain();
    require(reset.size() == 1, "Empty reset fence was lost");
    bool sawReset = false;
    require(queue.deliver(reset.begin()->second, [&](const auto &pcm) {
        sawReset = pcm.reset && pcm.pcm.isEmpty() && pcm.generation == 2;
    }) && sawReset, "The current empty reset fence was not delivered");
    queue.enqueue(frame(1));
    require(queue.drain().empty(), "Late older generation was accepted after its reset fence");
}

void postDrainDeleteAndReadd() {
    DspBranchAudioQueue queue;
    queue.setActiveSinks({QStringLiteral("audio")});
    queue.enqueue(frame(7));
    const auto old = queue.drain();
    const auto reject = [](const auto &) { throw std::runtime_error("Invalidated sink reached delivery"); };
    queue.setActiveSinks({});
    require(!queue.deliver(old.begin()->second, reject), "Deleted sink was accepted");
    queue.setActiveSinks({QStringLiteral("audio")});
    require(!queue.deliver(old.begin()->second, reject), "Re-added sink revived an old drained frame");
    queue.enqueue(frame(7));
    require(!queue.deliver(old.begin()->second, reject), "Same-generation re-add revived an old frame");
    const auto fresh = queue.drain();
    bool called = false;
    require(queue.deliver(fresh.begin()->second, [&](const auto &) { called = true; }) && called,
            "Re-added sink rejected its new admission");
}

void sameGenerationResetAndDevice() {
    DspBranchAudioQueue queue;
    queue.setActiveSinks({QStringLiteral("audio")});
    queue.enqueue(frame(9));
    auto old = queue.drain();
    queue.enqueue(frame(9, true, 0));
    require(!queue.deliver(old.begin()->second, [](const auto &) {}), "Same-generation reset failed to fence drained PCM");
    old = queue.drain();
    auto replacement = frame(9);
    replacement.device = 2;
    queue.enqueue(replacement);
    require(!queue.deliver(old.begin()->second, [](const auto &) {}), "Device change failed to fence drained PCM");
    const auto current = queue.drain();
    require(current.begin()->second.frame.reset, "Device change did not request player reset");
}

void continuityAndBounds() {
    DspBranchAudioQueue queue;
    queue.setActiveSinks({QStringLiteral("audio")});
    queue.enqueue(frame(1));
    const auto first = queue.drain();
    queue.setActiveSinks({QStringLiteral("audio")});
    queue.enqueue(frame(1));
    queue.enqueue(frame(1));
    require(queue.deliver(first.begin()->second, [](const auto &) {}), "Normal append or unchanged sinks invalidated continuous PCM");
    const auto second = queue.drain();
    require(second.begin()->second.frame.pcm.size() == 16 && !second.begin()->second.frame.reset,
            "Continuous chunks did not concatenate without a reset");

    queue.enqueue(frame(1, false, DspBranchAudioQueue::MaxPendingBytes + 2));
    require(!queue.deliver(second.begin()->second, [](const auto &) {}), "Overrun failed to invalidate previously drained PCM");
    const auto bounded = queue.drain();
    require(bounded.begin()->second.frame.pcm.size() == DspBranchAudioQueue::MaxPendingBytes &&
            bounded.begin()->second.frame.reset && queue.droppedFrames() == 1, "PCM pending queue is not bounded");
    queue.enqueue(frame(1, false, 3));
    require(queue.drain().empty(), "Odd PCM byte count was accepted");

    QSet<QString> ids;
    for (int i = 0; i < DspMaxBranches + 2; ++i) ids.insert(QString::number(i));
    queue.setActiveSinks(ids);
    for (const auto &id : ids) {
        auto pcm = frame(2);
        pcm.sinkId = id;
        queue.enqueue(pcm);
    }
    require(queue.drain().size() == DspMaxBranches, "Active sink count is not bounded");
}
}

bool runDspBranchAudioQueueTests() {
    postDrainReset();
    postDrainDeleteAndReadd();
    sameGenerationResetAndDevice();
    continuityAndBounds();
    std::cout << "DSP audio queue admission tests passed (no audio device opened)\n";
    return true;
}
