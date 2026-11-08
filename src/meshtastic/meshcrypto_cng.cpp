#include "meshpki.h"
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace obrii::meshtastic {
namespace {
void ok(NTSTATUS status) { if(status<0)throw std::runtime_error("Windows CNG cryptographic operation failed"); }
struct Alg {
    BCRYPT_ALG_HANDLE h=nullptr;
    explicit Alg(LPCWSTR name,ULONG flags=0) { ok(BCryptOpenAlgorithmProvider(&h,name,nullptr,flags)); }
    ~Alg() { if(h)BCryptCloseAlgorithmProvider(h,0); }
};
struct Key { BCRYPT_KEY_HANDLE h=nullptr;~Key(){if(h)BCryptDestroyKey(h);} };
struct Secret { BCRYPT_SECRET_HANDLE h=nullptr;~Secret(){if(h)BCryptDestroySecret(h);} };
struct Hash { BCRYPT_HASH_HANDLE h=nullptr;~Hash(){if(h)BCryptDestroyHash(h);} };
struct Sensitive {
    Bytes b;explicit Sensitive(std::size_t size):b(size){}~Sensitive(){secureErase(b.data(),b.size());}
};
ULONG length(std::size_t n) { if(n>1048576)throw std::length_error("Crypto input exceeds 1MiB");return ULONG(n); }
ULONG objectSize(BCRYPT_ALG_HANDLE h) {
    ULONG n=0,written=0;ok(BCryptGetProperty(h,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&n),sizeof(n),&written,0));return n;
}
Key32 hash(const Bytes &input,const Key32 *key) {
    Alg alg(BCRYPT_SHA256_ALGORITHM,key?BCRYPT_ALG_HANDLE_HMAC_FLAG:0);Sensitive object(objectSize(alg.h));Hash h;
    ok(BCryptCreateHash(alg.h,&h.h,object.b.data(),length(object.b.size()),key?const_cast<PUCHAR>(key->data()):nullptr,key?32:0,0));
    ok(BCryptHashData(h.h,const_cast<PUCHAR>(input.data()),length(input.size()),0));
    Key32 result{};ok(BCryptFinishHash(h.h,result.data(),32,0));return result;
}
class CngCrypto final:public CryptoProvider {
public:
    Bytes random(std::size_t size) override {
        const auto n=length(size);Bytes out(size);if(size)ok(BCryptGenRandom(nullptr,out.data(),n,BCRYPT_USE_SYSTEM_PREFERRED_RNG));return out;
    }
    Key32 x25519(const Key32 &privateKey,const Key32 &publicKey) override {
        Alg alg(BCRYPT_ECDH_ALGORITHM);
        const wchar_t curve[]=BCRYPT_ECC_CURVE_25519;
        ok(BCryptSetProperty(alg.h,BCRYPT_ECC_CURVE_NAME,reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(curve)),sizeof(curve),0));
        Sensitive local(sizeof(BCRYPT_ECCKEY_BLOB)+96);Bytes remote(sizeof(BCRYPT_ECCKEY_BLOB)+64);
        BCRYPT_ECCKEY_BLOB priv{BCRYPT_ECDH_PRIVATE_GENERIC_MAGIC,32},pub{BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC,32};
        std::memcpy(local.b.data(),&priv,sizeof(priv));std::memcpy(remote.data(),&pub,sizeof(pub));
        // CNG uses the scalar in a private blob for ECDH. The basepoint public
        // coordinate is also used when deriving the true public key via ECDH(9).
        local.b[sizeof(priv)]=9;
        std::copy(privateKey.begin(),privateKey.end(),local.b.begin()+sizeof(priv)+64);
        local.b[sizeof(priv)+64]&=248;local.b.back()=(local.b.back()&127)|64;
        std::copy(publicKey.begin(),publicKey.end(),remote.begin()+sizeof(pub));remote[sizeof(pub)+31]&=127;
        Key own,peer;ok(BCryptImportKeyPair(alg.h,nullptr,BCRYPT_ECCPRIVATE_BLOB,&own.h,local.b.data(),length(local.b.size()),0));
        ok(BCryptImportKeyPair(alg.h,nullptr,BCRYPT_ECCPUBLIC_BLOB,&peer.h,remote.data(),length(remote.size()),0));
        Secret secret;ok(BCryptSecretAgreement(own.h,peer.h,&secret.h,0));Key32 result{};ULONG written=0;
        ok(BCryptDeriveKey(secret.h,BCRYPT_KDF_RAW_SECRET,nullptr,result.data(),32,&written,0));
        // CNG exports raw ECDH in reverse order relative to RFC7748 X25519.
        std::reverse(result.begin(),result.end());
        if(written!=32||std::all_of(result.begin(),result.end(),[](auto b){return b==0;})) {
            secureErase(result.data(),result.size());throw std::invalid_argument("Weak X25519 peer key");
        }
        return result;
    }
    Key32 sha256(const Bytes &input) override { return hash(input,nullptr); }
    Key32 hmacSha256(const Key32 &key,const Bytes &input) override { return hash(input,&key); }
    Bytes ccmSeal(const Key32 &key,const Bytes &nonce,const Bytes &plain) override {
        auto result=ccm(key,nonce,plain,{},true);if(!result)throw std::runtime_error("CNG CCM encryption failed");return *result;
    }
    std::optional<Bytes> ccmOpen(const Key32 &key,const Bytes &nonce,const Bytes &cipher) override {
        if(cipher.size()<8)return {};
        return ccm(key,nonce,Bytes(cipher.begin(),cipher.end()-8),Bytes(cipher.end()-8,cipher.end()),false);
    }
private:
    std::optional<Bytes> ccm(const Key32 &key,const Bytes &nonce,const Bytes &input,Bytes tag,bool encrypt) {
        if(nonce.size()!=13||input.size()>65535||(!encrypt&&tag.size()!=8))throw std::invalid_argument("Invalid CCM nonce/tag/length");
        if(encrypt)tag.resize(8);
        Alg alg(BCRYPT_AES_ALGORITHM);const wchar_t mode[]=BCRYPT_CHAIN_MODE_CCM;
        ok(BCryptSetProperty(alg.h,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(mode)),sizeof(mode),0));
        Sensitive object(objectSize(alg.h));Key aes;
        ok(BCryptGenerateSymmetricKey(alg.h,&aes.h,object.b.data(),length(object.b.size()),const_cast<PUCHAR>(key.data()),32,0));
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce=const_cast<PUCHAR>(nonce.data());info.cbNonce=13;info.pbTag=tag.data();info.cbTag=8;
        Bytes result(std::max<std::size_t>(input.size(),1));ULONG written=0;
        const auto status=encrypt?
            BCryptEncrypt(aes.h,const_cast<PUCHAR>(input.data()),length(input.size()),&info,nullptr,0,result.data(),length(result.size()),&written,0):
            BCryptDecrypt(aes.h,const_cast<PUCHAR>(input.data()),length(input.size()),&info,nullptr,0,result.data(),length(result.size()),&written,0);
        if(!encrypt&&status==NTSTATUS(0xc000a002)) { secureErase(result.data(),result.size());return {}; }
        if(status<0) { secureErase(result.data(),result.size());ok(status); }
        if(written!=input.size())throw std::runtime_error("Invalid CCM output length");
        result.resize(written);if(encrypt)result.insert(result.end(),tag.begin(),tag.end());return result;
    }
};
}
std::shared_ptr<CryptoProvider> platformCrypto() { return std::make_shared<CngCrypto>(); }
}
