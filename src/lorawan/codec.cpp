#include "lorawan.h"
#include <algorithm>

namespace obrii::lorawan {
namespace {
void put(Bytes &out, std::uint32_t n, unsigned size) {
    for(unsigned i=0; i<size; ++i) out.push_back(std::uint8_t(n>>(8*i)));
}
std::uint32_t get(const Bytes &in, std::size_t pos, unsigned size) {
    if(pos+size>in.size()) throw Error("Truncated LoRaWAN frame");
    std::uint32_t result=0;
    for(unsigned i=0; i<size; ++i) result|=std::uint32_t(in[pos+i])<<(8*i);
    return result;
}
void mic(Bytes &wire, const Key &key, const Bytes &prefix={}) {
    Bytes input=prefix; input.insert(input.end(),wire.begin(),wire.end());
    const auto tag=cmac(key,input); wire.insert(wire.end(),tag.begin(),tag.begin()+4);
}
void checkMic(const Bytes &wire, const Key &key, const Bytes &prefix={}) {
    Bytes expected(wire.begin(),wire.end()-4); mic(expected,key,prefix);
    std::uint8_t difference=0;
    for(std::size_t i=wire.size()-4; i<wire.size(); ++i) difference|=wire[i]^expected[i];
    if(difference) throw Error("LoRaWAN MIC mismatch");
}
void header(const Bytes &wire, FrameType type, std::size_t minimum) {
    if(wire.size()<minimum || wire.size()>255 || wire[0]!=(std::uint8_t(type)<<5))
        throw Error("Invalid LoRaWAN header, major version or length");
}
FrameType dataType(const Bytes &wire) {
    if(wire.empty() || (wire[0]&31) || wire[0]<0x40 || wire[0]>0xa0)
        throw Error("Expected LoRaWAN 1.0.x data frame");
    return FrameType(wire[0]>>5);
}
Bytes block0(Direction dir, std::uint32_t address, std::uint32_t counter, std::size_t size) {
    Bytes block{0x49,0,0,0,0,std::uint8_t(dir)};
    put(block,address,4); put(block,counter,4); block.push_back(0); block.push_back(std::uint8_t(size));
    return block;
}
Bytes crypt(const Bytes &input, const Key &key, Direction dir, std::uint32_t address, std::uint32_t counter, unsigned domain=0) {
    Bytes output=input;
    for(std::size_t offset=0; offset<input.size(); offset+=16) {
        auto ai=block0(dir,address,counter,offset/16+1); ai[0]=1; ai[4]=std::uint8_t(domain);
        Block block{}; std::copy(ai.begin(),ai.end(),block.begin());
        block=aesEncrypt(key,block);
        for(std::size_t i=0; i<16 && offset+i<input.size(); ++i) output[offset+i]^=block[i];
    }
    return output;
}
std::array<std::uint8_t,4> dataMic(const Bytes &wire, const SessionKeys &keys, DataContext context,
                                  Direction dir, std::uint32_t addr, std::uint32_t counter) {
    if(keys.version!=Version::V1_0_4 && keys.version!=Version::V1_1) throw Error("Unsupported LoRaWAN version");
    auto b=block0(dir,addr,counter,wire.size());
    auto hash=[&](const Key &key, const Bytes &prefix) {
        auto input=prefix; input.insert(input.end(),wire.begin(),wire.end()); return cmac(key,input);
    };
    const auto forwarding=hash(keys.network,b);
    if(keys.version==Version::V1_0_4) return {forwarding[0],forwarding[1],forwarding[2],forwarding[3]};
    if(wire[5]&0x20) { b[1]=std::uint8_t(context.confirmedCounter); b[2]=std::uint8_t(context.confirmedCounter>>8); }
    if(dir==Direction::Uplink) { b[3]=context.txDataRate; b[4]=context.txChannel; }
    const auto serving=hash(keys.servingIntegrity,b);
    if(dir==Direction::Downlink) return {serving[0],serving[1],serving[2],serving[3]};
    return {serving[0],serving[1],forwarding[0],forwarding[1]};
}
void validate(const DataFrame &f) {
    const auto dir=direction(f.type);
    if(f.options.size()>15 || (f.control&15) || (dir==Direction::Downlink && (f.control&0x40))
       || (!f.port && !f.payload.empty()) || (f.port && *f.port==0 && !f.options.empty()))
        throw Error("Invalid FCtrl, FOpts, FPort or payload combination");
    if(12+f.options.size()+(f.port ? 1:0)+f.payload.size()>255)
        throw Error("LoRaWAN PHYPayload exceeds 255 bytes");
}
}
Direction direction(FrameType type) {
    if(type==FrameType::ConfirmedUp || type==FrameType::UnconfirmedUp) return Direction::Uplink;
    if(type==FrameType::ConfirmedDown || type==FrameType::UnconfirmedDown) return Direction::Downlink;
    throw Error("Frame is not a supported data frame");
}
bool confirmed(FrameType type) { return type==FrameType::ConfirmedUp || type==FrameType::ConfirmedDown; }
SessionKeys deriveSessionKeys(const Key &key, std::uint32_t nonce, std::uint32_t netId, std::uint16_t devNonce) {
    if(nonce>0xffffff || netId>0xffffff) throw Error("JoinNonce/NetID exceeds 24 bits");
    Bytes data{1}; put(data,nonce,3); put(data,netId,3); put(data,devNonce,2); data.resize(16);
    Block block{}; std::copy(data.begin(),data.end(),block.begin());
    SessionKeys result; result.network=aesEncrypt(key,block); block[0]=2;
    result.application=aesEncrypt(key,block); return result;
}
Bytes encodeJoinRequest(const JoinRequest &r, const Key &key) {
    Bytes wire{0}; wire.insert(wire.end(),r.joinEui.rbegin(),r.joinEui.rend());
    wire.insert(wire.end(),r.devEui.rbegin(),r.devEui.rend()); put(wire,r.devNonce,2); mic(wire,key); return wire;
}
JoinRequest decodeJoinRequest(const Bytes &wire, const Key &key) {
    header(wire,FrameType::JoinRequest,23); if(wire.size()!=23) throw Error("Join request must be 23 bytes");
    checkMic(wire,key); JoinRequest r;
    std::reverse_copy(wire.begin()+1,wire.begin()+9,r.joinEui.begin());
    std::reverse_copy(wire.begin()+9,wire.begin()+17,r.devEui.begin()); r.devNonce=std::uint16_t(get(wire,17,2)); return r;
}
Bytes encodeJoinAccept(const JoinAccept &a, const Key &key) {
    if(a.optNeg || a.joinNonce>0xffffff || a.netId>0xffffff || !a.devAddr || a.rx1Offset>7 || a.rx2DataRate>15 || a.rxDelay>15)
        throw Error("Invalid join accept parameters; LoRaWAN 1.1 OptNeg is not supported");
    Bytes plain{0x20}; put(plain,a.joinNonce,3); put(plain,a.netId,3); put(plain,a.devAddr,4);
    plain.push_back(std::uint8_t(a.rx1Offset<<4|a.rx2DataRate)); plain.push_back(a.rxDelay);
    if(a.cfList) plain.insert(plain.end(),a.cfList->begin(),a.cfList->end());
    mic(plain,key); Bytes wire{0x20};
    for(std::size_t offset=1; offset<plain.size(); offset+=16) {
        Block block{}; std::copy_n(plain.data()+offset,16,block.data()); block=aesDecrypt(key,block);
        wire.insert(wire.end(),block.begin(),block.end());
    }
    return wire;
}
JoinAccept decodeJoinAccept(const Bytes &wire, const Key &key) {
    header(wire,FrameType::JoinAccept,17); if(wire.size()!=17 && wire.size()!=33) throw Error("Join accept must be 17 or 33 bytes");
    Bytes plain{0x20};
    for(std::size_t offset=1; offset<wire.size(); offset+=16) {
        Block block{}; std::copy_n(wire.data()+offset,16,block.data()); block=aesEncrypt(key,block);
        plain.insert(plain.end(),block.begin(),block.end());
    }
    checkMic(plain,key);
    if((plain[11]&0x80) || (plain[12]&0xf0)) throw Error("Unsupported OptNeg or reserved join accept bits");
    JoinAccept a; a.joinNonce=get(plain,1,3); a.netId=get(plain,4,3); a.devAddr=get(plain,7,4);
    a.rx1Offset=(plain[11]>>4)&7; a.rx2DataRate=plain[11]&15; a.rxDelay=plain[12]&15;
    if(!a.devAddr) throw Error("Zero DevAddr");
    if(wire.size()==33) { a.cfList.emplace(); std::copy_n(plain.begin()+13,16,a.cfList->begin()); }
    return a;
}
Bytes encodeData(const DataFrame &f, const SessionKeys &keys, DataContext context) {
    validate(f); const auto dir=direction(f.type); Bytes wire{std::uint8_t(std::uint8_t(f.type)<<5)};
    put(wire,f.devAddr,4); wire.push_back(std::uint8_t(f.control|f.options.size())); put(wire,f.counter&0xffff,2);
    const bool v11=keys.version==Version::V1_1;
    const bool appDown=dir==Direction::Downlink && f.port && *f.port>0;
    const auto options=v11 ? crypt(f.options,keys.networkEncryption,dir,f.devAddr,f.counter,appDown ? 2:1):f.options;
    wire.insert(wire.end(),options.begin(),options.end());
    if(f.port) {
        wire.push_back(*f.port);
        const auto payload=crypt(f.payload,*f.port==0 ? (v11 ? keys.networkEncryption:keys.network):keys.application,dir,f.devAddr,f.counter);
        wire.insert(wire.end(),payload.begin(),payload.end());
    }
    const auto tag=dataMic(wire,keys,context,dir,f.devAddr,f.counter);
    wire.insert(wire.end(),tag.begin(),tag.end()); return wire;
}
DataFrame decodeData(const Bytes &wire, const SessionKeys &keys, std::uint32_t counter, DataContext context) {
    const auto type=dataType(wire); header(wire,type,12);
    const auto count=wire[5]&15; const auto end=wire.size()-4;
    if(std::size_t(8+count)>end || get(wire,6,2)!=(counter&0xffff)) throw Error("Truncated FOpts or mismatched FCnt");
    const auto address=get(wire,1,4); const auto dir=direction(type);
    const auto tag=dataMic(Bytes(wire.begin(),wire.end()-4),keys,context,dir,address,counter);
    std::uint8_t difference=0; for(unsigned i=0;i<4;++i) difference|=tag[i]^wire[end+i];
    if(difference) throw Error("LoRaWAN MIC mismatch");
    DataFrame f; f.type=type; f.devAddr=address; f.counter=counter; f.control=wire[5]&0xf0;
    f.options.assign(wire.begin()+8,wire.begin()+8+count);
    if(end>std::size_t(8+count)) {
        f.port=wire[8+count]; Bytes body(wire.begin()+9+count,wire.begin()+end);
        f.payload=crypt(body,*f.port==0 ? (keys.version==Version::V1_1 ? keys.networkEncryption:keys.network):keys.application,dir,address,counter);
    }
    if(keys.version==Version::V1_1)
        f.options=crypt(f.options,keys.networkEncryption,dir,address,counter,dir==Direction::Downlink && f.port && *f.port>0 ? 2:1);
    validate(f); return f;
}
std::uint16_t wireCounter(const Bytes &wire) { const auto type=dataType(wire); header(wire,type,12); return std::uint16_t(get(wire,6,2)); }
std::uint32_t wireAddress(const Bytes &wire) { const auto type=dataType(wire); header(wire,type,12); return get(wire,1,4); }
}
