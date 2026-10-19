#ifndef M_PI
#define M_PI_F 3.1415927f
#endif

#include "audioprocessor.h"
#include "iqbuffer.h"
#include "diagnosticlogging.h"
#include "fmphasediscriminator.h"
#include <limits>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

namespace {
constexpr double AUDIO_OUTPUT_RATE = 48000.0;
constexpr double TWO_PI = 6.28318530717958647692;
constexpr double CW_TONE_HZ = 700.0;
constexpr double AM_CHANNEL_RATE = 192000.0;
constexpr double FM_MIN_CHANNEL_RATE = 240000.0;
constexpr double FM_MAX_CHANNEL_RATE = 768000.0;
constexpr double FM_DEEMPHASIS_SECONDS = 50e-6;
constexpr double NFM_DEEMPHASIS_SECONDS = 300e-6;
constexpr size_t MAX_AUDIO_BUFFER_SAMPLES = BUFFER_SIZE * 10;
constexpr size_t AUDIO_START_BUFFER_SAMPLES = BUFFER_SIZE * 2;
constexpr double SSB_LOW_CUT_HZ = 250.0;
constexpr double SSB_MAX_AUDIO_HZ = 3600.0;
constexpr double SAM_LOCK_RANGE_HZ = 1500.0;
constexpr float HF_NOISE_CANCEL_MAX_COEFF = 2.5f;
constexpr double DMR_CHANNEL_RATE = 192000.0;
constexpr double DMR_HIGH_RF_CHANNEL_RATE = 768000.0;
constexpr double DMR_HIGH_RF_THRESHOLD = 10000000.0;
constexpr double DMR_FOURFSK_CHANNEL_CUTOFF_HZ = 9500.0;
constexpr double DMR_FOURFSK_SYMBOL_FILTER_HZ = 6200.0;
constexpr double DMR_FOURFSK_DC_TRACK_HZ = 12.0;
constexpr double DMR_FOURFSK_OUTER_DEVIATION_HZ = 1944.0;
constexpr double DMR_FOURFSK_OUTPUT_OUTER_LEVEL = 0.15;
constexpr float DMR_FOURFSK_LIMIT_INPUT = 4.0f;
constexpr float DMR_FOURFSK_SOFT_LIMIT_DRIVE = 0.85f;
constexpr int DMR_CHANNEL_CIC_STAGES = 5;

double clampDouble(double value, double low, double high) {
    return (std::max)(low, (std::min)(value, high));
}

double wrapRadians(double phase) {
    return std::remainder(phase, TWO_PI);
}

float estimateHfNoiseCancelCoefficient(const std::vector<float> &iqBlock) {
    const size_t iqSamples = iqBlock.size() / 2;
    if (iqSamples <= 8) {
        return 0.0f;
    }

    double sumMain = 0.0;
    double sumRef = 0.0;
    double sumCross = 0.0;
    double sumRefSquared = 0.0;
    size_t count = 0;

    for (size_t n = 0; n < iqSamples; ++n) {
        const float mainSample = iqBlock[2 * n];
        const float refSample = iqBlock[2 * n + 1];
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

    const double meanMain = sumMain / static_cast<double>(count);
    const double meanRef = sumRef / static_cast<double>(count);
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

double channelCutoffForMode(int modulationType, double bandwidth) {
    switch (modulationType) {
    case MOD_ATV:
        return (std::min)(250000.0, (std::max)(80000.0, bandwidth * 0.15));
    case MOD_WFM:
        return (std::min)(140000.0, (std::max)(80000.0, bandwidth * 0.55));
    case MOD_NFM:
        return (std::min)(25000.0, (std::max)(6000.0, bandwidth * 0.55));
    case MOD_DMR:
        return (std::min)(9500.0, (std::max)(6000.0, bandwidth * 0.75));
    case MOD_APT:
        return (std::min)(36000.0, (std::max)(18000.0, bandwidth * 0.55));
    case MOD_FSK:
        return (std::min)(12000.0, (std::max)(2500.0, bandwidth * 0.55));
    case MOD_USB:
    case MOD_LSB:
    case MOD_FT8:
    case MOD_RTTY:
    case MOD_PSK:
    case MOD_SSTV:
    case MOD_WEFAX:
        return (std::min)(SSB_MAX_AUDIO_HZ, (std::max)(700.0, bandwidth * 0.95));
    case MOD_CW:
        return (std::min)(1500.0, (std::max)(250.0, bandwidth * 0.5));
    case MOD_DSB:
    case MOD_SAM:
        return (std::min)(12000.0, (std::max)(1000.0, bandwidth * 0.45));
    case MOD_AM:
    default:
        return (std::min)(10000.0, (std::max)(1000.0, bandwidth * 0.45));
    }
}

double demodAudioCutoffForMode(int modulationType, double bandwidth) {
    switch (modulationType) {
    case MOD_ATV:
        return 15000.0;
    case MOD_WFM:
        return 15000.0;
    case MOD_NFM:
        return 3000.0;
    case MOD_DMR:
        return 6000.0;
    case MOD_APT:
        return 12000.0;
    case MOD_FSK:
        return 5000.0;
    case MOD_USB:
    case MOD_LSB:
    case MOD_FT8:
    case MOD_RTTY:
    case MOD_PSK:
    case MOD_SSTV:
    case MOD_WEFAX:
        return (std::min)(SSB_MAX_AUDIO_HZ, (std::max)(700.0, bandwidth * 0.95));
    case MOD_CW:
        return 1200.0;
    case MOD_DSB:
    case MOD_SAM:
        return (std::min)(6500.0, (std::max)(1000.0, bandwidth * 0.45));
    case MOD_AM:
    default:
        return (std::min)(6000.0, (std::max)(1000.0, bandwidth * 0.45));
    }
}

double targetChannelRate(int modulationType, double bandwidth) {
    if (modulationType == MOD_ATV) {
        return clampDouble((std::max)(384000.0, bandwidth * 0.2), 384000.0, FM_MAX_CHANNEL_RATE);
    }
    if (modulationType == MOD_WFM) {
        return clampDouble((std::max)(384000.0, bandwidth * 3.0), 384000.0, FM_MAX_CHANNEL_RATE);
    }
    if (modulationType == MOD_APT) {
        return 240000.0;
    }
    if (modulationType == MOD_DMR) {
        return 192000.0;
    }
    if (modulationType == MOD_NFM || modulationType == MOD_FSK) {
        return clampDouble((std::max)(FM_MIN_CHANNEL_RATE, bandwidth * 4.0),
                           FM_MIN_CHANNEL_RATE,
                           384000.0);
    }
    return AM_CHANNEL_RATE;
}

bool isDigitalAudioMode(int modulationType) {
    return modulationType == MOD_FT8 ||
           modulationType == MOD_RTTY ||
           modulationType == MOD_FSK ||
           modulationType == MOD_PSK ||
           modulationType == MOD_SSTV ||
           modulationType == MOD_WEFAX ||
           modulationType == MOD_DMR;
}

int channelDecimationFactor(double inputRate, int modulationType, double bandwidth) {
    const double targetRate = targetChannelRate(modulationType, bandwidth);
    return (std::max)(1, static_cast<int>(std::floor(inputRate / targetRate)));
}

}

AudioProcessor::AudioProcessor(QObject *parent)
    : QObject(parent),
      running(false),
      workerThread(nullptr)
#ifdef _WIN32
      , hWaveOut(nullptr)
#endif
{
#ifdef _WIN32
    ZeroMemory(&format, sizeof(WAVEFORMATEX));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = (format.nChannels * format.wBitsPerSample) / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;
#endif

    for (int i = 0; i < NUM_BUFFERS; i++) {
        waveBuffers[i].resize(BUFFER_SIZE);
#ifdef _WIN32
        ZeroMemory(&waveHdrs[i], sizeof(WAVEHDR));
        waveHdrs[i].lpData = reinterpret_cast<LPSTR>(waveBuffers[i].data());
        waveHdrs[i].dwBufferLength = BUFFER_SIZE * sizeof(short);
#endif
        bufferReady[i] = true;
    }
}


AudioProcessor::~AudioProcessor() {
    stopDemodulation();
    closeAudioDevice();
}

bool AudioProcessor::processIndependentIq(const std::vector<float> &iq,
                                         const RadioSettings &settings,
                                         bool discontinuity, std::vector<short> &pcm) {
    pcm.clear();
    const bool supportedModulation =
        (settings.modulationType >= MOD_AM && settings.modulationType <= MOD_WFM) ||
        settings.modulationType == MOD_SSTV;
    if (running.load() || settings.inputMode != INPUT_RF ||
        !std::isfinite(settings.sampleRate) || settings.sampleRate < 48000.0 ||
        settings.sampleRate > 2000000.0 || iq.size() % 2 || iq.size() > 262144U ||
        !supportedModulation) return false;
    configure(settings);
    const bool requestedReset = demodulatorResetRequested.exchange(false);
    if (discontinuity || requestedReset) resetDemodulatorState();
    audioFilterChain.configure(settings.audioFilterChainJson, AUDIO_OUTPUT_RATE);
    if (discontinuity || requestedReset) audioFilterChain.reset();
    std::vector<short> unused;
    int unusedRate = 0;
    processDemodulatorBlock(iq, pcm, unused, unusedRate, settings);
    audioFilterChain.process(pcm);
    return true;
}

void AudioProcessor::configure(const RadioSettings &settings) {
    std::lock_guard<std::mutex> lock(settingsMutex);
    const bool resetDemodulator =
        audioSettings.inputMode != settings.inputMode ||
        audioSettings.modulationType != settings.modulationType ||
        audioSettings.sstvDemodulationMode != settings.sstvDemodulationMode ||
        std::abs(audioSettings.centerFrequency - settings.centerFrequency) > 0.5 ||
        std::abs(audioSettings.listeningFrequency - settings.listeningFrequency) > 0.5 ||
        std::abs(audioSettings.sampleRate - settings.sampleRate) > 0.5 ||
        std::abs(audioSettings.bandwidth - settings.bandwidth) > 1.0 ||
        normalizedDmrBasebandSampleRate(audioSettings.dmrBasebandSampleRate) !=
            normalizedDmrBasebandSampleRate(settings.dmrBasebandSampleRate);
    const bool resetNoiseCancel =
        audioSettings.inputMode != settings.inputMode ||
        std::abs(audioSettings.listeningFrequency - settings.listeningFrequency) > 0.5 ||
        std::abs(audioSettings.sampleRate - settings.sampleRate) > 0.5 ||
        std::abs(audioSettings.hfNoiseCancelRefGainDb - settings.hfNoiseCancelRefGainDb) > 0.001 ||
        std::abs(audioSettings.hfNoiseCancelRefDelayNs - settings.hfNoiseCancelRefDelayNs) > 0.001 ||
        std::abs(audioSettings.hfNoiseCancelRefTiltDb - settings.hfNoiseCancelRefTiltDb) > 0.001;
    audioSettings = settings;
    selectedAudioDeviceID = settings.audioDeviceId;
    if (resetDemodulator) {
        demodulatorResetRequested = true;
    }
    if (resetNoiseCancel) {
        hfNoiseCancelResetRequested = true;
    }
}

void AudioProcessor::resetHfNoiseCancelState() {
    hfNoiseCancelResetRequested = true;
}

void AudioProcessor::setAudioDevice(int deviceID) {
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[Audio] setAudioDevice" << deviceID;
    }
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        selectedAudioDeviceID = deviceID;
        audioSettings.audioDeviceId = deviceID;
    }

    const bool wasRunning = running.load();
    if (wasRunning) {
        stopDemodulation();
        startDemodulation();
        return;
    }

#ifdef _WIN32
    if (hWaveOut && openedAudioDeviceID != deviceID) {
        closeAudioDevice();
    }
#endif
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[Audio] device selected; it will be opened when audio starts.";
    }
}

void AudioProcessor::setLocalPlaybackEnabled(bool enabled) {
    const bool wasEnabled = localPlaybackEnabled.exchange(enabled);
    if (wasEnabled == enabled) {
        return;
    }
    qDebug() << "[Audio] local playback" << (enabled ? "enabled" : "disabled");
    if (!enabled) {
        closeAudioDevice();
    }
}

void AudioProcessor::setVolume(float volume) {
    outputVolume.store(std::clamp(volume, 0.0f, 3.0f));
}

void AudioProcessor::enqueueExternalPcm(const QByteArray &pcmData) {
    if (!running.load() || pcmData.size() < static_cast<int>(sizeof(qint16))) {
        if (fobosVerboseLoggingEnabled()) {
            static std::atomic<int> droppedLogCount{0};
            const int logIndex = droppedLogCount.fetch_add(1);
            if (logIndex < 20 || (logIndex % 100) == 0) {
                qDebug() << "[Audio] external PCM dropped"
                         << "running" << running.load()
                         << "bytes" << pcmData.size();
            }
        }
        return;
    }

    const int sampleCount = pcmData.size() / static_cast<int>(sizeof(qint16));
    std::vector<short> samples;
    samples.reserve(static_cast<size_t>(sampleCount));
    const char *raw = pcmData.constData();
    for (int i = 0; i < sampleCount; ++i) {
        const unsigned char lo = static_cast<unsigned char>(raw[i * 2]);
        const unsigned char hi = static_cast<unsigned char>(raw[i * 2 + 1]);
        const qint16 value = static_cast<qint16>(static_cast<quint16>(lo) |
                                                 (static_cast<quint16>(hi) << 8));
        samples.push_back(static_cast<short>(value));
    }

    bool notifyPlayback = false;
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        const size_t queuedSamples = queuedAudioSamples();
        const size_t overflow = queuedSamples + samples.size() > MAX_AUDIO_BUFFER_SAMPLES
                                    ? queuedSamples + samples.size() - MAX_AUDIO_BUFFER_SAMPLES
                                    : 0;
        if (overflow > 0) {
            discardAudioSamples(overflow);
        }
        audioBuffer.insert(audioBuffer.end(), samples.begin(), samples.end());
        compactAudioBufferIfNeeded();
        const size_t afterInsertSamples = queuedAudioSamples();
        if (!externalAudioPrimed) {
            externalAudioPrimed = afterInsertSamples >= AUDIO_START_BUFFER_SAMPLES;
        }
        notifyPlayback = externalAudioPrimed;
        if (fobosVerboseLoggingEnabled()) {
            static std::atomic<int> queuedLogCount{0};
            const int logIndex = queuedLogCount.fetch_add(1);
            if (logIndex < 40 || (logIndex % 100) == 0) {
                qDebug() << "[Audio] external PCM queued"
                         << "samples" << samples.size()
                         << "queued" << afterInsertSamples
                         << "primed" << externalAudioPrimed
                         << "notify" << notifyPlayback
                         << "overflow" << overflow;
            }
        }
    }
    if (notifyPlayback) {
        cv.notify_one();
    }
}

void AudioProcessor::clearExternalPcm() {
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        audioBuffer.clear();
        audioBufferReadOffset = 0;
        externalAudioPrimed = false;
    }
    cv.notify_one();
}

void AudioProcessor::startDemodulation() {
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true)) {
        qDebug() << "Audio demodulation is already running.";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(audioMutex);
        audioBuffer.clear();
        audioBufferReadOffset = 0;
        externalAudioPrimed = false;
        for (int i = 0; i < NUM_BUFFERS; ++i) {
            bufferReady[i] = true;
        }
    }
    resetDemodulatorState();

    if (localPlaybackEnabled.load()) {
#ifdef _WIN32
        const int audioDeviceId = currentSettingsSnapshot().audioDeviceId;
        if (!openAudioDevice(audioDeviceId)) {
            closeAudioDevice();
            qDebug() << "Failed to open local audio output; demodulation will continue without waveOut playback.";
        }
#else
        qDebug() << "[Audio] local playback is routed through the Qt audio bridge on this platform.";
#endif
    } else {
        qDebug() << "[Audio] local audio output disabled; demodulation will continue for streaming.";
    }
    // Запуск SDR-потока
    sdrWorker = std::thread(&AudioProcessor::SDRThread, this);

    // Запуск потока вывода аудио
    audioWorker = std::thread(&AudioProcessor::startAudioOutput, this);
}



void AudioProcessor::stopDemodulation() {
    stopDemodulationImpl(true);
}

void AudioProcessor::stopDemodulationForStreamRestart() {
    stopDemodulationImpl(false);
}

void AudioProcessor::stopDemodulationImpl(bool closeOutputDevice) {
    const bool runningNow = running.load();
#ifdef _WIN32
    const bool audioDeviceOpen = hWaveOut != nullptr;
#else
    const bool audioDeviceOpen = false;
#endif
    if (!runningNow && !sdrWorker.joinable() && !audioWorker.joinable() && !audioDeviceOpen) {
        return;
    }

    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[Audio] stopDemodulation enter"
                 << "closeOutputDevice" << closeOutputDevice
                 << "running" << runningNow
#ifdef _WIN32
                 << "hWaveOut" << hWaveOut
#endif
                 << "sdrJoinable" << sdrWorker.joinable()
                 << "audioJoinable" << audioWorker.joinable();
    }
    const bool wasRunning = running.exchange(false);
    cv.notify_all();

    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[Audio] stopDemodulation joining workers" << "wasRunning" << wasRunning;
    }
    joinWorkerThreads();
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[Audio] stopDemodulation workers joined";
    }
    if (wasRunning) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[Audio] stopDemodulation resetting audio device";
        }
        pauseAudioDevice();
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[Audio] stopDemodulation audio device reset";
        }
    }
    if (closeOutputDevice) {
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[Audio] stopDemodulation closing audio device";
        }
        closeAudioDevice();
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[Audio] stopDemodulation audio device closed";
        }
    } else if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[Audio] stopDemodulation output retained for stream restart";
    }

    {
        std::lock_guard<std::mutex> lock(audioMutex);
        audioBuffer.clear();
        audioBufferReadOffset = 0;
        for (int i = 0; i < NUM_BUFFERS; ++i) {
            bufferReady[i] = true;
        }
    }
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[Audio] stopDemodulation exit";
    }
}

bool AudioProcessor::openAudioDevice(int deviceID) {
#ifndef _WIN32
    Q_UNUSED(deviceID);
    return true;
#else
    if (hWaveOut && waveHeadersPrepared && openedAudioDeviceID == deviceID) {
        audioDeviceClosing = false;
        return true;
    }

    if (hWaveOut) {
        closeAudioDevice();
    }

    std::lock_guard<std::mutex> lock(waveOutMutex);
    audioDeviceClosing = false;
    MMRESULT result = waveOutOpen(&hWaveOut, deviceID, &format,
                                  reinterpret_cast<DWORD_PTR>(&AudioProcessor::WaveOutCallback),
                                  reinterpret_cast<DWORD_PTR>(this),
                                  CALLBACK_FUNCTION);

    if (result != MMSYSERR_NOERROR) {
        hWaveOut = nullptr;
        return false;
    }

    openedAudioDeviceID = deviceID;
    prepareWaveHeaders();
    return waveHeadersPrepared;
#endif
}

void AudioProcessor::closeAudioDevice() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(waveOutMutex);
    if (!hWaveOut) {
        return;
    }

    audioDeviceClosing = true;
    waveOutReset(hWaveOut);
    unprepareWaveHeaders();
    waveOutClose(hWaveOut);
    hWaveOut = nullptr;
    openedAudioDeviceID = -1;
#endif
}

void AudioProcessor::pauseAudioDevice() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(waveOutMutex);
    if (!hWaveOut) {
        return;
    }

    audioDeviceClosing = true;
    waveOutReset(hWaveOut);
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        bufferReady[i] = true;
    }
    cv.notify_all();
#else
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        bufferReady[i] = true;
    }
    cv.notify_all();
#endif
}

void AudioProcessor::prepareWaveHeaders() {
    waveHeadersPrepared = false;

#ifdef _WIN32
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        ZeroMemory(&waveHdrs[i], sizeof(WAVEHDR));
        waveHdrs[i].lpData = reinterpret_cast<LPSTR>(waveBuffers[i].data());
        waveHdrs[i].dwBufferLength = BUFFER_SIZE * sizeof(short);
        bufferReady[i] = true;

        MMRESULT result = waveOutPrepareHeader(hWaveOut, &waveHdrs[i], sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            qDebug() << "waveOutPrepareHeader failed!";
            unprepareWaveHeaders();
            return;
        }
    }

    waveHeadersPrepared = true;
#endif
}

void AudioProcessor::unprepareWaveHeaders() {
#ifdef _WIN32
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        if (waveHdrs[i].dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(hWaveOut, &waveHdrs[i], sizeof(WAVEHDR));
        }
        bufferReady[i] = true;
    }
#else
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        bufferReady[i] = true;
    }
#endif
    waveHeadersPrepared = false;
}

void AudioProcessor::joinWorkerThreads() {
    if (sdrWorker.joinable() && sdrWorker.get_id() != std::this_thread::get_id()) {
        sdrWorker.join();
    }
    if (audioWorker.joinable() && audioWorker.get_id() != std::this_thread::get_id()) {
        audioWorker.join();
    }
}

void AudioProcessor::resetDemodulatorState() {
    audioIqChannelizer.reset();
    audioChannelizedIq.clear();
    ncoPhase = 0.0;
    audioResamplePhase = 0.0;
    amLowPassState = std::complex<float>(0.0f, 0.0f);
    dmrChannelPreLowPassStates.fill(std::complex<float>(0.0f, 0.0f));
    sidebandLowPassStates.fill(std::complex<float>(0.0f, 0.0f));
    channelDecimationSum = std::complex<float>(0.0f, 0.0f);
    channelDecimationCount = 0;
    fmPreviousSample = std::complex<float>(1.0f, 0.0f);
    fmPreviousValid = false;
    fmDeemphasisState = 0.0f;
    demodAudioLowPassState = 0.0f;
    demodAudioLowPassState2 = 0.0f;
    demodAudioLowPassState3 = 0.0f;
    demodAudioHighPassState = 0.0f;
    audioFilterChain.reset();
    samCarrierPhase = 0.0;
    samCarrierFrequency = 0.0;
    sidebandFilterPhase = 0.0;
    cwTonePhase = 0.0;
    amDcEstimate = 0.0f;
    amAgcLevel = 0.05f;
    dmrCicDecimator.reset();
    dmrRecordingAgcLevel = 0.01f;
    dmrPcmPending.clear();
    dmrResamplePhase = 0.0;
    dmrLowPassState = std::complex<float>(0.0f, 0.0f);
    dmrPreviousSample = std::complex<float>(1.0f, 0.0f);
    dmrPreviousValid = false;
    dmrDiscriminatorDc = 0.0f;
    dmrFskLowPassState = 0.0f;
    dmrFskLowPassState2 = 0.0f;
    dmrPreviousOutputSample = 0.0f;
    dmrPreviousOutputValid = false;
    dmrLastLoggedOutputRate = 0;
    dmrLastLoggedDecimationFactor = 0;
    dmrLastLoggedChannelRate = 0.0;
    hfNoiseCancelCoeff = {0.0f, 0.0f};
    hfNoiseCancelRefDecimationSum = {0.0f, 0.0f};
    hfNoiseCancelTapCoeffs.fill(std::complex<float>(0.0f, 0.0f));
    hfNoiseCancelRefHistory.fill(std::complex<float>(0.0f, 0.0f));
    hfNoiseCancelRefHistoryIndex = 0;
    hfAudioBlankerEnvelope = 0.0f;
    hfAudioBlankerHoldSamples = 0;
    hfAudioBlankerLastCleanSample = {0.0f, 0.0f};
}

size_t AudioProcessor::queuedAudioSamples() const {
    return audioBuffer.size() > audioBufferReadOffset
               ? audioBuffer.size() - audioBufferReadOffset
               : 0;
}

void AudioProcessor::discardAudioSamples(size_t count) {
    audioBufferReadOffset += (std::min)(count, queuedAudioSamples());
    compactAudioBufferIfNeeded();
}

void AudioProcessor::compactAudioBufferIfNeeded() {
    if (audioBufferReadOffset == 0) {
        return;
    }

    if (audioBufferReadOffset >= audioBuffer.size()) {
        audioBuffer.clear();
        audioBufferReadOffset = 0;
        return;
    }

    if (audioBufferReadOffset >= BUFFER_SIZE * 4 && audioBufferReadOffset >= audioBuffer.size() / 2) {
        audioBuffer.erase(audioBuffer.begin(), audioBuffer.begin() + static_cast<std::ptrdiff_t>(audioBufferReadOffset));
        audioBufferReadOffset = 0;
    }
}

RadioSettings AudioProcessor::currentSettingsSnapshot() const {
    std::lock_guard<std::mutex> lock(settingsMutex);
    return audioSettings;
}

void AudioProcessor::processDmrIqDemodulatorBlock(const std::vector<float>& iqBlock,
                                                  std::vector<short>& dmrBasebandSamples,
                                                  int &dmrBasebandSampleRate,
                                                  const RadioSettings &settings) {
    dmrBasebandSamples.clear();
    dmrBasebandSampleRate = 0;

    const double rfInputRate = settings.sampleRate;
    if (iqBlock.size() < 2 || rfInputRate <= 0.0) {
        return;
    }

    const std::size_t iqSamples = iqBlock.size() / 2;
    const int requestedOutputRate =
        normalizedDmrBasebandSampleRate(settings.dmrBasebandSampleRate);
    const int manualChannelRate = settings.dmrChannelSampleRate;
    const bool manualChannelProfile =
        manualChannelRate == 192000 ||
        manualChannelRate == 384000 ||
        manualChannelRate == 768000 ||
        manualChannelRate == 1536000;
    const double preferredDmrChannelRate =
        manualChannelProfile
            ? static_cast<double>(manualChannelRate)
            : (rfInputRate >= DMR_HIGH_RF_THRESHOLD ? DMR_HIGH_RF_CHANNEL_RATE : DMR_CHANNEL_RATE);
    const double dmrChannelTargetRate =
        (std::max)(preferredDmrChannelRate, static_cast<double>(requestedOutputRate));
    const int decimationFactor =
        (std::max)(1, static_cast<int>(std::floor(rfInputRate / dmrChannelTargetRate)));
    const double channelRate = rfInputRate / static_cast<double>(decimationFactor);
    if (channelRate + 0.5 < static_cast<double>(requestedOutputRate)) {
        qWarning() << "[DMR demod] requested 4FSK output rate exceeds channel rate"
                   << "requestedRate" << requestedOutputRate
                   << "channelRate" << channelRate
                   << "rfRate" << rfInputRate
                   << "decimation" << decimationFactor;
        return;
    }

    const double liveCarrierOffset = settings.liveDigitalSyncEnabled
                                         ? settings.liveDigitalSyncCarrierOffsetHz
                                         : 0.0;
    const double fShift = settings.listeningFrequency - settings.centerFrequency +
                          liveCarrierOffset;
    const double phaseIncrement = -TWO_PI * fShift / rfInputRate;
    const float channelLowPassAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI *
                       (std::min)(DMR_FOURFSK_CHANNEL_CUTOFF_HZ, channelRate * 0.42) /
                       channelRate),
        0.000001,
        1.0));
    const float fskLowPassAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI *
                       (std::min)(DMR_FOURFSK_SYMBOL_FILTER_HZ, channelRate * 0.42) /
                       channelRate),
        0.000001,
        1.0));
    const float dcAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI * DMR_FOURFSK_DC_TRACK_HZ / channelRate),
        0.000001,
        0.02));
    const double outputStep = static_cast<double>(requestedOutputRate) / channelRate;
    const double outputScale =
        DMR_FOURFSK_OUTPUT_OUTER_LEVEL / DMR_FOURFSK_OUTER_DEVIATION_HZ;

    dmrBasebandSampleRate = requestedOutputRate;
    dmrBasebandSamples.reserve(static_cast<std::size_t>(
        (static_cast<double>(iqSamples) / static_cast<double>(decimationFactor)) *
            outputStep +
        8.0));
    if (dmrLastLoggedOutputRate != requestedOutputRate ||
        dmrLastLoggedDecimationFactor != decimationFactor ||
        std::abs(dmrLastLoggedChannelRate - channelRate) > 1.0) {
        dmrLastLoggedOutputRate = requestedOutputRate;
        dmrLastLoggedDecimationFactor = decimationFactor;
        dmrLastLoggedChannelRate = channelRate;
        if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DMR demod] IQ FM/4FSK path"
                     << "rfRate" << rfInputRate
                     << "targetChannelRate" << dmrChannelTargetRate
                     << "channelRate" << channelRate
                     << "decimation" << decimationFactor
                     << "profile" << (manualChannelProfile
                                           ? QStringLiteral("manual")
                                           : (rfInputRate >= DMR_HIGH_RF_THRESHOLD
                                                  ? QStringLiteral("high-rf")
                                                  : QStringLiteral("normal")))
                     << "manualChannelRate" << manualChannelRate
                     << "outputRate" << requestedOutputRate
                     << "samplesPerSymbol"
                     << (static_cast<double>(requestedOutputRate) / 4800.0)
                     << "cicStages" << DMR_CHANNEL_CIC_STAGES
                     << "channelCutoffHz" << DMR_FOURFSK_CHANNEL_CUTOFF_HZ
                     << "symbolFilterHz" << DMR_FOURFSK_SYMBOL_FILTER_HZ
                     << "outerLevel" << DMR_FOURFSK_OUTPUT_OUTER_LEVEL;
        }
    }

    float rotI = static_cast<float>(std::cos(ncoPhase));
    float rotQ = static_cast<float>(std::sin(ncoPhase));
    const float rotStepI = static_cast<float>(std::cos(phaseIncrement));
    const float rotStepQ = static_cast<float>(std::sin(phaseIncrement));
    dmrCicDecimator.configure(decimationFactor);
    std::complex<float> channelLowPass = dmrLowPassState;
    std::complex<float> previousSample = dmrPreviousSample;
    bool previousValid = dmrPreviousValid;
    float discriminatorDc = dmrDiscriminatorDc;
    float fskLowPass = dmrFskLowPassState;
    float fskLowPass2 = dmrFskLowPassState2;
    float previousOutputSample = dmrPreviousOutputSample;
    bool previousOutputValid = dmrPreviousOutputValid;
    double resamplePhase = dmrResamplePhase;

    const auto pushDemodSample = [&](float normalizedSample) {
        if (!std::isfinite(normalizedSample)) {
            normalizedSample = 0.0f;
        }
        normalizedSample =
            (std::clamp)(normalizedSample, -DMR_FOURFSK_LIMIT_INPUT, DMR_FOURFSK_LIMIT_INPUT);

        if (!previousOutputValid) {
            previousOutputSample = normalizedSample;
            previousOutputValid = true;
            return;
        }

        const double phaseBefore = resamplePhase;
        resamplePhase += outputStep;
        while (resamplePhase >= 1.0) {
            const double fraction =
                outputStep > 0.0
                    ? (std::clamp)((1.0 - phaseBefore) / outputStep, 0.0, 1.0)
                    : 1.0;
            const float interpolated =
                previousOutputSample +
                static_cast<float>(fraction) * (normalizedSample - previousOutputSample);
            const float clamped =
                std::tanh(interpolated * DMR_FOURFSK_SOFT_LIMIT_DRIVE);
            dmrBasebandSamples.push_back(
                static_cast<short>(std::lrint(clamped * 32767.0f)));
            resamplePhase -= 1.0;
        }
        previousOutputSample = normalizedSample;
    };

    const bool captureChannelIq = settings.inputMode == INPUT_RF && dmrChannelIqEnabled.load();
    QByteArray channelIq;
    std::size_t capturedSamples = 0;
    if (captureChannelIq) {
        channelIq.resize(static_cast<int>((iqSamples / std::size_t(decimationFactor) + 1) * 4));
    }
    for (std::size_t n = 0; n < iqSamples; ++n) {
        float iSample = iqBlock[2 * n];
        float qSample = iqBlock[2 * n + 1];
        if (!std::isfinite(iSample)) iSample = 0.0f;
        if (!std::isfinite(qSample)) qSample = 0.0f;

        if (settings.inputMode == INPUT_HF_COMBINED) {
            if (fShift < 0.0) {
                qSample = 0.0f;
            } else {
                iSample = qSample;
                qSample = 0.0f;
            }
        } else if (settings.inputMode == INPUT_HF1) {
            qSample = 0.0f;
        } else if (settings.inputMode == INPUT_HF2) {
            iSample = qSample;
            qSample = 0.0f;
        }

        const float mixedI = iSample * rotI - qSample * rotQ;
        const float mixedQ = iSample * rotQ + qSample * rotI;
        std::complex<float> channelSample;
        const bool channelSampleReady = dmrCicDecimator.push({mixedI, mixedQ}, channelSample);
        const float nextRotI = rotI * rotStepI - rotQ * rotStepQ;
        const float nextRotQ = rotI * rotStepQ + rotQ * rotStepI;
        rotI = nextRotI;
        rotQ = nextRotQ;
        if ((n & 4095) == 4095) {
            const float norm = std::sqrt(rotI * rotI + rotQ * rotQ);
            if (norm > 0.0f) { rotI /= norm; rotQ /= norm; }
        }
        if (!channelSampleReady) continue;

        channelLowPass += channelLowPassAlpha * (channelSample - channelLowPass);
        std::complex<float> limited = channelLowPass;
        if (captureChannelIq) {
            const float magnitude = std::abs(limited);
            const float agcAlpha = magnitude > dmrRecordingAgcLevel ? 0.01f : 0.0002f;
            dmrRecordingAgcLevel += agcAlpha * (magnitude - dmrRecordingAgcLevel);
            const float gain = 0.2f / (std::max)(dmrRecordingAgcLevel, 0.00001f);
            const auto quantize = [gain](float value) {
                return static_cast<quint16>(static_cast<qint16>(std::lrint(
                    (std::clamp)(value * gain, -1.0f, 1.0f) * 32767.0f)));
            };
            const quint16 i = quantize(channelLowPass.real()), q = quantize(channelLowPass.imag());
            char *dst = channelIq.data() + capturedSamples++ * 4;
            dst[0] = char(i & 255); dst[1] = char(i >> 8);
            dst[2] = char(q & 255); dst[3] = char(q >> 8);
        }
        // Phase must not depend on receiver gain. Scale before the conjugate
        // product to avoid underflow, without fabricating a phase for zero IQ.
        const float componentScale = (std::max)(std::abs(limited.real()),
                                                std::abs(limited.imag()));
        if (componentScale >= (std::numeric_limits<float>::min)()) {
            limited /= componentScale;
        } else {
            limited = std::complex<float>(0.0f, 0.0f);
        }

        if (previousValid) {
            const std::complex<float> delta = limited * std::conj(previousSample);
            const float phase = delta == std::complex<float>(0.0f, 0.0f)
                                    ? 0.0f
                                    : std::atan2(std::imag(delta), std::real(delta));
            const float instantaneousHz =
                static_cast<float>(static_cast<double>(phase) * channelRate / TWO_PI);
            discriminatorDc += dcAlpha * (instantaneousHz - discriminatorDc);
            const float centeredHz = instantaneousHz - discriminatorDc;
            fskLowPass += fskLowPassAlpha * (centeredHz - fskLowPass);
            fskLowPass2 += fskLowPassAlpha * (fskLowPass - fskLowPass2);
            pushDemodSample(static_cast<float>(fskLowPass2 * outputScale));
        }
        previousSample = limited;
        previousValid = true;
    }

    ncoPhase = std::remainder(ncoPhase + phaseIncrement * static_cast<double>(iqSamples), TWO_PI);
    dmrLowPassState = channelLowPass;
    dmrPreviousSample = previousSample;
    dmrPreviousValid = previousValid;
    dmrDiscriminatorDc = discriminatorDc;
    dmrFskLowPassState = fskLowPass;
    dmrFskLowPassState2 = fskLowPass2;
    dmrPreviousOutputSample = previousOutputSample;
    dmrPreviousOutputValid = previousOutputValid;
    dmrResamplePhase = std::remainder(resamplePhase, 1.0);
    if (dmrResamplePhase < 0.0) {
        dmrResamplePhase += 1.0;
    }
    if (capturedSamples > 0) {
        channelIq.resize(static_cast<int>(capturedSamples * 4));
        emit dmrChannelIqFrameReady(channelIq, channelRate, static_cast<int>(capturedSamples));
    }
}

void AudioProcessor::processDemodulatorBlock(const std::vector<float>& iqBlock,
                                             std::vector<short>& audioSamples,
                                             std::vector<short>& dmrBasebandSamples,
                                             int &dmrBasebandSampleRate,
                                             const RadioSettings &settings) {
    audioSamples.clear();
    dmrBasebandSamples.clear();
    dmrBasebandSampleRate = 0;

    double rfInputRate = settings.sampleRate;
    double audioTimingRate = rfInputRate;

    if (iqBlock.size() < 2 || rfInputRate <= 0.0 || audioTimingRate <= 0.0) {
        return;
    }

    const int modulationType = settings.modulationType;
    const int demodulationType = modulationType == MOD_SSTV
                                     ? sstvDemodulationModulationType(
                                           settings.sstvDemodulationMode)
                                     : modulationType;
    const bool sstvNfm = modulationType == MOD_SSTV &&
                         demodulationType == MOD_NFM;
    if (modulationType == MOD_DMR) {
        processDmrIqDemodulatorBlock(iqBlock, dmrBasebandSamples, dmrBasebandSampleRate, settings);
        return;
    }

    const int sourceInputMode = settings.inputMode;
    int inputMode = sourceInputMode;
    const std::vector<float> *demodIq = &iqBlock;
    bool channelizedInput = false;
    // Every SSTV demodulator needs a phase-accurate, properly anti-aliased
    // narrow channel. The inexpensive streaming decimator is adequate for
    // listening, but at wide RF rates its boxcar response folds enough noise
    // into NFM to corrupt the tightly timed VIS tones.
    const bool preciseSstvChannelizer = modulationType == MOD_SSTV;
    if (sourceInputMode != INPUT_HF_NOISE_CANCEL &&
        (!settings.simplifiedAudioChannelizer || preciseSstvChannelizer)) {
        RadioSettings channelSettings = settings;
        channelSettings.modulationType = demodulationType;
        if (sstvNfm) {
            channelSettings.bandwidth = (std::max)(channelSettings.bandwidth, 25000.0);
        }
        if (settings.liveDigitalSyncEnabled &&
            std::isfinite(settings.liveDigitalSyncCarrierOffsetHz)) {
            channelSettings.listeningFrequency +=
                settings.liveDigitalSyncCarrierOffsetHz;
        }
        const IqChannelizer::Result channelResult =
            audioIqChannelizer.processFloatIq(iqBlock.data(),
                                              iqBlock.size(),
                                              channelSettings,
                                              audioChannelizedIq,
                                              false);
        if (!channelResult.valid || audioChannelizedIq.size() < 2U) {
            return;
        }
        if (settings.liveDigitalSyncEnabled &&
            (demodulationType == MOD_PSK || demodulationType == MOD_FT8) &&
            std::isfinite(settings.liveDigitalSyncPhaseRadians)) {
            const float rotationI = static_cast<float>(
                std::cos(-settings.liveDigitalSyncPhaseRadians));
            const float rotationQ = static_cast<float>(
                std::sin(-settings.liveDigitalSyncPhaseRadians));
            for (std::size_t index = 0; index + 1U < audioChannelizedIq.size(); index += 2U) {
                const float inputI = audioChannelizedIq[index];
                const float inputQ = audioChannelizedIq[index + 1U];
                audioChannelizedIq[index] = inputI * rotationI - inputQ * rotationQ;
                audioChannelizedIq[index + 1U] = inputI * rotationQ + inputQ * rotationI;
            }
        }
        demodIq = &audioChannelizedIq;
        rfInputRate = channelResult.outputRate;
        audioTimingRate = rfInputRate;
        inputMode = INPUT_RF;
        channelizedInput = true;
    }

    const size_t iqSamples = demodIq->size() / 2;
    const double fShift = channelizedInput
                              ? 0.0
                              : settings.listeningFrequency - settings.centerFrequency;
    const double phaseIncrement = -TWO_PI * fShift / rfInputRate;
    // The normal SSTV preset is the 3 kHz audio passband used by SSB.  It is
    // too narrow when the same tone stream is carried by NFM: deviation and
    // the 2.3 kHz top tone both have to fit in the RF channel.
    const double bandwidth = sstvNfm
                                 ? (std::max)(settings.bandwidth, 25000.0)
                                 : settings.bandwidth;
    const int decimationFactor = channelizedInput
                                     ? 1
                                     : channelDecimationFactor(rfInputRate,
                                                               demodulationType,
                                                               bandwidth);
    const double rfChannelRate = rfInputRate / decimationFactor;
    const double audioTimingChannelRate = audioTimingRate / decimationFactor;
    const double outputStep = AUDIO_OUTPUT_RATE / audioTimingChannelRate;

    double cutoff = channelCutoffForMode(demodulationType, bandwidth);
    cutoff = (std::min)(cutoff, rfChannelRate * 0.45);
    const float lowPassAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI * cutoff / rfChannelRate),
        0.000001,
        1.0
        ));
    const bool digitalAudioMode = isDigitalAudioMode(modulationType);
    const bool fastFm = !sstvNfm && settings.simplifiedAudioChannelizer &&
        (demodulationType == MOD_NFM || demodulationType == MOD_WFM ||
         demodulationType == MOD_ATV || demodulationType == MOD_APT || demodulationType == MOD_FSK);
    const bool rotateIq = fShift != 0.0 || ncoPhase != 0.0;
    double demodAudioCutoff = demodAudioCutoffForMode(demodulationType, bandwidth);
    if (!digitalAudioMode && settings.audioLowPassHz > 0.0) {
        demodAudioCutoff = clampDouble(settings.audioLowPassHz, 100.0, 20000.0);
    }
    demodAudioCutoff = (std::min)(demodAudioCutoff, AUDIO_OUTPUT_RATE * 0.45);
    const double demodAudioFilterRate =
        modulationType == MOD_DMR ? audioTimingChannelRate : AUDIO_OUTPUT_RATE;
    const float demodAudioLowPassAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI * demodAudioCutoff / demodAudioFilterRate),
        0.000001,
        1.0
        ));
    const double demodAudioHighPassCutoff =
        (!digitalAudioMode && settings.audioHighPassHz > 0.0)
            ? clampDouble(settings.audioHighPassHz, 10.0, 1000.0)
            : 0.0;
    const float demodAudioHighPassAlpha =
        demodAudioHighPassCutoff > 0.0
            ? static_cast<float>((std::clamp)(
                  1.0 - std::exp(-TWO_PI * demodAudioHighPassCutoff / AUDIO_OUTPUT_RATE),
                  0.000001,
                  1.0))
            : 0.0f;

    const bool lowerSideband = isLowerSidebandMode(demodulationType);
    const double sidebandHighCut = (std::min)(cutoff, rfChannelRate * 0.45);
    const double sidebandLowCut = (std::min)(SSB_LOW_CUT_HZ, sidebandHighCut * 0.5);
    const double sidebandCenter = (sidebandLowCut + sidebandHighCut) * 0.5;
    const double sidebandHalfWidth = (std::max)(50.0, (sidebandHighCut - sidebandLowCut) * 0.5);
    const double sidebandShiftIncrement = (lowerSideband ? TWO_PI : -TWO_PI) * sidebandCenter / rfChannelRate;
    const float sidebandLowPassAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI * sidebandHalfWidth / rfChannelRate),
        0.000001,
        1.0
        ));

    const size_t reserveCount = static_cast<size_t>((iqSamples / decimationFactor + 1) * outputStep) + 8;
    audioSamples.reserve(reserveCount);

    float rotI = static_cast<float>(std::cos(ncoPhase));
    float rotQ = static_cast<float>(std::sin(ncoPhase));
    const float rotStepI = static_cast<float>(std::cos(phaseIncrement));
    const float rotStepQ = static_cast<float>(std::sin(phaseIncrement));
    float channelSumI = std::real(channelDecimationSum);
    float channelSumQ = std::imag(channelDecimationSum);
    int decimationCount = channelDecimationCount;
    float lowPassI = std::real(amLowPassState);
    float lowPassQ = std::imag(amLowPassState);
    float fmPrevI = std::real(fmPreviousSample);
    float fmPrevQ = std::imag(fmPreviousSample);
    std::array<std::complex<float>, 4> sidebandStates = sidebandLowPassStates;
    double sidebandPhase = sidebandFilterPhase;
    float audioLowPass = demodAudioLowPassState;
    float audioLowPass2 = demodAudioLowPassState2;
    float audioLowPass3 = demodAudioLowPassState3;
    float audioHighPass = demodAudioHighPassState;
    double samPhase = samCarrierPhase;
    double samFrequency = samCarrierFrequency;
    const double samMaxFrequency = TWO_PI * SAM_LOCK_RANGE_HZ / rfChannelRate;
    const double fmDeemphasisSeconds = demodulationType == MOD_NFM
                                           ? NFM_DEEMPHASIS_SECONDS
                                           : FM_DEEMPHASIS_SECONDS;
    const float fmDeemphasisAlpha = static_cast<float>(
        1.0 - std::exp(-1.0 / ((std::max)(1.0, rfChannelRate) * fmDeemphasisSeconds))
        );
    const bool adaptiveNoiseCancel = inputMode == INPUT_HF_NOISE_CANCEL;
    const bool hfAudioBlankerEnabled =
        settings.hfAudioBlankerEnabled &&
        isDirectInputMode(sourceInputMode) &&
        !digitalAudioMode;
    const float hfAudioBlankerThreshold = static_cast<float>(
        (std::clamp)(settings.hfAudioBlankerThreshold, 2.0, 20.0));
    float hfBlankerEnvelope = hfAudioBlankerEnvelope;
    int hfBlankerHoldSamples = hfAudioBlankerHoldSamples;
    std::complex<float> hfBlankerLastClean = hfAudioBlankerLastCleanSample;
    const int hfBlankerHoldTarget =
        (std::max)(1, static_cast<int>(std::lround(rfChannelRate * 0.00008)));
    const float hfBlankerEnvAlpha = static_cast<float>((std::clamp)(
        1.0 - std::exp(-TWO_PI * 80.0 / rfChannelRate),
        0.000001,
        1.0));
    std::complex<float> refChannelSum = hfNoiseCancelRefDecimationSum;
    std::complex<float> adaptiveCoeff = hfNoiseCancelCoeff;
    auto adaptiveTaps = hfNoiseCancelTapCoeffs;
    auto refHistory = hfNoiseCancelRefHistory;
    int refHistoryIndex = hfNoiseCancelRefHistoryIndex;
    if (hfNoiseCancelResetRequested.exchange(false)) {
        refChannelSum = {0.0f, 0.0f};
        adaptiveCoeff = {0.0f, 0.0f};
        adaptiveTaps.fill(std::complex<float>(0.0f, 0.0f));
        refHistory.fill(std::complex<float>(0.0f, 0.0f));
        refHistoryIndex = 0;
        hfNoiseCancelCoeff = {0.0f, 0.0f};
        hfNoiseCancelRefDecimationSum = {0.0f, 0.0f};
        hfNoiseCancelTapCoeffs.fill(std::complex<float>(0.0f, 0.0f));
        hfNoiseCancelRefHistory.fill(std::complex<float>(0.0f, 0.0f));
        hfNoiseCancelRefHistoryIndex = 0;
    }
    if (!adaptiveNoiseCancel) {
        hfNoiseCancelCoeff = {0.0f, 0.0f};
        hfNoiseCancelRefDecimationSum = {0.0f, 0.0f};
        hfNoiseCancelTapCoeffs.fill(std::complex<float>(0.0f, 0.0f));
        hfNoiseCancelRefHistory.fill(std::complex<float>(0.0f, 0.0f));
        hfNoiseCancelRefHistoryIndex = 0;
    }
    const float noiseCancelDepth =
        static_cast<float>((std::clamp)(settings.hfNoiseCancelDepth, 0.0, 2.0));
    const std::complex<float> manualRefCoeff =
        hfNoiseCancelReferenceCoefficient(settings, settings.listeningFrequency);
    constexpr float adaptiveMu = 0.075f;
    constexpr float adaptiveEpsilon = 1.0e-8f;

    for (size_t n = 0; n < iqSamples; ++n) {
        float iSample = (*demodIq)[2 * n];
        float qSample = (*demodIq)[2 * n + 1];
        if (!std::isfinite(iSample)) {
            iSample = 0.0f;
        }
        if (!std::isfinite(qSample)) {
            qSample = 0.0f;
        }

        if (adaptiveNoiseCancel) {
            const std::complex<float> oscillator(rotI, rotQ);
            channelSumI += std::real(iSample * oscillator);
            channelSumQ += std::imag(iSample * oscillator);
            refChannelSum += qSample * oscillator;
        } else {
            if (inputMode == INPUT_HF_COMBINED) {
                if (fShift < 0.0) {
                    qSample = 0.0f;
                } else {
                    iSample = qSample;
                    qSample = 0.0f;
                }
            } else if (inputMode == INPUT_HF1) {
                qSample = 0.0f;
            } else if (inputMode == INPUT_HF2) {
                iSample = qSample;
                qSample = 0.0f;
            }
            const float mixedI = rotateIq ? iSample * rotI - qSample * rotQ : iSample;
            const float mixedQ = rotateIq ? iSample * rotQ + qSample * rotI : qSample;
            std::complex<float> mixedSample(mixedI, mixedQ);
            channelSumI += std::real(mixedSample);
            channelSumQ += std::imag(mixedSample);
        }
        ++decimationCount;

        if (rotateIq) {
            const float nextRotI = rotI * rotStepI - rotQ * rotStepQ;
            const float nextRotQ = rotI * rotStepQ + rotQ * rotStepI;
            rotI = nextRotI;
            rotQ = nextRotQ;
            if ((n & 4095) == 4095) {
                const float norm = std::sqrt(rotI * rotI + rotQ * rotQ);
                if (norm > 0.0f) { rotI /= norm; rotQ /= norm; }
            }
        }

        if (decimationCount < decimationFactor) {
            continue;
        }

        const float invDecimationCount = 1.0f / static_cast<float>(decimationCount);
        std::complex<float> channelSample(channelSumI * invDecimationCount,
                                          channelSumQ * invDecimationCount);
        if (adaptiveNoiseCancel) {
            std::complex<float> refSample = manualRefCoeff * refChannelSum * invDecimationCount;
            if (!std::isfinite(std::real(refSample)) || !std::isfinite(std::imag(refSample))) {
                refSample = std::complex<float>(0.0f, 0.0f);
            }
            refHistory[static_cast<std::size_t>(refHistoryIndex)] = refSample;

            std::complex<float> prediction(0.0f, 0.0f);
            float refPower = adaptiveEpsilon;
            for (int tap = 0; tap < HF_NOISE_CANCEL_TAP_COUNT; ++tap) {
                int historyIndex = refHistoryIndex - tap;
                if (historyIndex < 0) {
                    historyIndex += HF_NOISE_CANCEL_TAP_COUNT;
                }
                const std::complex<float> historySample =
                    refHistory[static_cast<std::size_t>(historyIndex)];
                prediction += adaptiveTaps[static_cast<std::size_t>(tap)] * historySample;
                refPower += std::norm(historySample);
            }

            if (!settings.hfNoiseCancelFreeze &&
                std::isfinite(refPower) &&
                refPower > adaptiveEpsilon * 2.0f) {
                const std::complex<float> error = channelSample - prediction;
                const float step = adaptiveMu / refPower;
                if (std::isfinite(std::real(error)) && std::isfinite(std::imag(error))) {
                    for (int tap = 0; tap < HF_NOISE_CANCEL_TAP_COUNT; ++tap) {
                        int historyIndex = refHistoryIndex - tap;
                        if (historyIndex < 0) {
                            historyIndex += HF_NOISE_CANCEL_TAP_COUNT;
                        }
                        const std::complex<float> historySample =
                            refHistory[static_cast<std::size_t>(historyIndex)];
                        adaptiveTaps[static_cast<std::size_t>(tap)] +=
                            step * error * std::conj(historySample);
                        adaptiveTaps[static_cast<std::size_t>(tap)] =
                            clampComplexMagnitude(adaptiveTaps[static_cast<std::size_t>(tap)],
                                                  HF_NOISE_CANCEL_MAX_COEFF);
                    }
                }
            }
            adaptiveCoeff = adaptiveTaps.front();
            channelSample -= noiseCancelDepth * prediction;
            refHistoryIndex = (refHistoryIndex + 1) % HF_NOISE_CANCEL_TAP_COUNT;
            refChannelSum = {0.0f, 0.0f};
        }
        if (hfAudioBlankerEnabled) {
            const float magnitude = std::abs(channelSample);
            if (std::isfinite(magnitude)) {
                if (hfBlankerEnvelope <= 0.0f) {
                    hfBlankerEnvelope = magnitude;
                } else {
                    hfBlankerEnvelope += hfBlankerEnvAlpha * (magnitude - hfBlankerEnvelope);
                }
            }

            const float triggerLevel =
                (std::max)(hfBlankerEnvelope * hfAudioBlankerThreshold, 0.00001f);
            if (magnitude > triggerLevel) {
                hfBlankerHoldSamples = hfBlankerHoldTarget;
                channelSample = hfBlankerLastClean;
            } else if (hfBlankerHoldSamples > 0) {
                --hfBlankerHoldSamples;
                channelSample = hfBlankerLastClean;
            } else {
                hfBlankerLastClean = channelSample;
            }
        } else {
            hfBlankerEnvelope = 0.0f;
            hfBlankerHoldSamples = 0;
            hfBlankerLastClean = channelSample;
        }

        const float channelI = std::real(channelSample);
        const float channelQ = std::imag(channelSample);
        channelSumI = 0.0f;
        channelSumQ = 0.0f;
        decimationCount = 0;

        lowPassI += lowPassAlpha * (channelI - lowPassI);
        lowPassQ += lowPassAlpha * (channelQ - lowPassQ);

        const std::complex<float> filteredChannel(lowPassI, lowPassQ);
        const float envelope = fastFm ? 0.0f : std::sqrt(lowPassI * lowPassI + lowPassQ * lowPassQ);
        float demodulatedSample = 0.0f;

        switch (demodulationType) {
        case MOD_ATV:
        case MOD_NFM:
        case MOD_APT:
        case MOD_WFM:
        case MOD_FSK:
        case MOD_DMR: {
            float limitedI = lowPassI;
            float limitedQ = lowPassQ;
            const float magnitude = fastFm ? 0.0f : std::sqrt(limitedI * limitedI + limitedQ * limitedQ);
            if (!fastFm && magnitude > 0.000001f) {
                const float invMagnitude = 1.0f / magnitude;
                limitedI *= invMagnitude;
                limitedQ *= invMagnitude;
            }
            if (fmPreviousValid) {
                if (fastFm) demodulatedSample = fmPhaseDifference(limitedI, limitedQ, fmPrevI, fmPrevQ);
                else {
                    const float discriminatorQ = limitedQ * fmPrevI - limitedI * fmPrevQ;
                    const float discriminatorI = limitedI * fmPrevI + limitedQ * fmPrevQ;
                    demodulatedSample = std::atan2(discriminatorQ, discriminatorI);
                }
            }
            fmPrevI = limitedI;
            fmPrevQ = limitedQ;
            fmPreviousValid = true;
            if (demodulationType == MOD_FSK || demodulationType == MOD_DMR) {
                demodulatedSample *= 10.0f;
            } else if (sstvNfm) {
                // SSTV tones encode their value directly as audio frequency.
                // Voice deemphasis tilts 1.1..2.3 kHz enough to weaken VIS and
                // line-sync decisions, so keep the discriminator response flat.
                demodulatedSample *= 4.0f;
            } else {
                fmDeemphasisState += fmDeemphasisAlpha * (demodulatedSample - fmDeemphasisState);
                demodulatedSample = fmDeemphasisState *
                                    ((demodulationType == MOD_WFM ||
                                      demodulationType == MOD_ATV)
                                         ? 2.5f
                                         : 8.0f);
                if (demodulationType == MOD_APT) {
                    demodulatedSample *= 0.5f;
                }
            }
            break;
        }
        case MOD_USB:
        case MOD_FT8:
        case MOD_RTTY:
        case MOD_PSK:
        case MOD_SSTV:
        case MOD_WEFAX:
        case MOD_LSB: {
            const float oscI = static_cast<float>(std::cos(sidebandPhase));
            const float oscQ = static_cast<float>(std::sin(sidebandPhase));
            const std::complex<float> shiftOscillator(oscI, oscQ);
            std::complex<float> shifted = filteredChannel * shiftOscillator;
            for (std::complex<float> &state : sidebandStates) {
                state += sidebandLowPassAlpha * (shifted - state);
                shifted = state;
            }
            const std::complex<float> restored = shifted * std::conj(shiftOscillator);
            demodulatedSample = std::real(restored);
            sidebandPhase = wrapRadians(sidebandPhase + sidebandShiftIncrement);
            break;
        }
        case MOD_SAM: {
            if (envelope > 0.000001f) {
                const float phaseError = std::atan2(lowPassQ, lowPassI);
                samFrequency = (std::clamp)(samFrequency + 0.00002 * phaseError,
                                            -samMaxFrequency,
                                            samMaxFrequency);
                samPhase = wrapRadians(samPhase + samFrequency + 0.02 * phaseError);
            }
            const float carrierI = static_cast<float>(std::cos(samPhase));
            const float carrierQ = static_cast<float>(std::sin(samPhase));
            demodulatedSample = lowPassI * carrierI + lowPassQ * carrierQ;
            break;
        }
        case MOD_DSB:
            demodulatedSample = lowPassI;
            break;
        case MOD_CW: {
            const float bfoI = static_cast<float>(std::cos(cwTonePhase));
            const float bfoQ = static_cast<float>(std::sin(cwTonePhase));
            demodulatedSample = lowPassI * bfoI - lowPassQ * bfoQ;
            cwTonePhase += TWO_PI * CW_TONE_HZ / rfChannelRate;
            if (cwTonePhase > M_PI_F || cwTonePhase < -M_PI_F) {
                cwTonePhase = std::remainder(cwTonePhase, TWO_PI);
            }
            break;
        }
        case MOD_AM:
        default:
            demodulatedSample = envelope;
            break;
        }

        audioLowPass += demodAudioLowPassAlpha * (demodulatedSample - audioLowPass);
        demodulatedSample = audioLowPass;
        if (!digitalAudioMode) {
            audioLowPass2 += demodAudioLowPassAlpha * (demodulatedSample - audioLowPass2);
            audioLowPass3 += demodAudioLowPassAlpha * (audioLowPass2 - audioLowPass3);
            demodulatedSample = audioLowPass3;
        } else {
            audioLowPass2 = demodulatedSample;
            audioLowPass3 = demodulatedSample;
        }

        audioResamplePhase += outputStep;
        while (audioResamplePhase >= 1.0) {
            audioResamplePhase -= 1.0;

            const float dcTrackingAlpha = sstvNfm
                                              ? 0.0026f
                                              : (digitalAudioMode ? 0.0001f : 0.0005f);
            amDcEstimate += dcTrackingAlpha *
                            (demodulatedSample - amDcEstimate);
            float acSample = demodulatedSample - amDcEstimate;
            if (demodAudioHighPassAlpha > 0.0f) {
                audioHighPass += demodAudioHighPassAlpha * (acSample - audioHighPass);
                acSample -= audioHighPass;
            } else {
                audioHighPass = 0.0f;
            }
            const float absSample = std::fabs(acSample);
            const float agcCoeff = absSample > amAgcLevel ? 0.01f : 0.0002f;
            amAgcLevel += agcCoeff * (absSample - amAgcLevel);
            amAgcLevel = (std::max)(amAgcLevel, 0.0001f);

            const float normalized = digitalAudioMode
                                         ? acSample * (0.25f / amAgcLevel)
                                         : std::tanh(acSample * (0.35f / amAgcLevel));
            const float finiteSample = std::isfinite(normalized) ? normalized : 0.0f;
            const float clamped = (std::clamp)(finiteSample, -1.0f, 1.0f);
            audioSamples.push_back(static_cast<short>(clamped * 32767.0f));
        }
    }

    channelDecimationSum = std::complex<float>(channelSumI, channelSumQ);
    channelDecimationCount = decimationCount;
    hfNoiseCancelCoeff = adaptiveNoiseCancel ? adaptiveCoeff : std::complex<float>(0.0f, 0.0f);
    hfNoiseCancelRefDecimationSum = adaptiveNoiseCancel ? refChannelSum : std::complex<float>(0.0f, 0.0f);
    if (adaptiveNoiseCancel) {
        hfNoiseCancelTapCoeffs = adaptiveTaps;
        hfNoiseCancelRefHistory = refHistory;
        hfNoiseCancelRefHistoryIndex = refHistoryIndex;
    } else {
        hfNoiseCancelTapCoeffs.fill(std::complex<float>(0.0f, 0.0f));
        hfNoiseCancelRefHistory.fill(std::complex<float>(0.0f, 0.0f));
        hfNoiseCancelRefHistoryIndex = 0;
    }
    hfAudioBlankerEnvelope = hfAudioBlankerEnabled ? hfBlankerEnvelope : 0.0f;
    hfAudioBlankerHoldSamples = hfAudioBlankerEnabled ? hfBlankerHoldSamples : 0;
    hfAudioBlankerLastCleanSample = hfAudioBlankerEnabled ? hfBlankerLastClean : std::complex<float>(0.0f, 0.0f);
    amLowPassState = std::complex<float>(lowPassI, lowPassQ);
    sidebandLowPassStates = sidebandStates;
    fmPreviousSample = std::complex<float>(fmPrevI, fmPrevQ);
    demodAudioLowPassState = audioLowPass;
    demodAudioLowPassState2 = audioLowPass2;
    demodAudioLowPassState3 = audioLowPass3;
    demodAudioHighPassState = audioHighPass;
    samCarrierPhase = wrapRadians(samPhase);
    samCarrierFrequency = samFrequency;
    sidebandFilterPhase = wrapRadians(sidebandPhase);
    ncoPhase = std::remainder(ncoPhase + phaseIncrement * static_cast<double>(iqSamples), TWO_PI);
}

void AudioProcessor::SDRThread() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "SDRThread started (Real IQ processing)";
    }

    std::vector<float> iqBlock;
    std::vector<short> audioSamples;
    std::vector<short> dmrBasebandSamples;
    RadioSettings activeSettings = currentSettingsSnapshot();
    int activeInputMode = activeSettings.inputMode;
    int activeModulationType = activeSettings.modulationType;
    int activeSstvDemodulationMode = activeSettings.sstvDemodulationMode;
    uint64_t audioBlockCounter = 0;
    double activeCenterFrequency = activeSettings.centerFrequency;
    double activeListeningFrequency = activeSettings.listeningFrequency;
    double activeSampleRate = activeSettings.sampleRate;
    double activeBandwidth = activeSettings.bandwidth;
    double activeAudioLowPassHz = activeSettings.audioLowPassHz;
    double activeAudioHighPassHz = activeSettings.audioHighPassHz;
    QString activeAudioFilterChainJson = activeSettings.audioFilterChainJson;
    audioFilterChain.configure(activeAudioFilterChainJson, AUDIO_OUTPUT_RATE);
    bool activeSimplifiedAudioChannelizer = activeSettings.simplifiedAudioChannelizer;
    bool activeHfAudioBlankerEnabled = activeSettings.hfAudioBlankerEnabled;
    double activeHfAudioBlankerThreshold = activeSettings.hfAudioBlankerThreshold;
    int activeDmrBasebandSampleRate =
        normalizedDmrBasebandSampleRate(activeSettings.dmrBasebandSampleRate);
    int activeDmrChannelSampleRate = activeSettings.dmrChannelSampleRate;
    auto healthWindowStart = std::chrono::steady_clock::now();
    std::uint64_t previousIqSequence = 0;
    std::uint64_t healthSequenceGaps = 0;
    std::uint64_t healthBlocks = 0;
    std::uint64_t healthIqSamples = 0;
    std::uint64_t healthAudioSamples = 0;
    double healthDemodMs = 0.0;
    double healthMaxDemodMs = 0.0;
    bool healthLoggingActive = fobosVerboseLoggingEnabled();
    std::uint64_t previousDroppedBlocks = healthLoggingActive ? IqBuffer::stats().droppedQueuedBlocks : 0;

    while (running) {
        const RadioSettings settings = currentSettingsSnapshot();
        const bool healthLogging = fobosVerboseLoggingEnabled();
        if (healthLogging != healthLoggingActive) {
            healthLoggingActive = healthLogging;
            healthWindowStart = std::chrono::steady_clock::now();
            healthBlocks = healthIqSamples = healthAudioSamples = healthSequenceGaps = 0;
            healthDemodMs = healthMaxDemodMs = 0.0;
            if (healthLogging) previousDroppedBlocks = IqBuffer::stats().droppedQueuedBlocks;
        }
        if (demodulatorResetRequested.exchange(false) ||
            activeInputMode != settings.inputMode ||
            activeModulationType != settings.modulationType ||
            activeSstvDemodulationMode != settings.sstvDemodulationMode ||
            std::abs(activeCenterFrequency - settings.centerFrequency) > 0.5 ||
            std::abs(activeListeningFrequency - settings.listeningFrequency) > 0.5 ||
            std::abs(activeSampleRate - settings.sampleRate) > 0.5 ||
            std::abs(activeBandwidth - settings.bandwidth) > 1.0 ||
            std::abs(activeAudioLowPassHz - settings.audioLowPassHz) > 1.0 ||
            std::abs(activeAudioHighPassHz - settings.audioHighPassHz) > 1.0 ||
             activeSimplifiedAudioChannelizer != settings.simplifiedAudioChannelizer ||
            activeHfAudioBlankerEnabled != settings.hfAudioBlankerEnabled ||
            std::abs(activeHfAudioBlankerThreshold - settings.hfAudioBlankerThreshold) > 0.01 ||
            activeDmrChannelSampleRate != settings.dmrChannelSampleRate ||
            activeDmrBasebandSampleRate !=
                normalizedDmrBasebandSampleRate(settings.dmrBasebandSampleRate)) {
            activeInputMode = settings.inputMode;
            activeModulationType = settings.modulationType;
            activeSstvDemodulationMode = settings.sstvDemodulationMode;
            activeCenterFrequency = settings.centerFrequency;
            activeListeningFrequency = settings.listeningFrequency;
            activeSampleRate = settings.sampleRate;
            activeBandwidth = settings.bandwidth;
            activeAudioLowPassHz = settings.audioLowPassHz;
            activeAudioHighPassHz = settings.audioHighPassHz;
            activeSimplifiedAudioChannelizer = settings.simplifiedAudioChannelizer;
            activeHfAudioBlankerEnabled = settings.hfAudioBlankerEnabled;
            activeHfAudioBlankerThreshold = settings.hfAudioBlankerThreshold;
            activeDmrChannelSampleRate = settings.dmrChannelSampleRate;
            activeDmrBasebandSampleRate =
                normalizedDmrBasebandSampleRate(settings.dmrBasebandSampleRate);
            resetDemodulatorState();
        }

        if (activeAudioFilterChainJson != settings.audioFilterChainJson) {
            activeAudioFilterChainJson = settings.audioFilterChainJson;
            audioFilterChain.configure(activeAudioFilterChainJson, AUDIO_OUTPUT_RATE);
        }

        std::uint64_t iqBlockSequence = 0;
        if (!IqBuffer::popBlock(iqBlock, &iqBlockSequence) || settings.sampleRate <= 0.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (previousIqSequence != 0 && iqBlockSequence > previousIqSequence + 1) {
            healthSequenceGaps += iqBlockSequence - previousIqSequence - 1;
            if (settings.modulationType == MOD_DMR) resetDemodulatorState();
        }
        previousIqSequence = iqBlockSequence;

        const auto demodStart = healthLogging ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        int dmrBasebandSampleRate = 0;
        processDemodulatorBlock(iqBlock,
                                audioSamples,
                                dmrBasebandSamples,
                                dmrBasebandSampleRate,
                                settings);
        const auto demodEnd = healthLogging ? std::chrono::steady_clock::now() : demodStart;
        const double demodMs = std::chrono::duration<double, std::milli>(demodEnd - demodStart).count();
        if (healthLogging) {
            ++healthBlocks;
            healthIqSamples += static_cast<std::uint64_t>(iqBlock.size() / 2U);
            healthAudioSamples += static_cast<std::uint64_t>(
                settings.modulationType == MOD_DMR ? dmrBasebandSamples.size() : audioSamples.size());
            healthDemodMs += demodMs;
            healthMaxDemodMs = (std::max)(healthMaxDemodMs, demodMs);

            const auto healthNow = std::chrono::steady_clock::now();
            const double healthElapsedMs =
                std::chrono::duration<double, std::milli>(healthNow - healthWindowStart).count();
            if (healthElapsedMs >= 1000.0) {
                const IqBuffer::Stats iqStats = IqBuffer::stats();
                const std::uint64_t droppedBlocks =
                    iqStats.droppedQueuedBlocks >= previousDroppedBlocks
                        ? iqStats.droppedQueuedBlocks - previousDroppedBlocks
                        : iqStats.droppedQueuedBlocks;
                const double elapsedSeconds = healthElapsedMs / 1000.0;
                const double producedRatio =
                    static_cast<double>(healthAudioSamples) /
                        ((std::max)(0.001, elapsedSeconds) *
                         (settings.modulationType == MOD_DMR ? activeDmrBasebandSampleRate : AUDIO_OUTPUT_RATE));
                const double queuedMs = settings.sampleRate > 0.0
                                            ? static_cast<double>(iqStats.queuedFloatCount) * 500.0 /
                                                  settings.sampleRate
                                            : 0.0;
                if (fobosVerboseLoggingEnabled()) {
                    qDebug() << "[Audio health]"
                               << "channelizer"
                               << (settings.modulationType == MOD_DMR ?
                                   (dmrLastLoggedDecimationFactor <= DmrCicDecimator::MaxFixedPointFactor ?
                                        "dmr-cic5" : "dmr-box5") :
                                   (settings.modulationType == MOD_SSTV &&
                                    sstvDemodulationModulationType(settings.sstvDemodulationMode) == MOD_NFM
                                        ? "sstv-nfm-multistage"
                                        : (settings.simplifiedAudioChannelizer
                                               ? "simplified"
                                               : "multistage")))
                               << "realtimeRatio" << producedRatio
                               << "configuredRate" << settings.sampleRate
                               << "estimatedRate" << iqStats.sampleRateEstimate
                               << "blocks" << static_cast<qulonglong>(healthBlocks)
                               << "iqMs" << (settings.sampleRate > 0.0
                                                 ? static_cast<double>(healthIqSamples) * 1000.0 /
                                                       settings.sampleRate
                                                 : 0.0)
                               << "pcmMs" << static_cast<double>(healthAudioSamples) * 1000.0 /
                                                 (settings.modulationType == MOD_DMR ? activeDmrBasebandSampleRate : AUDIO_OUTPUT_RATE)
                               << "demodMs" << healthDemodMs
                               << "maxDemodMs" << healthMaxDemodMs
                               << "queuedMs" << queuedMs
                               << "sequenceGaps" << static_cast<qulonglong>(healthSequenceGaps)
                               << "droppedBlocks" << static_cast<qulonglong>(droppedBlocks);
                }
                healthWindowStart = healthNow;
                healthSequenceGaps = 0;
                healthBlocks = 0;
                healthIqSamples = 0;
                healthAudioSamples = 0;
                healthDemodMs = 0.0;
                healthMaxDemodMs = 0.0;
                previousDroppedBlocks = iqStats.droppedQueuedBlocks;
            }
        }
        if (!dmrBasebandSamples.empty() && dmrBasebandSampleRate > 0) {
            dmrPcmPending.append(
                reinterpret_cast<const char *>(dmrBasebandSamples.data()),
                static_cast<int>(dmrBasebandSamples.size() * sizeof(short)));
            const int packetBytes = dmrBasebandSampleRate / 50 * int(sizeof(short));
            while (dmrPcmPending.size() >= packetBytes) {
                emit dmrBasebandFrameReady(dmrPcmPending.left(packetBytes), dmrBasebandSampleRate);
                dmrPcmPending.remove(0, packetBytes);
            }
        }
        if (audioSamples.empty()) {
            continue;
        }

        if (fobosVerboseLoggingEnabled() &&
            (audioBlockCounter < 5 || (audioBlockCounter % 100) == 0)) {
            qDebug() << "[Audio] demod block"
                     << "blockCounter" << audioBlockCounter
                     << "iqSequence" << static_cast<qulonglong>(iqBlockSequence)
                     << "iqFloats" << iqBlock.size()
                     << "configuredRate" << settings.sampleRate
                     << "estimatedRate" << IqBuffer::sampleRateEstimate()
                     << "audioSamples" << audioSamples.size()
                     << "dmrBasebandSamples" << dmrBasebandSamples.size()
                     << "dmrBasebandRate" << dmrBasebandSampleRate
                     << "demodMs" << demodMs;
        }
        ++audioBlockCounter;

        const QByteArray demodPcmFrame(reinterpret_cast<const char *>(audioSamples.data()),
                                       static_cast<int>(audioSamples.size() * sizeof(short)));
        emit demodulatorFrameReady(demodPcmFrame);

        if (settings.modulationType == MOD_DMR) {
            continue;
        }

        audioFilterChain.process(audioSamples);

        {
            std::lock_guard<std::mutex> lock(audioMutex);
            const size_t queuedSamples = queuedAudioSamples();
            const size_t overflow = queuedSamples + audioSamples.size() > MAX_AUDIO_BUFFER_SAMPLES
                                        ? queuedSamples + audioSamples.size() - MAX_AUDIO_BUFFER_SAMPLES
                                        : 0;
            if (overflow > 0) {
                discardAudioSamples(overflow);
            }
            audioBuffer.insert(audioBuffer.end(), audioSamples.begin(), audioSamples.end());
            compactAudioBufferIfNeeded();
        }
        cv.notify_one();
    }

    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "SDRThread finished";
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
void CALLBACK AudioProcessor::WaveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance,
                                              DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WOM_DONE) {
        AudioProcessor* self = reinterpret_cast<AudioProcessor*>(dwInstance);
        if (!self || self->audioDeviceClosing.load()) {
            return;
        }
        WAVEHDR* hdr = reinterpret_cast<WAVEHDR*>(dwParam1);

        for (int i = 0; i < NUM_BUFFERS; ++i) {
            if (hdr == &self->waveHdrs[i]) {
                self->bufferReady[i] = true;
                self->cv.notify_one();
                break;
            }
        }
    }
}
#endif



void AudioProcessor::startAudioOutput() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    bool playbackPrimed = false;
    bool deviceFramesQueued = false;
    std::uint64_t rebufferEvents = 0;
    std::uint64_t outputFrames = 0;
    std::uint64_t writeErrors = 0;
    auto outputHealthStart = std::chrono::steady_clock::now();
    auto lastWriteErrorLog = outputHealthStart - std::chrono::seconds(1);
    while (running.load()) {
        std::unique_lock<std::mutex> lock(audioMutex);
        // waveOut notifies without taking audioMutex. A bounded wait also
        // covers a callback arriving between the predicate and the wait.
        const bool ready = cv.wait_for(lock, std::chrono::milliseconds(10), [&] {
            if (!running.load()) {
                return true;
            }
            bool freeBuffer = false;
            bool inFlight = false;
            for (int i = 0; i < NUM_BUFFERS; ++i) {
                if (bufferReady[i].load()) freeBuffer = true;
                else inFlight = true;
            }
            if (deviceFramesQueued && !inFlight) {
                playbackPrimed = false;
                deviceFramesQueued = false;
                ++rebufferEvents;
            }
            if (!freeBuffer) return false;
            const size_t queuedSamples = queuedAudioSamples();
            if (!playbackPrimed) {
                if (queuedSamples < AUDIO_START_BUFFER_SAMPLES) return false;
                playbackPrimed = true;
            }
            return queuedSamples >= static_cast<size_t>(BUFFER_SIZE);
        });

        if (!running.load()) break;
        const auto now = std::chrono::steady_clock::now();
        if (fobosVerboseLoggingEnabled() && now - outputHealthStart >= std::chrono::seconds(1)) {
            const auto queuedSamples = queuedAudioSamples();
            int inFlightBuffers = 0;
            for (int i = 0; i < NUM_BUFFERS; ++i) if (!bufferReady[i].load()) ++inFlightBuffers;
            lock.unlock();
            qDebug() << "[Audio output health]"
                     << "pcmQueuedMs" << queuedSamples * 1000.0 / AUDIO_OUTPUT_RATE
                     << "inFlightBuffers" << inFlightBuffers
                     << "primed" << playbackPrimed
                     << "rebufferEvents" << static_cast<qulonglong>(rebufferEvents)
                     << "outputFrames" << static_cast<qulonglong>(outputFrames)
                     << "writeErrors" << static_cast<qulonglong>(writeErrors);
            lock.lock();
            outputHealthStart = now;
            outputFrames = 0;
        }
        if (!ready || !running.load()) continue;
        for (int i = 0; i < NUM_BUFFERS; ++i) {
            if (!bufferReady[i]) continue;

            const size_t queuedBeforeCopy = queuedAudioSamples();
            if (queuedBeforeCopy < static_cast<size_t>(BUFFER_SIZE)) {
                continue;
            }

            QByteArray pcmFrame;
            const size_t samplesToCopy = (std::min)(queuedBeforeCopy, static_cast<size_t>(BUFFER_SIZE));
            if (samplesToCopy > 0) {
                const float volume = outputVolume.load();

                for (size_t n = 0; n < samplesToCopy; ++n) {
                    const short in = audioBuffer[audioBufferReadOffset + n];

                    int sample = static_cast<int>(in * volume);
                    sample = std::clamp(sample, -32768, 32767);

                    waveBuffers[i][n] = static_cast<short>(sample);
                }

                discardAudioSamples(samplesToCopy);
            }

            if (samplesToCopy < static_cast<size_t>(BUFFER_SIZE)) {
                std::fill(waveBuffers[i].begin() + samplesToCopy, waveBuffers[i].end(), 0);
            }

            pcmFrame = QByteArray(reinterpret_cast<const char *>(waveBuffers[i].data()),
                                  static_cast<int>(waveBuffers[i].size() * sizeof(short)));

            bufferReady[i] = false;
            lock.unlock();

            emit audioFrameReady(pcmFrame);
            ++outputFrames;

#ifdef _WIN32
            MMRESULT result = MMSYSERR_NOERROR;
            bool submitted = false;
            {
                std::lock_guard<std::mutex> waveLock(waveOutMutex);
                if (hWaveOut && !audioDeviceClosing.load()) {
                    result = waveOutWrite(hWaveOut, &waveHdrs[i], sizeof(WAVEHDR));
                    submitted = result == MMSYSERR_NOERROR;
                }
            }
            if (submitted) deviceFramesQueued = true;
            else bufferReady[i] = true;
            if (result != MMSYSERR_NOERROR) {
                ++writeErrors;
                const auto errorTime = std::chrono::steady_clock::now();
                if (errorTime - lastWriteErrorLog >= std::chrono::seconds(1)) {
                    qWarning() << "[Audio] waveOutWrite failed" << result;
                    lastWriteErrorLog = errorTime;
                }
            }
#else
            bufferReady[i] = true;
#endif
            lock.lock();

        }
    }
}
