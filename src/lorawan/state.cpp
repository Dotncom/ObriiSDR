#include "lorawan.h"
#include "regional.h"
#include <algorithm>

namespace obrii::lorawan {
namespace {
constexpr std::size_t MaxBlob=2048;
std::uint32_t crc(const Bytes &b, std::size_t count) {
    std::uint32_t value=0xffffffff;
    for(std::size_t i=0; i<count; ++i) {
        value^=b[i];
        for(unsigned j=0; j<8; ++j) value=(value>>1)^((value&1) ? 0xedb88320:0);
    }
    return ~value;
}
struct Writer {
    Bytes data;
    explicit Writer(char type) :data{'O','W','L',1,std::uint8_t(type)} {}
    void number(std::uint64_t n, unsigned size) { for(unsigned i=0; i<size; ++i) data.push_back(std::uint8_t(n>>(8*i))); }
    void bytes(const Bytes &b) { if(b.size()>MaxBlob) throw Error("State field too large"); number(b.size(),2); data.insert(data.end(),b.begin(),b.end()); }
    void optional(std::optional<std::uint32_t> n) { number(bool(n),1); if(n) number(*n,4); }
    Bytes finish() { number(crc(data,data.size()),4); if(data.size()>MaxBlob) throw Error("State too large"); return data; }
};
struct Reader {
    const Bytes &data; std::size_t at=5;
    Reader(const Bytes &b, char type):data(b) {
        if(b.size()<9 || b.size()>MaxBlob || b[0]!='O' || b[1]!='W' || b[2]!='L' || b[3]!=1 || b[4]!=std::uint8_t(type))
            throw Error("Unsupported or truncated state blob");
        at=b.size()-4; const auto checksum=number(4); at=5;
        if(checksum!=crc(b,b.size()-4)) throw Error("Corrupt state checksum");
    }
    std::uint64_t number(unsigned size) {
        if(at+size>data.size()) throw Error("Truncated state");
        std::uint64_t n=0; for(unsigned i=0; i<size; ++i) n|=std::uint64_t(data[at++])<<(8*i); return n;
    }
    bool boolean() { const auto n=number(1); if(n>1) throw Error("Invalid state flag"); return n!=0; }
    std::optional<std::uint32_t> optional() { if(boolean()) return std::uint32_t(number(4)); return {}; }
    Bytes bytes() { const auto count=number(2); if(at+count>data.size()-4) throw Error("Truncated state field"); Bytes b(data.begin()+at,data.begin()+at+std::size_t(count)); at+=std::size_t(count); return b; }
    void finish() { if(at!=data.size()-4) throw Error("Unexpected state data"); }
};
void validate(const SessionState &s) {
    if((s.role!=Role::Device && s.role!=Role::Network) || !s.devAddr || s.nextTransmit>0x100000000ULL
       || s.maxUplinkCopies<1 || s.maxUplinkCopies>15 || s.uplinkCopies>15 || s.transmitCopies>15
       || s.lastUplink.size()>255 || s.lastTransmitted.size()>255)
        throw Error("Invalid session state");
    if(!s.lastUplink.empty()) {
        if(s.role!=Role::Network || !s.lastReceived || !s.uplinkCopies) throw Error("Invalid uplink cache");
        const auto frame=decodeData(s.lastUplink,s.keys,*s.lastReceived);
        if(frame.devAddr!=s.devAddr || direction(frame.type)!=Direction::Uplink) throw Error("Invalid cached uplink");
    }
    if(!s.lastTransmitted.empty()) {
        if(!s.nextTransmit || !s.transmitCopies) throw Error("Invalid transmit cache counter");
        const auto frame=decodeData(s.lastTransmitted,s.keys,std::uint32_t(s.nextTransmit-1));
        if(frame.devAddr!=s.devAddr || direction(frame.type)!=(s.role==Role::Device ? Direction::Uplink:Direction::Downlink))
            throw Error("Invalid cached transmit direction");
    }
    if(s.mayRepeat && (s.role!=Role::Device || s.lastTransmitted.empty())) throw Error("Invalid repetition state");
}
}
Bytes saveSession(const SessionState &s) {
    validate(s); Writer w('S'); w.number(std::uint8_t(s.role),1); w.number(s.devAddr,4);
    w.data.insert(w.data.end(),s.keys.network.begin(),s.keys.network.end());
    w.data.insert(w.data.end(),s.keys.application.begin(),s.keys.application.end());
    w.number(s.nextTransmit,8); w.optional(s.lastReceived); w.bytes(s.lastUplink); w.bytes(s.lastTransmitted);
    w.number(s.uplinkCopies,1); w.number(s.maxUplinkCopies,1); w.number(s.transmitCopies,1);
    w.number(s.mayRepeat,1); w.number(s.acknowledgmentPending,1); return w.finish();
}
SessionState loadSession(const Bytes &blob) {
    Reader r(blob,'S'); SessionState s; s.role=Role(r.number(1)); s.devAddr=std::uint32_t(r.number(4));
    for(auto &b:s.keys.network) b=std::uint8_t(r.number(1));
    for(auto &b:s.keys.application) b=std::uint8_t(r.number(1));
    s.nextTransmit=r.number(8); s.lastReceived=r.optional(); s.lastUplink=r.bytes(); s.lastTransmitted=r.bytes();
    s.uplinkCopies=std::uint8_t(r.number(1)); s.maxUplinkCopies=std::uint8_t(r.number(1)); s.transmitCopies=std::uint8_t(r.number(1));
    s.mayRepeat=r.boolean(); s.acknowledgmentPending=r.boolean(); r.finish(); validate(s); return s;
}
Bytes saveDeviceJoin(const DeviceJoinState &s) {
    if(s.nextDevNonce>65536 || (s.pendingDevNonce && std::uint32_t(*s.pendingDevNonce)+1!=s.nextDevNonce)
       || (s.lastJoinNonce && *s.lastJoinNonce>0xffffff) || (s.activated && s.activated->role!=Role::Device))
        throw Error("Invalid device join state");
    Writer w('D'); w.number(s.nextDevNonce,4);
    w.optional(s.pendingDevNonce ? std::optional<std::uint32_t>(*s.pendingDevNonce):std::nullopt);
    w.optional(s.lastJoinNonce); w.number(bool(s.activated),1); if(s.activated) w.bytes(saveSession(*s.activated)); return w.finish();
}
DeviceJoinState loadDeviceJoin(const Bytes &blob) {
    Reader r(blob,'D'); DeviceJoinState s; s.nextDevNonce=std::uint32_t(r.number(4)); const auto nonce=r.optional();
    if(nonce) { if(*nonce>65535) throw Error("DevNonce overflow"); s.pendingDevNonce=std::uint16_t(*nonce); }
    s.lastJoinNonce=r.optional(); if(r.boolean()) s.activated=loadSession(r.bytes()); r.finish(); (void)saveDeviceJoin(s); return s;
}
Bytes saveJoinServer(const JoinServerState &s) {
    if(s.nextJoinNonce>0x1000000 || (s.activated && s.activated->role!=Role::Network)) throw Error("Invalid join server state");
    Writer w('N'); w.number(s.nextJoinNonce,4);
    w.optional(s.lastDevNonce ? std::optional<std::uint32_t>(*s.lastDevNonce):std::nullopt);
    w.number(bool(s.activated),1); if(s.activated) w.bytes(saveSession(*s.activated)); return w.finish();
}
JoinServerState loadJoinServer(const Bytes &blob) {
    Reader r(blob,'N'); JoinServerState s; s.nextJoinNonce=std::uint32_t(r.number(4)); const auto nonce=r.optional();
    if(nonce) { if(*nonce>65535) throw Error("DevNonce overflow"); s.lastDevNonce=std::uint16_t(*nonce); }
    if(r.boolean()) s.activated=loadSession(r.bytes()); r.finish(); (void)saveJoinServer(s); return s;
}
Bytes saveRegion(const Eu868 &s) {
    s.validate(); Writer w('R');
    for(const auto &c:s.channels) {
        w.number(c.frequencyHz,4); w.number(c.downlinkHz,4); w.number(c.minRate,1); w.number(c.maxRate,1); w.number(c.enabled,1);
    }
    w.number(s.dataRate,1); w.number(s.txPower,1); w.number(s.nbTrans,1); w.number(s.dutyExponent,1);
    w.number(s.rx1Offset,1); w.number(s.rx2DataRate,1); w.number(s.receiveDelay,1); w.number(s.rx2FrequencyHz,4);
    w.number(s.repeaterCompatible,1); w.number(s.adrEnabled,1); w.number(s.adrAckCount,4); w.bytes(s.stickyAnswers); return w.finish();
}
Eu868 loadRegion(const Bytes &blob) {
    Reader r(blob,'R'); Eu868 s;
    for(auto &c:s.channels) {
        c.frequencyHz=std::uint32_t(r.number(4)); c.downlinkHz=std::uint32_t(r.number(4));
        c.minRate=std::uint8_t(r.number(1)); c.maxRate=std::uint8_t(r.number(1)); c.enabled=r.boolean();
    }
    s.dataRate=std::uint8_t(r.number(1)); s.txPower=std::uint8_t(r.number(1)); s.nbTrans=std::uint8_t(r.number(1)); s.dutyExponent=std::uint8_t(r.number(1));
    s.rx1Offset=std::uint8_t(r.number(1)); s.rx2DataRate=std::uint8_t(r.number(1)); s.receiveDelay=std::uint8_t(r.number(1));
    s.rx2FrequencyHz=std::uint32_t(r.number(4)); s.repeaterCompatible=r.boolean(); s.adrEnabled=r.boolean();
    s.adrAckCount=std::uint32_t(r.number(4)); s.stickyAnswers=r.bytes(); r.finish(); s.validate(); return s;
}
Bytes saveDuty(const DutyState &s) {
    Writer w('T'); for(auto t:s.bands) w.number(t,8); w.number(s.aggregate,8); return w.finish();
}
DutyState loadDuty(const Bytes &blob) {
    Reader r(blob,'T'); DutyState s; for(auto &t:s.bands) t=r.number(8); s.aggregate=r.number(8); r.finish(); return s;
}
}
