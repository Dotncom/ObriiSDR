#ifndef AUDIOPROCESSOR_H
#define AUDIOPROCESSOR_H

#define NUM_BUFFERS 3
#define BUFFER_SIZE 4800  // 100 ms at 48 kHz

#include <QTimer>
#include <QObject>
#include <QMutex>
#include <QByteArray>
#include <QWaitCondition>
#include <QThread>
#include <QDebug>
#include <complex>
#include <vector>
#include <array>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cmath>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <string>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif
#include "radiosettings.h"
#include "audiofilterchain.h"
#include "iqchannelizer.h"
#include "dmrcicdecimator.h"

class AudioProcessor : public QObject {
    Q_OBJECT

public:
    explicit AudioProcessor(QObject *parent = nullptr);
    ~AudioProcessor();
    void configure(const RadioSettings &settings);
    void resetHfNoiseCancelState();
    void setAudioDevice(int deviceID);
    void setLocalPlaybackEnabled(bool enabled);
    void setDmrChannelIqEnabled(bool enabled) { dmrChannelIqEnabled.store(enabled); }
    void stopDemodulationForStreamRestart();
    void startAudioOutput();
    void processAudioBuffer();
public:
    void setVolume(float volume);


public slots:
    void startDemodulation();
    void stopDemodulation();
    void enqueueExternalPcm(const QByteArray &pcmData);
    void clearExternalPcm();

signals:
    void audioFrameReady(const QByteArray &pcmData);
    void demodulatorFrameReady(const QByteArray &pcmData);
    void dmrBasebandFrameReady(const QByteArray &pcmData, int sampleRate);
    void dmrChannelIqFrameReady(const QByteArray &iqData, double sampleRate, int sampleCount);

private:
    friend struct AudioProcessorTestAccess;
    static constexpr int HF_NOISE_CANCEL_TAP_COUNT = 9;

    void SDRThread();
    bool openAudioDevice(int deviceID);
    void closeAudioDevice();
    void pauseAudioDevice();
    void prepareWaveHeaders();
    void unprepareWaveHeaders();
    void joinWorkerThreads();
    void stopDemodulationImpl(bool closeOutputDevice);
    void resetDemodulatorState();
    RadioSettings currentSettingsSnapshot() const;
    void processDemodulatorBlock(const std::vector<float>& iqBlock,
                                 std::vector<short>& audioSamples,
                                 std::vector<short>& dmrBasebandSamples,
                                 int &dmrBasebandSampleRate,
                                 const RadioSettings &settings);
    void processDmrIqDemodulatorBlock(const std::vector<float>& iqBlock,
                                       std::vector<short>& dmrBasebandSamples,
                                       int &dmrBasebandSampleRate,
                                       const RadioSettings &settings);
    size_t queuedAudioSamples() const;
    void discardAudioSamples(size_t count);
    void compactAudioBufferIfNeeded();
    std::vector<short> waveBuffers[NUM_BUFFERS];
    std::vector<short> audioBuffer;
    size_t audioBufferReadOffset = 0;
    bool externalAudioPrimed = false;
#ifdef _WIN32
    WAVEHDR waveHdrs[NUM_BUFFERS];
#endif
    std::atomic<bool> bufferReady[NUM_BUFFERS];
    std::atomic<int> currentBufferIndex = 0;
    std::atomic<float> outputVolume = 1.0f;
    std::atomic<bool> hfNoiseCancelResetRequested = false;
    std::atomic<bool> demodulatorResetRequested = false;
    std::condition_variable cv;
    std::mutex audioMutex;
#ifdef _WIN32
    std::mutex waveOutMutex;
#endif
    mutable std::mutex settingsMutex;
    std::thread sdrWorker;
    std::thread audioWorker;
    QThread *workerThread = nullptr;
    QMutex mutex;
    QWaitCondition condition;
    QTimer *update1Timer;

#ifdef _WIN32
    WAVEFORMATEX format;
    HWAVEOUT hWaveOut = nullptr;
#endif
    bool waveHeadersPrepared = false;
    int openedAudioDeviceID = -1;
    int selectedAudioDeviceID = 0;
    std::atomic<bool> audioDeviceClosing = false;
    std::atomic<bool> localPlaybackEnabled = true;
    RadioSettings audioSettings;
    double ncoPhase = 0.0;
    double audioResamplePhase = 0.0;
    std::complex<float> amLowPassState = {0.0f, 0.0f};
    std::array<std::complex<float>, 3> dmrChannelPreLowPassStates = {};
    std::array<std::complex<float>, 4> sidebandLowPassStates = {};
    std::complex<float> channelDecimationSum = {0.0f, 0.0f};
    int channelDecimationCount = 0;
    std::complex<float> fmPreviousSample = {1.0f, 0.0f};
    bool fmPreviousValid = false;
    float fmDeemphasisState = 0.0f;
    float demodAudioLowPassState = 0.0f;
    float demodAudioLowPassState2 = 0.0f;
    float demodAudioLowPassState3 = 0.0f;
    float demodAudioHighPassState = 0.0f;
    AudioFilterChain audioFilterChain;
    IqChannelizer audioIqChannelizer;
    std::vector<float> audioChannelizedIq;
    double samCarrierPhase = 0.0;
    double samCarrierFrequency = 0.0;
    double sidebandFilterPhase = 0.0;
    double cwTonePhase = 0.0;
    float amDcEstimate = 0.0f;
    float amAgcLevel = 0.05f;
    double dmrResamplePhase = 0.0;
    DmrCicDecimator dmrCicDecimator;
    std::atomic<bool> dmrChannelIqEnabled{false};
    float dmrRecordingAgcLevel = 0.01f;
    QByteArray dmrPcmPending;
    std::complex<float> dmrLowPassState = {0.0f, 0.0f};
    std::complex<float> dmrPreviousSample = {1.0f, 0.0f};
    bool dmrPreviousValid = false;
    float dmrDiscriminatorDc = 0.0f;
    float dmrFskLowPassState = 0.0f;
    float dmrFskLowPassState2 = 0.0f;
    float dmrPreviousOutputSample = 0.0f;
    bool dmrPreviousOutputValid = false;
    int dmrLastLoggedOutputRate = 0;
    int dmrLastLoggedDecimationFactor = 0;
    double dmrLastLoggedChannelRate = 0.0;
    std::complex<float> hfNoiseCancelCoeff = {0.0f, 0.0f};
    std::complex<float> hfNoiseCancelRefDecimationSum = {0.0f, 0.0f};
    std::array<std::complex<float>, HF_NOISE_CANCEL_TAP_COUNT> hfNoiseCancelTapCoeffs = {};
    std::array<std::complex<float>, HF_NOISE_CANCEL_TAP_COUNT> hfNoiseCancelRefHistory = {};
    int hfNoiseCancelRefHistoryIndex = 0;
    float hfAudioBlankerEnvelope = 0.0f;
    int hfAudioBlankerHoldSamples = 0;
    std::complex<float> hfAudioBlankerLastCleanSample = {0.0f, 0.0f};

	QByteArray byteArray;
    QString audioDeviceName;

    std::atomic<bool> running = false;

    //QUdpSocket *udpSocket;
    //QString serverIp;
    //quint16 serverPort;
    //UdpSender *udpSender;


#ifdef _WIN32
    static void CALLBACK WaveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance,
                                         DWORD_PTR dwParam1, DWORD_PTR dwParam2);
#endif

};

#endif // AUDIOPROCESSOR_H
