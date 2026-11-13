#include "multisdrliveiq.h"

#include "coherentiqlive.h"
#include "coherentiqsession.h"
#include "fftplannermutex.h"
#include "ffttransformcore.h"
#include "multisdrsignalalignment.h"

#include <QDateTime>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <memory>
#include <random>
#include <system_error>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int VirtualRate = 48000;
constexpr double VirtualCenter = 145500000.0;
constexpr int VirtualDelay = 11;
constexpr int VirtualBlock = VirtualRate / 100;
constexpr double Tau = 6.2831853071795864769;
std::atomic<std::uint64_t> nextEpoch{1};

class VirtualPair final {
public:
    void publish(const std::vector<DspSourceIdentity> &sources, std::uint64_t epoch, bool stitchBands) {
        const auto gain = std::polar(0.65, 73.0 * Tau / 360.0);
        std::array<std::array<float, VirtualBlock * 2>, 2> iq;
        for (int i = 0; i < VirtualBlock; ++i) {
            const double seconds = double(first + i) / VirtualRate;
            const auto signal = std::polar(0.21, Tau * 3700 * seconds) +
                std::polar(0.09, -Tau * 6800 * seconds) + 0.22 * noise();
            const auto delayed = delay[cursor];
            delay[cursor] = signal;
            cursor = (cursor + 1) % delay.size();
            const auto a = stitchBands ? std::polar(0.6, Tau * 3750 * seconds) : signal + 0.02 * noise();
            const auto b = stitchBands ? std::polar(0.4, -Tau * 7500 * seconds) : gain * delayed + 0.02 * noise();
            iq[0][2 * i] = float(a.real()); iq[0][2 * i + 1] = float(a.imag());
            iq[1][2 * i] = float(b.real()); iq[1][2 * i + 1] = float(b.imag());
        }
        DspIqTimingMetadata timing;
        timing.epoch = epoch;
        timing.firstComplexSample = first;
        timing.clockId = QStringLiteral("virtual-coherent-pair");
        timing.sharedSampleTimeline = timing.sampleClockValid = timing.clockLocked = true;
        timing.nativeSampleFormat = QStringLiteral("Virtual FC32");
        for (std::size_t i = 0; i < iq.size(); ++i) {
            const auto center = VirtualCenter + (stitchBands ? (double(i) - 0.5) * VirtualRate : 0.0);
            DspIqSourceRegistry::instance().publish(sources[i], iq[i].data(), iq[i].size(),
                VirtualRate, center, false, &timing);
        }
        first += VirtualBlock;
    }

private:
    std::complex<double> noise() { return {distribution(random), distribution(random)}; }
    std::mt19937 random{731};
    std::uniform_real_distribution<double> distribution{-1.0, 1.0};
    std::array<std::complex<double>, VirtualDelay> delay{};
    std::size_t cursor = 0;
    std::uint64_t first = 0;
};

AnalyzerSpectrumFrame spectrumFrame(FftTransformCore &fft, const DspIqSourceSnapshot &iq, quint64 sequence) {
    AnalyzerSpectrumFrame frame;
    const int length = fft.length();
    fft.stageInputRange(iq.iq.data(), 0, length);
    fft.execute();
    std::vector<float> magnitudes(length);
    fft.writeMagnitudes(magnitudes.data(), 0, length, 20.0 * std::log10(fft.windowAmplitudeSum(length)));
    frame.levelsDb.resize(length);
    fft.writeShiftedMagnitudes(magnitudes.data(), frame.levelsDb.data());
    frame.sequence = sequence;
    frame.utcMs = QDateTime::currentMSecsSinceEpoch();
    frame.centerHz = iq.centerHz;
    frame.minimumHz = iq.centerHz - iq.sampleRateHz * 0.5;
    frame.maximumHz = iq.centerHz + iq.sampleRateHz * (0.5 - 1.0 / length);
    frame.sourceFftLength = length;
    return frame;
}

bool preparePreviewFft(FftTransformCore &fft) {
    if (fft.allocateBuffers(CoherentIqLive::BlockSamples, FftTransformCore::Storage::OutOfPlace)) {
        std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
        fft.createPlanLocked();
    }
    if (!fft.ready()) return false;
    fft.ensureWindow(FFT_WINDOW_HANN);
    return true;
}
}

class MultiSdrLiveIq::Recorder final {
public:
    explicit Recorder(const QString &directory) : writer(&Recorder::run, this, directory) {}
    ~Recorder() { close(); join(); }

    void submit(std::vector<DspIqSourceSnapshot> block, const std::vector<double> &combiningWeights) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closing) return;
        if (queue.size() >= 64) {
            closing = true;
            interruption = QStringLiteral("IQ recording disk queue overflow");
        } else {
            if (weights.empty() && !combiningWeights.empty()) weights = combiningWeights;
            queue.push_back(std::move(block));
        }
        ready.notify_one();
    }

    void close(const QString &reason = {}) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closing) return;
        closing = true;
        interruption = reason;
        ready.notify_one();
    }

    void join() {
        if (writer.joinable()) writer.join();
    }

    RecordingStatus status() const {
        std::lock_guard<std::mutex> lock(mutex);
        return {!closing, closing && !finished, samples, manifest, interruption};
    }

private:
    void run(QString directory) {
        CoherentIqRecording recording;
        QString error;
        std::vector<DspIqSourceSnapshot> segment;
        if (recording.create(directory, error)) {
            const auto flush = [&]() {
                if (segment.empty()) return true;
                if (!recording.append(segment, error)) return false;
                std::lock_guard<std::mutex> lock(mutex);
                samples += segment.front().iq.size() / 2;
                segment.clear();
                return true;
            };
            while (error.isEmpty()) {
                std::vector<DspIqSourceSnapshot> block;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ready.wait(lock, [&]() { return closing || !queue.empty(); });
                    if (queue.empty()) break;
                    block = std::move(queue.front());
                    queue.pop_front();
                }
                if (segment.empty()) {
                    segment = std::move(block);
                } else if (block.size() == segment.size() && block.front().epoch == segment.front().epoch &&
                           block.front().startComplexSample == segment.front().endComplexSample) {
                    for (std::size_t i = 0; i < segment.size(); ++i) {
                        auto &channel = segment[i];
                        channel.iq.insert(channel.iq.end(), block[i].iq.begin(), block[i].iq.end());
                        channel.endComplexSample = block[i].endComplexSample;
                    }
                } else {
                    const auto gap = QStringLiteral("IQ recording sample timeline changed");
                    if (flush()) error = gap;
                }
                if (!segment.empty() && segment.front().iq.size() / 2 >= 2 * segment.front().sampleRateHz)
                    flush();
            }
            if (error.isEmpty()) flush();
        }
        QString reason;
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
            reason = error.isEmpty() ? interruption : error;
            queue.clear();
        }
        QString finishError;
        const QString path = recording.finish(reason, finishError, weights);
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
        manifest = path;
        interruption = reason;
        if (!finishError.isEmpty()) {
            if (!interruption.isEmpty()) interruption += QStringLiteral(" — ");
            interruption += finishError;
        }
    }

    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::vector<DspIqSourceSnapshot>> queue;
    bool closing = false;
    bool finished = false;
    quint64 samples = 0;
    QString manifest;
    QString interruption;
    std::vector<double> weights;
    std::thread writer;
};

MultiSdrLiveIq::~MultiSdrLiveIq() { stop(); }

QString MultiSdrLiveIq::start(std::vector<DspSourceIdentity> sources, int maximumDelay, bool virtualPair,
                              bool alignBySignal, bool stitchBands, CoherentIqLiveOptions options) {
    if (stitchBands && (alignBySignal || (!virtualPair && sources.size() != 2)))
        return QStringLiteral("IQ band stitching requires two synchronized sources");
    stop();
    stopping = false;
    signalRestart = false;
    {
        std::lock_guard<std::mutex> lock(previewMutex);
        recordable = !stitchBands;
    }
    try {
        worker = std::thread(&MultiSdrLiveIq::run, this, std::move(sources), maximumDelay,
                             virtualPair, alignBySignal, stitchBands, options);
    } catch (const std::system_error &) {
        std::lock_guard<std::mutex> lock(previewMutex);
        recordable = false;
        return QStringLiteral("Could not start the coherent IQ worker");
    }
    return {};
}

QString MultiSdrLiveIq::startReplay(const QString &manifest) {
    stop();
    stopping = false;
    try {
        worker = std::thread(&MultiSdrLiveIq::runReplay, this, manifest);
    } catch (const std::system_error &) {
        return QStringLiteral("Could not start the IQ replay worker");
    }
    return {};
}

void MultiSdrLiveIq::stop() {
    stopping = true;
    if (worker.joinable()) worker.join();
    const auto current = activeRecorder();
    if (current) {
        current->close();
        current->join();
    }
    std::lock_guard<std::mutex> lock(previewMutex);
    recordable = false;
    latest = {};
}

std::shared_ptr<MultiSdrLiveIq::Recorder> MultiSdrLiveIq::activeRecorder() const {
    std::lock_guard<std::mutex> lock(previewMutex);
    return recorder;
}

QString MultiSdrLiveIq::startRecording(const QString &directory) {
    std::lock_guard<std::mutex> lock(previewMutex);
    if (!recordable) return QStringLiteral("Start live coherent IQ before recording");
    if (recorder) {
        const auto status = recorder->status();
        if (status.active || status.finishing) return QStringLiteral("IQ recording is already running");
    }
    try {
        recorder = std::make_shared<Recorder>(directory);
    } catch (const std::system_error &) {
        return QStringLiteral("Could not start the IQ recording writer");
    }
    return {};
}

void MultiSdrLiveIq::stopRecording(const QString &reason) {
    if (const auto current = activeRecorder()) current->close(reason);
}

MultiSdrLiveIq::RecordingStatus MultiSdrLiveIq::recordingStatus() const {
    const auto current = activeRecorder();
    return current ? current->status() : RecordingStatus{};
}

MultiSdrLiveIq::Preview MultiSdrLiveIq::preview() const {
    std::lock_guard<std::mutex> lock(previewMutex);
    return latest;
}

void MultiSdrLiveIq::run(std::vector<DspSourceIdentity> sources, int maximumDelay, bool virtualPair,
                         bool alignBySignal, bool stitchBands, CoherentIqLiveOptions options) {
    auto &registry = DspIqSourceRegistry::instance();
    const auto outputIdentity = CoherentIqLive::outputIdentity();
    if (virtualPair) {
        sources = {DspSourceIdentity::normalized(QStringLiteral("coherent-virtual-a"), QStringLiteral("rx0")),
                   DspSourceIdentity::normalized(QStringLiteral("coherent-virtual-b"), QStringLiteral("rx0"))};
        maximumDelay = std::max(maximumDelay, VirtualDelay + 1);
    }
    QSet<QString> keys;
    for (const auto &source : sources) keys.insert(source.key());
    registry.setSubscriptions(this, keys);
    QString error;
    FftTransformCore fft;
    if (!preparePreviewFft(fft)) error = QStringLiteral("Could not initialize the coherent spectrum FFT");
    CoherentIqLive combiner(alignBySignal ? CoherentIqLive::Timeline::Signal : CoherentIqLive::Timeline::Hardware, options);
    CoherentIqBandJoin bandJoin;
    auto signal = alignBySignal ? std::make_unique<MultiSdrSignalAlignment>() : nullptr;
    VirtualPair simulation;
    auto lastOutput = Clock::now();
    auto nextVirtual = lastOutput;
    auto nextPreview = lastOutput;
    auto nextCalibration = lastOutput;
    auto nextDelayCheck = lastOutput;
    auto epoch = nextEpoch.fetch_add(1, std::memory_order_relaxed);
    bool calibrated = false;
    quint64 sequence = 0;
    quint64 total = 0;
    std::vector<DspIqSourceSnapshot> inputs(sources.size());
    const auto waitForSignal = [&](const QString &reason) {
        if (calibrated) {
            if (const auto current = activeRecorder()) current->close(reason);
            registry.clear(outputIdentity);
            epoch = nextEpoch.fetch_add(1, std::memory_order_relaxed);
            total = 0;
            calibrated = false;
        }
        std::lock_guard<std::mutex> lock(previewMutex);
        latest = {{}, 0, int(inputs.size()), {}, reason};
    };
    while (!stopping && error.isEmpty()) {
        const auto now = Clock::now();
        if (virtualPair) {
            if (now - nextVirtual > 150ms) {
                error = QStringLiteral("Virtual source clock fell behind; restart the test");
                break;
            }
            while (now >= nextVirtual) {
                simulation.publish(sources, epoch, stitchBands);
                nextVirtual += 10ms;
            }
        }
        bool complete = true;
        if (signal) {
            if (signalRestart.exchange(false) || (calibrated && now - lastOutput > 500ms)) {
                signal->discardHistory();
                waitForSignal(QStringLiteral("Waiting for two Fobos IQ streams"));
            }
            bool received = false, gap = false;
            if (!signal->update(sources, received, gap, error)) break;
            if (gap) waitForSignal(QStringLiteral("IQ discontinuity; reacquiring signal alignment"));
            if (calibrated && received && now >= nextDelayCheck) {
                nextDelayCheck = now + 750ms;
                QString reason;
                if (!signal->align(true, reason)) waitForSignal(reason);
            }
            if (!calibrated && received && now >= nextCalibration) {
                nextCalibration = now + 200ms;
                QString reason;
                if (signal->align(false, reason)) {
                    signal->windows(inputs);
                    calibrated = combiner.calibrate(inputs, 4, reason);
                    nextDelayCheck = now + 750ms;
                    lastOutput = now;
                }
                if (!calibrated) waitForSignal(reason);
            }
            if (calibrated && received) signal->windows(inputs);
            complete = received;
        } else {
            for (std::size_t i = 0; i < sources.size(); ++i)
                complete &= registry.snapshotSince(sources[i], 0, 0, 32768, inputs[i], false);
        }
        if (!signal && complete && !calibrated && now >= nextCalibration) {
            nextCalibration = now + 50ms;
            QString calibrationError;
            calibrated = stitchBands ? bandJoin.configure(inputs, calibrationError)
                                     : combiner.calibrate(inputs, maximumDelay, calibrationError);
            if (!calibrated && (now - lastOutput > 2s ||
                std::any_of(inputs.begin(), inputs.end(), [](const auto &input) {
                    return !input.sharedSampleTimeline || input.sampleRateHz > CoherentIqLive::MaximumRate;
                }))) {
                error = calibrationError;
                break;
            }
        }
        if (complete && calibrated) {
            auto current = activeRecorder();
            if (current && !current->status().active) current.reset();
            for (int attempt = 0; attempt < (signal ? 8 : 3) && !stopping; ++attempt) {
                DspIqSourceSnapshot output;
                std::vector<std::vector<float>> aligned;
                const bool ready = stitchBands ? bandJoin.takeBlock(inputs, output, error)
                    : combiner.takeBlock(inputs, output, error, current ? &aligned : nullptr);
                if (!ready || output.iq.empty()) break;
                DspIqTimingMetadata timing;
                timing.epoch = epoch;
                timing.firstComplexSample = output.startComplexSample;
                timing.hostTimestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                timing.clockId = output.clockId;
                timing.sampleClockValid = true;
                timing.sharedSampleTimeline = output.sharedSampleTimeline;
                timing.clockLocked = output.clockLocked;
                timing.nativeSampleFormat = stitchBands ? QStringLiteral("Stitched FC32") :
                    signal ? QStringLiteral("Signal-aligned FC32") : QStringLiteral("Coherent FC32");
                registry.publish(outputIdentity, output.iq.data(), output.iq.size(), output.sampleRateHz,
                                 output.centerHz, total == 0, &timing);
                if (current) {
                    std::vector<DspIqSourceSnapshot> block;
                    block.reserve(inputs.size());
                    for (std::size_t i = 0; i < inputs.size(); ++i) {
                        DspIqSourceSnapshot channel;
                        channel.source = inputs[i].source;
                        channel.iq = std::move(aligned[i]);
                        channel.epoch = epoch;
                        channel.startComplexSample = output.startComplexSample;
                        channel.endComplexSample = output.endComplexSample;
                        channel.hostTimestampNs = timing.hostTimestampNs;
                        channel.sampleRateHz = output.sampleRateHz;
                        channel.centerHz = output.centerHz;
                        channel.clockId = output.clockId;
                        channel.sampleClockValid = true;
                        channel.clockLocked = output.clockLocked;
                        channel.sharedSampleTimeline = output.sharedSampleTimeline;
                        channel.nativeSampleFormat = QStringLiteral("Aligned FC32");
                        block.push_back(std::move(channel));
                    }
                    current->submit(std::move(block), combiner.combiningWeights());
                }
                lastOutput = now;
                total += output.iq.size() / 2;
                if (now >= nextPreview) {
                    nextPreview = now + 100ms;
                    auto frame = spectrumFrame(fft, output, ++sequence);
                    std::lock_guard<std::mutex> lock(previewMutex);
                    latest = {std::move(frame), total, int(inputs.size()), {}, {}, combiner.maximumPhaseDriftHz(),
                              false, {}, combiner.combiningWeights()};
                }
            }
        }
        if (signal && !error.isEmpty()) {
            waitForSignal(error);
            error.clear();
            nextCalibration = now + 200ms;
        }
        if (!signal && error.isEmpty() && now - lastOutput > 2s)
            error = QStringLiteral("Timed out waiting for continuous IQ from all sources");
        if (!stopping && error.isEmpty()) std::this_thread::sleep_for(5ms);
    }
    registry.removeSubscriptions(this);
    registry.clear(outputIdentity);
    {
        std::lock_guard<std::mutex> lock(previewMutex);
        recordable = false;
        if (recorder) recorder->close(error);
    }
    if (!error.isEmpty()) {
        std::lock_guard<std::mutex> lock(previewMutex);
        latest = {{}, total, int(inputs.size()), error};
    }
}

void MultiSdrLiveIq::runReplay(QString manifest) {
    const auto outputIdentity = CoherentIqLive::outputIdentity();
    auto &registry = DspIqSourceRegistry::instance();
    CoherentIqRecording recording;
    QString error;
    FftTransformCore fft;
    if (recording.open(manifest, error) && !preparePreviewFft(fft))
        error = QStringLiteral("Could not initialize the replay spectrum FFT");
    const auto &weights = recording.combiningWeights();
    std::vector<DspIqSourceSnapshot> channels;
    auto next = Clock::now();
    auto nextPreview = next;
    const auto waitForBlock = [&]() {
        while (!stopping && Clock::now() < next) {
            // A low sample rate must not delay a user-requested stop.
            std::this_thread::sleep_until(std::min(next, Clock::now() + 5ms));
        }
    };
    quint64 total = 0;
    quint64 sequence = 0;
    DspIqTimingMetadata timing;
    timing.epoch = nextEpoch.fetch_add(1, std::memory_order_relaxed);
    timing.sampleClockValid = true;
    timing.nativeSampleFormat = QStringLiteral("Replay FC32");
    constexpr int Block = CoherentIqLive::BlockSamples;
    while (!stopping && error.isEmpty() && recording.readNext(channels, error) && !channels.empty()) {
        const auto &reference = channels.front();
        if (reference.iq.size() % (2 * Block) != 0) {
            error = QStringLiteral("IQ recording segment ends inside a processing block");
            break;
        }
        DspIqSourceSnapshot output;
        output.iq.resize(2 * Block);
        output.sampleRateHz = reference.sampleRateHz;
        output.centerHz = reference.centerHz;
        const auto blockDuration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(Block / reference.sampleRateHz));
        for (std::size_t first = 0; first < reference.iq.size() && !stopping; first += 2 * Block) {
            waitForBlock();
            if (stopping) break;
            const auto now = Clock::now();
            next = (now - next > 100ms ? now : next) + blockDuration;
            for (std::size_t n = 0; n < output.iq.size(); ++n) {
                double sum = 0.0;
                for (std::size_t i = 0; i < channels.size(); ++i)
                    sum += channels[i].iq[first + n] * (weights.empty() ? 1.0 : weights[i]);
                output.iq[n] = float(weights.empty() ? sum / double(channels.size()) : sum);
            }
            timing.firstComplexSample = reference.startComplexSample + first / 2;
            timing.hostTimestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
            registry.publish(outputIdentity, output.iq.data(), output.iq.size(), output.sampleRateHz,
                             output.centerHz, total == 0, &timing);
            total += Block;
            if (now >= nextPreview) {
                nextPreview = now + 100ms;
                auto frame = spectrumFrame(fft, output, ++sequence);
                std::lock_guard<std::mutex> lock(previewMutex);
                latest = {std::move(frame), total, int(channels.size()), {}, {}, {}, false, {}, weights};
            }
        }
    }
    if (!stopping && error.isEmpty() && total == 0)
        error = QStringLiteral("IQ recording is empty");
    if (error.isEmpty()) waitForBlock();
    registry.clear(outputIdentity);
    std::lock_guard<std::mutex> lock(previewMutex);
    latest.combinedSamples = total;
    latest.error = error;
    latest.replayFinished = true;
    latest.replayInterruption = recording.interruptionReason();
}
