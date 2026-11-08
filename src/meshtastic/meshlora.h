#pragma once
#include "meshtastic.h"
#include "loraphy.h"
namespace obrii::meshtastic {
struct AirPacket {
    Bytes radioBytes;
    std::uint64_t payloadSampleOffset=0;
    double frequencyOffsetHz=0;
};
// Adapter for the sub-GHz Meshtastic PHY: normal IQ, explicit header, CRC,
// sync0x2b, preamble>=16. Channel selection/resampling and RF I/O are external.
class LoRaRadio {
public:
    explicit LoRaRadio(lora::Config config = {});
    lora::Iq modulate(const Transmission &tx,float amplitude=0.7f) const;
    lora::Iq modulate(const Bytes &radioBytes,float amplitude=0.7f) const;
    std::vector<AirPacket> push(const std::complex<float> *iq,std::size_t count);
    std::vector<AirPacket> push(const lora::Iq &iq) { return push(iq.data(),iq.size()); }
    void reset() { receiver_.reset(); }
    const lora::Config &config() const { return config_; }
private:
    lora::Config config_;
    lora::Modem modem_;
    lora::Receiver receiver_;
};
}
