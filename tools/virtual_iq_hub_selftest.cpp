#include "virtualiqhub.h"
#include "virtualsignaltransport.h"

#include <QCoreApplication>
#include <QDebug>

#include <cmath>
#include <complex>

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

    VirtualIqHubSettings routedSettings;
    routedSettings.defaultRoute = false;
    routedSettings.collisionWindowMs = 10;
    VirtualIqHub routed(routedSettings);
    if (!check(routed.addSubscriber("left") && routed.addSubscriber("right") &&
               routed.setLink("tx1", "left", {true, 5, -6.0206f}) &&
               routed.setLink("tx2", "right", {}) &&
               !routed.setLink("tx2", "missing", {}),
               "configure isolated links")) return 10;
    const std::complex<float> sample(1.0f, -0.5f);
    VirtualSignalFrame routedFrame = frame(1);
    routedFrame.payload = virtualSignalComplexFloat32Payload(&sample, 1);
    routed.publish("tx1", routedFrame, 100);
    routed.publish("tx2", routedFrame, 105);
    if (!check(routed.takeDue("left", 115).isEmpty(), "per-link delay")) return 11;
    const auto left = routed.takeDue("left", 116);
    const auto right = routed.takeDue("right", 116);
    QVector<float> leftIq;
    if (!check(left.size() == 1 && right.size() == 1 &&
               routed.counters("left").collisionFrames == 0 &&
               routed.counters("right").collisionFrames == 0 &&
               routed.counters("left").filteredFrames == 1 &&
               routed.counters("right").filteredFrames == 1 &&
               virtualSignalDecodeComplexFloat32(left.first().payload, &leftIq) &&
               std::fabs(leftIq.first() - 0.5f) < 0.001f &&
               std::fabs(leftIq.last() + 0.25f) < 0.001f &&
               right.first().payload == routedFrame.payload,
               "isolated collision domains and gain")) return 12;

    VirtualIqHubSettings blockedSettings;
    blockedSettings.defaultRoute = true;
    VirtualIqHub blocked(blockedSettings);
    blocked.addSubscriber("rx");
    blocked.setLink("tx2", "rx", {false, 0, 0.0f});
    blocked.publish("tx1", frame(1), 1);
    blocked.publish("tx2", frame(2), 2);
    blocked.publish("tx1", frame(3), 3);
    const auto unblocked = blocked.takeDue("rx", 3);
    if (!check(unblocked.size() == 2 && !unblocked.last().discontinuity &&
               blocked.counters("rx").filteredFrames == 1,
               "disabled link must not disturb another source")) return 13;

    VirtualIqHubSettings orderedSettings;
    orderedSettings.defaultRoute = false;
    VirtualIqHub ordered(orderedSettings);
    ordered.addSubscriber("rx");
    ordered.setLink("tx1", "rx", {true, 100, 0.0f});
    ordered.setLink("tx2", "rx", {true, 0, 0.0f});
    ordered.publish("tx1", frame(1), 100);
    ordered.publish("tx2", frame(2), 101);
    const auto faster = ordered.takeDue("rx", 101);
    const auto slower = ordered.takeDue("rx", 200);
    if (!check(faster.size() == 1 && slower.size() == 1 &&
               faster.first().metadata.value(QStringLiteral("hubSource")).toString() ==
                   QStringLiteral("tx2") &&
               slower.first().metadata.value(QStringLiteral("hubSource")).toString() ==
                   QStringLiteral("tx1") && slower.first().discontinuity,
               "per-link delay order and delivered source discontinuity")) return 14;
    qInfo() << "Virtual IQ hub selftest passed";
    return 0;
}
