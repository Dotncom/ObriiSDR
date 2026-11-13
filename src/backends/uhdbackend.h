#ifndef UHD_BACKEND_H
#define UHD_BACKEND_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <complex>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

QStringList enumerateUhdDeviceArgs(QString *errorMessage = nullptr);

struct UhdRxChannelSettings {
    int channel = 0;
    double gainDb = 30.0;
    QString antenna;
};

struct UhdRxTimestamp {
    std::int64_t seconds = 0;
    double fractionalSeconds = 0.0;
};

class UhdTxSession {
public:
    ~UhdTxSession();
    bool open(int deviceIndex, int channel, double sampleRateHz, double centerHz,
              double gainDb, double bandwidthHz, const QString &antenna,
              const QString &fpgaImagePath, QString *errorMessage);
    bool send(const std::complex<float> *iq, std::size_t count, bool startOfBurst,
              QString *errorMessage, double timeoutSeconds = 1.0);
    bool finish(QString *errorMessage);
    void close();

private:
    friend class UhdRxSession;
    bool setRate(int channel, double sampleRateHz, QString *errorMessage, bool dual = false);
    bool openStreamer(QString *errorMessage);
    bool tune(std::size_t channel, double centerHz, double gainDb, double bandwidthHz,
              const QString &antenna, QString *errorMessage);
    bool sendBuffers(const std::array<const std::complex<float> *, 2> &iq, std::size_t count,
                     bool startOfBurst, QString *errorMessage, double timeoutSeconds);
    void *device_ = nullptr;
    void *streamer_ = nullptr;
    std::array<std::size_t, 2> channels_{};
    std::size_t channelCount_ = 0;
    bool ownsDevice_ = false;
};

struct UhdDuplexTxStatus {
    bool active = false;
    std::uint64_t samples = 0;
    QString state = QStringLiteral("idle");
    QString error;
    int targetChannel = -1;
};

struct UhdClockStatus {
    QString source;
    std::optional<bool> locked;
};

struct UhdGnssStatus {
    QString state = QStringLiteral("idle");
    QString gga;
    QString rmc;
    QString gsv;
    QString ubxHex;
};

QString validatedUhdGnssUbx(const QByteArray &payload);

struct UhdGnssSatellite {
    QString talker;
    int id = -1;
    int elevationDeg = -1;
    int azimuthDeg = -1;
    int cn0DbHz = -1;
};

struct UhdGnssSkySummary {
    int satellitesInView = -1;
    int satellitesWithSignal = 0;
    int strongestCn0DbHz = -1;
    bool incomplete = false;
    std::vector<UhdGnssSatellite> satellites;
};

UhdGnssSkySummary summarizeUhdGnssGsv(const QString &gsv);

bool uhdClockControlAvailable();

class UhdRxSession {
public:
    static constexpr double DuplexClockHz = 30.72e6;
    static constexpr double DuplexTxRateHz = 1.92e6;
    UhdRxSession() = default;
    UhdRxSession(const UhdRxSession &) = delete;
    UhdRxSession &operator=(const UhdRxSession &) = delete;
    ~UhdRxSession();
    bool open(int deviceIndex, int channel, double sampleRateHz, double centerHz,
              double gainDb, const QString &antenna, const QString &fpgaImagePath,
              QString *errorMessage, const QString &clockSource = {}, int gnssBaud = 0);
    bool open(const QString &deviceAddress, int channel, double sampleRateHz, double centerHz,
              double gainDb, const QString &antenna, const QString &fpgaImagePath,
              QString *errorMessage, const QString &clockSource = {}, int gnssBaud = 0);
    bool open(int deviceIndex, const std::vector<UhdRxChannelSettings> &channels,
              double sampleRateHz, double centerHz, const QString &fpgaImagePath,
              QString *errorMessage, int duplexTxChannel = -1, const QString &clockSource = {},
              int gnssBaud = 0);
    bool open(const QString &deviceAddress, const std::vector<UhdRxChannelSettings> &channels,
              double sampleRateHz, double centerHz, const QString &fpgaImagePath,
              QString *errorMessage, int duplexTxChannel = -1, const QString &clockSource = {},
              int gnssBaud = 0);
    UhdClockStatus clockStatus() const;
    // The RX owner requests and reads cached data. Sensor I/O runs off the IQ read path.
    void requestGnssStatus();
    UhdGnssStatus gnssStatus() const { return gnssStatus_; }
    // A target of -1 uses the default channel. In 2+2 mode, 2 selects both TX channels.
    bool startTransmitTone(double frequencyHz, double gainDb, int durationMs, QString *errorMessage,
                           int toneChannel = -1);
    void requestTransmitStop() { cancelTransmit_.store(true, std::memory_order_relaxed); }
    UhdDuplexTxStatus transmitStatus() const;
    // Both channels use the same center when sharing one device and RX streamer.
    bool retune(double centerHz, QString *errorMessage);
    void beginTimestampedTuneSettling();
    std::optional<std::size_t> samplesBeforeTuneCutoff(std::size_t received, double sampleRateHz);
    int receive(float *interleavedIq, std::size_t capacity, QString *errorMessage);
    // Buffer order follows the channel list passed to open. The result is the
    // same number of complex samples in each buffer.
    int receive(float *firstInterleavedIq, float *secondInterleavedIq,
                std::size_t capacityPerChannel, QString *errorMessage);
    std::optional<UhdRxTimestamp> lastReceiveTimestamp() const;
    // Cumulative across open/close for the lifetime of this session object.
    std::uint64_t receiveOverflows() const { return receiveOverflows_.load(std::memory_order_relaxed); }
    std::uint64_t receiveTimeouts() const { return receiveTimeouts_.load(std::memory_order_relaxed); }
    std::uint64_t receiveTimestampGaps() const { return receiveTimestampGaps_.load(std::memory_order_relaxed); }
    std::uint64_t receiveMissingSamples() const { return receiveMissingSamples_.load(std::memory_order_relaxed); }
    std::uint64_t receiveTimestampMissingBlocks() const {
        return receiveTimestampMissingBlocks_.load(std::memory_order_relaxed);
    }
    void close();

private:
    void stopTransmit();
    UhdGnssStatus readGnssSensors();
    int receiveBuffers(void **buffers, std::size_t bufferCount, std::size_t capacity,
                       QString *errorMessage);
    void accountReceiveTimestamp(std::size_t received);
    QString deviceAddress_;
    void *device_ = nullptr;
    void *streamer_ = nullptr;
    void *metadata_ = nullptr;
    std::array<std::size_t, 2> channels_{};
    std::size_t channelCount_ = 0;
    double centerHz_ = 0.0;
    double receiveSampleRateHz_ = 0.0;
    std::optional<UhdRxTimestamp> previousReceiveTimestamp_;
    std::size_t previousReceiveSamples_ = 0;
    std::atomic<std::uint64_t> receiveOverflows_{0};
    std::atomic<std::uint64_t> receiveTimeouts_{0};
    std::atomic<std::uint64_t> receiveTimestampGaps_{0};
    std::atomic<std::uint64_t> receiveMissingSamples_{0};
    std::atomic<std::uint64_t> receiveTimestampMissingBlocks_{0};
    bool streamStarted_ = false;
    bool lastReceiveHasSamples_ = false;
    bool tuneCutoffPending_ = false;
    std::int64_t tuneCutoffSeconds_ = 0;
    double tuneCutoffFraction_ = 0.0;
    UhdTxSession sharedTx_;
    int duplexTxChannel_ = -1;
    std::thread transmitThread_;
    std::atomic<bool> cancelTransmit_{false};
    mutable std::mutex transmitStatusMutex_;
    UhdDuplexTxStatus transmitStatus_;
    mutable std::mutex deviceControlMutex_;
    mutable UhdClockStatus lastClockStatus_;
    std::future<UhdGnssStatus> gnssJob_;
    std::chrono::steady_clock::time_point gnssLastPoll_{};
    UhdGnssStatus gnssStatus_;
    bool gnssSensorsDiscovered_ = false;
    const char *gnssGgaSensor_ = nullptr;
    const char *gnssRmcSensor_ = nullptr;
    const char *gnssGsvSensor_ = nullptr;
    const char *gnssUbxSensor_ = nullptr;
};

#endif // UHD_BACKEND_H
