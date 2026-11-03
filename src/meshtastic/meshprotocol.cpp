#include "meshtastic.h"
#include "meshcrypto_internal.h"
#include "meshprotobuf_internal.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace obrii::meshtastic {
namespace {
void validateHeader(const Header &h) {
    if(h.from<=3||h.from==Broadcast||h.to<=3||!h.id)
        throw std::invalid_argument("Invalid Meshtastic sender, destination or packet ID");
    if(h.hopLimit>7||h.hopStart>7)throw std::invalid_argument("Meshtastic hops must be 0..7");
}
void put32(Bytes &b,std::uint32_t value) {
    for(int i=0;i<4;++i)b.push_back(std::uint8_t(value>>(8*i)));
}
std::uint32_t get32(const Bytes &b,std::size_t offset) {
    std::uint32_t result=0;
    for(int i=0;i<4;++i)result|=std::uint32_t(b[offset+i])<<(8*i);
    return result;
}
}
Bytes encodeRadio(const RadioPacket &packet) {
    validateHeader(packet.header);
    if(packet.body.empty()||packet.body.size()>DataMtu)throw std::invalid_argument("Meshtastic body must be 1..239 bytes");
    const auto &h=packet.header;
    Bytes out;out.reserve(RadioHeaderBytes+packet.body.size());
    put32(out,h.to);put32(out,h.from);put32(out,h.id);
    out.push_back(std::uint8_t(h.hopLimit|(h.wantAck?8:0)|(h.viaMqtt?16:0)|(h.hopStart<<5)));
    out.push_back(h.channelHash);out.push_back(h.nextHop);out.push_back(h.relayNode);
    out.insert(out.end(),packet.body.begin(),packet.body.end());return out;
}
RadioPacket decodeRadio(const Bytes &bytes) {
    if(bytes.size()<=RadioHeaderBytes||bytes.size()>RadioMtu)throw std::invalid_argument("Invalid Meshtastic radio packet size");
    RadioPacket p;auto &h=p.header;
    h.to=get32(bytes,0);h.from=get32(bytes,4);h.id=get32(bytes,8);
    h.hopLimit=bytes[12]&7;h.hopStart=bytes[12]>>5;h.wantAck=(bytes[12]&8)!=0;h.viaMqtt=(bytes[12]&16)!=0;
    h.channelHash=bytes[13];h.nextHop=bytes[14];h.relayNode=bytes[15];validateHeader(h);
    p.body.assign(bytes.begin()+RadioHeaderBytes,bytes.end());return p;
}
std::uint8_t relayId(std::uint32_t id) {
    const auto last=std::uint8_t(id);return last?last:0xff;
}
void Channel::validate() const {
    if(name.empty()||name.size()>64||name.find('\0')!=std::string::npos||!detail::validUtf8(name))
        throw std::invalid_argument("Channel needs a resolved UTF-8 name of 1..64 bytes");
    if(psk.size()>32)throw std::invalid_argument("Meshtastic PSK exceeds 32 bytes");
}
Bytes Channel::effectiveKey() const {
    validate();
    if(psk.empty()||(psk.size()==1&&psk[0]==0))return {};
    if(psk.size()==1) {
        // Public default PSK; this is a protocol constant, not a secret.
        Bytes key{0xd4,0xf1,0xbb,0x3a,0x20,0x29,0x07,0x59,0xf0,0xbc,0xff,0xab,0xcf,0x4e,0x69,0x01};
        key.back()=std::uint8_t(key.back()+psk[0]-1);return key;
    }
    Bytes key=psk;key.resize(key.size()<=16?16:32,0);return key;
}
std::uint8_t Channel::hash() const {
    const auto key=effectiveKey();std::uint8_t value=0;
    for(unsigned char ch:name)value^=ch;
    for(auto b:key)value^=b;
    return value;
}
std::array<std::uint8_t,16> nonce(std::uint32_t from,std::uint64_t packetId) {
    std::array<std::uint8_t,16> out{};
    for(int i=0;i<8;++i)out[i]=std::uint8_t(packetId>>(8*i));
    for(int i=0;i<4;++i)out[8+i]=std::uint8_t(from>>(8*i));
    return out;
}
Bytes cryptPayload(const Bytes &key,std::uint32_t from,std::uint64_t packetId,const Bytes &body) {
    if(body.size()>DataMtu)throw std::invalid_argument("Meshtastic crypto body exceeds MTU");
    Bytes result=body;
    if(key.empty())return result;
    const auto iv=nonce(from,packetId);
    if(key.size()==16)detail::aes128Ctr(key,iv,result);
    else if(key.size()==32)detail::aes256Ctr(key,iv,result);
    else throw std::invalid_argument("Effective PSK must be empty, 16 or 32 bytes");
    return result;
}
Bytes seal(Header header,const Data &data,const Channel &channel) {
    if(!channel.enabled)throw std::invalid_argument("Meshtastic channel disabled");
    if(!data.port||data.port>511)throw std::invalid_argument("Meshtastic application port must be 1..511");
    header.channelHash=channel.hash();
    return encodeRadio({header,cryptPayload(channel.effectiveKey(),header.from,header.id,encodeData(data))});
}
DecodeResult open(const RadioPacket &radio,const std::vector<Channel> &channels) {
    validateHeader(radio.header);
    if(radio.body.empty()||radio.body.size()>DataMtu||channels.size()>8)
        throw std::invalid_argument("Invalid Meshtastic packet or channel count");
    DecodeResult result;bool matched=false;
    for(std::size_t i=0;i<channels.size();++i) {
        const auto &ch=channels[i];if(!ch.enabled||ch.hash()!=radio.header.channelHash)continue;
        matched=true;
        try {
            auto data=decodeData(cryptPayload(ch.effectiveKey(),radio.header.from,radio.header.id,radio.body));
            if(!data.port||data.port>511)continue;
            if(result.packet) { result.packet.reset();result.status=DecodeStatus::AmbiguousChannel;return result; }
            result.packet=Decoded{radio,std::move(data),i};
        } catch(const std::invalid_argument &) { /* No valid protobuf for this hash candidate. */ }
    }
    result.status=result.packet?DecodeStatus::Decoded:matched?DecodeStatus::InvalidPayload:DecodeStatus::UnknownChannel;
    return result;
}
PacketIdSequence::PacketIdSequence(std::uint64_t next):next_(next) {
    if(!next||next>std::uint64_t(std::numeric_limits<std::uint32_t>::max())+1)
        throw std::invalid_argument("Invalid next packet ID");
}
std::uint32_t PacketIdSequence::next() {
    if(next_>std::numeric_limits<std::uint32_t>::max())throw std::overflow_error("Meshtastic packet IDs exhausted; rotate identity/key");
    return std::uint32_t(next_++);
}
ApiFramer::ApiFramer(std::size_t maximum):limit_(maximum) {
    if(!maximum||maximum>4096)throw std::invalid_argument("API payload limit must be 1..4096");
}
Bytes ApiFramer::encode(const Bytes &payload) const {
    if(payload.empty()||payload.size()>limit_)throw std::invalid_argument("Invalid Meshtastic API frame length");
    Bytes out{0x94,0xc3,std::uint8_t(payload.size()>>8),std::uint8_t(payload.size())};
    out.insert(out.end(),payload.begin(),payload.end());return out;
}
std::vector<Bytes> ApiFramer::push(const std::uint8_t *bytes,std::size_t count) {
    if(!bytes&&count)throw std::invalid_argument("Null Meshtastic API input");
    std::vector<Bytes> output;
    for(std::size_t i=0;i<count;++i) {
        buffer_.push_back(bytes[i]);
        while(!buffer_.empty()) {
            if(buffer_[0]!=0x94) { buffer_.erase(buffer_.begin());continue; }
            if(buffer_.size()<2)break;
            if(buffer_[1]!=0xc3) { buffer_.erase(buffer_.begin());continue; }
            if(buffer_.size()<4)break;
            const std::size_t length=(buffer_[2]<<8)|buffer_[3];
            if(!length||length>limit_) { buffer_.erase(buffer_.begin());continue; }
            if(buffer_.size()<length+4)break;
            output.emplace_back(buffer_.begin()+4,buffer_.begin()+4+length);
            buffer_.erase(buffer_.begin(),buffer_.begin()+4+length);
        }
    }
    return output;
}
}
