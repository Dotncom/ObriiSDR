#include "dspbranchaudiooutput.h"
#include "dspbranchaudioqueue_p.h"
#include "remoteaudioplayer.h"
#include <QThread>
#include <QTimer>
#include <map>
#include <atomic>

struct DspBranchAudioOutput::Impl : QThread {
    DspBranchAudioQueue queue;
    std::atomic_bool stopping{false};

    void run() override {
        std::map<QString, std::unique_ptr<RemoteAudioPlayer>> players;
        std::map<QString, std::uint64_t> generations;
        QTimer timer;
        timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer, &QTimer::timeout, &timer, [&]() {
            if (stopping.load()) { quit(); return; }
            auto frames = queue.drain();
            queue.withActiveSinks([&](const QSet<QString> &sinks) {
                for (auto it = players.begin(); it != players.end();) {
                    if (!sinks.contains(it->first)) { generations.erase(it->first); it = players.erase(it); }
                    else ++it;
                }
            });
            for (auto &entry : frames) {
                queue.deliver(entry.second, [&](const DspBranchPcmFrame &f) {
                    if (stopping.load()) return;
                    auto &player = players[entry.first];
                    if (!player) {
                        player = std::make_unique<RemoteAudioPlayer>();
                        player->setMaximumQueuedMs(250);
                    }
                    if (f.reset || generations[entry.first] != f.generation) player->stop();
                    generations[entry.first] = f.generation;
                    player->setOutputDevice(f.device);
                    player->setVolume(f.volume);
                    player->playPcmFrame(f.pcm);
                });
            }
        });
        timer.start(10);
        if (!stopping.load()) exec();
    }
};

DspBranchAudioOutput::DspBranchAudioOutput() : impl(std::make_unique<Impl>()) {
    impl->start(QThread::HighPriority);
}
DspBranchAudioOutput::~DspBranchAudioOutput() {
    impl->stopping.store(true);
    impl->quit();
    impl->wait();
}
void DspBranchAudioOutput::setActiveSinks(const QSet<QString> &ids) {
    impl->queue.setActiveSinks(ids);
}
void DspBranchAudioOutput::enqueue(const DspBranchPcmFrame &frame) {
    impl->queue.enqueue(frame);
}
std::uint64_t DspBranchAudioOutput::droppedFrames() const {
    return impl->queue.droppedFrames();
}
