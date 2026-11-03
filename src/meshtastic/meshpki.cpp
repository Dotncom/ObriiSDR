#include "meshpki.h"
#include <algorithm>
#include <stdexcept>
namespace obrii::meshtastic {
namespace {
struct WipeBytes { Bytes &bytes;~WipeBytes(){secureErase(bytes.data(),bytes.size());} };
struct WipeKey { Key32 &key;~WipeKey(){secureErase(key.data(),key.size());} };
}
void secureErase(void *data,std::size_t size) {
    volatile auto *p=static_cast<volatile unsigned char *>(data);while(size--)*p++=0;
}
bool constantTimeEqual(const Bytes &a,const Bytes &b) {
    if(a.size()!=b.size())return false;unsigned diff=0;
    for(std::size_t i=0;i<a.size();++i)diff|=a[i]^b[i];return diff==0;
}
bool constantTimeEqual(const Key32 &a,const Key32 &b) {
    unsigned diff=0;for(std::size_t i=0;i<a.size();++i)diff|=a[i]^b[i];return diff==0;
}
Key32 generatePrivateKey(CryptoProvider &p) {
    auto random=p.random(32);if(random.size()!=32)throw std::runtime_error("Crypto RNG failed");
    Key32 key{};std::copy(random.begin(),random.end(),key.begin());secureErase(random.data(),random.size());
    key[0]&=248;key[31]=(key[31]&127)|64;return key;
}
Key32 publicKey(CryptoProvider &p,const Key32 &key) { Key32 base{};base[0]=9;return p.x25519(key,base); }
PkiContext::PkiContext(std::shared_ptr<CryptoProvider> p,const Key32 &key,std::size_t maxPeers)
    :provider_(std::move(p)),private_(key),maxPeers_(maxPeers) {
    if(!provider_||!maxPeers||maxPeers>4096) { secureErase(private_.data(),private_.size());throw std::invalid_argument("Invalid PKI provider/peer limit"); }
    if(std::all_of(private_.begin(),private_.end(),[](auto b){return b==0;}))throw std::invalid_argument("Zero PKI private key");
    try { public_=obrii::meshtastic::publicKey(*provider_,private_); }
    catch(...) { secureErase(private_.data(),private_.size());throw; }
}
PkiContext::~PkiContext() { secureErase(private_.data(),private_.size()); }
void PkiContext::pinPeer(std::uint32_t id,const Key32 &key) {
    if(id<=3||id==Broadcast)throw std::invalid_argument("Invalid PKI peer ID");
    const auto existing=peers_.find(id);
    if(existing!=peers_.end()) {
        if(!constantTimeEqual(existing->second,key))throw std::invalid_argument("PKI peer key changed; remove the old pin explicitly");
        return;
    }
    if(peers_.size()>=maxPeers_)throw std::length_error("PKI peer table full");
    // Provider rejects low-order/all-zero shared secrets before trusting a pin.
    auto shared=provider_->x25519(private_,key);secureErase(shared.data(),shared.size());
    peers_.emplace(id,key);
}
void PkiContext::removePeer(std::uint32_t id) { peers_.erase(id); }
std::optional<Key32> PkiContext::peerKey(std::uint32_t id) const {
    const auto it=peers_.find(id);if(it==peers_.end())return {};return it->second;
}
Key32 PkiContext::sharedKey(std::uint32_t peer) const {
    const auto key=peerKey(peer);if(!key)throw std::invalid_argument("PKI peer has no pinned public key");
    auto shared=provider_->x25519(private_,*key);WipeKey wipeShared{shared};Bytes input(shared.begin(),shared.end());WipeBytes wipeInput{input};
    return provider_->sha256(input);
}
namespace {
Bytes pkiNonce(const Header &h,const Bytes &extra) {
    if(extra.size()!=4)throw std::invalid_argument("PKI extra nonce must have four bytes");
    const auto n=nonce(h.from,h.id);Bytes out(n.begin(),n.begin()+13);
    std::copy(extra.begin(),extra.end(),out.begin()+4);return out;
}
void append32(Bytes &out,std::uint32_t n) { for(int i=0;i<4;++i)out.push_back(std::uint8_t(n>>(8*i))); }
}
Bytes PkiContext::seal(Header h,const Data &data) const {
    if(h.to==Broadcast||h.to<=3||!data.port||data.port>511)throw std::invalid_argument("PKI requires a unicast application packet");
    auto plain=encodeData(data);WipeBytes wipePlain{plain};if(plain.size()>DataMtu-12)throw std::length_error("PKI plaintext exceeds 227-byte MTU");
    auto key=sharedKey(h.to);WipeKey wipeKey{key};const auto extra=provider_->random(4);h.channelHash=0;
    Bytes body;
    body=provider_->ccmSeal(key,pkiNonce(h,extra),plain);
    if(body.size()!=plain.size()+8)throw std::runtime_error("PKI AEAD provider returned an invalid length");
    body.insert(body.end(),extra.begin(),extra.end());return encodeRadio({h,std::move(body)});
}
std::optional<Decoded> PkiContext::open(const RadioPacket &packet,std::uint32_t local) const {
    const auto &h=packet.header;
    if(h.to!=local||h.to==Broadcast||h.channelHash||packet.body.size()<14||packet.body.size()>DataMtu)return {};
    const auto peer=peerKey(h.from);if(!peer)return {};
    auto key=sharedKey(h.from);std::optional<Bytes> plain;
    try {
        const Bytes extra(packet.body.end()-4,packet.body.end());
        plain=provider_->ccmOpen(key,pkiNonce(h,extra),Bytes(packet.body.begin(),packet.body.end()-4));
        secureErase(key.data(),key.size());
    } catch(...) { secureErase(key.data(),key.size());throw; }
    if(!plain)return {};
    try {
        auto data=decodeData(*plain);secureErase(plain->data(),plain->size());
        if(!data.port||data.port>511)return {};
        return Decoded{packet,std::move(data),0,Security::Pki,*peer};
    } catch(const std::invalid_argument &) { secureErase(plain->data(),plain->size());return {}; }
}
Bytes PkiContext::ackProof(std::uint32_t peer,std::uint32_t from,std::uint32_t to,std::uint32_t request,const Routing &routing) const {
    auto r=routing;r.ackProof.clear();Bytes input{'a','c','k'};append32(input,from);append32(input,to);append32(input,request);
    const auto encoded=encodeRouting(r);input.insert(input.end(),encoded.begin(),encoded.end());auto key=sharedKey(peer);
    try {
        auto mac=provider_->hmacSha256(key,input);secureErase(key.data(),key.size());
        Bytes proof(mac.begin(),mac.begin()+8);secureErase(mac.data(),mac.size());return proof;
    } catch(...) { secureErase(key.data(),key.size());throw; }
}
bool PkiContext::verifyAckProof(std::uint32_t peer,std::uint32_t from,std::uint32_t to,std::uint32_t id,const Routing &r) const {
    if(r.ackProof.size()!=8||!peerKey(peer))return false;
    return constantTimeEqual(r.ackProof,ackProof(peer,from,to,id,r));
}
}
