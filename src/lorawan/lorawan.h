#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

namespace obrii::lorawan {
using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t,16>;
using Eui = std::array<std::uint8_t,8>; // Canonical display order; reversed on air.
using Block = std::array<std::uint8_t,16>;
struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
enum class Direction : std::uint8_t { Uplink=0, Downlink=1 };
enum class FrameType : std::uint8_t { JoinRequest=0, JoinAccept=1, UnconfirmedUp=2,
    UnconfirmedDown=3, ConfirmedUp=4, ConfirmedDown=5 };
Direction direction(FrameType type);
bool confirmed(FrameType type);
Block aesEncrypt(const Key &key, Block block);
Block aesDecrypt(const Key &key, Block block);
Block cmac(const Key &key, const Bytes &message);
struct SessionKeys { Key network{}, application{}; };
SessionKeys deriveSessionKeys(const Key &appKey, std::uint32_t joinNonce,
                              std::uint32_t netId, std::uint16_t devNonce);
struct JoinRequest { Eui joinEui{}, devEui{}; std::uint16_t devNonce=0; };
struct JoinAccept {
    std::uint32_t joinNonce=0, netId=0, devAddr=0;
    std::uint8_t rx1Offset=0, rx2DataRate=0, rxDelay=1;
    std::optional<std::array<std::uint8_t,16>> cfList;
};
Bytes encodeJoinRequest(const JoinRequest &request, const Key &appKey);
JoinRequest decodeJoinRequest(const Bytes &wire, const Key &appKey);
Bytes encodeJoinAccept(const JoinAccept &accept, const Key &appKey);
JoinAccept decodeJoinAccept(const Bytes &wire, const Key &appKey);
struct DataFrame {
    FrameType type=FrameType::UnconfirmedUp;
    std::uint32_t devAddr=0, counter=0;
    std::uint8_t control=0; // High FCtrl bits only: ADR=0x80, ACK=0x20, etc.
    Bytes options;
    std::optional<std::uint8_t> port;
    Bytes payload; // Plaintext. FPort=0 uses NwkSKey; all other ports AppSKey.
};
Bytes encodeData(const DataFrame &frame, const SessionKeys &keys);
// Caller supplies the full 32-bit FCnt; low 16 bits MUST match the wire.
DataFrame decodeData(const Bytes &wire, const SessionKeys &keys, std::uint32_t counter);
std::uint16_t wireCounter(const Bytes &wire);
std::uint32_t wireAddress(const Bytes &wire);

enum class Role { Device, Network };
struct SessionState {
    Role role=Role::Device;
    std::uint32_t devAddr=0;
    SessionKeys keys;
    // uint64_t represents the exhausted 2^32 sentinel without wrapping.
    std::uint64_t nextTransmit=0;
    std::optional<std::uint32_t> lastReceived;
    Bytes lastUplink; // Network-side deduplication, persisted across restart.
    Bytes lastTransmitted;
    std::uint8_t uplinkCopies=0, maxUplinkCopies=1;
    std::uint8_t transmitCopies=0;
    bool mayRepeat=false;
    bool acknowledgmentPending=false;
};
struct Received { DataFrame frame; bool duplicate=false; };
class Session {
public:
    using Persist = std::function<void(const SessionState &)>;
    // Persist MUST durably commit or throw, before TX bytes / RX payload escape.
    // One owner per session. The host provides storage and exclusive ownership.
    Session(Role role, SessionState restored, Persist persist);
    Session(const Session &)=delete;
    Session &operator=(const Session &)=delete;
    const SessionState &state() const { return state_; }
    Bytes transmit(DataFrame frame); // Assigns DevAddr/FCnt and pending ACK.
    Bytes repeatUplink(); // Exact bytes/FCnt, bounded by maxUplinkCopies (NbTrans).
    void setNbTrans(std::uint8_t copies);
    Received receive(const Bytes &wire, std::optional<std::uint32_t> counterHint={});
private:
    void commit(const SessionState &next);
    Role role_;
    SessionState state_;
    Persist persist_;
    bool busy_=false, failed_=false;
};
struct Credentials { Eui joinEui{}, devEui{}; Key appKey{}; };
struct DeviceJoinState {
    std::uint32_t nextDevNonce=0; // Exhausted at 65536; must never wrap.
    std::optional<std::uint16_t> pendingDevNonce;
    std::optional<std::uint32_t> lastJoinNonce;
    std::optional<SessionState> activated;
};
class OtaaDevice {
public:
    using Persist = std::function<void(const DeviceJoinState &)>;
    OtaaDevice(Credentials credentials, DeviceJoinState restored, Persist persist,
               bool requireIncreasingJoinNonce=true);
    OtaaDevice(const OtaaDevice &)=delete;
    OtaaDevice &operator=(const OtaaDevice &)=delete;
    Bytes request();
    JoinAccept accept(const Bytes &wire, const std::function<void(const JoinAccept &)> &validateParameters={});
    // Bind Session's persistence callback here to keep activation and live
    // counters in one atomic DeviceJoinState record, including across reboot.
    void updateSession(const SessionState &state);
    const DeviceJoinState &state() const { return state_; }
private:
    Credentials credentials_;
    DeviceJoinState state_;
    Persist persist_;
    bool increasing_;
    bool busy_=false, failed_=false;
};
struct JoinServerState {
    std::uint32_t nextJoinNonce=0;
    std::optional<std::uint16_t> lastDevNonce;
    std::optional<SessionState> activated;
};
struct JoinResponse { Bytes wire; JoinAccept parameters; SessionState session; };
class JoinServer {
public:
    using Persist = std::function<void(const JoinServerState &)>;
    JoinServer(Credentials provisioned, JoinServerState restored, Persist persist);
    JoinServer(const JoinServer &)=delete;
    JoinServer &operator=(const JoinServer &)=delete;
    // Network ID/address/region parameters are supplied by the network host.
    JoinResponse accept(const Bytes &request, JoinAccept parameters);
    void updateSession(const SessionState &state);
    const JoinServerState &state() const { return state_; }
private:
    Credentials credentials_;
    JoinServerState state_;
    Persist persist_;
    bool busy_=false, failed_=false;
};

// Versioned bounded state blobs with CRC32 corruption checks, not encryption.
// Store privately and atomically. Restoring old snapshots can reuse counters.
Bytes saveSession(const SessionState &state);
SessionState loadSession(const Bytes &blob);
Bytes saveDeviceJoin(const DeviceJoinState &state);
DeviceJoinState loadDeviceJoin(const Bytes &blob);
Bytes saveJoinServer(const JoinServerState &state);
JoinServerState loadJoinServer(const Bytes &blob);
}
