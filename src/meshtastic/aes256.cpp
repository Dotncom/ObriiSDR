#include "meshcrypto_internal.h"
#include <cstring>
#include <stdexcept>
#undef AES256
#undef AES192
#define AES192 0
#undef CBC
#undef ECB
#undef CTR
#define CBC 0
#define ECB 0
#define CTR 1
#define AES256 1
namespace obrii::meshtastic::detail::aes256 {
#include "../../third_party/tiny_aes/aes.c"
}
namespace obrii::meshtastic::detail {
void aes256Ctr(const Bytes &key, const std::array<std::uint8_t,16> &iv, Bytes &body) {
    if(key.size()!=32)throw std::invalid_argument("AES256 key must have 32 bytes");
    aes256::AES_ctx ctx{};
    aes256::AES_init_ctx_iv(&ctx,key.data(),iv.data());
    aes256::AES_CTR_xcrypt_buffer(&ctx,body.data(),body.size());
    volatile auto *p=reinterpret_cast<volatile unsigned char *>(&ctx);
    for(std::size_t i=0;i<sizeof(ctx);++i)p[i]=0;
}
}
