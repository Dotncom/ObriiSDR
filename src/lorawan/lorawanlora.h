#pragma once
#include "regional.h"
#include "loraphy.h"

namespace obrii::lorawan {
obrii::lora::Config phyConfig(unsigned eu868Rate, Direction direction, int oversampling=2);
struct RadioPacket {
    Bytes wire;
    std::uint64_t payloadSampleOffset=0;
    double frequencyOffsetHz=0;
};
// Direction describes the radio packets, not the local role. A device uses
// Uplink for TX and Downlink for RX; a gateway uses the inverse pair.
// Direct LoRaWAN PHYPayload: no RNode/OBRN/Meshtastic envelope.
class LoRaBearer {
public:
    LoRaBearer(unsigned eu868Rate, Direction direction, int oversampling=2, bool repeaterCompatible=false);
    const obrii::lora::Config &config() const { return modem_.config(); }
    obrii::lora::Iq modulate(const Bytes &wire, float amplitude=0.7f) const;
    double airtimeSeconds(const Bytes &wire) const;
    std::vector<RadioPacket> receive(const std::complex<float> *samples, std::size_t count);
    std::vector<RadioPacket> receive(const obrii::lora::Iq &iq) { return receive(iq.data(),iq.size()); }
    void reset() { receiver_.reset(); }
private:
    bool valid(const Bytes &wire) const;
    unsigned rate_;
    Direction direction_;
    bool repeater_;
    obrii::lora::Modem modem_;
    obrii::lora::Receiver receiver_;
};
}
