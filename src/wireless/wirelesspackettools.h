#ifndef WIRELESS_PACKET_TOOLS_H
#define WIRELESS_PACKET_TOOLS_H

#include <QByteArray>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <cstdint>
#include <cstddef>

namespace Wireless {
enum class Protocol { Wifi, Bluetooth, Gsm, Lte };
QString displayFilter(Protocol protocol);
QStringList tsharkFields();
QStringList tsharkOutputArguments();
bool parsePacketLine(const QByteArray &line, QStringList &fields);
int wifiChannel(int frequencyMhz);

struct IqStreamDescriptor {
    double sampleRateHz = 0.0;
    double usableBandwidthHz = 0.0;
    double centerFrequencyHz = 0.0;
    std::uint64_t firstSample = 0;
    std::uint64_t epoch = 0;
    QString clockId;
    bool continuous = false;
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
};
struct PacketTxRequest {
    QByteArray frame;
    std::uint64_t targetSample = 0;
    std::uint64_t epoch = 0;
    QString clockId;
    bool timed = false;
    bool simultaneousRxOnSameDevice = false;
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
