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

VirtualSignalFrame tone(int first, int count) {
    auto result = frame(0, count, Timestamp + qint64(first) * 125000);
    QVector<std::complex<float>> samples(count);
    for (int i = 0; i < count; ++i)
        samples[i] = std::polar(0.25f, float((first + i) % 20) * 0.31415926536f);
    result.payload = virtualSignalComplexFloat32Payload(samples.constData(), count);
    return result;
}

bool checkClockResampling(VirtualIqMediumSettings settings) {
    settings.blockSamples = 1;
    settings.maximumBufferedBlocks = 8192;
    VirtualIqMedium medium(settings), whole(settings);
    for (auto *m : {&medium, &whole}) {
        for (const auto id : {"fast", "slow", "nominal"}) if (!m->addSubscriber(id)) return false;
        if (m->setLink("a", "fast", {true, 0, 0, 0, 1000.01}) ||
            !m->setLink("a", "fast", {true, 0, 0, 0, 1000}) ||
            !m->setLink("a", "slow", {true, 0, 0, 0, -1000}) ||
            !m->setLink("a", "nominal", {})) return false;
    }
    constexpr int count = 4096;
    if (!whole.publish("a", tone(0, count), Arrival)) return false;
    for (int index = 0; index < count;) {
        const int length = (std::min)(count - index, index % 11 + 1);
        if (!medium.publish("a", tone(index, length), Arrival + qint64(index) * 125000)) return false;
        index += length;
    }
    for (const auto id : {"fast", "slow", "nominal"}) {
        const double step = id == QLatin1String("fast") ? 1.001 : id == QLatin1String("slow") ? 0.999 : 1.0;
        const auto output = medium.takeReady(id, Arrival + 600000000, 8192);
        const auto reference = whole.takeReady(id, Arrival + 600000000, 8192);
        if (!check(output.size() == int(std::ceil(count / step)) && output.size() == reference.size(),
                   "clock output count is independent of frame splits")) {
            qCritical() << id << output.size() << reference.size() << std::ceil(count / step);
            return false;
        }
        std::complex<double> phaseSum{};
        std::complex<double> previous{};
        for (int i = 0; i < output.size(); ++i) {
            QVector<float> iq;
            if (output[i].payload != reference[i].payload || (i && output[i].discontinuity) ||
                output[i].timestampNs != Timestamp + qint64(i) * 125000 ||
                !virtualSignalDecodeComplexFloat32(output[i].payload, &iq)) {
                qCritical() << "split-frame mismatch" << id << i << output[i].discontinuity;
                return false;
            }
            const std::complex<double> value{iq[0], iq[1]};
            if (i > 32) phaseSum += value * std::conj(previous);
            previous = value;
        }
        if (std::abs(std::arg(phaseSum) - 0.31415926536 * step) > 2e-6 ||
            medium.counters(id).gapSamples || medium.counters(id).overlapSamples) {
            qCritical() << "tone phase or IQ occupancy" << id << std::arg(phaseSum) - 0.31415926536 * step
                        << medium.counters(id).gapSamples << medium.counters(id).overlapSamples;
            return false;
        }
    }
    // Changing the rate preserves the output cursor and fractional position.
    if (!medium.setLink("a", "fast", {}) ||
        !medium.publish("a", tone(count, 32), Arrival + count * 125000LL)) return false;
    const auto continued = medium.takeReady("fast", Arrival + 800000000, 64);
    if (!check(continued.size() == 32 && !continued.first().discontinuity &&
        continued.first().timestampNs == Timestamp + qint64(std::ceil(count / 1.001)) * 125000,
        "setting clock error back to zero preserves the sample cursor")) return false;

    auto restarted = tone(count + 32, 32);
    restarted.discontinuity = true;
    if (!medium.publish("a", restarted, Arrival + 800000000)) return false;
    const auto reset = medium.takeReady("fast", Arrival + 800000000, 64);
    return !reset.isEmpty() && reset.last().timestampNs == restarted.timestampNs + 31 * 125000 &&
           reset[reset.size() - 32].discontinuity;
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

    VirtualIqMedium offset(settings);
    offset.addSubscriber("rx");
    constexpr double offsetHz = -137.5;
    if (!check(!offset.setLink("a", "rx", {true, 0, 0, 4000.0}) &&
               offset.setLink("a", "rx", {true, 0, 0, offsetHz}), "frequency offset limits")) return 14;
    // Use a frame boundary that also falls inside a mix block.
    if (!check(offset.publish("a", frame(0.25f, 39), Arrival) &&
               offset.publish("a", frame(0.25f, 217, Timestamp + 4875000LL), Arrival + 4875000LL),
               "offset frame publish")) return 14;
    const auto offsetFrames = offset.takeReady("rx", Arrival + 50000000LL);
    if (!check(offsetFrames.size() == 2, "frequency offset output size")) return 14;
    int sample = 0;
    for (const auto &packet : offsetFrames) {
        QVector<float> iq;
        if (!check(virtualSignalDecodeComplexFloat32(packet.payload, &iq), "offset IQ decode")) return 14;
        for (int i = 0; i < iq.size(); i += 2, ++sample) {
            const std::complex<double> observed{iq[i], iq[i + 1]};
            const auto expected = std::complex<double>{0.25, -0.25} *
                std::polar(1.0, 6.2831853071795864769 * offsetHz * sample / 8000.0);
            if (!check(std::abs(observed - expected) < 1e-6, "offset phase across frame and mix block boundaries")) return 14;
        }
    }
    if (!check(checkClockResampling(settings), "independent clock rates, spectrum shift, continuous split frames and restart")) return 15;
    VirtualIqMedium longSession(delayedSettings);
    longSession.addSubscriber("rx");
    constexpr qint64 LongPause = 120000000000LL;
    if (!check(longSession.publish("a", frame(0.1f, 128), Arrival) &&
               longSession.publish("a", frame(0.1f, 128, Timestamp + LongPause), Arrival + LongPause) &&
               !longSession.publish("a", frame(0.1f, 128, Timestamp + LongPause * 2), Arrival + LongPause),
               "timestamp guard follows running clock and rejects clock jumps")) return 16;
    qInfo() << "Virtual IQ medium selftest passed";
    return 0;
}
