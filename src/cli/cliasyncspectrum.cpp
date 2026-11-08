#include "cliasyncspectrum.h"
#include "clispectrumaccumulator.h"
#include "clispectrummeter.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

struct CliAsyncSpectrum::Impl : QThread {
    explicit Impl(int length) : size(length), accumulator(length) {}
    ~Impl() override {
        { std::lock_guard<std::mutex> lock(mutex); quitting = true; }
        changed.notify_one();
        wait(); // Only after RX stops; FFTW execution cannot be safely killed.
    }
    struct Job {
        std::unique_ptr<float[]> iq;
        quint64 generation = 0, firstSample = 0;
        double rate = 0, center = 0;
        bool fullTrace = true;
        qint64 utcMs = 0;
    } job;
    struct Completed {
        CliSpectrumResult result;
        std::unique_ptr<float[]> iq;
        quint64 generation = 0;
    } completed;
    const int size;
    CliSpectrumAccumulator accumulator;
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool quitting = false, occupied = false, pending = false, haveResult = false;
    bool started = false;
    quint64 generation = 0, cursor = 0, windowStart = 0;
    quint64 submitted = 0, delivered = 0, stale = 0, skippedBusy = 0, skippedInterval = 0;
    double rate = 0, center = 0, lastComputeMs = 0;
    QElapsedTimer lastSubmission;

    void run() override {
        std::unique_ptr<CliSpectrumMeter> meter;
        for (;;) {
            Job current;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return quitting || pending; });
                if (quitting) return;
                current = std::move(job); pending = false;
            }
            Completed next;
            next.generation = current.generation;
            auto &result = next.result;
            result.sampleRateHz = current.rate; result.centerHz = current.center;
            result.firstSample = current.firstSample; result.utcMs = current.utcMs;
            QElapsedTimer timer; timer.start();
            try {
                if (!meter) meter = std::make_unique<CliSpectrumMeter>(size);
                if (!meter->valid()) result.error = QStringLiteral("Cannot create asynchronous FFT plan");
                else {
                    result.summary = meter->analyze(current.iq.get(), size, current.rate, current.center, &result.trace);
                    if (result.summary.isEmpty()) result.error = QStringLiteral("Asynchronous FFT failed or could not allocate its result");
                    else {
                        const int columns = (std::min)(size, 512);
                        result.display.resize(columns);
                        for (int i = 0; i < columns; ++i) {
                            const auto first = static_cast<qint64>(i) * size / columns;
                            const auto last = static_cast<qint64>(i + 1) * size / columns;
                            result.display[i] = *std::max_element(result.trace.begin() + first, result.trace.begin() + last);
                        }
                        if (!current.fullTrace) std::vector<float>().swap(result.trace);
                    }
                }
            } catch (const std::bad_alloc &) { result.error = QStringLiteral("Asynchronous FFT allocation failed"); }
            catch (const std::length_error &) { result.error = QStringLiteral("Asynchronous FFT size exceeds available storage"); }
            result.computeMs = timer.nsecsElapsed() / 1e6;
            next.iq = std::move(current.iq);
            {
                std::lock_guard<std::mutex> lock(mutex);
                completed = std::move(next); haveResult = true;
            }
        }
    }
};

CliAsyncSpectrum::CliAsyncSpectrum(int size) : impl(new Impl(size)) {}
CliAsyncSpectrum::~CliAsyncSpectrum() = default;
bool CliAsyncSpectrum::valid() const { return impl->accumulator.valid(); }
bool CliAsyncSpectrum::busy() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->occupied;
}
void CliAsyncSpectrum::reset() {
    ++impl->generation;
    impl->accumulator.reset();
    impl->rate = impl->center = 0;
    impl->lastSubmission.invalidate();
}
void CliAsyncSpectrum::submit(const float *iq, int count, double rate, double center, int intervalMs, bool fullTrace) {
    auto &p = *impl;
    if (!valid() || !iq || count <= 0 || !std::isfinite(rate) || rate <= 0 || !std::isfinite(center)) return;
    const quint64 first = p.cursor; p.cursor += static_cast<quint64>(count);
    if (p.rate != rate || p.center != center) { reset(); p.rate = rate; p.center = center; }
    if (busy()) { p.skippedBusy += count; return; }
    if (!p.accumulator.available() && p.lastSubmission.isValid() && p.lastSubmission.elapsed() < (std::max)(0, intervalMs)) {
        p.skippedInterval += count; return;
    }
    if (!p.accumulator.available()) p.windowStart = first;
    const int consumed = p.accumulator.append(iq, count);
    if (consumed < 0) {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.completed.result = {};
        p.completed.result.error = QStringLiteral("Cannot allocate IQ window for asynchronous FFT");
        p.completed.generation = p.generation;
        p.occupied = p.haveResult = true;
        return;
    }
    if (!p.accumulator.ready()) return;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.job = {};
        p.job.iq = p.accumulator.takeWindow();
        p.job.generation = p.generation; p.job.firstSample = p.windowStart;
        p.job.rate = rate; p.job.center = center; p.job.fullTrace = fullTrace;
        p.job.utcMs = QDateTime::currentMSecsSinceEpoch();
        p.occupied = p.pending = true;
        ++p.submitted;
    }
    p.skippedBusy += count - consumed;
    p.lastSubmission.start();
    if (!p.started) { p.started = true; p.start(QThread::LowPriority); }
    p.changed.notify_one();
}
bool CliAsyncSpectrum::poll(CliSpectrumResult &result) {
    auto &p = *impl;
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.haveResult) return false;
    p.haveResult = p.occupied = false;
    p.accumulator.recycleWindow(std::move(p.completed.iq));
    if (p.completed.generation != p.generation) {
        ++p.stale; p.completed.result = {}; return false;
    }
    result = std::move(p.completed.result);
    p.lastComputeMs = result.computeMs;
    ++p.delivered;
    result.summary.insert(QStringLiteral("spectrumFirstSample"), QString::number(result.firstSample));
    result.summary.insert(QStringLiteral("spectrumUtcMs"), static_cast<double>(result.utcMs));
    result.summary.insert(QStringLiteral("fftComputeMs"), result.computeMs);
    result.summary.insert(QStringLiteral("spectrumEpoch"), QString::number(p.generation));
    return true;
}
QJsonObject CliAsyncSpectrum::status() const {
    const auto &p = *impl;
    return {{"asynchronous", true}, {"workerStarted", p.started}, {"busy", busy()},
        {"fftSize", p.size}, {"accumulatedSamples", p.accumulator.available()},
        {"submittedWindows", QString::number(p.submitted)}, {"completedWindows", QString::number(p.delivered)},
        {"discardedStaleWindows", QString::number(p.stale)}, {"skippedBusySamples", QString::number(p.skippedBusy)},
        {"skippedIntervalSamples", QString::number(p.skippedInterval)}, {"lastComputeMs", p.lastComputeMs},
        {"maximumInFlightWindows", 1}, {"continuousTemporalCoverage", false}};
}
