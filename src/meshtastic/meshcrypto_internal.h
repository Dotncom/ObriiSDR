#pragma once
#include "meshtastic.h"
namespace obrii::meshtastic::detail {
void aes128Ctr(const Bytes &key, const std::array<std::uint8_t,16> &iv, Bytes &body);
void aes256Ctr(const Bytes &key, const std::array<std::uint8_t,16> &iv, Bytes &body);
}
