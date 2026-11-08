#ifndef ZOOMSPECTRUMPROCESSOR_H
#define ZOOMSPECTRUMPROCESSOR_H

#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct ZoomSpectrumFrame {
    std::vector<float> frequencies;
    std::vector<float> levels;
    double selectedLowHz = 0.0;
    double selectedHighHz = 0.0;
    double channelSampleRateHz = 0.0;
    double binWidthHz = 0.0;
    int fftLength = 0;
    int decimation = 1;
    std::uint64_t sequence = 0;
};

class ZoomSpectrumProcessor {
public:
    ZoomSpectrumProcessor();
    ~ZoomSpectrumProcessor();

    ZoomSpectrumProcessor(const ZoomSpectrumProcessor &) = delete;
    ZoomSpectrumProcessor &operator=(const ZoomSpectrumProcessor &) = delete;

    void configure(double lowHz,
                   double highHz,
                   double requestedBinWidthHz,
                   int updateIntervalMs,
                   bool enabled);
    void setEnabled(bool enabled);
    bool enabled() const noexcept { return enabledFlag.load(std::memory_order_acquire); }
    void consumeIq(const float *interleavedIq,
                   std::uint32_t complexSamples,
                   double inputSampleRateHz,
                   double inputCenterHz);
    bool takeFrames(std::vector<ZoomSpectrumFrame> &frames);

private:
    struct Configuration {
        double lowHz = 0.0;
        double highHz = 0.0;
        double requestedBinWidthHz = 1.0;
        int updateIntervalMs = 50;
        bool enabled = false;
        std::uint64_t generation = 0;
    };

    void run();
    Configuration configuration() const;
    void resetDspState(std::uint64_t generation,
                       double inputSampleRateHz,
                       double inputCenterHz,
                       int decimation,
                       double channelSampleRateHz,
                       int fftLength);

    mutable std::mutex configMutex;
    Configuration config;
    std::atomic<bool> enabledFlag{false};

    std::mutex sampleMutex;
    std::condition_variable sampleCondition;
    std::deque<std::complex<float>> channelSamples;
    std::uint64_t totalChannelSamples = 0;
    std::uint64_t lastFrameEndSample = 0;
    std::size_t sampleCapacity = 0;
    bool stopping = false;

    std::mutex frameMutex;
    std::deque<ZoomSpectrumFrame> readyFrames;
    std::uint64_t nextFrameSequence = 0;

    std::thread workerThread;

    std::uint64_t activeGeneration = 0;
    double activeInputSampleRateHz = 0.0;
    double activeInputCenterHz = 0.0;
    int activeDecimation = 1;
    double activeChannelSampleRateHz = 0.0;
    int activeFftLength = 0;
    std::complex<float> oscillator{1.0f, 0.0f};
    std::complex<float> lowPassState1{0.0f, 0.0f};
    std::complex<float> lowPassState2{0.0f, 0.0f};
    std::complex<double> decimationSum{0.0, 0.0};
    int decimationCount = 0;
    std::uint64_t oscillatorSamples = 0;
};

#endif
