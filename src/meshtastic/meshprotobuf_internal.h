#pragma once
#include <string>
#include "meshtastic.h"
namespace obrii::meshtastic::detail {
bool validUtf8(const std::string &text);
struct ProtoField { unsigned tag=0,wire=0;std::uint64_t value=0;Bytes bytes; };
std::vector<ProtoField> protoFields(const Bytes &bytes);
Bytes protoEncode(const std::vector<ProtoField> &fields);
}
