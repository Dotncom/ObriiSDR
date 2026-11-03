#include "lorawanlora.h"
#include <utility>

namespace obrii::lorawan {
obrii::lora::Config phyConfig(unsigned rate, Direction dir, int oversampling) {
    if(dir!=Direction::Uplink && dir!=Direction::Downlink) throw Error("Invalid radio direction");
    obrii::lora::Config c;
    c.spreadingFactor=Eu868::spreadingFactor(rate); c.bandwidthHz=Eu868::bandwidth(rate);
    c.codingRate=1; c.oversampling=oversampling; c.preambleSymbols=8; c.syncWord=0x34;
    c.explicitHeader=true; c.payloadCrc=dir==Direction::Uplink; c.invertIq=dir==Direction::Downlink;
    c.ldro=obrii::lora::Ldro::Auto; c.validate(); return c;
}
LoRaBearer::LoRaBearer(unsigned rate, Direction dir, int os, bool repeater)
    :rate_(rate),direction_(dir),repeater_(repeater),modem_(phyConfig(rate,dir,os)),receiver_(modem_.config()) {}
bool LoRaBearer::valid(const Bytes &wire) const {
    if(wire.size()<12 || wire.size()>Eu868::maxMacPayload(rate_,repeater_)+5 || (wire[0]&31)) return false;
    const auto type=wire[0]>>5;
    if(direction_==Direction::Uplink) return type==0 ? wire.size()==23:type==2 || type==4;
    return type==1 ? wire.size()==17 || wire.size()==33:type==3 || type==5;
}
obrii::lora::Iq LoRaBearer::modulate(const Bytes &wire, float amplitude) const {
    if(!valid(wire)) throw Error("Invalid LoRaWAN PHY packet for selected direction/rate");
    return modem_.modulate(wire,amplitude);
}
double LoRaBearer::airtimeSeconds(const Bytes &wire) const {
    if(!valid(wire)) throw Error("Invalid LoRaWAN PHY packet for selected direction/rate");
    return obrii::lora::PacketCodec(modem_.config()).timeOnAirSeconds(int(wire.size()));
}
std::vector<RadioPacket> LoRaBearer::receive(const std::complex<float> *iq, std::size_t count) {
    std::vector<RadioPacket> output;
    for(auto &r:receiver_.push(iq,count))
        if(valid(r.packet.payload) && r.packet.header.codingRate==1
           && (direction_==Direction::Downlink || r.packet.crcChecked))
            output.push_back({std::move(r.packet.payload),r.payloadSampleOffset,r.frequencyOffsetHz});
    return output;
}
}
