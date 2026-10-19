#include "fft.h"
#include "gpufftbackend.h"
#include "diagnosticlogging.h"
#include "iqbuffer.h"

#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFuture>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

extern int globalMode;

extern int fftLength;
extern std::vector<float> fftMagnitudes;
extern std::vector<float> fftFrequencies;
extern double minFrequency;
extern double maxFrequency;

namespace {
constexpr float FFT_MAGNITUDE_FLOOR_DB = -160.0f;
constexpr float HF_NOISE_CANCEL_MAX_COEFF = 2.5f;
constexpr int FFT_THREADED_PLAN_MIN_LENGTH = 524288;
constexpr int FFT_PARALLEL_POSTPROCESS_MIN_LENGTH = 1048576;
constexpr int FFT_PROFILE_MIN_LENGTH = 524288;
constexpr int FFT_AUTO_GPU_BENCHMARK_MAX_LENGTH = 8388608;
constexpr unsigned int FFT_MAX_WORKER_THREADS = 4;

bool fftwWisdomImported = false;
#ifdef FOBOSAPP_HAS_FFTW_THREADS
bool fftwThreadsInitializationAttempted = false;
bool fftwThreadsAvailable = false;
#endif

int fftWorkerCount(int length) {
    if (length < FFT_PARALLEL_POSTPROCESS_MIN_LENGTH) {
        return 1;
    }
    const unsigned int hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads < 2U) {
        return 1;
    }
    return static_cast<int>(std::clamp(hardwareThreads / 2U,
                                       1U,
                                       FFT_MAX_WORKER_THREADS));
}

template <typename Function>
void parallelForBins(int count, Function function) {
    const int workerCount = fftWorkerCount(count);
    if (workerCount <= 1 || count <= 0) {
        function(0, count);
        return;
    }

    std::vector<QFuture<void>> workers;
    workers.reserve(static_cast<std::size_t>(workerCount - 1));
    const int chunkSize = (count + workerCount - 1) / workerCount;
    for (int worker = 1; worker < workerCount; ++worker) {
        const int begin = worker * chunkSize;
        const int end = std::min(count, begin + chunkSize);
        if (begin < end) {
            workers.emplace_back(
                QtConcurrent::run([begin, end, &function]() { function(begin, end); }));
        }
    }
    function(0, std::min(count, chunkSize));
    for (QFuture<void> &worker : workers) {
        worker.waitForFinished();
    }
}

QByteArray fftwWisdomFilePath() {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (directory.isEmpty()) {
        return QByteArray();
    }
    QDir().mkpath(directory);
    return QFile::encodeName(QDir(directory).filePath(QStringLiteral("fftwf-wisdom.dat")));
}

void importFftwWisdom() {
    if (fftwWisdomImported) {
        return;
    }
    fftwWisdomImported = true;
    const QByteArray path = fftwWisdomFilePath();
    if (!path.isEmpty()) {
        fftwf_import_wisdom_from_filename(path.constData());
    }
}

float estimateHfNoiseCancelCoefficient(const std::vector<float> &iqSnapshot,
                                       int sourceStart,
                                       int sampleCount) {
    if (sampleCount <= 8) {
        return 0.0f;
    }

    double sumMain = 0.0;
    double sumRef = 0.0;
    double sumCross = 0.0;
    double sumRefSquared = 0.0;
    int count = 0;

    for (int i = 0; i < sampleCount; ++i) {
        const int sourceIndex = sourceStart + i;
        const float mainSample = iqSnapshot[2 * sourceIndex];
        const float refSample = iqSnapshot[2 * sourceIndex + 1];
        if (!std::isfinite(mainSample) || !std::isfinite(refSample)) {
            continue;
        }
        sumMain += mainSample;
        sumRef += refSample;
        sumCross += static_cast<double>(mainSample) * refSample;
        sumRefSquared += static_cast<double>(refSample) * refSample;
        ++count;
    }

    if (count <= 8) {
        return 0.0f;
    }

    const double meanMain = sumMain / count;
    const double meanRef = sumRef / count;
    const double covariance = sumCross - static_cast<double>(count) * meanMain * meanRef;
    const double refVariance = sumRefSquared - static_cast<double>(count) * meanRef * meanRef;
    if (!std::isfinite(covariance) || !std::isfinite(refVariance) || refVariance <= 1.0e-12) {
        return 0.0f;
    }

    return (std::clamp)(static_cast<float>(covariance / refVariance),
                        -HF_NOISE_CANCEL_MAX_COEFF,
                        HF_NOISE_CANCEL_MAX_COEFF);
}

std::complex<float> clampComplexMagnitude(std::complex<float> value, float maxMagnitude) {
    const float magnitude = std::abs(value);
    if (!std::isfinite(magnitude) || magnitude <= maxMagnitude || magnitude <= 0.0f) {
        return value;
    }
    return value * (maxMagnitude / magnitude);
}
}

FFTResult::FFTResult(bool optimizeLargePlans, QObject *parent)
    : QObject(parent),
      fftIn(nullptr),
      fftOut(nullptr),
      plan(nullptr),
      planLength(0),
      optimizeLargePlans(optimizeLargePlans) {
}

FFTResult::~FFTResult() {
    releasePlan();
}

void FFTResult::setBackendPreference(int preference) {
    const int normalized = normalizedFftBackendPreference(preference);
    if (backendPreference == normalized) {
        return;
    }
    backendPreference = normalized;
    loggedGpuSuccessLength = 0;
    loggedGpuFailureLength = 0;
    autoBackendDecisionLength = 0;
    autoBackendDecision = -1;
    if (backendPreference == FFT_BACKEND_CPU_FFTW && gpuBackend) {
        gpuBackend->releasePlan();
    }
}

void FFTResult::resetHfNoiseCancelState() {
    hfNoiseCancelBins.clear();
    hfNoiseCancelCrossPower.clear();
    hfNoiseCancelMainPower.clear();
    hfNoiseCancelRefPower.clear();
}

void FFTResult::releasePlan() {
    if (gpuBackend) {
        gpuBackend->releasePlan();
    }
    if (plan) {
        std::lock_guard<std::mutex> plannerLock(fftwPlannerGlobalMutex());
        fftwf_destroy_plan(plan);
        plan = nullptr;
    }
    if (fftIn) {
        fftwf_free(fftIn);
        fftIn = nullptr;
    }
    if (fftOut) {
        fftwf_free(fftOut);
        fftOut = nullptr;
    }
    planLength = 0;
    autoBackendDecisionLength = 0;
    autoBackendDecision = -1;
}

void FFTResult::executeTransform(int length) {
    constexpr int AutoGpuMinimumLength = 1048576;
    const bool automaticGpuCandidate =
        backendPreference == FFT_BACKEND_AUTO &&
        length >= AutoGpuMinimumLength &&
        length <= FFT_AUTO_GPU_BENCHMARK_MAX_LENGTH;
    const bool gpuRequested = backendPreference == FFT_BACKEND_GPU_VKFFT ||
                              automaticGpuCandidate;
    if (automaticGpuCandidate && autoBackendDecisionLength != length) {
        autoBackendDecisionLength = length;
        autoBackendDecision = -1;
    }
    if (automaticGpuCandidate && autoBackendDecision == 0) {
        fftwf_execute(plan);
        return;
    }
    if (gpuRequested && GpuFftBackend::isCompiled()) {
        if (!gpuBackend) {
            gpuBackend = std::make_unique<GpuFftBackend>();
        }
        QString error;
        const auto executeGpu = [&]() {
            return gpuBackend->execute(reinterpret_cast<const float *>(fftIn),
                                       reinterpret_cast<float *>(fftOut),
                                       length,
                                       &error);
        };
        if (automaticGpuCandidate && autoBackendDecision < 0) {
            QElapsedTimer gpuTimer;
            if (!executeGpu()) {
                autoBackendDecision = 0;
            } else {
                gpuTimer.start();
                const bool measuredGpu = executeGpu();
                const qint64 gpuNs = gpuTimer.nsecsElapsed();

                fftwf_execute(plan);
                QElapsedTimer cpuTimer;
                cpuTimer.start();
                fftwf_execute(plan);
                const qint64 cpuNs = cpuTimer.nsecsElapsed();

                autoBackendDecision = measuredGpu && gpuNs < cpuNs - cpuNs / 10 ? 1 : 0;
                if (fobosVerboseLoggingEnabled()) {
                    qInfo() << "[FFT] Auto backend benchmark"
                            << "length" << length
                            << "device" << gpuBackend->deviceName()
                            << "cpuMs" << (cpuNs / 1000000.0)
                            << "gpuMs" << (gpuNs / 1000000.0)
                            << "selected" << (autoBackendDecision == 1 ? "VkFFT" : "FFTW");
                }
                if (autoBackendDecision == 0) {
                    gpuBackend->releasePlan();
                }
                return;
            }
        } else if (executeGpu()) {
            if (loggedGpuSuccessLength != length) {
                loggedGpuSuccessLength = length;
                loggedGpuFailureLength = 0;
                if (fobosVerboseLoggingEnabled()) {
                    qInfo() << "[FFT] VkFFT active"
                            << "length" << length
                            << "device" << gpuBackend->deviceName();
                }
            }
            return;
        }
        if (automaticGpuCandidate) {
            autoBackendDecision = 0;
            gpuBackend->releasePlan();
        }
        if (loggedGpuFailureLength != length) {
            loggedGpuFailureLength = length;
            qWarning() << "[FFT] VkFFT unavailable, using FFTW"
                       << "length" << length
                       << "error" << error;
        }
    } else if (gpuRequested && loggedGpuFailureLength != length) {
        loggedGpuFailureLength = length;
        qWarning() << "[FFT] GPU backend requested but this build has no VkFFT; using FFTW";
    }
    fftwf_execute(plan);
}

bool FFTResult::ensurePlan(int length) {
    if (length <= 0) {
        releasePlan();
        return false;
    }

    if (plan && fftIn && fftOut && planLength == length) {
        return true;
    }

    releasePlan();
    fftIn = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * length);
    fftOut = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * length);
    if (!fftIn || !fftOut) {
        qDebug() << "Failed to allocate FFT buffers for length" << length;
        releasePlan();
        return false;
    }

    {
        std::lock_guard<std::mutex> plannerLock(fftwPlannerGlobalMutex());
        importFftwWisdom();
        const bool useThreadedPlan =
            optimizeLargePlans && length >= FFT_THREADED_PLAN_MIN_LENGTH;
        int planThreads = 1;
#ifdef FOBOSAPP_HAS_FFTW_THREADS
        if (!fftwThreadsInitializationAttempted) {
            fftwThreadsInitializationAttempted = true;
            fftwThreadsAvailable = fftwf_init_threads() != 0;
        }
        if (useThreadedPlan && fftwThreadsAvailable) {
            planThreads = fftWorkerCount(length);
        }
#endif
#ifdef FOBOSAPP_HAS_FFTW_THREADS
        if (fftwThreadsAvailable) {
            fftwf_plan_with_nthreads(planThreads);
        }
#endif
        plan = fftwf_plan_dft_1d(length, fftIn, fftOut, FFTW_FORWARD, FFTW_ESTIMATE);
        if (fobosVerboseLoggingEnabled()) {
            qInfo() << "[FFT] adaptive plan selected"
                    << "length" << length
                    << "threads" << planThreads
                    << "benchmark" << "skipped";
        }
#ifdef FOBOSAPP_HAS_FFTW_THREADS
        if (fftwThreadsAvailable) {
            fftwf_plan_with_nthreads(1);
        }
#endif
    }
    if (!plan) {
        qDebug() << "Failed to create FFTW plan for length" << length;
        releasePlan();
        return false;
    }

    planLength = length;
    return true;
}

void FFTResult::ensureWindow(int length, int requestedWindowType) {
    const int normalizedType = normalizedFftWindowType(requestedWindowType);
    if (windowLength == length && windowType == normalizedType) {
        return;
    }

    windowLength = length;
    windowType = normalizedType;
    windowAmplitudeSum = 0.0;
    windowCoefficients.clear();
    if (length <= 0 || normalizedType == FFT_WINDOW_RECTANGULAR) {
        windowAmplitudeSum = (std::max)(0, length);
        return;
    }

    windowCoefficients.resize(static_cast<std::size_t>(length), 1.0f);
    constexpr double TwoPi = 6.28318530717958647692;
    const double denominator = static_cast<double>((std::max)(1, length - 1));
    parallelForBins(length, [&](int begin, int end) {
        for (int index = begin; index < end; ++index) {
            const double phase = TwoPi * static_cast<double>(index) / denominator;
            double coefficient = 1.0;
            switch (normalizedType) {
            case FFT_WINDOW_HANN:
                coefficient = 0.5 - 0.5 * std::cos(phase);
                break;
            case FFT_WINDOW_HAMMING:
                coefficient = 0.54 - 0.46 * std::cos(phase);
                break;
            case FFT_WINDOW_BLACKMAN_HARRIS:
                coefficient = 0.35875 - 0.48829 * std::cos(phase) +
                              0.14128 * std::cos(2.0 * phase) -
                              0.01168 * std::cos(3.0 * phase);
                break;
            case FFT_WINDOW_FLAT_TOP:
                coefficient = 0.21557895 - 0.41663158 * std::cos(phase) +
                              0.277263158 * std::cos(2.0 * phase) -
                              0.083578947 * std::cos(3.0 * phase) +
                              0.006947368 * std::cos(4.0 * phase);
                break;
            case FFT_WINDOW_RECTANGULAR:
            default:
                coefficient = 1.0;
                break;
            }
            windowCoefficients[static_cast<std::size_t>(index)] =
                static_cast<float>(coefficient);
        }
    });
    for (float coefficient : windowCoefficients) {
        windowAmplitudeSum += coefficient;
    }
}

float FFTResult::windowCoefficient(int index) const {
    if (windowCoefficients.empty() || index < 0 || index >= windowLength) {
        return 1.0f;
    }
    return windowCoefficients[static_cast<std::size_t>(index)];
}

double FFTResult::windowAmplitudeSumForSamples(int sampleCount) const {
    const int clampedCount = (std::clamp)(sampleCount, 0, windowLength);
    if (windowCoefficients.empty()) {
        return static_cast<double>(clampedCount);
    }
    if (clampedCount == windowLength) {
        return windowAmplitudeSum;
    }
    double sum = 0.0;
    for (int index = 0; index < clampedCount; ++index) {
        sum += windowCoefficients[static_cast<std::size_t>(index)];
    }
    return sum;
}


void FFTResult::performFFTInThread() {
    QtConcurrent::run(this, &FFTResult::storeFFTResults);
}

bool FFTResult::prepareSnapshotComputation(const RadioSettings &settings,
                                           std::vector<float> &outFrequencies,
                                           std::vector<float> &outMagnitudes,
                                           bool &profileFrame) {
    const int currentFftLength = settings.fftLength;
    const double sampleRate = settings.sampleRate;
    if (currentFftLength <= 0 || sampleRate <= 0.0) {
        outMagnitudes.clear();
        outFrequencies.clear();
        return false;
    }

    const bool planWasReady = plan && planLength == currentFftLength;
    if (!ensurePlan(currentFftLength)) {
        outMagnitudes.clear();
        outFrequencies.clear();
        return false;
    }

    profileFrame = fobosVerboseLoggingEnabled() &&
                   optimizeLargePlans && planWasReady &&
                   currentFftLength >= FFT_PROFILE_MIN_LENGTH;
    return true;
}

bool FFTResult::computeSnapshot(const RadioSettings &settings,
                                const std::vector<float> &interleavedIq,
                                std::vector<float> &frequencies,
                                std::vector<float> &magnitudes,
                                std::vector<float> *referenceMagnitudes,
                                const IqBuffer::BlockMetadata *metadata) {
    bool profileFrame = false;
    if (!prepareSnapshotComputation(settings, frequencies, magnitudes, profileFrame)) {
        return false;
    }
    magnitudes.resize(static_cast<std::size_t>(settings.fftLength));
    if (referenceMagnitudes) {
        referenceMagnitudes->clear();
    }
    return computePreparedSnapshot(settings, interleavedIq, frequencies, magnitudes,
                                   referenceMagnitudes, metadata, profileFrame, 0);
}

bool FFTResult::storeFFTResults(const RadioSettings &settings,
                                std::vector<float> &outFrequencies,
                                std::vector<float> &outMagnitudes,
                                std::vector<float> *outReferenceMagnitudes,
                                IqBuffer::BlockMetadata *outMetadata,
                                std::uint64_t snapshotEndFloatCount) {
    bool profileFrame = false;
    if (!prepareSnapshotComputation(settings, outFrequencies, outMagnitudes, profileFrame)) {
        return false;
    }
    const int currentFftLength = settings.fftLength;
    QElapsedTimer profileTimer;
    if (profileFrame) {
        profileTimer.start();
    }
    outMagnitudes.resize(static_cast<std::size_t>(currentFftLength));
    if (outReferenceMagnitudes) {
        outReferenceMagnitudes->clear();
    }
    const std::size_t requestedSnapshotFloats =
        static_cast<std::size_t>((std::max)(1, currentFftLength)) * 2U;
    IqBuffer::BlockMetadata snapshotMetadata;
    const bool snapshotReady = snapshotEndFloatCount > 0
                                   ? IqBuffer::snapshotRecentEndingAt(iqSnapshotScratch,
                                                                      requestedSnapshotFloats,
                                                                      snapshotEndFloatCount,
                                                                      nullptr,
                                                                      &snapshotMetadata)
                                   : IqBuffer::snapshotRecent(iqSnapshotScratch,
                                                              requestedSnapshotFloats,
                                                              nullptr,
                                                              &snapshotMetadata);
    if (!snapshotReady) {
        return false;
    }
    const qint64 snapshotNs = profileFrame ? profileTimer.nsecsElapsed() : 0;
    if (outMetadata) {
        *outMetadata = snapshotMetadata;
    }
    return computePreparedSnapshot(settings, iqSnapshotScratch, outFrequencies, outMagnitudes,
                                   outReferenceMagnitudes, &snapshotMetadata, profileFrame,
                                   snapshotNs);
}

bool FFTResult::computePreparedSnapshot(const RadioSettings &settings,
                                        const std::vector<float> &interleavedIq,
                                        std::vector<float> &outFrequencies,
                                        std::vector<float> &outMagnitudes,
                                        std::vector<float> *outReferenceMagnitudes,
                                        const IqBuffer::BlockMetadata *metadata,
                                        bool profileFrame,
                                        qint64 snapshotNs) {
    const int currentFftLength = settings.fftLength;
    const double sampleRate = settings.sampleRate;
    double centerFrequency = settings.centerFrequency;
    const int inputMode = settings.inputMode;
    // Simplified mode trims optional DSP branches and uses the lower-cost
    // audio channelizer. The main spectrum remains a core receiver function,
    // so large-FFT post-processing must keep its normal bounded parallelism.
    auto processBins = [&](int count, auto function) {
        parallelForBins(count, function);
    };
    QElapsedTimer profileTimer;
    if (profileFrame) {
        profileTimer.start();
    }
    if (metadata && metadata->valid &&
        std::isfinite(metadata->centerFrequencyHz) &&
        metadata->centerFrequencyHz > 0.0) {
        centerFrequency = metadata->centerFrequencyHz;
    }
    outFrequencies.resize(currentFftLength);

    const std::size_t availableIqSamples = interleavedIq.size() / 2;
    if (availableIqSamples == 0) {
        return false;
    }

    const int samplesToCopy = static_cast<int>((std::min)(
        static_cast<std::size_t>(currentFftLength), availableIqSamples));
    const std::size_t sourceStart = availableIqSamples - samplesToCopy;
    ensureWindow(currentFftLength, settings.fftWindowType);

    const float normalizationDb =
        20.0f * std::log10(static_cast<float>((std::max)(1.0,
                                                          windowAmplitudeSumForSamples(samplesToCopy))));
    auto magnitudeDb = [this, normalizationDb](int index) {
        const float re = std::isfinite(fftOut[index][0]) ? fftOut[index][0] : 0.0f;
        const float im = std::isfinite(fftOut[index][1]) ? fftOut[index][1] : 0.0f;
        const float power = re * re + im * im;
        if (std::isfinite(power) && power > 0.0f) {
            return (std::max)(FFT_MAGNITUDE_FLOOR_DB,
                              10.0f * std::log10(power) - normalizationDb);
        }
        return FFT_MAGNITUDE_FLOOR_DB;
    };

    if (inputMode == INPUT_HF_COMBINED) {
        const int halfLength = currentFftLength / 2;
        std::vector<float> hf1Positive(halfLength + 1, FFT_MAGNITUDE_FLOOR_DB);
        std::vector<float> hf2Positive(halfLength + 1, FFT_MAGNITUDE_FLOOR_DB);
        std::vector<float> shiftedMagnitude(currentFftLength, FFT_MAGNITUDE_FLOOR_DB);

        for (int i = 0; i < currentFftLength; ++i) {
            fftIn[i][0] = 0.0f;
            fftIn[i][1] = 0.0f;
        }
        for (int i = 0; i < samplesToCopy; ++i) {
            const std::size_t sourceIndex = sourceStart + i;
            const float iValue = interleavedIq[2 * sourceIndex];
            fftIn[i][0] = std::isfinite(iValue) ? iValue * windowCoefficient(i) : 0.0f;
        }
        executeTransform(currentFftLength);
        for (int k = 0; k <= halfLength; ++k) {
            hf1Positive[k] = magnitudeDb(k);
        }

        for (int i = 0; i < currentFftLength; ++i) {
            fftIn[i][0] = 0.0f;
            fftIn[i][1] = 0.0f;
        }
        for (int i = 0; i < samplesToCopy; ++i) {
            const std::size_t sourceIndex = sourceStart + i;
            const float qValue = interleavedIq[2 * sourceIndex + 1];
            fftIn[i][0] = std::isfinite(qValue) ? qValue * windowCoefficient(i) : 0.0f;
        }
        executeTransform(currentFftLength);
        for (int k = 0; k <= halfLength; ++k) {
            hf2Positive[k] = magnitudeDb(k);
        }

        processBins(currentFftLength, [&](int begin, int end) {
            for (int i = begin; i < end; ++i) {
                if (i < halfLength) {
                    shiftedMagnitude[i] = hf1Positive[halfLength - i];
                } else {
                    shiftedMagnitude[i] = hf2Positive[i - halfLength];
                }
                outMagnitudes[(i + halfLength) % currentFftLength] = shiftedMagnitude[i];
                outFrequencies[i] =
                    (i - currentFftLength / 2) * (sampleRate / currentFftLength) + centerFrequency;
            }
        });
        return true;
    }

    if (inputMode == INPUT_HF_NOISE_CANCEL) {
        std::vector<std::complex<float>> mainSpectrum(currentFftLength);
        std::vector<std::complex<float>> refSpectrum(currentFftLength);

        for (int i = 0; i < currentFftLength; ++i) {
            fftIn[i][0] = 0.0f;
            fftIn[i][1] = 0.0f;
        }
        for (int i = 0; i < samplesToCopy; ++i) {
            const std::size_t sourceIndex = sourceStart + i;
            const float iValue = interleavedIq[2 * sourceIndex];
            fftIn[i][0] = std::isfinite(iValue) ? iValue * windowCoefficient(i) : 0.0f;
        }
        executeTransform(currentFftLength);
        for (int i = 0; i < currentFftLength; ++i) {
            mainSpectrum[i] = std::complex<float>(std::isfinite(fftOut[i][0]) ? fftOut[i][0] : 0.0f,
                                                  std::isfinite(fftOut[i][1]) ? fftOut[i][1] : 0.0f);
        }

        for (int i = 0; i < currentFftLength; ++i) {
            fftIn[i][0] = 0.0f;
            fftIn[i][1] = 0.0f;
        }
        for (int i = 0; i < samplesToCopy; ++i) {
            const std::size_t sourceIndex = sourceStart + i;
            const float qValue = interleavedIq[2 * sourceIndex + 1];
            fftIn[i][0] = std::isfinite(qValue) ? qValue * windowCoefficient(i) : 0.0f;
        }
        executeTransform(currentFftLength);
        for (int i = 0; i < currentFftLength; ++i) {
            refSpectrum[i] = std::complex<float>(std::isfinite(fftOut[i][0]) ? fftOut[i][0] : 0.0f,
                                                 std::isfinite(fftOut[i][1]) ? fftOut[i][1] : 0.0f);
        }

        if (hfNoiseCancelBins.size() != static_cast<std::size_t>(currentFftLength)) {
            hfNoiseCancelBins.assign(currentFftLength, std::complex<float>(0.0f, 0.0f));
            hfNoiseCancelCrossPower.assign(currentFftLength, std::complex<float>(0.0f, 0.0f));
            hfNoiseCancelMainPower.assign(currentFftLength, 0.0f);
            hfNoiseCancelRefPower.assign(currentFftLength, 0.0f);
        }

        constexpr float adaptiveAlphaCold = 0.18f;
        constexpr float adaptiveAlphaWarm = 0.035f;
        constexpr float adaptiveEpsilon = 1.0e-7f;
        constexpr float coherenceStart = 0.08f;
        constexpr float coherenceFull = 0.38f;
        auto binFrequency = [currentFftLength, sampleRate, centerFrequency](int index) {
            const int shiftedIndex = index <= currentFftLength / 2
                                         ? index
                                         : index - currentFftLength;
            return shiftedIndex * (sampleRate / currentFftLength) + centerFrequency;
        };
        const float noiseCancelDepth =
            static_cast<float>((std::clamp)(settings.hfNoiseCancelDepth, 0.0, 2.0));
        if (!settings.hfNoiseCancelFreeze) {
            for (int i = 0; i < currentFftLength; ++i) {
                const std::complex<float> adjustedRef =
                    hfNoiseCancelReferenceCoefficient(settings, binFrequency(i)) * refSpectrum[i];
                const float refPowerInstant = std::norm(adjustedRef);
                const float mainPowerInstant = std::norm(mainSpectrum[i]);
                if (!std::isfinite(refPowerInstant) ||
                    !std::isfinite(mainPowerInstant) ||
                    refPowerInstant <= adaptiveEpsilon) {
                    continue;
                }
                const std::complex<float> crossInstant = mainSpectrum[i] * std::conj(adjustedRef);
                const float alpha = hfNoiseCancelRefPower[i] <= adaptiveEpsilon
                                        ? adaptiveAlphaCold
                                        : adaptiveAlphaWarm;
                hfNoiseCancelCrossPower[i] += alpha * (crossInstant - hfNoiseCancelCrossPower[i]);
                hfNoiseCancelMainPower[i] += alpha * (mainPowerInstant - hfNoiseCancelMainPower[i]);
                hfNoiseCancelRefPower[i] += alpha * (refPowerInstant - hfNoiseCancelRefPower[i]);
                if (hfNoiseCancelRefPower[i] > adaptiveEpsilon) {
                    hfNoiseCancelBins[i] =
                        clampComplexMagnitude(hfNoiseCancelCrossPower[i] /
                                                  (hfNoiseCancelRefPower[i] + adaptiveEpsilon),
                                              HF_NOISE_CANCEL_MAX_COEFF);
                }
            }
        }

        auto complexMagnitudeDb = [normalizationDb](std::complex<float> value) {
            const float power = std::norm(value);
            if (std::isfinite(power) && power > 0.0f) {
                return (std::max)(FFT_MAGNITUDE_FLOOR_DB,
                                  10.0f * std::log10(power) - normalizationDb);
            }
            return FFT_MAGNITUDE_FLOOR_DB;
        };
        if (outReferenceMagnitudes) {
            outReferenceMagnitudes->assign(currentFftLength, FFT_MAGNITUDE_FLOOR_DB);
        }

        processBins(currentFftLength, [&](int begin, int end) {
            for (int i = begin; i < end; ++i) {
                const std::complex<float> adjustedRef =
                    hfNoiseCancelReferenceCoefficient(settings, binFrequency(i)) * refSpectrum[i];
                const float coherence =
                    (std::norm(hfNoiseCancelCrossPower[i]) /
                     ((hfNoiseCancelMainPower[i] * hfNoiseCancelRefPower[i]) + adaptiveEpsilon));
                const float coherenceWeight =
                    (std::clamp)((coherence - coherenceStart) / (coherenceFull - coherenceStart),
                                 0.0f,
                                 1.0f);
                const std::complex<float> effectiveRef =
                    coherenceWeight * hfNoiseCancelBins[i] * adjustedRef;
                const std::complex<float> cleaned =
                    mainSpectrum[i] - noiseCancelDepth * effectiveRef;
                outMagnitudes[i] = complexMagnitudeDb(cleaned);
                if (outReferenceMagnitudes) {
                    (*outReferenceMagnitudes)[i] = complexMagnitudeDb(adjustedRef);
                }
                outFrequencies[i] =
                    (i - currentFftLength / 2) * (sampleRate / currentFftLength) + centerFrequency;
            }
        });
        return true;
    }

    if (!hfNoiseCancelBins.empty()) {
        hfNoiseCancelBins.clear();
        hfNoiseCancelCrossPower.clear();
        hfNoiseCancelMainPower.clear();
        hfNoiseCancelRefPower.clear();
    }
    if (profileFrame) {
        profileTimer.restart();
    }
    processBins(samplesToCopy, [&](int begin, int end) {
        for (int i = begin; i < end; ++i) {
            const std::size_t sourceIndex = sourceStart + i;
            const float coefficient = windowCoefficient(i);
            if (inputMode == INPUT_HF1) {
                const float iValue = interleavedIq[2 * sourceIndex];
                fftIn[i][0] = std::isfinite(iValue) ? iValue * coefficient : 0.0f;
                fftIn[i][1] = 0.0f;
            } else if (inputMode == INPUT_HF2) {
                const float qValue = interleavedIq[2 * sourceIndex + 1];
                fftIn[i][0] = 0.0f;
                fftIn[i][1] = std::isfinite(qValue) ? qValue * coefficient : 0.0f;
            } else {
                const float iValue = interleavedIq[2 * sourceIndex];
                const float qValue = interleavedIq[2 * sourceIndex + 1];
                fftIn[i][0] = std::isfinite(iValue) ? iValue * coefficient : 0.0f;
                fftIn[i][1] = std::isfinite(qValue) ? qValue * coefficient : 0.0f;
            }
        }
    });
    for (int i = samplesToCopy; i < currentFftLength; ++i) {
        fftIn[i][0] = 0.0f;
        fftIn[i][1] = 0.0f;
    }
    const qint64 inputNs = profileFrame ? profileTimer.nsecsElapsed() : 0;

    if (profileFrame) {
        profileTimer.restart();
    }
    executeTransform(currentFftLength);
    const qint64 executeNs = profileFrame ? profileTimer.nsecsElapsed() : 0;
    if (profileFrame) {
        profileTimer.restart();
    }
    processBins(currentFftLength, [&](int begin, int end) {
        for (int i = begin; i < end; ++i) {
            outMagnitudes[i] = magnitudeDb(i);
            outFrequencies[i] =
                (i - currentFftLength / 2) * (sampleRate / currentFftLength) + centerFrequency;
        }
    });
    if (profileFrame) {
        const qint64 outputNs = profileTimer.nsecsElapsed();
        if (profileLength != currentFftLength) {
            profileLength = currentFftLength;
            profileFrames = 0;
            profileSnapshotNs = 0;
            profileInputNs = 0;
            profileExecuteNs = 0;
            profileOutputNs = 0;
        }
        ++profileFrames;
        profileSnapshotNs += snapshotNs;
        profileInputNs += inputNs;
        profileExecuteNs += executeNs;
        profileOutputNs += outputNs;
        constexpr int ProfileFrames = 30;
        if (profileFrames >= ProfileFrames) {
            const double scale = 1.0 / (1000000.0 * profileFrames);
            qDebug() << "[FFT profile]"
                     << "length" << profileLength
                     << "frames" << profileFrames
                     << "snapshotMs" << profileSnapshotNs * scale
                     << "inputMs" << profileInputNs * scale
                     << "executeMs" << profileExecuteNs * scale
                     << "outputMs" << profileOutputNs * scale
                     << "totalMs" << (profileSnapshotNs + profileInputNs +
                                       profileExecuteNs + profileOutputNs) * scale;
            profileFrames = 0;
            profileSnapshotNs = 0;
            profileInputNs = 0;
            profileExecuteNs = 0;
            profileOutputNs = 0;
        }
    }
    return true;
}

void FFTResult::storeFFTResults() {
    RadioSettings settings;
    settings.inputMode = globalMode;
    settings.centerFrequency = globalFrequency;
    settings.sampleRate = globalSampleRate;
    settings.fftLength = fftLength;
    storeFFTResults(settings, fftFrequencies, fftMagnitudes);
}
