#include "meshpki.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <algorithm>
#include <climits>
#include <stdexcept>
namespace obrii::meshtastic {
namespace {
void ok(int n) { if(n!=1)throw std::runtime_error("OpenSSL cryptographic operation failed"); }
int length(std::size_t n) { if(n>1048576)throw std::length_error("Crypto input exceeds 1MiB");return int(n); }
using KeyPtr=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
class OpenSslCrypto final:public CryptoProvider {
public:
    Bytes random(std::size_t n) override { const auto count=length(n);Bytes b(n);if(n)ok(RAND_bytes(b.data(),count));return b; }
    Key32 x25519(const Key32 &priv,const Key32 &pub) override {
        KeyPtr own(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519,nullptr,priv.data(),32),EVP_PKEY_free);
        KeyPtr peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519,nullptr,pub.data(),32),EVP_PKEY_free);
        if(!own||!peer)throw std::runtime_error("OpenSSL X25519 import failed");
        std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new(own.get(),nullptr),EVP_PKEY_CTX_free);
        if(!ctx)throw std::runtime_error("OpenSSL context allocation failed");
        ok(EVP_PKEY_derive_init(ctx.get()));ok(EVP_PKEY_derive_set_peer(ctx.get(),peer.get()));Key32 out{};std::size_t n=32;
        if(EVP_PKEY_derive(ctx.get(),out.data(),&n)!=1||n!=32||std::all_of(out.begin(),out.end(),[](auto b){return b==0;})) {
            secureErase(out.data(),out.size());throw std::invalid_argument("Weak X25519 peer key");
        }
        return out;
    }
    Key32 sha256(const Bytes &input) override {
        Key32 out{};unsigned n=0;ok(EVP_Digest(input.data(),input.size(),out.data(),&n,EVP_sha256(),nullptr));
        if(n!=32)throw std::runtime_error("Invalid SHA256 length");return out;
    }
    Key32 hmacSha256(const Key32 &key,const Bytes &input) override {
        Key32 out{};unsigned n=0;
        if(!HMAC(EVP_sha256(),key.data(),32,input.data(),input.size(),out.data(),&n)||n!=32)throw std::runtime_error("HMAC-SHA256 failed");return out;
    }
    Bytes ccmSeal(const Key32 &key,const Bytes &nonce,const Bytes &plain) override {
        auto out=ccm(key,nonce,plain,{},true);if(!out)throw std::runtime_error("CCM encryption failed");return *out;
    }
    std::optional<Bytes> ccmOpen(const Key32 &key,const Bytes &nonce,const Bytes &cipher) override {
        if(cipher.size()<8)return {};
        return ccm(key,nonce,Bytes(cipher.begin(),cipher.end()-8),Bytes(cipher.end()-8,cipher.end()),false);
    }
private:
    std::optional<Bytes> ccm(const Key32 &key,const Bytes &nonce,const Bytes &input,Bytes tag,bool encrypt) {
        if(nonce.size()!=13||input.size()>65535)throw std::invalid_argument("Invalid CCM nonce/length");
        std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
        if(!ctx)throw std::runtime_error("OpenSSL allocation failed");
        ok(EVP_CipherInit_ex(ctx.get(),EVP_aes_256_ccm(),nullptr,nullptr,nullptr,encrypt));
        ok(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_AEAD_SET_IVLEN,13,nullptr));
        if(encrypt)tag.resize(8);
        ok(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_AEAD_SET_TAG,8,encrypt?nullptr:tag.data()));
        ok(EVP_CipherInit_ex(ctx.get(),nullptr,nullptr,key.data(),nonce.data(),encrypt));int n=0;
        ok(EVP_CipherUpdate(ctx.get(),nullptr,&n,nullptr,length(input.size())));
        Bytes out(std::max<std::size_t>(input.size(),1));const unsigned char dummy=0;
        const auto status=EVP_CipherUpdate(ctx.get(),out.data(),&n,input.empty()?&dummy:input.data(),length(input.size()));
        if(!encrypt&&status!=1) { secureErase(out.data(),out.size());return {}; }
        ok(status);out.resize(n);
        if(encrypt) {
            unsigned char final[16]{};int produced=0;ok(EVP_CipherFinal_ex(ctx.get(),final,&produced));
            if(produced)throw std::runtime_error("Unexpected CCM final output");
            ok(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_AEAD_GET_TAG,8,tag.data()));out.insert(out.end(),tag.begin(),tag.end());
        }
        return out;
    }
};
}
std::shared_ptr<CryptoProvider> platformCrypto() { return std::make_shared<OpenSslCrypto>(); }
}
