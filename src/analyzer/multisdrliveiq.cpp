#include "multisdrliveiq.h"

#include "coherentiqlive.h"
#include "fftplannermutex.h"
#include "ffttransformcore.h"
#include "multisdrsignalalignment.h"

#include <QDateTime>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
    void publish(const std::vector<DspSourceIdentity> &sources, std::uint64_t epoch) {
        const auto gain = std::polar(0.65, 73.0 * Tau / 360.0);
        std::array<std::array<float, VirtualBlock * 2>, 2> iq;
        for (int i = 0; i < VirtualBlock; ++i) {
            const double seconds = double(first + i) / VirtualRate;
            const auto signal = std::polar(0.21, Tau * 3700 * seconds) +
                std::polar(0.09, -Tau * 6800 * seconds) + 0.22 * noise();
            const auto delayed = delay[cursor];
            delay[cursor] = signal;
            cursor = (cursor + 1) % delay.size();
            const auto a = signal + 0.02 * noise();
            const auto b = gain * delayed + 0.02 * noise();
            iq[0][2 * i] = float(a.real()); iq[0][2 * i + 1] = float(a.imag());
            iq[1][2 * i] = float(b.real()); iq[1][2 * i + 1] = float(b.imag());
        }
        DspIqTimingMetadata timing;
        timing.epoch = epoch;
        timing.firstComplexSample = first;
        timing.clockId = QStringLiteral("virtual-coherent-pair");
        timing.sharedSampleTimeline = timing.sampleClockValid = timing.clockLocked = true;
        timing.nativeSampleFormat = QStringLiteral("Virtual FC32");
        for (std::size_t i = 0; i < iq.size(); ++i)
            DspIqSourceRegistry::instance().publish(sources[i], iq[i].data(), iq[i].size(),
                VirtualRate, VirtualCenter, false, &timing);
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
}

MultiSdrLiveIq::~MultiSdrLiveIq() { stop(); }

QString MultiSdrLiveIq::start(std::vector<DspSourceIdentity> sources, int maximumDelay, bool virtualPair, bool alignBySignal) {
    stop();
    stopping = false;
    signalRestart = false;
    try {
        worker = std::thread(&MultiSdrLiveIq::run, this, std::move(sources), maximumDelay, virtualPair, alignBySignal);
    } catch (const std::system_error &) {
        return QStringLiteral("Could not start the coherent IQ worker");
    }
    return {};
}

void MultiSdrLiveIq::stop() {
    stopping = true;
    if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> lock(previewMutex);
    latest = {};
}

MultiSdrLiveIq::Preview MultiSdrLiveIq::preview() const {
    std::lock_guard<std::mutex> lock(previewMutex);
    return latest;
}

void MultiSdrLiveIq::run(std::vector<DspSourceIdentity> sources, int maximumDelay, bool virtualPair, bool alignBySignal) {
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
    if (fft.allocateBuffers(CoherentIqLive::BlockSamples, FftTransformCore::Storage::OutOfPlace)) {
        std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
        fft.createPlanLocked();
    }
    if (!fft.ready()) error = QStringLiteral("Could not initialize the coherent spectrum FFT");
    else fft.ensureWindow(FFT_WINDOW_HANN);
    CoherentIqLive combiner(alignBySignal ? CoherentIqLive::Timeline::Signal : CoherentIqLive::Timeline::Hardware);
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
                simulation.publish(sources, epoch);
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
            calibrated = combiner.calibrate(inputs, maximumDelay, calibrationError);
            if (!calibrated && (now - lastOutput > 2s ||
                std::any_of(inputs.begin(), inputs.end(), [](const auto &input) {
                    return !input.sharedSampleTimeline || input.sampleRateHz > CoherentIqLive::MaximumRate;
                }))) {
                error = calibrationError;
                break;
            }
        }
        if (complete && calibrated) {
            for (int attempt = 0; attempt < (signal ? 8 : 3) && !stopping; ++attempt) {
                DspIqSourceSnapshot output;
                if (!combiner.takeBlock(inputs, output, error) || output.iq.empty()) break;
                DspIqTimingMetadata timing;
                timing.epoch = epoch;
                timing.firstComplexSample = output.startComplexSample;
                timing.hostTimestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                timing.clockId = output.clockId;
                timing.sampleClockValid = true;
                timing.sharedSampleTimeline = output.sharedSampleTimeline;
                timing.clockLocked = output.clockLocked;
                timing.nativeSampleFormat = signal ? QStringLiteral("Signal-aligned FC32") : QStringLiteral("Coherent FC32");
                registry.publish(outputIdentity, output.iq.data(), output.iq.size(), output.sampleRateHz,
                                 output.centerHz, total == 0, &timing);
                lastOutput = now;
                total += output.iq.size() / 2;
                if (now >= nextPreview) {
                    nextPreview = now + 100ms;
                    auto frame = spectrumFrame(fft, output, ++sequence);
                    std::lock_guard<std::mutex> lock(previewMutex);
                    latest = {std::move(frame), total, int(inputs.size()), {}};
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
    if (!error.isEmpty()) {
        std::lock_guard<std::mutex> lock(previewMutex);
        latest = {{}, total, int(inputs.size()), error};
    }
}
