#include "virtualiqmedium.h"
#include "virtualsignaltransport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>

#include <cmath>
#include <complex>

namespace {
constexpr qint64 Timestamp = 1000000000LL;
constexpr qint64 Arrival = 2000000000LL;

VirtualSignalFrame frame(float real, int samples, qint64 timestamp = Timestamp) {
    VirtualSignalFrame result;
    result.timestampNs = timestamp;
    result.payloadType = VirtualSignalPayloadType::ComplexFloat32;
    result.sampleRate = 8000;
    result.centerFrequencyHz = 1000000;
    QVector<std::complex<float>> iq(samples, {real, -real});
    result.payload = virtualSignalComplexFloat32Payload(iq.constData(), samples);
    return result;
}

bool check(bool value, const char *message) {
    if (!value) qCritical() << message;
    return value;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    VirtualIqMediumSettings settings;
    settings.sampleRate = 8000;
    settings.blockSamples = 128;
    settings.playoutDelayMs = 10;
    settings.defaultRoute = false;
    VirtualIqMedium medium(settings);
    if (!check(medium.addSubscriber("left") && medium.addSubscriber("right") &&
               medium.setLink("a", "left", {}) &&
               medium.setLink("b", "left", {true, 0, -6.0206f}) &&
               medium.setLink("a", "right", {}), "routes")) return 1;
    auto a = frame(0.25f, 128);
    auto b = frame(0.25f, 128);
    if (!check(medium.publish("a", a, Arrival) && medium.publish("b", b, Arrival), "publish")) return 2;
    if (!check(medium.takeReady("left", Arrival + 25000000LL).isEmpty(), "playout latency")) return 3;
    const auto left = medium.takeReady("left", Arrival + 27000000LL);
    const auto right = medium.takeReady("right", Arrival + 27000000LL);
    QVector<float> leftIq, rightIq;
    if (!check(left.size() == 1 && right.size() == 1 &&
               virtualSignalDecodeComplexFloat32(left.first().payload, &leftIq) &&
               virtualSignalDecodeComplexFloat32(right.first().payload, &rightIq) &&
               std::fabs(leftIq.first() - 0.375f) < 0.001f &&
               std::fabs(rightIq.first() - 0.25f) < 0.001f &&
               left.first().metadata.value("hubOverlapSamples").toInt() == 128 &&
               left.first().metadata.value("hubSources").toArray().size() == 2,
               "sample sum, gain, isolated RX")) return 4;
    if (!check(medium.publish("a", frame(0.25f, 128, Timestamp + 32000000LL), Arrival + 32000000LL),
               "later publish")) return 5;
    const auto gaps = medium.takeReady("left", Arrival + 100000000LL);
    if (!check(gaps.size() == 2 && gaps.first().discontinuity &&
               gaps.first().metadata.value("hubGapSamples").toInt() == 128 &&
               gaps.last().timestampNs == Timestamp + 32000000LL &&
               medium.counters("left").gapSamples == 128, "gap fill and timestamp")) return 6;
    auto wrong = frame(0.1f, 128);
    wrong.sampleRate = 16000;
    if (!check(!medium.publish("a", wrong, Arrival), "reject mismatched rate")) return 7;
    VirtualIqMediumSettings delayedSettings = settings;
    delayedSettings.defaultRoute = true;
    VirtualIqMedium delayed(delayedSettings);
    delayed.addSubscriber("rx");
    delayed.setLink("b", "rx", {true, 8, 0});
    delayed.publish("a", frame(0.2f, 128), Arrival);
    delayed.publish("b", frame(0.3f, 128), Arrival);
    const auto shifted = delayed.takeReady("rx", Arrival + 27000000LL);
    QVector<float> shiftedIq;
    if (!check(shifted.size() == 1 &&
               virtualSignalDecodeComplexFloat32(shifted.first().payload, &shiftedIq) &&
               std::fabs(shiftedIq[0] - 0.2f) < 0.001f &&
               std::fabs(shiftedIq[64 * 2] - 0.5f) < 0.001f,
               "sample-aligned delay")) return 8;
    VirtualIqMediumSettings boundedSettings = settings;
    boundedSettings.defaultRoute = true;
    boundedSettings.maximumBufferedBlocks = 2;
    VirtualIqMedium bounded(boundedSettings);
    bounded.addSubscriber("rx");
    for (int i = 0; i < 5; ++i)
        bounded.publish("a", frame(0.1f, 128, Timestamp + i * 16000000LL), Arrival + i * 16000000LL);
    if (!check(bounded.counters("rx").bufferedBlocks <= 2 &&
               bounded.counters("rx").bufferDrops > 0, "bounded memory")) return 9;
    VirtualIqMediumSettings jumpSettings = settings;
    jumpSettings.defaultRoute = true;
    jumpSettings.maximumBufferedBlocks = 2;
    VirtualIqMedium jump(jumpSettings);
    jump.addSubscriber("rx");
    jump.publish("a", frame(0.1f, 128), Arrival);
    jump.publish("a", frame(0.1f, 128, Timestamp + 1000000000LL), Arrival + 1000000000LL);
    if (!check(jump.takeReady("rx", Arrival + 1030000000LL).size() == 1 &&
               jump.counters("rx").bufferDrops > 0, "large gap skips instead of filling")) return 10;
    VirtualIqMediumSettings noisySettings = settings;
    noisySettings.defaultRoute = true;
    noisySettings.noiseSigma = 0.01f;
    noisySettings.randomSeed = 42;
    VirtualIqMedium noisyA(noisySettings), noisyB(noisySettings);
    noisyA.addSubscriber("rx");
    noisyB.addSubscriber("rx");
    noisyA.publish("a", frame(0.1f, 128), Arrival);
    noisyB.publish("a", frame(0.1f, 128), Arrival);
    if (!check(noisyA.takeReady("rx", Arrival + 30000000LL).first().payload ==
               noisyB.takeReady("rx", Arrival + 30000000LL).first().payload,
               "seeded noise reproducibility")) return 11;
    VirtualIqMediumSettings partialSettings = settings;
    partialSettings.defaultRoute = true;
    VirtualIqMedium partial(partialSettings);
    partial.addSubscriber("rx");
    partial.publish("a", frame(0.1f, 64), Arrival);
    const auto partialFrames = partial.takeReady("rx", Arrival + 30000000LL);
    if (!check(partialFrames.size() == 1 &&
               partialFrames.first().metadata.value("hubGapSamples").toInt() == 64 &&
               partialFrames.first().discontinuity, "partial final block")) return 12;
    VirtualIqMediumSettings capacitySettings = settings;
    capacitySettings.maximumBufferedBlocks = 2;
    VirtualIqMedium capacity(capacitySettings);
    capacity.addSubscriber("rx");
    if (!check(!capacity.setLink("a", "rx", {true, 100, 0.0f}),
               "reject delay beyond capacity")) return 13;
    qInfo() << "Virtual IQ medium selftest passed";
    return 0;
}
