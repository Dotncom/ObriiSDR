#include "spectrumfftworker.h"

#include "diagnosticlogging.h"
#include "fft.h"
#include "remotespectrumworker.h"

#include <QElapsedTimer>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <utility>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
std::size_t maxRowsPerFftBatch(int fftLength) {
    if (fftLength <= 32768) {
        return 32;
    }
    if (fftLength <= 65536) {
        return 16;
    }
    if (fftLength <= 131072) {
        return 8;
    }
    if (fftLength <= 262144) {
        return 4;
    }
    return 1;
}
}

SpectrumFftWorker::SpectrumFftWorker() { workerThread = std::thread(&SpectrumFftWorker::run, this); }

SpectrumFftWorker::~SpectrumFftWorker() {
    remoteWorker.reset();
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        hasRequest = false;
        hasResult = false;
        requestOutstanding.store(false, std::memory_order_release);
        resultAvailable.store(false, std::memory_order_release);
        ++generation;
    }
    wakeCondition.notify_one();
    if (workerThread.joinable()) {
        workerThread.join();
    }
}

void SpectrumFftWorker::request(const RadioSettings &settings,
                                int fftBackendPreference,
                                int updateIntervalMs,
                                int overlapPercent,
                                bool batchWaterfallRows) {
    if (remoteEnabled()) {
        if (!remoteWorker) remoteWorker = std::make_unique<RemoteSpectrumWorker>();
        remoteWorker->request(settings, remoteEndpoint, remoteOptions, updateIntervalMs, overlapPercent, fftBackendPreference);
        return;
    }
    bool expected = false;
    if (!requestOutstanding.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        pendingSettings = settings;
        pendingFftBackendPreference = normalizedFftBackendPreference(fftBackendPreference);
        pendingUpdateIntervalMs = (std::max)(0, updateIntervalMs);
        pendingOverlapPercent = (std::clamp)(overlapPercent, 0, 95);
        pendingBatchWaterfallRows = batchWaterfallRows;
        pendingGeneration = generation;
        hasRequest = true;
        ++nextRequestId;
    }
    wakeCondition.notify_one();
}

bool SpectrumFftWorker::takeLatest(SpectrumFftFrame &frame) {
    if (remoteEnabled()) return remoteWorker && remoteWorker->takeLatest(frame);
    std::lock_guard<std::mutex> lock(mutex);
    if (!hasResult) {
        return false;
    }
    frame = std::move(latestResult);
    hasResult = false;
    resultAvailable.store(false, std::memory_order_release);
    return true;
}

void SpectrumFftWorker::resetHfNoiseCancelState() {
    if (remoteWorker) remoteWorker->cancel();
    {
        std::lock_guard<std::mutex> lock(mutex);
        hasRequest = false;
        hasResult = false;
        requestOutstanding.store(false, std::memory_order_release);
        resultAvailable.store(false, std::memory_order_release);
        resetRequested = true;
        ++generation;
    }
    wakeCondition.notify_one();
}

void SpectrumFftWorker::configureRemote(const DspStreamEndpoint &endpoint, const SpectrumComputeOptions &options) {
    if (remoteEndpoint == endpoint && remoteOptions == options) return;
    releaseLocalPlan.store(options.mode != SpectrumComputeMode::Local, std::memory_order_release);
    resetHfNoiseCancelState();
    remoteEndpoint = endpoint; remoteOptions = options;
}
QString SpectrumFftWorker::remoteStatus() const { return remoteWorker ? remoteWorker->status() : QString(); }

void SpectrumFftWorker::run() {
#ifdef __linux__
    // Keep live audio responsive on small ARM systems. Linux applies nice
    // values per thread, so only the background FFT worker is deprioritized.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 5);
#endif
    std::unique_ptr<FFTResult> fft;
    QElapsedTimer profileLogTimer;
    profileLogTimer.start();

    for (;;) {
        RadioSettings settings;
        quint64 requestId = 0;
        quint64 requestGeneration = 0;
        int backendPreference = FFT_BACKEND_AUTO;
        int updateIntervalMs = 0;
        int overlapPercent = 0;
        bool batchWaterfallRows = false;
        bool doReset = false;

        {
            std::unique_lock<std::mutex> lock(mutex);
            wakeCondition.wait(lock, [this]() {
                return stopping || hasRequest || resetRequested;
            });
            if (stopping) {
                break;
            }

            doReset = resetRequested;
            resetRequested = false;
            if (doReset) {
                if (releaseLocalPlan.exchange(false)) fft.reset();
                else if (fft) fft->resetHfNoiseCancelState();
                lastProducedEndFloatCount = 0;
                lastProducedEpoch = 0;
                lastProducedFftLength = 0;
                lastProducedSampleRate = 0.0;
                lastProducedUpdateIntervalMs = 0;
                lastProducedOverlapPercent = 0;
            }

            if (!hasRequest) {
                continue;
            }

            settings = pendingSettings;
            backendPreference = pendingFftBackendPreference;
            updateIntervalMs = pendingUpdateIntervalMs;
            overlapPercent = pendingOverlapPercent;
            batchWaterfallRows = pendingBatchWaterfallRows;
            requestId = nextRequestId;
            requestGeneration = pendingGeneration;
            hasRequest = false;
        }

        SpectrumFftFrame frame;
        frame.requestId = requestId;
        frame.generation = requestGeneration;
        frame.settings = settings;
        QElapsedTimer requestTimer;
        requestTimer.start();
        std::uint64_t skippedRows = 0;
        std::size_t requestedRows = 0;

        try {
            if (!fft) fft = std::make_unique<FFTResult>(true);
            fft->setBackendPreference(backendPreference);
            if (!batchWaterfallRows) {
                frame.valid = fft->storeFFTResults(settings,
                                                  frame.frequencies,
                                                  frame.magnitudes,
                                                  &frame.referenceMagnitudes,
                                                  &frame.metadata);
                lastProducedEndFloatCount = frame.metadata.totalFloatCount;
                lastProducedEpoch = frame.metadata.epoch;
                lastProducedFftLength = settings.fftLength;
                lastProducedSampleRate = settings.sampleRate;
                lastProducedUpdateIntervalMs = updateIntervalMs;
                lastProducedOverlapPercent = overlapPercent;
            } else {
            const IqBuffer::Stats iqStats = IqBuffer::stats();
            const std::uint64_t fftFloats = static_cast<std::uint64_t>(
                (std::max)(1, settings.fftLength)) * 2ULL;
            const std::uint64_t intervalFloats = updateIntervalMs > 0 &&
                                                         settings.sampleRate > 0.0
                                                     ? static_cast<std::uint64_t>(std::llround(
                                                           settings.sampleRate * 2.0 *
                                                           static_cast<double>(updateIntervalMs) /
                                                           1000.0))
                                                     : 0ULL;
            const std::uint64_t overlapHopFloats = (std::max)(
                2ULL,
                fftFloats * static_cast<std::uint64_t>(100 - overlapPercent) / 100ULL);
            const std::uint64_t hopFloats = (std::max)(overlapHopFloats, intervalFloats);
            const bool sameTimeline = batchWaterfallRows &&
                                      iqStats.epoch == lastProducedEpoch &&
                                      settings.fftLength == lastProducedFftLength &&
                                      std::abs(settings.sampleRate - lastProducedSampleRate) < 0.5 &&
                                      updateIntervalMs == lastProducedUpdateIntervalMs &&
                                      overlapPercent == lastProducedOverlapPercent;

            std::vector<std::uint64_t> frameEnds;
            const std::size_t maxRowsPerBatch =
                maxRowsPerFftBatch(settings.fftLength);
            if (sameTimeline &&
                iqStats.totalFloatCount > lastProducedEndFloatCount &&
                hopFloats > 0) {
                std::uint64_t nextEnd = lastProducedEndFloatCount + hopFloats;
                const std::uint64_t retainedStart =
                    iqStats.totalFloatCount - static_cast<std::uint64_t>(iqStats.snapshotSize);
                const std::uint64_t earliestCompleteEnd = retainedStart + fftFloats;
                if (nextEnd < earliestCompleteEnd) {
                    nextEnd = earliestCompleteEnd;
                }
                if (nextEnd <= iqStats.totalFloatCount) {
                    const std::uint64_t availableRows =
                        (iqStats.totalFloatCount - nextEnd) / hopFloats + 1ULL;
                    if (availableRows > maxRowsPerBatch) {
                        skippedRows = availableRows - maxRowsPerBatch;
                        nextEnd += skippedRows * hopFloats;
                    }
                    for (std::uint64_t end = nextEnd;
                         end <= iqStats.totalFloatCount &&
                         frameEnds.size() < maxRowsPerBatch;
                         end += hopFloats) {
                        frameEnds.push_back(end);
                        if (std::numeric_limits<std::uint64_t>::max() - end < hopFloats) {
                            break;
                        }
                    }
                }
            }
            if (frameEnds.empty() &&
                (!sameTimeline || lastProducedEndFloatCount == 0) &&
                iqStats.totalFloatCount >= fftFloats) {
                frameEnds.push_back(iqStats.totalFloatCount);
            }
            requestedRows = frameEnds.size();

            std::vector<float> rowFrequencies;
            for (const std::uint64_t frameEnd : frameEnds) {
                std::vector<float> rowMagnitudes;
                std::vector<float> rowReferenceMagnitudes;
                IqBuffer::BlockMetadata rowMetadata;
                if (!fft->storeFFTResults(settings,
                                         rowFrequencies,
                                         rowMagnitudes,
                                         &rowReferenceMagnitudes,
                                         &rowMetadata,
                                         frameEnd)) {
                    continue;
                }
                if (!frame.magnitudes.empty()) {
                    SpectrumFftHistoryRow historyRow;
                    historyRow.magnitudes = std::move(frame.magnitudes);
                    historyRow.referenceMagnitudes = std::move(frame.referenceMagnitudes);
                    historyRow.metadata = frame.metadata;
                    frame.historyRows.push_back(std::move(historyRow));
                }
                frame.frequencies.swap(rowFrequencies);
                frame.magnitudes = std::move(rowMagnitudes);
                frame.referenceMagnitudes = std::move(rowReferenceMagnitudes);
                frame.metadata = rowMetadata;
                frame.valid = true;
                lastProducedEndFloatCount = frameEnd;
            }

            lastProducedEpoch = iqStats.epoch;
            lastProducedFftLength = settings.fftLength;
            lastProducedSampleRate = settings.sampleRate;
            lastProducedUpdateIntervalMs = updateIntervalMs;
            lastProducedOverlapPercent = overlapPercent;
            }
        } catch (const std::bad_alloc &error) {
            frame.badAlloc = true;
            frame.error = QString::fromLatin1(error.what());
        } catch (const std::exception &error) {
            frame.error = QString::fromLatin1(error.what());
        } catch (...) {
            frame.error = QStringLiteral("unknown exception");
        }

        if (fobosVerboseLoggingEnabled() && profileLogTimer.elapsed() >= 1000) {
            const auto delivery = IqBuffer::discontinuityStats();
            qInfo() << "[FFT worker]"
                    << "length" << settings.fftLength
                    << "batch" << batchWaterfallRows
                    << "rows" << requestedRows
                    << "skippedStaleRows" << skippedRows
                    << "snapshotDeferred" << static_cast<qulonglong>(delivery.deferredSnapshotBlocks)
                    << "snapshotLost" << static_cast<qulonglong>(delivery.skippedSnapshotBlocks)
                    << "elapsedMs" << requestTimer.nsecsElapsed() / 1000000.0
                    << "valid" << frame.valid;
            profileLogTimer.restart();
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (requestGeneration != generation || stopping) {
                requestOutstanding.store(false, std::memory_order_release);
                continue;
            }
            latestResult = std::move(frame);
            hasResult = true;
            resultAvailable.store(true, std::memory_order_release);
            requestOutstanding.store(false, std::memory_order_release);
        }
    }
}
