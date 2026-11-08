#include "meshlora.h"
#include <stdexcept>
namespace obrii::meshtastic {
namespace {
lora::Config checked(lora::Config c) {
    c.validate();
    if(!c.explicitHeader||!c.payloadCrc||c.invertIq||c.syncWord!=0x2b||c.preambleSymbols<16)
        throw std::invalid_argument("Meshtastic needs explicit header, CRC, normal IQ, sync0x2b and preamble>=16");
    return c;
}
}
LoRaRadio::LoRaRadio(lora::Config config):config_(checked(config)),modem_(config_),receiver_(config_) {}
lora::Iq LoRaRadio::modulate(const Transmission &tx,float amplitude) const { return modulate(tx.radioBytes,amplitude); }
lora::Iq LoRaRadio::modulate(const Bytes &wire,float amplitude) const {
    decodeRadio(wire);return modem_.modulate(wire,amplitude);
}
std::vector<AirPacket> LoRaRadio::push(const std::complex<float> *iq,std::size_t count) {
    std::vector<AirPacket> packets;
    for(auto &rx:receiver_.push(iq,count)) {
        if(!rx.packet.crcChecked)continue;
        try { decodeRadio(rx.packet.payload); }
        catch(const std::invalid_argument &) { continue; }
        packets.push_back({std::move(rx.packet.payload),rx.payloadSampleOffset,rx.frequencyOffsetHz});
    }
    return packets;
}
}
