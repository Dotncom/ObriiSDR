#ifndef FFT_H
#define FFT_H

#include <fftw3.h>
#include <vector>
#include <cmath>
#include <QVector>
#include <thread>
#include <mutex>
#include <complex>
#include <QObject>
#include <QThread>
#include <QWaitCondition>
#include <algorithm>
#include <memory>
#include <QtConcurrent/QtConcurrent>
#include "iqbuffer.h"
#include "radiosettings.h"

class GpuFftBackend;

std::mutex &fftwPlannerGlobalMutex();

enum FftBackendPreference {
    FFT_BACKEND_AUTO = 0,
    FFT_BACKEND_CPU_FFTW = 1,
    FFT_BACKEND_GPU_VKFFT = 2
};

inline int normalizedFftBackendPreference(int preference) {
    return (std::clamp)(preference,
                        static_cast<int>(FFT_BACKEND_AUTO),
                        static_cast<int>(FFT_BACKEND_GPU_VKFFT));
}

inline const char *fftBackendPreferenceName(int preference) {
    switch (normalizedFftBackendPreference(preference)) {
    case FFT_BACKEND_CPU_FFTW:
        return "CPU FFTW";
    case FFT_BACKEND_GPU_VKFFT:
        return "GPU VkFFT";
    case FFT_BACKEND_AUTO:
    default:
        return "Auto";
    }
}

extern int DEFAULT_BUF_LEN;

extern float* iqData;
extern int globalMode;
extern std::vector<float> fftMagnitudes;
extern std::vector<float> fftFrequencies;
extern int fftLength;
extern double currentScale;
extern double minFrequency;
extern double maxFrequency;
extern double globalFrequency;
extern double globalSampleRate;

class FFTResult : public QObject {
    Q_OBJECT
public:
    explicit FFTResult(bool optimizeLargePlans = false, QObject *parent = nullptr);
    ~FFTResult();
    // Uses the newest complete IQ pairs, zero-padding short snapshots. Magnitudes
    // stay in natural FFT order; frequencies retain the centered display axis.
    // The caller owns the input (distinct from outputs) and serializes this instance.
    bool computeSnapshot(const RadioSettings &settings,
                         const std::vector<float> &interleavedIq,
                         std::vector<float> &frequencies,
                         std::vector<float> &magnitudes,
                         std::vector<float> *referenceMagnitudes = nullptr,
                         const IqBuffer::BlockMetadata *metadata = nullptr);
    bool storeFFTResults(const RadioSettings &settings,
                         std::vector<float> &outFrequencies,
                         std::vector<float> &outMagnitudes,
                         std::vector<float> *outReferenceMagnitudes = nullptr,
                         IqBuffer::BlockMetadata *outMetadata = nullptr,
                         std::uint64_t snapshotEndFloatCount = 0);
    void storeFFTResults();
    void resetHfNoiseCancelState();
    void setBackendPreference(int preference);
    std::mutex fftMutex;
	void performFFTInThread();
private:
    bool prepareSnapshotComputation(const RadioSettings &settings,
                                    std::vector<float> &outFrequencies,
                                    std::vector<float> &outMagnitudes,
                                    bool &profileFrame);
    bool computePreparedSnapshot(const RadioSettings &settings,
                                 const std::vector<float> &interleavedIq,
                                 std::vector<float> &outFrequencies,
                                 std::vector<float> &outMagnitudes,
                                 std::vector<float> *outReferenceMagnitudes,
                                 const IqBuffer::BlockMetadata *metadata,
                                 bool profileFrame,
                                 qint64 snapshotNs);
    bool ensurePlan(int length);
    void ensureWindow(int length, int windowType);
    float windowCoefficient(int index) const;
    double windowAmplitudeSumForSamples(int sampleCount) const;
    void executeTransform(int length);
    void releasePlan();
    fftwf_complex *fftIn;
    fftwf_complex *fftOut;
    fftwf_plan plan;
    int planLength;
    int windowLength = 0;
    int windowType = FFT_WINDOW_RECTANGULAR;
    double windowAmplitudeSum = 0.0;
    std::vector<float> windowCoefficients;
    bool optimizeLargePlans;
    int profileLength = 0;
    int profileFrames = 0;
    qint64 profileSnapshotNs = 0;
    qint64 profileInputNs = 0;
    qint64 profileExecuteNs = 0;
    qint64 profileOutputNs = 0;
    std::vector<std::complex<float>> hfNoiseCancelBins;
    std::vector<std::complex<float>> hfNoiseCancelCrossPower;
    std::vector<float> hfNoiseCancelMainPower;
    std::vector<float> hfNoiseCancelRefPower;
    std::vector<float> iqSnapshotScratch;
    std::unique_ptr<GpuFftBackend> gpuBackend;
    int backendPreference = FFT_BACKEND_AUTO;
    int loggedGpuSuccessLength = 0;
    int loggedGpuFailureLength = 0;
    int autoBackendDecisionLength = 0;
    int autoBackendDecision = -1;
};


#endif // FFT_H
