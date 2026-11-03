#include "lorawan.h"
#include <algorithm>
#include <cstring>
#undef AES256
#undef AES192
#undef CBC
#undef CTR
#undef ECB
#define AES256 0
#define AES192 0
#define CBC 0
#define CTR 0
#define ECB 1
namespace obrii::lorawan::detail::aes {
#include "../../third_party/tiny_aes/aes.c"
}
namespace obrii::lorawan {
namespace {
void wipe(void *memory, std::size_t count) {
    auto *p=static_cast<volatile std::uint8_t *>(memory);
    while(count--) *p++=0;
}
Block doubled(const Block &input) {
    Block output{};
    for(std::size_t i=0; i<16; ++i)
        output[i]=std::uint8_t((input[i]<<1) | (i+1<16 ? input[i+1]>>7 : 0));
    if(input[0]&0x80) output[15]^=0x87;
    return output;
}
}
Block aesEncrypt(const Key &key, Block block) {
    detail::aes::AES_ctx context{};
    detail::aes::AES_init_ctx(&context,key.data());
    detail::aes::AES_ECB_encrypt(&context,block.data());
    wipe(&context,sizeof(context)); return block;
}
Block aesDecrypt(const Key &key, Block block) {
    detail::aes::AES_ctx context{};
    detail::aes::AES_init_ctx(&context,key.data());
    detail::aes::AES_ECB_decrypt(&context,block.data());
    wipe(&context,sizeof(context)); return block;
}
Block cmac(const Key &key, const Bytes &message) {
    if(message.size()>4096) throw Error("CMAC input exceeds 4096 bytes");
    const auto k1=doubled(aesEncrypt(key,{})), k2=doubled(k1);
    const std::size_t blocks=std::max<std::size_t>(1,(message.size()+15)/16);
    Block value{};
    for(std::size_t i=0; i<blocks; ++i) {
        Block block{};
        const auto offset=i*16;
        const auto size=offset<message.size() ? std::min<std::size_t>(16,message.size()-offset) : 0;
        if(size) std::copy_n(message.data()+offset,size,block.data());
        if(i+1==blocks) {
            const auto &subkey=size==16 ? k1:k2;
            if(size!=16) block[size]=0x80;
            for(std::size_t j=0; j<16; ++j) block[j]^=subkey[j];
        }
        for(std::size_t j=0; j<16; ++j) block[j]^=value[j];
        value=aesEncrypt(key,block);
    }
    return value;
}
}
