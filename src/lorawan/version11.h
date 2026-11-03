#pragma once
#include "lorawan.h"

namespace obrii::lorawan {
struct JoinServerKeys { Key encryption{}, integrity{}; };
JoinServerKeys deriveJoinServerKeys(const Key &nwkKey, const Eui &devEui);
SessionKeys deriveSessionKeys11(const Key &nwkKey, const Key &appKey,
    std::uint32_t joinNonce, const Eui &joinEui, std::uint16_t devNonce);
// requestType=255 for JoinRequest, 0/1/2 for RejoinRequest. The nonce is
// DevNonce or RJcount as appropriate. Rejoin accepts use JSEncKey.
Bytes encodeJoinAccept11(const JoinAccept &, const Credentials &, std::uint16_t nonce,
                         std::uint8_t requestType=255);
JoinAccept decodeJoinAccept11(const Bytes &, const Credentials &, std::uint16_t nonce,
                              std::uint8_t requestType=255);
struct RejoinRequest {
    std::uint8_t type=0;
    std::uint32_t netId=0;
    Eui joinEui{}, devEui{};
    std::uint16_t counter=0;
};
// Type 1 uses JSIntKey; types 0/2 use the current SNwkSIntKey.
Bytes encodeRejoinRequest(const RejoinRequest &, const Key &integrityKey);
RejoinRequest decodeRejoinRequest(const Bytes &, const Key &integrityKey);
}
