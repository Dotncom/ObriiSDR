#include "coherentiqaligner.h"
#include "coherentiqlive.h"
#include "coherentiqsession.h"
#include "multisdrliveiq.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void testLiveAlignment() {
    constexpr std::uint64_t origin = (1ULL << 53) + 100;
    constexpr std::size_t length = 8192;
    constexpr int delay = 11;
    std::mt19937 random(432);
    std::uniform_real_distribution<float> distribution(-0.5f, 0.5f);
    std::vector<float> reference(length * 2), target(length * 2);
    const auto gain = std::polar(0.65f, 1.27409f);
    for (std::size_t i = 0; i < length; ++i) {
        reference[2 * i] = distribution(random);
        reference[2 * i + 1] = distribution(random);
        if (i < delay) continue;
        const auto value = gain * std::complex<float>(reference[2 * (i - delay)],
                                                     reference[2 * (i - delay) + 1]);
        target[2 * i] = value.real();
        target[2 * i + 1] = value.imag();
    }
    const auto a = DspSourceIdentity::normalized(QStringLiteral("live-selftest-a"), QStringLiteral("rx0"));
    const auto b = DspSourceIdentity::normalized(QStringLiteral("live-selftest-b"), QStringLiteral("rx0"));
    auto &registry = DspIqSourceRegistry::instance();
    registry.setSubscriptions(&registry, {a.key(), b.key()});
    DspIqTimingMetadata timing;
    timing.epoch = 77;
    timing.clockId = QStringLiteral("shared-selftest");
    timing.sharedSampleTimeline = timing.sampleClockValid = timing.clockLocked = true;
    const auto publish = [&](const DspSourceIdentity &id, const std::vector<float> &data,
                             std::size_t first, std::size_t end) {
        timing.firstComplexSample = origin + first;
        require(registry.publish(id, data.data() + first * 2, (end - first) * 2,
                                 48000, 145500000, false, &timing),
                "Could not publish live fixture IQ");
    };
    std::vector<DspIqSourceSnapshot> inputs(2);
    const auto read = [&]() {
        require(registry.snapshotSince(a, 0, 0, length * 2, inputs[0], false) &&
                registry.snapshotSince(b, 0, 0, length * 2, inputs[1], false),
                "Could not read live fixture IQ");
    };
    publish(a, reference, 0, 4096);
    publish(b, target, 29, 4096);
    read();
    CoherentIqLive combiner;
    QString error;
    inputs[1].sharedSampleTimeline = false;
    require(!combiner.calibrate(inputs, 32, error), "Accepted local-only sample indices");
    read();
    inputs[1].clockId += QStringLiteral("-other");
    require(!combiner.calibrate(inputs, 32, error), "Accepted different clocks");
    read();
    require(combiner.calibrate(inputs, 32, error), "Could not calibrate virtual delay, phase and gain");
    publish(a, reference, 4096, 6144);
    read();
    DspIqSourceSnapshot output;
    require(combiner.takeBlock(inputs, output, error) && output.iq.empty(),
            "Emitted IQ before all sources arrived");
    publish(b, target, 4096, 6144);
    read();
    require(combiner.takeBlock(inputs, output, error) &&
            output.startComplexSample == origin + 4096 &&
            output.endComplexSample == origin + 5120, "Unexpected output sample range");
    double squaredError = 0;
    for (std::size_t i = 0; i < output.iq.size(); ++i)
        squaredError += std::pow(output.iq[i] - reference[8192 + i], 2);
    require(std::sqrt(squaredError / output.iq.size()) < 0.004,
            "Aligned waveform differs from the known reference");
    require(combiner.takeBlock(inputs, output, error) && output.iq.empty(),
            "Emitted a duplicate or incomplete block");
    publish(a, reference, 6144, 8192);
    publish(b, target, 6144, 8192);
    read();
    for (auto &value : inputs[1].iq) value = -value;
    require(!combiner.takeBlock(inputs, output, error) && output.iq.empty() &&
            error.contains(QStringLiteral("phase lock")), "Accepted a 180 degree phase reversal");

    registry.clear(a);
    registry.clear(b);
    publish(a, reference, 0, 4096);
    publish(b, target, 0, 4096);
    read();
    require(combiner.calibrate(inputs, 32, error), "Could not restart live calibration");
    const auto before = inputs[1];
    publish(b, target, 4097, 8192);
    DspIqSourceSnapshot otherReader;
    require(registry.snapshotSince(b, before.epoch, before.endFloatCount, length * 2, otherReader) &&
            otherReader.gap && otherReader.epoch == before.epoch, "Expected a gap without epoch change");
    read();
    require(!combiner.takeBlock(inputs, output, error) && output.iq.empty() &&
            error.contains(QStringLiteral("IQ gap")), "Missed a gap after another reader cleared the flag");
    registry.removeSubscriptions(&registry);
}

void testVirtualWorker() {
    MultiSdrLiveIq worker;
    auto &registry = DspIqSourceRegistry::instance();
    const auto source = CoherentIqLive::outputIdentity();
    registry.setSubscriptions(&source, {source.key()});
    require(worker.start({}, 32, true).isEmpty(), "Could not start virtual Multi-SDR");
    QElapsedTimer elapsed;
    elapsed.start();
    std::uint64_t epoch = 0;
    std::uint64_t endFloat = 0;
    std::uint64_t endSample = 0;
    std::size_t samples = 0;
    while (elapsed.elapsed() < 3500 && samples < 96000) {
        QThread::msleep(10);
        require(worker.preview().error.isEmpty(), "Virtual Multi-SDR worker stopped prematurely");
        DspIqSourceSnapshot frame;
        if (!registry.snapshotSince(source, epoch, endFloat, 32768, frame)) continue;
        require(frame.sampleRateHz == 48000 && frame.centerHz == 145500000 &&
                frame.sharedSampleTimeline && frame.clockLocked && !frame.iq.empty(),
                "Virtual output lost timing or format metadata");
        require(!epoch || (!frame.gap && frame.epoch == epoch && frame.startComplexSample == endSample),
                "Virtual output has an IQ discontinuity");
        epoch = frame.epoch;
        endFloat = frame.endFloatCount;
        endSample = frame.endComplexSample;
        samples += frame.iq.size() / 2;
    }
    require(samples >= 96000, "Virtual worker did not sustain 48 kS/s");
    const auto preview = worker.preview();
    require(preview.frame.sequence >= 12 && preview.sourceCount == 2 &&
            preview.frame.levelsDb.size() == CoherentIqLive::BlockSamples, "No live spectrum preview");
    const auto peak = std::max_element(preview.frame.levelsDb.begin(), preview.frame.levelsDb.end());
    const double peakHz = preview.frame.minimumHz + (peak - preview.frame.levelsDb.begin()) *
                          48000.0 / CoherentIqLive::BlockSamples;
    require(std::abs(peakHz - 145503700) <= 48, "Virtual pilot is at the wrong frequency");
    worker.stop();
    DspIqSourceSnapshot afterStop;
    require(!registry.snapshotSince(source, 0, 0, 32768, afterStop), "Virtual IQ was retained after Stop");
    registry.removeSubscriptions(&source);
    std::cout << "Virtual Multi-SDR: " << samples << " continuous IQ samples, peak "
              << (peakHz - 145500000) << " Hz from center, " << preview.frame.sequence << " previews\n";
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        constexpr std::size_t samples = 16384;
        constexpr int delay = 137;
        constexpr double phase = 0.61;
        constexpr double targetGain = 0.42;
        std::vector<float> reference(samples * 2U), target(samples * 2U);
        for (std::size_t index = 0; index < samples; ++index) {
            const double carrier = 0.071 * index + 0.37 * std::sin(0.0031 * index);
            const std::complex<double> value = std::polar(0.55 + 0.2 * std::sin(0.017 * index), carrier);
            reference[index * 2U] = static_cast<float>(value.real());
            reference[index * 2U + 1U] = static_cast<float>(value.imag());
            if (index >= std::size_t(delay)) {
                const std::complex<double> shifted(reference[(index - delay) * 2U],
                                                   reference[(index - delay) * 2U + 1U]);
                const auto observed = shifted * std::polar(targetGain, -phase);
                target[index * 2U] = static_cast<float>(observed.real());
                target[index * 2U + 1U] = static_cast<float>(observed.imag());
            }
        }
        const auto calibration = CoherentIqAligner::calibrate(
            DspSourceIdentity::normalized(QStringLiteral("main"), QStringLiteral("rx0")), reference,
            DspSourceIdentity::normalized(QStringLiteral("multi-rx-2"), QStringLiteral("rx0")), target,
            10.0e6, 100.0e6, 512);
        require(calibration.valid, "Calibration rejected a correlated signal");
        require(std::abs(calibration.integerDelaySamples - delay) <= 1, "Integer delay is wrong");
        require(std::abs(calibration.phaseCorrectionRadians - phase) < 0.02, "Phase correction is wrong");
        require(std::abs(calibration.gainCorrection - 1.0 / targetGain) < 0.03, "Gain correction is wrong");
        require(calibration.coherence > 0.995, "Coherence is unexpectedly low");
        const auto aligned = CoherentIqAligner::alignTarget(target, calibration, 4096);
        require(aligned.size() == 8192U, "Aligned span has an unexpected size");

        DspIqSourceSnapshot referenceSnapshot;
        referenceSnapshot.source = calibration.reference;
        referenceSnapshot.iq = reference;
        referenceSnapshot.epoch = 11;
        referenceSnapshot.startComplexSample = 5000;
        referenceSnapshot.endComplexSample = 5000 + samples;
        referenceSnapshot.sampleRateHz = 10.0e6;
        referenceSnapshot.centerHz = 100.0e6;
        referenceSnapshot.clockId = QStringLiteral("simulated-10mhz");
        referenceSnapshot.sampleClockValid = true;
        referenceSnapshot.clockLocked = true;
        referenceSnapshot.nativeAdcBits = 14;
        referenceSnapshot.nativeSampleFormat = QStringLiteral("simulated ADC14");
        DspIqSourceSnapshot targetSnapshot = referenceSnapshot;
        targetSnapshot.source = calibration.target;
        targetSnapshot.iq = target;
        targetSnapshot.nativeAdcBits = 8;
        targetSnapshot.nativeSampleFormat = QStringLiteral("simulated SC8");

        const std::vector<DspIqSourceSnapshot> snapshots{referenceSnapshot, targetSnapshot};
        const std::vector<CoherentIqCalibration> calibrations{calibration};
        for (const auto mode : {CoherentIqCombineMode::ReferenceOnly,
                                CoherentIqCombineMode::BestSnr,
                                CoherentIqCombineMode::EqualAverage,
                                CoherentIqCombineMode::MaximumRatio}) {
            const auto combined = CoherentIqSession::combine(
                snapshots, calibrations, referenceSnapshot.source, mode, 4096);
            require(combined.valid && combined.iq.size() == 8192U,
                    "Coherent combiner rejected valid mixed-bit IQ");
            double weightSum = 0.0;
            for (const double weight : combined.weights) weightSum += weight;
            require(std::abs(weightSum - 1.0) < 1.0e-9,
                    "Coherent combiner weights are not normalized");
        }

        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create temporary coherent-session directory");
        QString manifestPath;
        QString error;
        require(CoherentIqSession::write(temporary.path(), snapshots, calibrations,
                                         manifestPath, error),
                error.toLocal8Bit().constData());
        std::vector<DspIqSourceSnapshot> restoredSnapshots;
        std::vector<CoherentIqCalibration> restoredCalibrations;
        require(CoherentIqSession::read(manifestPath, restoredSnapshots,
                                        restoredCalibrations, error),
                error.toLocal8Bit().constData());
        require(restoredSnapshots.size() == 2U && restoredCalibrations.size() == 1U &&
                    restoredSnapshots[0].iq == reference && restoredSnapshots[1].iq == target &&
                    restoredSnapshots[0].nativeAdcBits == 14 &&
                    restoredSnapshots[1].nativeAdcBits == 8,
                "Coherent-session roundtrip lost IQ or native precision metadata");
        QFile payload(QFileInfo(manifestPath).absoluteDir().filePath(QStringLiteral("rx-02.cf32")));
        require(payload.open(QIODevice::ReadWrite), "Cannot open fixture payload");
        char altered = 0;
        require(payload.getChar(&altered) && payload.seek(0) && payload.putChar(altered ^ 1),
                "Cannot alter the fixture without changing its length");
        payload.close();
        require(!CoherentIqSession::read(manifestPath, restoredSnapshots, restoredCalibrations, error) &&
                    restoredSnapshots.empty() && restoredCalibrations.empty() && error.contains(QStringLiteral("SHA-256")),
                "Same-length IQ damage was accepted or exposed a partial session");
        QFile manifest(manifestPath);
        require(manifest.open(QIODevice::ReadOnly), "Cannot open manifest fixture");
        auto legacy = QJsonDocument::fromJson(manifest.readAll()).object();
        manifest.close();
        legacy.insert(QStringLiteral("schemaVersion"), 1);
        require(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write version 1 fixture");
        const QByteArray bytes = QJsonDocument(legacy).toJson();
        require(manifest.write(bytes) == bytes.size(), "Incomplete version 1 fixture");
        manifest.close();
        require(CoherentIqSession::read(manifestPath, restoredSnapshots, restoredCalibrations, error) &&
                    restoredSnapshots.size() == 2U, "Legacy coherent-session support was lost");
        testLiveAlignment();
        testVirtualWorker();
        std::cout << "Coherent IQ delay, phase, gain and alignment self-test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
