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
    bool setOutputEnabled(bool enabled, QString *error) override;
    bool drain(int timeoutMs, QString *error) override;
    void stop() override;
    bool isRunning() const override;

    bool streamStarted() const;
    quint64 outputSamples() const;
    quint64 sourceUnderrunSamples() const;
    int bufferedSourceSamples() const;
    const TxConfiguration &configuration() const;

private:
    static int transferCallback(HackRfTransfer *transfer);
    int fillTransfer(HackRfTransfer *transfer);
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
};

#endif // HACKRFTRANSMITTERBACKEND_H
