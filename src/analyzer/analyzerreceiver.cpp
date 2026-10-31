#include "analyzerreceiver.h"
#include "computebudgetmanager.h"
#include "dspiqsourceregistry.h"

#include "fobosbackend.h"
#include "hackrfbackend.h"
#include "rtlsdrbackend.h"
#include "soapysdrbackend.h"
#include "fftplannermutex.h"

#include <QDateTime>
#include <QMetaObject>

#include <fftw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kThreadedFftMinimumLength = 524288;

#ifdef FOBOSAPP_HAS_FFTW_THREADS
std::once_flag analyzerFftwThreadsOnce;
bool analyzerFftwThreadsAvailable = false;

bool initializeAnalyzerFftThreads() {
    std::call_once(analyzerFftwThreadsOnce, []() {
        analyzerFftwThreadsAvailable = fftwf_init_threads() != 0;
    });
    return analyzerFftwThreadsAvailable;
}
#endif

QString receiverErrorText(const QString &operation, int result) {
    return QStringLiteral("%1 failed (%2)").arg(operation).arg(result);
}
}

struct AnalyzerReceiver::Impl {
    explicit Impl(AnalyzerReceiver *owner) : owner(owner) {}
    ~Impl() { stop(); }

    AnalyzerReceiver *owner = nullptr;
    AnalyzerReceiverSettings settings;
    std::atomic<bool> running{false};
    std::atomic<int> pendingFrames{0};
    std::atomic<bool> nativeAgileScanActive{false};
    std::atomic<double> activeCenterHz{100.0e6};
    int activeAgileScanIndex = -1;
    std::thread streamThread;
    std::thread fftThread;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<float> ring;
    std::size_t writePosition = 0;
    std::size_t availableFloats = 0;
    quint64 sequence = 0;
    std::uint64_t streamEpoch = 0;
    std::atomic<std::uint64_t> iqSampleCounter{0};
    bool clockLocked = false;
    QString error;
    fobos_dev_t *fobosDevice = nullptr;
    fobos_sdr_dev_t *fobosAgileDevice = nullptr;
    void *externalDevice = nullptr;
    void *soapyStream = nullptr;

    void reportError(const QString &message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = message;
        }
        QMetaObject::invokeMethod(owner, [this, message]() {
            emit owner->receiverError(message);
        }, Qt::QueuedConnection);
    }

    void append(const float *values, std::size_t floatCount) {
        if (!running.load(std::memory_order_acquire) || !values || floatCount < 2 || ring.empty()) return;
        floatCount -= floatCount % 2;
        if (floatCount > ring.size()) {
            values += floatCount - ring.size();
            floatCount = ring.size();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            const std::size_t first = std::min(floatCount, ring.size() - writePosition);
            std::copy_n(values, first, ring.begin() + static_cast<std::ptrdiff_t>(writePosition));
            std::copy(values + first, values + floatCount, ring.begin());
            writePosition = (writePosition + floatCount) % ring.size();
            availableFloats = std::min(ring.size(), availableFloats + floatCount);
        }
        condition.notify_one();
        const DspSourceIdentity source = DspSourceIdentity::normalized(
            settings.receiverId, settings.channelId);
        if (!settings.receiverId.trimmed().isEmpty() && source.isValid()) {
            DspIqTimingMetadata timing;
            timing.epoch = streamEpoch;
            timing.firstComplexSample = iqSampleCounter.fetch_add(
                floatCount / 2U, std::memory_order_relaxed);
            timing.hostTimestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            timing.clockId = settings.clockId;
            timing.sampleClockValid = true;
            timing.clockLocked = clockLocked;
            timing.nativeAdcBits = settings.nativeAdcBits;
            timing.nativeSampleFormat = settings.nativeSampleFormat;
            DspIqSourceRegistry::instance().publish(
                source, values, floatCount, settings.sampleRateHz,
                activeCenterHz.load(std::memory_order_acquire), false, &timing);
        }
        emit owner->iqBlockReady(values, static_cast<quint64>(floatCount),
                                 settings.sampleRateHz,
                                 activeCenterHz.load(std::memory_order_acquire));
    }

    static void fobosCallback(float *buffer, uint32_t samples, void *context) {
        auto *self = static_cast<Impl *>(context);
        if (self) self->append(buffer, static_cast<std::size_t>(samples) * 2);
    }

    static void fobosAgileCallback(float *buffer, uint32_t samples,
                                   fobos_sdr_dev_t *callbackDevice, void *context) {
        auto *self = static_cast<Impl *>(context);
        if (!self) return;
        if (!self->nativeAgileScanActive.load(std::memory_order_acquire)) {
            self->append(buffer, static_cast<std::size_t>(samples) * 2);
            return;
        }
        const int scanIndex = getFobosAgileScanIndexSafely(callbackDevice);
        if (scanIndex < 0) return;

        std::size_t floatCount = static_cast<std::size_t>(samples) * 2;
        if (!buffer || floatCount < 2 || self->ring.empty()) return;
        floatCount -= floatCount % 2;
        const float *values = buffer;
        if (floatCount > self->ring.size()) {
            values += floatCount - self->ring.size();
            floatCount = self->ring.size();
        }
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            if (!self->nativeAgileScanActive.load(std::memory_order_acquire) ||
                scanIndex >= self->settings.agileScanFrequenciesHz.size())
                return;
            if (scanIndex != self->activeAgileScanIndex) {
                self->activeAgileScanIndex = scanIndex;
                self->writePosition = 0;
                self->availableFloats = 0;
            }
            self->activeCenterHz.store(self->settings.agileScanFrequenciesHz.at(scanIndex),
                                       std::memory_order_release);
            const std::size_t first = std::min(floatCount, self->ring.size() - self->writePosition);
            std::copy_n(values, first,
                        self->ring.begin() + static_cast<std::ptrdiff_t>(self->writePosition));
            std::copy(values + first, values + floatCount, self->ring.begin());
            self->writePosition = (self->writePosition + floatCount) % self->ring.size();
            self->availableFloats = std::min(self->ring.size(), self->availableFloats + floatCount);
        }
        self->condition.notify_one();
    }

    static int hackRfCallback(HackRfTransfer *transfer) {
        auto *self = transfer ? static_cast<Impl *>(transfer->rxContext) : nullptr;
        if (!self || !transfer->buffer || transfer->validLength < 2) return 0;
        if (!self->running.load(std::memory_order_acquire)) return -1;
        const int bytes = transfer->validLength - transfer->validLength % 2;
        thread_local std::vector<float> converted;
        converted.resize(static_cast<std::size_t>(bytes));
        for (int index = 0; index < bytes; ++index) {
            converted[static_cast<std::size_t>(index)] =
                static_cast<float>(static_cast<std::int8_t>(transfer->buffer[index])) / 128.0f;
        }
        self->append(converted.data(), converted.size());
        return self->running.load(std::memory_order_acquire) ? 0 : -1;
    }

    static void rtlCallback(unsigned char *buffer, uint32_t bytes, void *context) {
        auto *self = static_cast<Impl *>(context);
        if (!self || !buffer || bytes < 2) return;
        bytes -= bytes % 2;
        thread_local std::vector<float> converted;
        converted.resize(bytes);
        for (uint32_t index = 0; index < bytes; ++index)
            converted[index] = (static_cast<float>(buffer[index]) - 127.5f) / 127.5f;
        self->append(converted.data(), converted.size());
    }

    bool configureFobos(bool agile) {
        int result = 0;
        if (agile) {
            result = openFobosAgileDeviceSafely(&fobosAgileDevice,
                                                static_cast<uint32_t>(settings.nativeIndex));
        } else {
            result = openFobosDeviceSafely(&fobosDevice,
                                           static_cast<uint32_t>(settings.nativeIndex));
        }
        if (result != 0) { reportError(receiverErrorText(QStringLiteral("Open Fobos"), result)); return false; }
        if (agile)
            result = setFobosAgileClockSourceSafely(fobosAgileDevice,
                                                    settings.clockRole == 2 ? 1 : 0);
        else
            result = setFobosClockSourceSafely(fobosDevice,
                                               settings.clockRole == 2 ? 1U : 0U);
        // libfobos exposes source selection but no lock detector. Correlation
        // calibration therefore remains the authority for a Fobos clock group.
        clockLocked = false;
        double actualRate = settings.sampleRateHz;
        if (result == 0 && agile) {
            result = setFobosAgileDirectSamplingSafely(fobosAgileDevice,
                                                       settings.directSampling ? 1U : 0U);
            if (result == 0)
                result = setFobosAgileSampleRateSafely(fobosAgileDevice, settings.sampleRateHz);
        } else if (result == 0) {
            result = setFobosDirectSamplingSafely(fobosDevice,
                                                  settings.directSampling ? 1U : 0U);
            if (result == 0)
                result = setFobosSampleRateSafely(fobosDevice, settings.sampleRateHz, &actualRate);
        }
        if (result == 0 && !settings.directSampling) {
            double actualCenter = settings.centerHz;
            result = agile
                         ? setFobosAgileFrequencySafely(fobosAgileDevice, settings.centerHz)
                         : setFobosFrequencySafely(fobosDevice, settings.centerHz, &actualCenter);
            if (result == 0 && agile) {
                double kickCenter = settings.centerHz + 1.0;
                result = setFobosAgileFrequencySafely(fobosAgileDevice, kickCenter);
                if (result == 0)
                    result = setFobosAgileFrequencySafely(fobosAgileDevice, settings.centerHz);
            }
        }
        if (result == 0) {
            result = agile
                         ? setFobosAgileLnaGainSafely(fobosAgileDevice,
                               static_cast<unsigned int>(std::max(0, settings.lnaGainDb)))
                         : setFobosLnaGainSafely(fobosDevice,
                               static_cast<unsigned int>(std::max(0, settings.lnaGainDb)));
        }
        if (result == 0) {
            result = agile
                         ? setFobosAgileVgaGainSafely(fobosAgileDevice,
                               static_cast<unsigned int>(std::max(0, settings.vgaGainDb)))
                         : setFobosVgaGainSafely(fobosDevice,
                               static_cast<unsigned int>(std::max(0, settings.vgaGainDb)));
        }
        if (result == 0 && agile && settings.agileScanFrequenciesHz.size() >= 2) {
            result = startFobosAgileScanSafely(
                fobosAgileDevice, settings.agileScanFrequenciesHz.data(),
                static_cast<unsigned int>(settings.agileScanFrequenciesHz.size()));
            nativeAgileScanActive.store(result == 0, std::memory_order_release);
            activeAgileScanIndex = -1;
        }
        if (result != 0) { reportError(receiverErrorText(QStringLiteral("Configure Fobos"), result)); return false; }
        settings.sampleRateHz = actualRate;
        return true;
    }

    void runStream() {
        int result = 0;
        if (settings.backendId == QStringLiteral("fobos-agile")) {
            if (!configureFobos(true)) { finishFromWorker(); return; }
            uint32_t blockSamples = 262144;
            if (nativeAgileScanActive.load(std::memory_order_acquire)) {
                double dwellSeconds = 0.04;
                if (settings.fftLength > 2097152) dwellSeconds = 0.16;
                else if (settings.fftLength > 1048576) dwellSeconds = 0.12;
                else if (settings.fftLength > 262144) dwellSeconds = 0.075;
                const double dwellSamples = std::max<double>(65536.0,
                                                              settings.sampleRateHz * dwellSeconds);
                const double requiredSamples = std::max<double>(settings.fftLength, dwellSamples);
                const qint64 aligned = ((static_cast<qint64>(std::ceil(requiredSamples)) + 8191) / 8192) * 8192;
                blockSamples = static_cast<uint32_t>(std::clamp<qint64>(aligned, 65536, 8388608));
            }
            result = readFobosAgileAsyncSafely(fobosAgileDevice, &Impl::fobosAgileCallback,
                                               this,
                                               nativeAgileScanActive.load(std::memory_order_acquire)
                                                   ? (blockSamples > 1048576 ? 4 : 8)
                                                   : 32,
                                               blockSamples);
        } else if (settings.backendId == QStringLiteral("fobos-standard")) {
            if (!configureFobos(false)) { finishFromWorker(); return; }
            result = readFobosAsyncSafely(fobosDevice, &Impl::fobosCallback, this, 16, 131072);
        } else if (settings.backendId == QStringLiteral("hackrf")) {
            result = openHackRfDeviceSafely(&externalDevice, settings.nativeIndex);
            if (result == 0)
                result = setHackRfClockOutEnabledSafely(externalDevice, settings.clockRole == 1);
            if (result == 0)
                result = setHackRfHardwareSyncEnabledSafely(externalDevice,
                                                            settings.hardwareStartSync);
            if (result == 0 && settings.clockRole == 2) {
                bool detected = false;
                const int clockResult = getHackRfClockInputStatusSafely(externalDevice, &detected);
                if (clockResult == 0) clockLocked = detected;
            } else if (result == 0) {
                clockLocked = settings.clockRole == 1;
            }
            if (result == 0) result = setHackRfSampleRateSafely(externalDevice, settings.sampleRateHz);
            if (result == 0) result = setHackRfBandwidthSafely(externalDevice,
                recommendedHackRfBandwidth(static_cast<uint32_t>(settings.sampleRateHz)));
            if (result == 0) result = setHackRfCenterFrequencySafely(externalDevice,
                static_cast<std::uint64_t>(settings.centerHz));
            if (result == 0) result = setHackRfLnaGainSafely(externalDevice,
                static_cast<std::uint32_t>(std::clamp(settings.lnaGainDb, 0, 40)));
            if (result == 0) result = setHackRfVgaGainSafely(externalDevice,
                static_cast<std::uint32_t>(std::clamp(settings.vgaGainDb, 0, 62)));
            if (result == 0) result = startHackRfRxSafely(externalDevice, &Impl::hackRfCallback, this);
            if (result == 0) {
                while (running.load(std::memory_order_acquire) && isHackRfStreamingSafely(externalDevice))
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        } else if (settings.backendId == QStringLiteral("rtl-sdr")) {
            result = openRtlSdrDeviceSafely(&externalDevice, static_cast<uint32_t>(settings.nativeIndex));
            if (result == 0) result = setRtlSdrSampleRateSafely(externalDevice,
                static_cast<uint32_t>(settings.sampleRateHz));
            if (result == 0) result = setRtlSdrCenterFrequencySafely(externalDevice,
                static_cast<uint32_t>(settings.centerHz));
            if (result == 0) result = setRtlSdrTunerGainModeSafely(externalDevice, 0);
            if (result == 0) result = resetRtlSdrBufferSafely(externalDevice);
            if (result == 0) result = readRtlSdrAsyncSafely(externalDevice, &Impl::rtlCallback,
                                                            this, 12, 65536);
        } else if (settings.backendId == QStringLiteral("soapy-sdr")) {
            result = openSoapySdrDeviceSafely(&externalDevice, settings.nativeIndex);
            if (result == 0) result = setSoapySdrSampleRateSafely(
                externalDevice, settings.sampleRateHz, settings.rxChannel);
            if (result == 0) result = setSoapySdrCenterFrequencySafely(
                externalDevice, settings.centerHz, settings.rxChannel);
            if (result == 0) result = setSoapySdrBandwidthSafely(
                externalDevice, settings.sampleRateHz, settings.rxChannel);
            if (result == 0) result = setSoapySdrGainSafely(
                externalDevice, settings.lnaGainDb, settings.rxChannel);
            if (result == 0) {
                soapyStream = setupSoapySdrRxStreamSafely(externalDevice, settings.rxChannel);
                if (!soapyStream) result = -1;
            }
            if (result == 0)
                result = activateSoapySdrStreamSafely(externalDevice, soapyStream);
            if (result == 0) {
                std::vector<float> buffer(262144 * 2);
                while (running.load(std::memory_order_acquire)) {
                    const int received = readSoapySdrStreamSafely(
                        externalDevice, soapyStream, buffer.data(), 262144, 100000);
                    if (received > 0)
                        append(buffer.data(), static_cast<std::size_t>(received) * 2);
                    else if (received < -1) {
                        result = received;
                        break;
                    }
                }
            }
        } else {
            reportError(QStringLiteral("Unsupported analyzer receiver backend"));
            finishFromWorker();
            return;
        }
        if (running.load(std::memory_order_acquire) && result != 0)
            reportError(receiverErrorText(QStringLiteral("Receiver stream"), result));
        finishFromWorker();
    }

    void finishFromWorker() {
        running.store(false, std::memory_order_release);
        condition.notify_all();
        QMetaObject::invokeMethod(owner, [this]() {
            // A new session may already have started while this queued callback
            // was waiting in the UI event loop. Do not let the old session move
            // the Start/Stop controls back to their stopped state.
            if (!running.load(std::memory_order_acquire))
                emit owner->runningChanged(false);
        }, Qt::QueuedConnection);
    }

    void runFft() {
        ComputeBudgetManager::instance().applyCurrentThreadRole(
            ComputeThreadRole::AuxiliaryFft, settings.computeSourceOrdinal);
        const int length = settings.fftLength;
        auto *input = reinterpret_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) * length));
        auto *output = reinterpret_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) * length));
        if (!input || !output) {
            if (input) fftwf_free(input);
            if (output) fftwf_free(output);
            reportError(QStringLiteral("FFT allocation failed"));
            running.store(false, std::memory_order_release);
            return;
        }
#ifdef FOBOSAPP_HAS_FFTW_THREADS
        const bool threadedPlanRequested = length >= kThreadedFftMinimumLength;
        const int automaticThreads = ComputeBudgetManager::instance().recommendedThreads(
            ComputeWorkClass::AuxiliaryFft, false, 1);
        const int planThreads = std::clamp(
            settings.fftWorkerThreads > 0 ? settings.fftWorkerThreads : automaticThreads, 1, 16);
#endif
        fftwf_plan plan = nullptr;
        {
            // FFTW's planner and its global thread-count selector are not safe
            // to mutate concurrently when two independent SDR sessions start.
            std::lock_guard<std::mutex> planLock(fftwPlannerGlobalMutex());
#ifdef FOBOSAPP_HAS_FFTW_THREADS
            const bool threadedPlan = threadedPlanRequested && initializeAnalyzerFftThreads();
            if (threadedPlan) fftwf_plan_with_nthreads(planThreads);
#endif
            plan = fftwf_plan_dft_1d(length, input, output, FFTW_FORWARD, FFTW_ESTIMATE);
#ifdef FOBOSAPP_HAS_FFTW_THREADS
            if (threadedPlanRequested && analyzerFftwThreadsAvailable)
                fftwf_plan_with_nthreads(1);
#endif
        }
        if (!plan) {
            fftwf_free(input); fftwf_free(output);
            reportError(QStringLiteral("FFT plan creation failed"));
            running.store(false, std::memory_order_release);
            return;
        }
        std::vector<float> snapshot;
        std::vector<float> window;
        try {
            snapshot.resize(static_cast<std::size_t>(length) * 2);
            window.resize(static_cast<std::size_t>(length));
        } catch (const std::bad_alloc &) {
            {
                std::lock_guard<std::mutex> planLock(fftwPlannerGlobalMutex());
                fftwf_destroy_plan(plan);
            }
            fftwf_free(input);
            fftwf_free(output);
            reportError(QStringLiteral("FFT working-memory allocation failed"));
            running.store(false, std::memory_order_release);
            return;
        }
        double windowSum = 0.0;
        for (int index = 0; index < length; ++index) {
            window[static_cast<std::size_t>(index)] = static_cast<float>(
                0.5 - 0.5 * std::cos(2.0 * kPi * index / std::max(1, length - 1)));
            windowSum += window[static_cast<std::size_t>(index)];
        }
        while (running.load(std::memory_order_acquire)) {
            double snapshotCenterHz = activeCenterHz.load(std::memory_order_acquire);
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait_for(lock, std::chrono::milliseconds(100), [this, &snapshot]() {
                    return !running.load(std::memory_order_acquire) || availableFloats >= snapshot.size();
                });
                if (!running.load(std::memory_order_acquire)) break;
                if (availableFloats < snapshot.size()) continue;
                const std::size_t start = (writePosition + ring.size() - snapshot.size()) % ring.size();
                const std::size_t first = std::min(snapshot.size(), ring.size() - start);
                std::copy_n(ring.begin() + static_cast<std::ptrdiff_t>(start), first, snapshot.begin());
                std::copy_n(ring.begin(), snapshot.size() - first,
                            snapshot.begin() + static_cast<std::ptrdiff_t>(first));
                snapshotCenterHz = activeCenterHz.load(std::memory_order_acquire);
                availableFloats = 0;
            }
            for (int index = 0; index < length; ++index) {
                const float coefficient = window[static_cast<std::size_t>(index)];
                if (settings.directSampling) {
                    const int channel = settings.directChannel == 2 ? 1 : 0;
                    input[index][0] = snapshot[static_cast<std::size_t>(index) * 2 + channel] * coefficient;
                    input[index][1] = 0.0f;
                } else {
                    input[index][0] = snapshot[static_cast<std::size_t>(index) * 2] * coefficient;
                    input[index][1] = snapshot[static_cast<std::size_t>(index) * 2 + 1] * coefficient;
                }
            }
            fftwf_execute(plan);
            AnalyzerSpectrumFrame frame;
            frame.sequence = ++sequence;
            frame.utcMs = QDateTime::currentMSecsSinceEpoch();
            const double center = snapshotCenterHz;
            frame.centerHz = settings.directSampling ? settings.sampleRateHz * 0.25 : center;
            frame.minimumHz = settings.directSampling ? 0.0 : center - settings.sampleRateHz * 0.5;
            frame.maximumHz = settings.directSampling ? settings.sampleRateHz * 0.5 : center + settings.sampleRateHz * 0.5;
            frame.sourceFftLength = length;
            const int outputLength = settings.directSampling ? length / 2 + 1 : length;
            const int ratio = std::max(0, settings.displayReductionRatio);
            const int displayLength = ratio > 0
                                          ? std::max(2, (outputLength + ratio - 1) / ratio)
                                          : std::min(outputLength, 65536);
            try {
                frame.levelsDb.resize(static_cast<std::size_t>(displayLength));
            } catch (const std::bad_alloc &) {
                reportError(QStringLiteral("FFT display-output allocation failed"));
                running.store(false, std::memory_order_release);
                break;
            }
            const double normalization = std::max(1.0, windowSum);
            const double normalizationPower = normalization * normalization;
            const int reductionMode = std::clamp(settings.displayReductionMode, 0, 4);
            const auto levelAt = [&](int shifted) {
                const int source = settings.directSampling
                                       ? shifted
                                       : (shifted + length / 2) % length;
                const double real = output[source][0];
                const double imag = output[source][1];
                const double magnitude = std::sqrt(real * real + imag * imag) / normalization;
                return static_cast<float>(20.0 * std::log10(std::max(1.0e-12, magnitude)));
            };
            for (int display = 0; display < displayLength; ++display) {
                const int first = static_cast<int>((static_cast<qint64>(display) * outputLength) /
                                                   displayLength);
                const int last = std::max(first + 1,
                    static_cast<int>((static_cast<qint64>(display + 1) * outputLength) /
                                     displayLength));
                const int boundedLast = std::min(last, outputLength);
                if (reductionMode == 3) {
                    const int sample = std::clamp(first + (boundedLast - first) / 2,
                                                  first, boundedLast - 1);
                    frame.levelsDb[static_cast<std::size_t>(display)] = levelAt(sample);
                    continue;
                }
                float value = reductionMode == 4 ? 240.0f : -240.0f;
                double sum = 0.0;
                int count = 0;
                for (int shifted = first; shifted < boundedLast; ++shifted) {
                    const int source = settings.directSampling
                                           ? shifted
                                           : (shifted + length / 2) % length;
                    const double real = output[source][0];
                    const double imag = output[source][1];
                    if (reductionMode == 2) {
                        sum += (real * real + imag * imag) / normalizationPower;
                    } else {
                        const float level = levelAt(shifted);
                        if (reductionMode == 0) value = std::max(value, level);
                        else if (reductionMode == 4) value = std::min(value, level);
                        else sum += level;
                    }
                    ++count;
                }
                if (reductionMode == 1)
                    value = count > 0 ? static_cast<float>(sum / count) : -240.0f;
                else if (reductionMode == 2)
                    value = count > 0 ? static_cast<float>(10.0 * std::log10(std::max(sum / count, 1.0e-30)))
                                      : -240.0f;
                frame.levelsDb[static_cast<std::size_t>(display)] = value;
            }
            const std::size_t frameBytes = frame.levelsDb.size() * sizeof(float);
            const int memoryBoundedLimit = frameBytes > 0
                                               ? static_cast<int>(std::clamp<std::size_t>(
                                                     (64U * 1024U * 1024U) / frameBytes, 2U, 16U))
                                               : 2;
            const int pendingLimit = nativeAgileScanActive.load(std::memory_order_acquire)
                                         ? memoryBoundedLimit
                                         : 1;
            int pending = pendingFrames.load(std::memory_order_acquire);
            while (pending < pendingLimit &&
                   !pendingFrames.compare_exchange_weak(pending, pending + 1,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
            }
            if (pending < pendingLimit) {
                QMetaObject::invokeMethod(owner, [this, frame = std::move(frame)]() {
                    emit owner->frameReady(frame);
                    pendingFrames.fetch_sub(1, std::memory_order_acq_rel);
                }, Qt::QueuedConnection);
            }
            // Native Agile scan advances independently in the device. Sleeping
            // here can let the next center overwrite the single IQ ring before
            // its FFT is consumed, producing wide missing bands. Rendering is
            // throttled separately by the analyzer UI.
            if (!nativeAgileScanActive.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        {
            std::lock_guard<std::mutex> planLock(fftwPlannerGlobalMutex());
            fftwf_destroy_plan(plan);
        }
        fftwf_free(input);
        fftwf_free(output);
    }

    bool start(const AnalyzerReceiverSettings &requested) {
        stop();
        settings = requested;
        ++streamEpoch;
        iqSampleCounter.store(0, std::memory_order_release);
        clockLocked = false;
        activeCenterHz.store(settings.centerHz, std::memory_order_release);
        settings.fftLength = std::clamp(settings.fftLength, 512, 67108864);
        if ((settings.fftLength & 1) != 0) ++settings.fftLength;
        if (!std::isfinite(settings.sampleRateHz) || settings.sampleRateHz < 1000.0 ||
            !std::isfinite(settings.centerHz) || settings.centerHz < 0.0) {
            reportError(QStringLiteral("Invalid receiver frequency or sample rate"));
            return false;
        }
        try {
            ring.assign(static_cast<std::size_t>(settings.fftLength) * 4, 0.0f);
        } catch (const std::bad_alloc &) {
            reportError(QStringLiteral("Receiver FFT buffer allocation failed"));
            return false;
        }
        writePosition = availableFloats = 0;
        error.clear();
        running.store(true, std::memory_order_release);
        fftThread = std::thread([this]() { runFft(); });
        streamThread = std::thread([this]() { runStream(); });
        emit owner->runningChanged(true);
        return true;
    }

    bool retune(double centerHz) {
        if (!running.load(std::memory_order_acquire) || settings.directSampling ||
            !std::isfinite(centerHz) || centerHz <= 0.0) return false;
        int result = -1;
        if (settings.backendId.startsWith(QStringLiteral("fobos-"))) {
            double actual = centerHz;
            result = settings.backendId == QStringLiteral("fobos-agile")
                         ? setFobosAgileFrequencySafely(fobosAgileDevice, centerHz)
                         : setFobosFrequencySafely(fobosDevice, centerHz, &actual);
            if (result == 0) centerHz = actual;
        } else if (settings.backendId == QStringLiteral("hackrf")) {
            result = setHackRfCenterFrequencySafely(externalDevice, static_cast<std::uint64_t>(centerHz));
        } else if (settings.backendId == QStringLiteral("rtl-sdr")) {
            result = setRtlSdrCenterFrequencySafely(externalDevice, static_cast<std::uint32_t>(centerHz));
        } else if (settings.backendId == QStringLiteral("soapy-sdr")) {
            result = setSoapySdrCenterFrequencySafely(externalDevice, centerHz, settings.rxChannel);
        }
        if (result != 0) {
            reportError(receiverErrorText(QStringLiteral("Live retune"), result));
            return false;
        }
        settings.centerHz = centerHz;
        activeCenterHz.store(centerHz, std::memory_order_release);
        return true;
    }

    bool setGains(int lnaGainDb, int vgaGainDb) {
        if (!running.load(std::memory_order_acquire)) return false;
        int result = 0;
        if (settings.backendId.startsWith(QStringLiteral("fobos-"))) {
            if (settings.backendId == QStringLiteral("fobos-agile")) {
                result = setFobosAgileLnaGainSafely(
                    fobosAgileDevice, static_cast<unsigned int>(std::max(0, lnaGainDb)));
                if (result == 0)
                    result = setFobosAgileVgaGainSafely(
                        fobosAgileDevice, static_cast<unsigned int>(std::max(0, vgaGainDb)));
            } else {
                result = setFobosLnaGainSafely(
                    fobosDevice, static_cast<unsigned int>(std::max(0, lnaGainDb)));
                if (result == 0)
                    result = setFobosVgaGainSafely(
                        fobosDevice, static_cast<unsigned int>(std::max(0, vgaGainDb)));
            }
        } else if (settings.backendId == QStringLiteral("hackrf")) {
            result = setHackRfLnaGainSafely(externalDevice,
                static_cast<std::uint32_t>(std::clamp(lnaGainDb, 0, 40)));
            if (result == 0) result = setHackRfVgaGainSafely(externalDevice,
                static_cast<std::uint32_t>(std::clamp(vgaGainDb, 0, 62)));
        } else if (settings.backendId == QStringLiteral("soapy-sdr")) {
            result = setSoapySdrGainSafely(externalDevice, lnaGainDb, settings.rxChannel);
        } else {
            return false;
        }
        if (result != 0) {
            reportError(receiverErrorText(QStringLiteral("Live gain"), result));
            return false;
        }
        settings.lnaGainDb = lnaGainDb;
        settings.vgaGainDb = vgaGainDb;
        return true;
    }

    bool stopNativeAgileScan() {
        if (!nativeAgileScanActive.load(std::memory_order_acquire)) return true;
        if (!fobosAgileDevice) return false;
        const int result = stopFobosAgileScanSafely(fobosAgileDevice);
        if (result != 0) {
            reportError(receiverErrorText(QStringLiteral("Stop Agile scan"), result));
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            nativeAgileScanActive.store(false, std::memory_order_release);
            settings.agileScanFrequenciesHz.clear();
            activeAgileScanIndex = -1;
            writePosition = 0;
            availableFloats = 0;
        }
        return true;
    }

    void stop() {
        const bool wasRunning = running.exchange(false, std::memory_order_acq_rel);
        condition.notify_all();
        if (fobosDevice) cancelFobosAsyncSafely(fobosDevice);
        if (fobosAgileDevice) {
            if (nativeAgileScanActive.exchange(false, std::memory_order_acq_rel))
                stopFobosAgileScanSafely(fobosAgileDevice);
            cancelFobosAgileAsyncSafely(fobosAgileDevice);
        }
        if (externalDevice) {
            if (settings.backendId == QStringLiteral("hackrf") && isHackRfStreamingSafely(externalDevice))
                stopHackRfRxSafely(externalDevice);
            if (settings.backendId == QStringLiteral("rtl-sdr")) cancelRtlSdrAsyncSafely(externalDevice);
            if (settings.backendId == QStringLiteral("soapy-sdr") && soapyStream)
                deactivateSoapySdrStreamSafely(externalDevice, soapyStream);
        }
        if (streamThread.joinable()) streamThread.join();
        if (fftThread.joinable()) fftThread.join();
        if (fobosDevice) { closeFobosDeviceSafely(fobosDevice); fobosDevice = nullptr; }
        if (fobosAgileDevice) {
            closeFobosAgileDeviceSafely(fobosAgileDevice);
            fobosAgileDevice = nullptr;
        }
        if (externalDevice) {
            if (settings.backendId == QStringLiteral("hackrf")) closeHackRfDeviceSafely(externalDevice);
            else if (settings.backendId == QStringLiteral("rtl-sdr")) closeRtlSdrDeviceSafely(externalDevice);
            else if (settings.backendId == QStringLiteral("soapy-sdr")) {
                if (soapyStream) closeSoapySdrStreamSafely(externalDevice, soapyStream);
                closeSoapySdrDeviceSafely(externalDevice);
            }
            externalDevice = nullptr;
            soapyStream = nullptr;
        }
        if (wasRunning) emit owner->runningChanged(false);
    }
};

AnalyzerReceiver::AnalyzerReceiver(QObject *parent)
    : QObject(parent), impl(std::make_unique<Impl>(this)) {
    qRegisterMetaType<AnalyzerSpectrumFrame>();
}

AnalyzerReceiver::~AnalyzerReceiver() = default;

QVector<AnalyzerReceiverDevice> AnalyzerReceiver::enumerateDevices() {
    QVector<AnalyzerReceiverDevice> result;
    const int agileCount = std::max(0, getFobosAgileDeviceCountSafely());
    for (int index = 0; index < agileCount; ++index)
        result.append({QStringLiteral("fobos-agile"),
                       QStringLiteral("Fobos SDR Agile #%1").arg(index), index});
    const int standardCount = std::max(0, getFobosStandardDeviceCountSafely());
    for (int index = 0; index < standardCount; ++index)
        result.append({QStringLiteral("fobos-standard"),
                       QStringLiteral("Fobos SDR Standard #%1").arg(index), index});
    const auto hackRfDevices = enumerateHackRfDevices();
    for (const auto &item : hackRfDevices)
        result.append({QStringLiteral("hackrf"), item.label, item.nativeIndex});
    const auto rtlDevices = enumerateRtlSdrDevices();
    for (const auto &item : rtlDevices)
        result.append({QStringLiteral("rtl-sdr"), item.label, item.nativeIndex});
    const auto soapyDevices = enumerateSoapySdrDevices();
    for (const auto &item : soapyDevices) {
        AnalyzerReceiverDevice deviceInfo;
        deviceInfo.backendId = QStringLiteral("soapy-sdr");
        deviceInfo.label = item.label;
        deviceInfo.nativeIndex = item.nativeIndex;
        deviceInfo.rxChannels = 1;
        deviceInfo.capabilitySummary = item.driver.isEmpty()
            ? QStringLiteral("SoapySDR device; capabilities are read on start")
            : QStringLiteral("SoapySDR driver: %1; capabilities are read on start").arg(item.driver);
        result.append(deviceInfo);
    }
    return result;
}

QVector<AnalyzerReceiverDevice> AnalyzerReceiver::enumerate() { return enumerateDevices(); }

bool AnalyzerReceiver::start(const AnalyzerReceiverSettings &settings) { return impl->start(settings); }
bool AnalyzerReceiver::retune(double centerHz) { return impl->retune(centerHz); }
bool AnalyzerReceiver::setGains(int lnaGainDb, int vgaGainDb) {
    return impl->setGains(lnaGainDb, vgaGainDb);
}
bool AnalyzerReceiver::stopNativeAgileScan() { return impl->stopNativeAgileScan(); }
bool AnalyzerReceiver::isNativeAgileScanActive() const {
    return impl->nativeAgileScanActive.load(std::memory_order_acquire);
}
void AnalyzerReceiver::stop() { impl->stop(); }
bool AnalyzerReceiver::isRunning() const { return impl->running.load(std::memory_order_acquire); }
QString AnalyzerReceiver::errorString() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->error;
}
