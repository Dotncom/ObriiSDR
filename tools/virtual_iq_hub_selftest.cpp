#include "virtualiqhub.h"

#include <QCoreApplication>
#include <QDebug>

namespace {
VirtualSignalFrame frame(quint32 sequence) {
    VirtualSignalFrame result;
    result.sequence = sequence;
    result.sampleRate = 48000;
    result.centerFrequencyHz = 1000000;
    result.payloadType = VirtualSignalPayloadType::ComplexFloat32;
    result.payload = QByteArray(16, '\0');
    return result;
}

bool check(bool condition, const char *message) {
    if (!condition) qCritical() << message;
    return condition;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    VirtualIqHubSettings settings;
    settings.maximumQueuedFrames = 2;
    settings.fixedDelayMs = 20;
    VirtualIqHub hub(settings);
    if (!check(hub.addSubscriber("slow") && hub.addSubscriber("fast"), "add subscribers")) return 1;
    for (quint32 i = 0; i < 3; ++i)
        if (!check(hub.publish("tx", frame(i), 100 + i), "publish")) return 2;
    const auto fast = hub.takeDue("fast", 130);
    if (!check(fast.size() == 2 && fast.first().discontinuity &&
               fast.first().sequence == 0 && fast.last().sequence == 1,
               "independent sequence and bounded queue")) return 3;
    if (!check(hub.counters("slow").queuedBytes > 0 &&
               hub.counters("fast").queuedBytes == 0 &&
               hub.totalCounters().queuedBytes == hub.counters("slow").queuedBytes,
               "queued byte counters")) return 4;
    if (!check(hub.takeDue("slow", 110).isEmpty(), "fixed delay")) return 5;
    if (!check(hub.takeDue("slow", 130).size() == 2, "slow subscriber independent")) return 6;

    VirtualIqHubSettings collisionSettings;
    collisionSettings.collisionWindowMs = 10;
    VirtualIqHub collisionHub(collisionSettings);
    collisionHub.addSubscriber("rx");
    collisionHub.publish("tx1", frame(1), 100);
    collisionHub.publish("tx2", frame(2), 105);
    collisionHub.publish("tx2", frame(3), 109);
    if (!check(collisionHub.takeDue("rx", 120).isEmpty() &&
               collisionHub.counters("rx").collisionFrames == 3,
               "cross-source collision burst")) return 7;
    collisionHub.publish("tx2", frame(4), 130);
    const auto recovered = collisionHub.takeDue("rx", 150);
    if (!check(recovered.size() == 1 && recovered.first().discontinuity,
               "recovery after collision")) return 8;

    VirtualIqHubSettings noiseSettings;
    noiseSettings.noiseSigma = 0.2f;
    noiseSettings.randomSeed = 17;
    VirtualIqHub first(noiseSettings), second(noiseSettings);
    first.addSubscriber("rx");
    second.addSubscriber("rx");
    first.publish("tx", frame(0), 1);
    second.publish("tx", frame(0), 1);
    if (!check(first.takeDue("rx", 1).first().payload ==
               second.takeDue("rx", 1).first().payload,
               "seeded noise must reproduce")) return 9;
    qInfo() << "Virtual IQ hub selftest passed";
    return 0;
}
