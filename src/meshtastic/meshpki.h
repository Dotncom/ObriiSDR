#pragma once
#include "meshtastic.h"
#include <map>
namespace obrii::meshtastic {
// Cryptographic primitives are provided by Windows CNG or OpenSSL EVP, not a
// local implementation of elliptic-curve/AES-CCM mathematics.
class CryptoProvider {
public:
    virtual ~CryptoProvider() = default;
    virtual Bytes random(std::size_t size) = 0;
    virtual Key32 x25519(const Key32 &privateKey,const Key32 &publicKey) = 0;
    virtual Key32 sha256(const Bytes &input) = 0;
    virtual Key32 hmacSha256(const Key32 &key,const Bytes &input) = 0;
    // Seal returns ciphertext followed by an 8-byte tag. Nonce must be 13 bytes.
    virtual Bytes ccmSeal(const Key32 &key,const Bytes &nonce,const Bytes &plain) = 0;
    virtual std::optional<Bytes> ccmOpen(const Key32 &key,const Bytes &nonce,const Bytes &cipherAndTag) = 0;
};
std::shared_ptr<CryptoProvider> platformCrypto();
Key32 generatePrivateKey(CryptoProvider &provider);
Key32 publicKey(CryptoProvider &provider,const Key32 &privateKey);
bool constantTimeEqual(const Bytes &a,const Bytes &b);
bool constantTimeEqual(const Key32 &a,const Key32 &b);
void secureErase(void *data,std::size_t size);

class PkiContext {
public:
    PkiContext(std::shared_ptr<CryptoProvider> provider,const Key32 &privateKey,std::size_t maxPeers=1024);
    ~PkiContext();
    PkiContext(const PkiContext &)=delete;
    PkiContext &operator=(const PkiContext &)=delete;
    const Key32 &publicKey() const { return public_; }
    CryptoProvider &crypto() const { return *provider_; }
    // Trust is explicit. NodeInfo never silently changes these pins.
    void pinPeer(std::uint32_t nodeId,const Key32 &key);
    void removePeer(std::uint32_t nodeId);
    std::optional<Key32> peerKey(std::uint32_t nodeId) const;
    Bytes seal(Header header,const Data &data) const;
    std::optional<Decoded> open(const RadioPacket &packet,std::uint32_t localNodeId) const;
    Bytes ackProof(std::uint32_t peer,std::uint32_t from,std::uint32_t to,std::uint32_t requestId,const Routing &routing) const;
    bool verifyAckProof(std::uint32_t peer,std::uint32_t from,std::uint32_t to,std::uint32_t requestId,const Routing &routing) const;
private:
    Key32 sharedKey(std::uint32_t peer) const;
    std::shared_ptr<CryptoProvider> provider_;
    Key32 private_{},public_{};
    std::map<std::uint32_t,Key32> peers_;
    std::size_t maxPeers_;
};
}
