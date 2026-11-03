#include "version11.h"
#include <algorithm>
#include <utility>

namespace obrii::lorawan {
namespace {
void put(Bytes &b,std::uint32_t v,unsigned size) { for(unsigned i=0;i<size;++i)b.push_back(std::uint8_t(v>>(8*i))); }
std::uint32_t get(const Bytes &b,std::size_t p,unsigned size) {
    if(p+size>b.size())throw Error("Truncated LoRaWAN 1.1 packet");
    std::uint32_t v=0; for(unsigned i=0;i<size;++i)v|=std::uint32_t(b[p+i])<<(8*i); return v;
}
Key derive(const Key &key,std::uint8_t type,Bytes b) {
    b.insert(b.begin(),type); if(b.size()>16)throw Error("Invalid key derivation input"); b.resize(16);
    Block block{}; std::copy(b.begin(),b.end(),block.begin()); return aesEncrypt(key,block);
}
Bytes prefix(const Credentials &c,std::uint16_t nonce,std::uint8_t type) {
    if(type!=255 && type>2)throw Error("Unsupported join request type");
    Bytes b{type}; b.insert(b.end(),c.joinEui.rbegin(),c.joinEui.rend()); put(b,nonce,2); return b;
}
void appendMic(Bytes &b,const Key &key,Bytes p={}) {
    p.insert(p.end(),b.begin(),b.end()); const auto tag=cmac(key,p); b.insert(b.end(),tag.begin(),tag.begin()+4);
}
void checkMic(const Bytes &b,const Key &key,Bytes p={}) {
    Bytes expected(b.begin(),b.end()-4); appendMic(expected,key,std::move(p));
    std::uint8_t diff=0; for(unsigned i=0;i<4;++i)diff|=b[b.size()-4+i]^expected[expected.size()-4+i];
    if(diff)throw Error("LoRaWAN 1.1 MIC mismatch");
}
Bytes acceptCrypt(const Bytes &wire,const Key &key,bool encrypt) {
    if((wire.size()!=17 && wire.size()!=33)||wire[0]!=0x20)throw Error("Invalid JoinAccept length/header");
    Bytes out{0x20}; for(std::size_t i=1;i<wire.size();i+=16){
        Block b{};std::copy_n(wire.begin()+i,16,b.begin());b=encrypt?aesDecrypt(key,b):aesEncrypt(key,b);
        out.insert(out.end(),b.begin(),b.end());
    }return out;
}
}
JoinServerKeys deriveJoinServerKeys(const Key &key,const Eui &dev) {
    const Bytes b(dev.rbegin(),dev.rend());return {derive(key,5,b),derive(key,6,b)};
}
SessionKeys deriveSessionKeys11(const Key &nwk,const Key &app,std::uint32_t nonce,const Eui &join,std::uint16_t dev) {
    if(nonce>0xffffff)throw Error("JoinNonce exceeds 24 bits");
    Bytes b;put(b,nonce,3);b.insert(b.end(),join.rbegin(),join.rend());put(b,dev,2);
    SessionKeys s; s.version=Version::V1_1; s.network=derive(nwk,1,b); s.application=derive(app,2,b);
    s.servingIntegrity=derive(nwk,3,b); s.networkEncryption=derive(nwk,4,b);return s;
}
Bytes encodeJoinAccept11(const JoinAccept &a,const Credentials &c,std::uint16_t nonce,std::uint8_t type) {
    if(c.version!=Version::V1_1 || !a.optNeg)throw Error("LoRaWAN 1.1 requires OptNeg and explicit credentials version");
    (void)prefix(c,nonce,type);
    auto legacy=a;legacy.optNeg=false;
    auto plain=acceptCrypt(encodeJoinAccept(legacy,c.nwkKey),c.nwkKey,false);
    plain.resize(plain.size()-4);plain[11]|=0x80;
    const auto js=deriveJoinServerKeys(c.nwkKey,c.devEui);
    appendMic(plain,js.integrity,prefix(c,nonce,type));
    return acceptCrypt(plain,type==255?c.nwkKey:js.encryption,true);
}
JoinAccept decodeJoinAccept11(const Bytes &wire,const Credentials &c,std::uint16_t nonce,std::uint8_t type) {
    if(c.version!=Version::V1_1)throw Error("Expected explicit LoRaWAN 1.1 credentials");
    const auto js=deriveJoinServerKeys(c.nwkKey,c.devEui);
    auto plain=acceptCrypt(wire,type==255?c.nwkKey:js.encryption,false);
    if(!(plain[11]&0x80))throw Error("LoRaWAN version downgrade refused; provision a 1.0 profile explicitly");
    checkMic(plain,js.integrity,prefix(c,nonce,type));
    plain[11]&=0x7f;plain.resize(plain.size()-4);appendMic(plain,c.nwkKey);
    auto a=decodeJoinAccept(acceptCrypt(plain,c.nwkKey,true),c.nwkKey);a.optNeg=true;return a;
}
Bytes encodeRejoinRequest(const RejoinRequest &r,const Key &key) {
    if(r.type>2 || r.netId>0xffffff)throw Error("Invalid RejoinRequest parameters");
    Bytes b{0xc0,r.type};if(r.type==1)b.insert(b.end(),r.joinEui.rbegin(),r.joinEui.rend());else put(b,r.netId,3);
    b.insert(b.end(),r.devEui.rbegin(),r.devEui.rend());put(b,r.counter,2);appendMic(b,key);return b;
}
RejoinRequest decodeRejoinRequest(const Bytes &b,const Key &key) {
    if(b.size()<2 || b[0]!=0xc0 || b[1]>2 || b.size()!=(b[1]==1?24u:19u))throw Error("Invalid RejoinRequest length/header");
    checkMic(b,key);RejoinRequest r;r.type=b[1];std::size_t at=2;
    if(r.type==1){std::reverse_copy(b.begin()+at,b.begin()+at+8,r.joinEui.begin());at+=8;}
    else {r.netId=get(b,at,3);at+=3;}
    std::reverse_copy(b.begin()+at,b.begin()+at+8,r.devEui.begin());r.counter=std::uint16_t(get(b,at+8,2));return r;
}
}
