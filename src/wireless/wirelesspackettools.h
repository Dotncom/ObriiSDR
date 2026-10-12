#ifndef WIRELESS_PACKET_TOOLS_H
#define WIRELESS_PACKET_TOOLS_H

#include <QByteArray>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <cstdint>
#include <cstddef>

namespace Wireless {
enum class Protocol { Wifi, Bluetooth, Gsm, Lte, Umts, All };
QString displayFilter(Protocol protocol);
QStringList tsharkFields();
QStringList tsharkOutputArguments();
bool parsePacketLine(const QByteArray &line, QStringList &fields);
int wifiChannel(int frequencyMhz);
QString ssidText(const QString &field);
QString findTshark();
QStringList availableProfiles();
struct CaptureInterface { QString id, label; };
QVector<CaptureInterface> parseInterfaces(const QString &report);
struct AnalysisRequest {
    QString inputFile, interfaceId, outputFile, displayFilter, captureFilter, profile;
    bool live = false, monitor = false, decryptWithProfile = false;
};
QStringList analysisArguments(const AnalysisRequest &request, QString *error);
enum class PayloadState { NotWifi, Clear, Decoded, Protected };
PayloadState payloadState(const QStringList &fields);

struct IqStreamDescriptor {
    double sampleRateHz = 0.0;
    double usableBandwidthHz = 0.0;
    double centerFrequencyHz = 0.0;
    std::uint64_t firstSample = 0;
    std::uint64_t epoch = 0;
    QString clockId;
    bool continuous = false;
    int channelIndex = 0;
    int activeChannels = 1;
    int activeTransmitChannels = 0;
    QString deviceSerial;
    std::uint64_t hardwareTick = 0;
    double hardwareTicksPerSecond = 0.0;
    bool hardwareTimeValid = false;
    bool clockLocked = false;
};
struct BackendCapabilities {
    QString id;
    Protocol protocol = Protocol::Wifi;
    bool receivesIq = false;
    bool receivesPackets = false;
    bool transmitsPackets = false;
    bool fullDuplex = false;
    bool timedTransmit = false;
    double maximumSampleRateHz = 0.0;
    double maximumBandwidthHz = 0.0;
    double minimumFrequencyHz = 0.0, maximumFrequencyHz = 0.0;
    double maximumDualChannelSampleRateHz = 0.0;
    int receiveChannels = 1, transmitChannels = 1;
    bool transmitsIq = false;
    bool externalReference = false, externalPps = false;
    bool sharedRxLo = false, sharedTxLo = false;
    bool requiresHardwareTime = false;
    bool planningOnly = false;
};
BackendCapabilities usrpB210PlanningCapabilities();
struct PacketTxRequest {
    QByteArray frame;
    std::uint64_t targetSample = 0;
    std::uint64_t epoch = 0;
    QString clockId;
    bool timed = false;
    bool simultaneousRxOnSameDevice = false;
    int channelIndex = 0;
    bool useHardwareTicks = false;
    std::uint64_t targetHardwareTick = 0;
    std::uint64_t minimumLeadTicks = 0;
};

// Future PHY/MAC engines plug in here, outside the receiver callback path.
// Timed transmission requires a shared hardware clock, not GUI wall time.
class PacketBackend {
public:
    virtual ~PacketBackend() = default;
    virtual BackendCapabilities capabilities() const = 0;
    virtual bool receiveIq(const float *, std::size_t, const IqStreamDescriptor &, QString &error) {
        error = QStringLiteral("IQ reception is not supported by this backend"); return false;
    }
    virtual bool transmit(const PacketTxRequest &, QString &error) {
        error = QStringLiteral("Packet transmission is not supported by this backend"); return false;
    }
};
bool validateIqStream(const BackendCapabilities &capabilities,
                      const IqStreamDescriptor &stream, double requiredBandwidthHz,
                      QString &error);
bool validateTransmission(const BackendCapabilities &capabilities,
                          const IqStreamDescriptor &stream,
                          const PacketTxRequest &request, QString &error);
}
#endif
