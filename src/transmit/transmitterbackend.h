#ifndef TRANSMITTERBACKEND_H
#define TRANSMITTERBACKEND_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include <complex>
#include <cstdint>

enum class TxModulation {
    Am,
    Nfm,
    Wfm,
    Dsb,
    Usb,
    Lsb,
    Cw,
    Ft8,
    Ook,
    Ask,
    Fsk,
    Gfsk,
    Gmsk,
    Bpsk,
    Qpsk,
    Rtty,
    Dmr
};

struct TxConfiguration {
    TxModulation modulation = TxModulation::Nfm;
    double frequencyHz = 100000000.0;
    int sampleRate = 48000;
    float level = 0.50f;
    double deviationHz = 2500.0;
    double signalBandwidthHz = 0.0;
    double toneHz = 700.0;
    int cwWpm = 18;
    double symbolRate = 1200.0;
    double gaussianBt = 0.5;
    float amDepth = 0.90f;
    float askDepth = 0.70f;
    bool invertData = false;
    bool alignFt8ToUtcSlot = true;
    int ft8SlotParity = -1;
    int dmrColorCode = 1;
    int dmrTimeslot = 1;
    std::uint32_t dmrSourceId = 1;
    std::uint32_t dmrDestinationId = 9;
    bool dmrPrivateCall = false;
    int dmrPrivacyMode = 0;
    int dmrPrivacyKeyId = 0;
    QByteArray dmrPrivacyKey;
    int deviceIndex = 0;
    int deviceSampleRate = 2000000;
    std::uint32_t bandwidthHz = 0;
    int txVgaGainDb = 0;
    bool rfAmpEnabled = false;
    // Zero disables the firmware shortfall stop. The host-side backend still
    // reports missing source samples and the transmit dialog has its own
    // watchdog, while a finite firmware limit can stop TX after one USB
    // transfer on a busy controller shared with a receiver.
    std::uint32_t txUnderrunLimit = 0;
    bool liveSource = false;
    int prebufferMilliseconds = 200;
};

class TransmitterBackend {
public:
    virtual ~TransmitterBackend() = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual bool isRfCapable() const = 0;
    virtual bool start(const TxConfiguration &configuration, QString *error) = 0;
    virtual bool writeIq(const std::complex<float> *samples, int count, QString *error) = 0;
    virtual bool setOutputEnabled(bool enabled, QString *error) = 0;
    virtual bool drain(int timeoutMs, QString *error) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
};

class SimulatorTransmitterBackend final : public TransmitterBackend {
public:
    QString id() const override;
    QString displayName() const override;
    bool isRfCapable() const override;
    bool start(const TxConfiguration &configuration, QString *error) override;
    bool writeIq(const std::complex<float> *samples, int count, QString *error) override;
    bool setOutputEnabled(bool enabled, QString *error) override;
    bool drain(int timeoutMs, QString *error) override;
    void stop() override;
    bool isRunning() const override;

    const QVector<std::complex<float>> &capturedIq() const;
    const TxConfiguration &configuration() const;
    void clear();

private:
    TxConfiguration currentConfiguration;
    QVector<std::complex<float>> iq;
    bool running = false;
    bool outputEnabled = true;
};

#endif // TRANSMITTERBACKEND_H
