#ifndef RECEIVERBACKEND_H
#define RECEIVERBACKEND_H

#include "radiosettings.h"

#include <QByteArray>
#include <QFlags>
#include <QList>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>

enum class ReceiverBackendFeature {
    HardwareBandwidth = 0,
    DirectSamplingHf1,
    DirectSamplingHf2,
    DirectSamplingHf1PlusHf2,
    GpoControl,
    FastRetune,
    AgileScan,
    BiasTee,
    PpmCorrection
};

Q_DECLARE_FLAGS(ReceiverBackendFeatures, ReceiverBackendFeature)
Q_DECLARE_OPERATORS_FOR_FLAGS(ReceiverBackendFeatures)

enum class ReceiverBackendStreamKind {
    FobosStandard = 0,
    FobosAgile,
    RtlSdrNative,
    RtlTcp,
    SoapySdr,
    UhdNative,
    BladeRfNative,
    HackRfNative,
    VirtualSignal
};

struct ReceiverBackendDeviceInfo {
    QString backendId;
    QString backendName;
    int nativeIndex = 0;
    QString label;
    QString serial;
    QString hardwareRevision;
    QString firmwareVersion;
    QString product;
    QString manufacturer;
};

struct ReceiverBackendCapabilities {
    ReceiverBackendFeatures features;
    QVector<double> sampleRatesHz;
    double minFrequencyHz = 0.0;
    double maxFrequencyHz = 0.0;
};

struct ReceiverIqFrame {
    const float *floatIq = nullptr;
    int floatCount = 0;
    QByteArray packedIq;
    double centerFrequencyHz = 0.0;
    double actualFrequencyHz = 0.0;
    double sampleRateHz = 0.0;
    quint64 sequence = 0;
    qint64 hostTimestampNs = 0;
    QString backendId;
    QString sampleFormat;
};

struct ReceiverStreamDescriptor {
    ReceiverBackendStreamKind kind = ReceiverBackendStreamKind::FobosStandard;
    void *nativeDevice = nullptr;
    FobosApiKind fobosApiKind = FobosApiKind::Standard;
    QString backendId = QStringLiteral("fobos");
    QString backendName = QStringLiteral("Fobos SDR");
    double sampleRateHz = 0.0;
    double centerFrequencyHz = 0.0;
    double frequencyCalibrationOffsetHz = 0.0;
    QString clockId;
    bool externalClock = false;
    bool clockLocked = false;
    bool hardwareTimestampAvailable = false;
    QString rtlTcpHost = QStringLiteral("127.0.0.1");
    quint16 rtlTcpPort = 1234;
    int rtlSdrNativeDeviceIndex = 0;
    int soapySdrDeviceIndex = 0;
    int uhdNativeDeviceIndex = 0;
    int uhdRxChannel = 0;
    bool uhdDuplexEnabled = false;
    QString uhdClockSource;
    int uhdGnssBaud = 0;
    double uhdRxGainDb = 30.0;
    QString uhdRxAntenna;
    QString uhdFpgaImagePath;
    int bladeRfNativeDeviceIndex = 0;
    int hackRfNativeDeviceIndex = 0;
    int hackRfLnaGainDb = 16;
    int hackRfVgaGainDb = 20;
    bool hackRfAmpEnabled = false;
    bool hackRfBiasTeeEnabled = false;
    bool hackRfAutomaticBandwidth = true;
    std::uint32_t hackRfBandwidthHz = 0;
    bool hackRfExplicitTuningEnabled = false;
    std::uint64_t hackRfExplicitIfHz = 2400000000ULL;
    std::uint64_t hackRfExplicitLoHz = 1000000000ULL;
    int hackRfExplicitPath = 0;
    bool hackRfClockOutEnabled = false;
    bool hackRfHardwareSyncEnabled = false;
    std::uint32_t hackRfRxOverrunLimit = 0;
    bool hackRfOperaCakeEnabled = false;
    int hackRfOperaCakeAddress = 0;
    int hackRfOperaCakeMode = 0;
    int hackRfOperaCakePortA = 0;
    int hackRfOperaCakePortB = 4;
    QString hackRfOperaCakeRangesJson = QStringLiteral("[]");
    QString hackRfOperaCakeDwellsJson = QStringLiteral("[]");
    bool virtualSignalUseTcp = false;
    QString virtualSignalLocalName = QStringLiteral("obrii-virtual-iq");
    QString virtualSignalListenAddress = QStringLiteral("127.0.0.1");
    quint16 virtualSignalPort = 17661;
    bool virtualSignalFollowMetadata = true;
    int rtlTcpTunerGainTenthsDb = -1;
    bool rtlTcpAgc = true;
    bool syncReader = false;
    bool queueAudioBlocks = false;
    bool publishIqSnapshot = true;
    bool emitIqFrames = false;
    bool agileScanEnabled = false;
    QVector<double> agileScanFrequenciesHz;
};

using ReceiverIqCallback = std::function<void(const ReceiverIqFrame &)>;

class ReceiverBackend {
public:
    virtual ~ReceiverBackend() = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual QList<ReceiverBackendDeviceInfo> enumerate() = 0;
    virtual ReceiverBackendCapabilities capabilities() const = 0;

    virtual bool open(int nativeIndex, QString *errorMessage = nullptr) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual bool applySettings(const RadioSettings &settings, QString *errorMessage = nullptr) = 0;
    virtual bool startStream(ReceiverIqCallback callback, QString *errorMessage = nullptr) = 0;
    virtual void stopStream() = 0;
};

#endif // RECEIVERBACKEND_H
