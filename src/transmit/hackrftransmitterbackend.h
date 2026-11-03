#ifndef HACKRFTRANSMITTERBACKEND_H
#define HACKRFTRANSMITTERBACKEND_H

#include "transmitterbackend.h"

#include <QMutex>
#include <QVector>

#include <atomic>
#include <complex>
#include <cstdint>

struct HackRfTransfer;

class HackRfTransmitterBackend final : public TransmitterBackend {
public:
    HackRfTransmitterBackend();
    ~HackRfTransmitterBackend() override;

    QString id() const override;
    QString displayName() const override;
    bool isRfCapable() const override;
    bool start(const TxConfiguration &configuration, QString *error) override;
    bool writeIq(const std::complex<float> *samples, int count, QString *error) override;
    bool writeFiniteBurst(const std::complex<float> *samples, int count, QString *error);
    bool finiteBurstComplete() const;
    bool finiteBurstSucceeded() const;
    bool setOutputEnabled(bool enabled, QString *error) override;
    bool drain(int timeoutMs, QString *error) override;
    void stop() override;
    bool isRunning() const override;

    bool streamStarted() const;
    bool hardwareStreamAlive() const;
    quint64 outputSamples() const;
    quint64 sourceUnderrunSamples() const;
    int bufferedSourceSamples() const;
    const TxConfiguration &configuration() const;

private:
    friend struct HackRfFiniteBurstTestAccess;
    enum class FiniteState { Inactive, Pending, Succeeded, Failed };
    static int transferCallback(HackRfTransfer *transfer);
    static void blockCompleteCallback(HackRfTransfer *transfer, int success);
    static void flushCallback(void *context, int success);
    int fillTransfer(HackRfTransfer *transfer);
    bool stageFiniteBurst(const std::complex<float> *samples, int count, QString *error);
    bool beginStreaming(QString *error);
    void closeDevice();

    mutable QMutex queueMutex;
    QVector<std::complex<float>> sourceQueue;
    double sourcePosition = 0.0;
    TxConfiguration currentConfiguration;
    void *device = nullptr;
    std::atomic_bool running{false};
    std::atomic_bool streaming{false};
    std::atomic_bool stopping{false};
    std::atomic_bool outputEnabled{true};
    std::atomic<quint64> generatedSamples{0};
    std::atomic<quint64> missingSourceSamples{0};
    std::atomic<int> bufferedSamples{0};
    std::atomic<FiniteState> finiteState{FiniteState::Inactive};
    std::atomic_bool finiteInputDrained{false};
    std::atomic_bool finiteEosReturned{false};
};

#endif // HACKRFTRANSMITTERBACKEND_H
