#include "coherentiqaligner.h"
#include "coherentiqlive.h"
#include "coherentiqsession.h"
#include "multisdrliveiq.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
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

void testBandStitching() {
    auto &registry = DspIqSourceRegistry::instance();
    const auto source = CoherentIqLive::outputIdentity();
    MultiSdrLiveIq worker;
    registry.setSubscriptions(&source, {source.key()});
    require(worker.start({}, 32, true, false, true).isEmpty(), "Could not start virtual band stitching");
    QElapsedTimer timer;
    timer.start();
    DspIqSourceSnapshot frame;
    while (timer.elapsed() < 3000) {
        QThread::msleep(10);
        require(worker.preview().error.isEmpty(), "Virtual stitching stopped prematurely");
        if (registry.snapshotSince(source, 0, 0, 4096, frame, false) && frame.iq.size() == 4096) break;
    }
    worker.stop();
    registry.removeSubscriptions(&source);
    require(frame.iq.size() == 4096 && frame.sampleRateHz == 96000 && frame.centerHz == 145500000 &&
                frame.sharedSampleTimeline, "Stitched IQ metadata or samples missing");
    const auto amplitude = [&](double offset) {
        std::complex<double> correlation{};
        for (std::size_t i = 0; i < frame.iq.size() / 2; ++i) {
            const auto iq = std::complex<double>(frame.iq[2 * i], frame.iq[2 * i + 1]);
            const double phase = -6.283185307179586 * offset * double(i) / frame.sampleRateHz;
            correlation += iq * std::polar(1.0, phase);
        }
        return std::abs(correlation) / (frame.iq.size() / 2);
    };
    require(amplitude(-20250) > 0.29 && amplitude(16500) > 0.19,
            "Stitched IQ lost one of the two source signals");
    require(amplitude(27750) < 0.001 && amplitude(-31500) < 0.001,
            "Stitched IQ retained interpolation images");
    std::cout << "Virtual IQ stitching: both bands present at 96 kS/s, interpolation images suppressed\n";
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

void testLivePhaseDrift() {
    constexpr double rate = 48000;
    constexpr double driftHz = -0.50;
    constexpr double radiansPerSample = 6.283185307179586 * driftHz / rate;
    constexpr std::size_t calibrationSamples = 4096;
    constexpr std::size_t length = 163840;
    std::vector<DspIqSourceSnapshot> inputs(2);
    std::mt19937 random(481);
    std::uniform_real_distribution<float> distribution(-0.5f, 0.5f);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto &input = inputs[i];
        input.source = DspSourceIdentity::normalized(QStringLiteral("phase-test-%1").arg(i), QStringLiteral("rx0"));
        input.sampleRateHz = rate;
        input.centerHz = 145500000;
        input.clockId = QStringLiteral("phase-test-clock");
        input.sampleClockValid = input.sharedSampleTimeline = input.clockLocked = true;
        input.continuityId = 1;
        input.endComplexSample = length;
        input.iq.resize(length * 2);
    }
    for (std::size_t n = 0; n < length; ++n) {
        const std::complex<float> reference{distribution(random), distribution(random)};
        const auto target = reference * std::polar(0.7f, float(0.2 + n * radiansPerSample));
        inputs[0].iq[2 * n] = reference.real();
        inputs[0].iq[2 * n + 1] = reference.imag();
        inputs[1].iq[2 * n] = target.real();
        inputs[1].iq[2 * n + 1] = target.imag();
    }
    auto calibration = inputs;
    for (auto &input : calibration) {
        input.iq.resize(2 * calibrationSamples);
        input.endComplexSample = calibrationSamples;
    }
    CoherentIqLive combiner;
    DspIqSourceSnapshot output;
    QString error;
    require(combiner.calibrate(calibration, 0, error) && !combiner.maximumPhaseDriftHz(),
            "Reported phase drift before receiving aligned blocks");
    for (int block = 0; block < 9; ++block)
        require(combiner.takeBlock(inputs, output, error) && !output.iq.empty(),
                "Phase drift test lost coherence before the measurement");
    const auto measured = combiner.maximumPhaseDriftHz();
    require(measured && std::abs(*measured - std::abs(driftHz)) < 0.02,
            "Did not recover the known relative phase drift");
    CoherentIqLive tracked(CoherentIqLive::Timeline::Hardware, {false, true});
    require(tracked.calibrate(calibration, 0, error), "Could not calibrate the phase tracker");
    double squareError = 0;
    std::size_t compared = 0;
    for (std::size_t block = 0; block + 1 < (length - calibrationSamples) / CoherentIqLive::BlockSamples; ++block) {
        require(tracked.takeBlock(inputs, output, error) && !output.iq.empty(),
                "Phase tracker lost the known slow drift");
        if (block < 24) continue;
        const auto first = std::size_t(output.startComplexSample) * 2;
        for (std::size_t n = 0; n < output.iq.size(); ++n)
            squareError += std::pow(output.iq[n] - inputs[0].iq[first + n], 2);
        compared += output.iq.size();
    }
    const double rms = std::sqrt(squareError / compared);
    require(rms < 0.005 && tracked.maximumPhaseDriftHz() &&
            std::abs(*tracked.maximumPhaseDriftHz() - std::abs(driftHz)) < 0.02,
            "Phase tracker did not hold the reference waveform");
    require(tracked.calibrate(calibration, 0, error), "Could not reset the phase tracker");
    for (auto &value : inputs[1].iq) value = -value;
    require(!tracked.takeBlock(inputs, output, error) && error.contains(QStringLiteral("phase lock")),
            "Phase tracker accepted an abrupt phase reversal");
    for (auto &value : inputs[1].iq) value = -value;
    ++inputs[1].continuityId;
    require(!combiner.takeBlock(inputs, output, error) && !combiner.maximumPhaseDriftHz(),
            "Retained stale phase drift after an IQ gap");
    std::cout << "Live phase tracking: 0.50 Hz relative drift, waveform RMS error " << rms << '\n';
}

void testLiveNoiseWeights() {
    constexpr int length = 16384;
    constexpr int calibrationSamples = 4096;
    constexpr int count = length - calibrationSamples - CoherentIqLive::BlockSamples;
    std::vector<DspIqSourceSnapshot> inputs(2);
    std::mt19937 random(189);
    std::normal_distribution<float> noise;
    std::vector<float> clean(2 * length);
    for (int n = 0; n < length; ++n) {
        const auto value = std::polar(0.4f, float(0.087 * n)) + std::polar(0.3f, float(-0.43 * n));
        clean[2 * n] = value.real();
        clean[2 * n + 1] = value.imag();
    }
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto &input = inputs[i];
        input.source = DspSourceIdentity::normalized(QStringLiteral("noise-test-%1").arg(i), QStringLiteral("rx0"));
        input.sampleRateHz = 48000;
        input.centerHz = 145500000;
        input.clockId = QStringLiteral("noise-test-clock");
        input.sampleClockValid = input.sharedSampleTimeline = input.clockLocked = true;
        input.continuityId = 1;
        input.endComplexSample = length;
        const float gain = i == 0 ? 1.0f : 0.55f;
        const float sigma = i == 0 ? 0.025f : 0.095f;
        for (float value : clean) input.iq.push_back(gain * value + sigma * noise(random));
    }
    auto calibration = inputs;
    for (auto &input : calibration) {
        input.iq.resize(2 * calibrationSamples);
        input.endComplexSample = calibrationSamples;
    }
    QString error;
    const auto mse = [&](CoherentIqLive &combiner) {
        require(combiner.calibrate(calibration, 0, error), "Could not calibrate the noisy virtual pair");
        double squareError = 0;
        DspIqSourceSnapshot block;
        for (int n = calibrationSamples; n < calibrationSamples + count; n += CoherentIqLive::BlockSamples) {
            require(combiner.takeBlock(inputs, block, error) && !block.iq.empty(), "Missing virtual IQ after weighting");
            for (std::size_t i = 0; i < block.iq.size(); ++i)
                squareError += std::pow(block.iq[i] - clean[2 * n + i], 2);
        }
        return squareError / (2 * count);
    };
    CoherentIqLive average;
    CoherentIqLive weighted(CoherentIqLive::Timeline::Hardware, {true, false});
    const auto improvementDb = 10 * std::log10(mse(average) / mse(weighted));
    const auto weights = weighted.combiningWeights();
    require(weights.size() == 2 && weights[0] > 0.9 && weights[1] > 0 &&
            std::abs(weights[0] + weights[1] - 1) < 1e-9 && improvementDb > 6,
            "MRC did not reduce the contribution of the noisier channel");
    std::cout << "Live MRC: " << improvementDb << " dB lower waveform MSE for unequal virtual channels\n";
}

void testVirtualWorker(CoherentIqLiveOptions options = {}) {
    MultiSdrLiveIq worker;
    QTemporaryDir directory;
    require(directory.isValid(), "Could not create IQ recording fixture directory");
    auto &registry = DspIqSourceRegistry::instance();
    const auto source = CoherentIqLive::outputIdentity();
    registry.setSubscriptions(&source, {source.key()});
    require(worker.start({}, 32, true, false, false, options).isEmpty(), "Could not start virtual Multi-SDR");
    require(worker.startRecording(directory.path()).isEmpty(), "Could not start virtual IQ recording");
    QElapsedTimer elapsed;
    elapsed.start();
    std::uint64_t epoch = 0;
    std::uint64_t endFloat = 0;
    std::uint64_t endSample = 0;
    std::size_t samples = 0;
    while (elapsed.elapsed() < 4500 && samples < 106000) {
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
    require(samples >= 106000, "Virtual worker did not sustain 48 kS/s");
    const auto preview = worker.preview();
    require(preview.frame.sequence >= 12 && preview.sourceCount == 2 &&
            preview.frame.levelsDb.size() == CoherentIqLive::BlockSamples, "No live spectrum preview");
    const auto peak = std::max_element(preview.frame.levelsDb.begin(), preview.frame.levelsDb.end());
    const double peakHz = preview.frame.minimumHz + (peak - preview.frame.levelsDb.begin()) *
                          48000.0 / CoherentIqLive::BlockSamples;
    require(std::abs(peakHz - 145503700) <= 48, "Virtual pilot is at the wrong frequency");
    worker.stopRecording();
    elapsed.restart();
    while (worker.recordingStatus().finishing && elapsed.elapsed() < 2500) QThread::msleep(5);
    const auto saved = worker.recordingStatus();
    require(saved.error.isEmpty() && !saved.manifest.isEmpty() && saved.samples >= 106000,
            "Continuous virtual IQ recording did not finalize");
    DspIqSourceSnapshot live;
    require(registry.snapshotSince(source, 0, 0, 2 * 1024 * 1024, live, false),
            "No live IQ to compare against the recording");
    worker.stop();
    DspIqSourceSnapshot afterStop;
    require(!registry.snapshotSince(source, 0, 0, 32768, afterStop), "Virtual IQ was retained after Stop");

    CoherentIqRecording reader;
    QString error;
    require(reader.open(saved.manifest, error), "Cannot open the continuous IQ recording");
    const auto &weights = reader.combiningWeights();
    require(options.noiseWeighted ? weights.size() == 2 && weights == preview.combiningWeights : weights.empty(),
            "Recording did not preserve its combining weights");
    const double weightA = weights.empty() ? 0.5 : weights[0];
    const double weightB = weights.empty() ? 0.5 : weights[1];
    std::vector<DspIqSourceSnapshot> channels;
    int segments = 0;
    std::uint64_t total = 0;
    std::uint64_t next = 0;
    std::vector<float> firstBlock;
    while (reader.readNext(channels, error) && !channels.empty()) {
        const auto &a = channels[0];
        const auto &b = channels[1];
        require(channels.size() == 2 && a.source != b.source && a.iq != b.iq &&
                a.startComplexSample == b.startComplexSample && a.endComplexSample == b.endComplexSample &&
                a.startComplexSample >= live.startComplexSample && a.endComplexSample <= live.endComplexSample &&
                (!segments || a.startComplexSample == next), "Recording lost a channel or the shared sample timeline");
        const auto offset = std::size_t(a.startComplexSample - live.startComplexSample) * 2;
        for (std::size_t n = 0; n < a.iq.size(); ++n)
            require(std::abs(weightA * a.iq[n] + weightB * b.iq[n] - live.iq[offset + n]) < 1e-7,
                    "Recorded channels no longer reconstruct the live waveform");
        if (!segments) firstBlock.assign(live.iq.begin() + offset, live.iq.begin() + offset + 2048);
        next = a.endComplexSample;
        total += a.iq.size() / 2;
        ++segments;
    }
    require(error.isEmpty() && segments >= 2 && total == saved.samples,
            "Recording index did not recover all full and partial segments");

    require(worker.startReplay(saved.manifest).isEmpty(), "Could not start coherent IQ replay");
    elapsed.restart();
    bool firstBlockMatched = false;
    while (!worker.preview().replayFinished && elapsed.elapsed() < 5000) {
        if (!firstBlockMatched) {
            DspIqSourceSnapshot playback;
            if (registry.snapshotSince(source, 0, 0, 32768, playback, false)) {
                require(playback.sampleRateHz == 48000 && playback.centerHz == 145500000 &&
                        playback.iq.size() >= firstBlock.size() && !playback.sharedSampleTimeline,
                        "Replay did not preserve RF metadata or asserted a live clock");
                for (std::size_t n = 0; n < firstBlock.size(); ++n)
                    require(std::abs(playback.iq[n] - firstBlock[n]) < 1e-7f, "Replay changed the IQ waveform");
                firstBlockMatched = true;
            }
        }
        QThread::msleep(5);
    }
    const auto replay = worker.preview();
    require(firstBlockMatched && replay.replayFinished && replay.error.isEmpty() &&
            replay.combinedSamples == total && replay.frame.sequence >= 12,
            "Replay lost IQ or did not finish");
    worker.stop();
    registry.removeSubscriptions(&source);

    QFile indexFile(saved.manifest);
    require(indexFile.open(QIODevice::ReadOnly), "Cannot open capture index fixture");
    auto index = QJsonDocument::fromJson(indexFile.readAll()).object();
    indexFile.close();
    if (options.noiseWeighted) {
        auto invalid = index;
        invalid.insert(QStringLiteral("combiningWeights"), QJsonArray{0.5, 0.25});
        require(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write invalid weights");
        indexFile.write(QJsonDocument(invalid).toJson());
        indexFile.close();
        require(!reader.open(saved.manifest, error), "Accepted a recording with invalid weights");
    }
    auto entries = index.value(QStringLiteral("segments")).toArray();
    const auto first = entries.at(0);
    entries.replace(0, entries.at(1));
    entries.replace(1, first);
    index.insert(QStringLiteral("segments"), entries);
    require(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot alter capture index fixture");
    const auto altered = QJsonDocument(index).toJson();
    require(indexFile.write(altered) == altered.size(), "Cannot write the reordered capture index");
    indexFile.close();
    require(reader.open(saved.manifest, error) && reader.readNext(channels, error) && !channels.empty() &&
            !reader.readNext(channels, error) && channels.empty() && error.contains(QStringLiteral("timeline")),
            "Reordered recording segments were stitched together");
    std::cout << "Virtual Multi-SDR: " << samples << " continuous IQ samples, peak "
              << (peakHz - 145500000) << " Hz from center, " << preview.frame.sequence << " previews\n";
    std::cout << "Coherent IQ recording: " << total << " samples per RX, " << segments
              << " segments, replay matched live IQ, reordered segments rejected\n";
}

void testInterruptedRecording() {
    QTemporaryDir directory;
    MultiSdrLiveIq worker;
    require(directory.isValid() && worker.start({}, 32, true).isEmpty() &&
            worker.startRecording(directory.path()).isEmpty(), "Could not start recording for the IQ gap test");
    QElapsedTimer elapsed;
    elapsed.start();
    while (worker.preview().combinedSamples < 4096 && elapsed.elapsed() < 2000) QThread::msleep(5);
    require(worker.preview().combinedSamples >= 4096, "No IQ before simulated recording gap");
    DspIqSourceRegistry::instance().clear(
        DspSourceIdentity::normalized(QStringLiteral("coherent-virtual-b"), QStringLiteral("rx0")));
    elapsed.restart();
    while (worker.recordingStatus().manifest.isEmpty() && elapsed.elapsed() < 2000) QThread::msleep(5);
    const auto status = worker.recordingStatus();
    require(!status.manifest.isEmpty() && status.samples >= 4096 &&
            status.error.contains(QStringLiteral("IQ gap")), "Recording did not preserve the prefix before an IQ gap");
    CoherentIqRecording recording;
    std::vector<DspIqSourceSnapshot> channels;
    QString error;
    require(recording.open(status.manifest, error) && recording.interruptionReason() == status.error &&
            recording.readNext(channels, error) && channels.size() == 2 &&
            channels[0].iq.size() / 2 == status.samples &&
            recording.readNext(channels, error) && channels.empty(), "Interrupted recording index is inconsistent");
    std::cout << "Interrupted recording retained " << status.samples << " samples per RX and the gap reason\n";
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
        testLivePhaseDrift();
        testLiveNoiseWeights();
        testVirtualWorker();
        testVirtualWorker({true, true});
        testBandStitching();
        testInterruptedRecording();
        std::cout << "Coherent IQ delay, phase, gain and alignment self-test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
