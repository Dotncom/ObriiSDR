#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Independent implementation of the public Meshtastic PSK/PKI radio format.
// See docs/meshtastic_core.md for the supported subset and integration contract.
namespace obrii::meshtastic {
using Bytes = std::vector<std::uint8_t>;
using Key32 = std::array<std::uint8_t,32>;
class PkiContext;
enum class Security { Psk, Pki };
constexpr std::uint32_t Broadcast = 0xffffffffu;
constexpr std::size_t RadioMtu = 255, RadioHeaderBytes = 16, DataMtu = 239;
constexpr std::uint32_t TextPort = 1, PositionPort = 3, NodeInfoPort = 4, RoutingPort = 5, AdminPort = 6;

struct Header {
    std::uint32_t to = Broadcast, from = 0, id = 0;
    std::uint8_t hopLimit = 3, hopStart = 3;
    bool wantAck = false, viaMqtt = false;
    std::uint8_t channelHash = 0, nextHop = 0, relayNode = 0;
};
struct RadioPacket { Header header; Bytes body; };
Bytes encodeRadio(const RadioPacket &packet);
RadioPacket decodeRadio(const Bytes &bytes);
std::uint8_t relayId(std::uint32_t nodeId); // low byte; 0 maps to 0xff (0 means no preference)

struct Data {
    std::uint32_t port = 0;
    Bytes payload;
    bool wantResponse = false;
    std::uint32_t dest = 0, source = 0, requestId = 0, replyId = 0, emoji = 0;
    std::optional<std::uint32_t> bitfield;
    Bytes signature, unknownFields;
};
Bytes encodeData(const Data &message);
Data decodeData(const Bytes &protobuf);
Data textMessage(const std::string &utf8);
std::string text(const Data &message); // validates port and UTF-8; no UI escaping

struct User {
    std::string id, longName, shortName;
    Bytes macAddress, publicKey, unknownFields;
    std::uint32_t hardwareModel = 0, role = 0;
    bool licensed = false;
    std::optional<bool> unmessagable;
};
Bytes encodeUser(const User &user);
User decodeUser(const Bytes &protobuf);
struct Position {
    std::optional<std::int32_t> latitudeE7, longitudeE7, altitudeMeters;
    std::uint32_t time = 0;
    Bytes unknownFields;
};
Bytes encodePosition(const Position &position);
Position decodePosition(const Bytes &protobuf);
struct Routing {
    enum class Kind { None, Request, Reply, Error } kind = Kind::None;
    std::uint32_t error = 0;
    Bytes route, ackProof, unknownFields;
};
Bytes encodeRouting(const Routing &routing);
Routing decodeRouting(const Bytes &protobuf);

struct Channel {
    std::string name = "LongFast"; // resolved effective name, not a local display alias
    Bytes psk{1}; // Meshtastic short-key convention; {0} explicitly means no encryption
    bool enabled = true;
    Bytes effectiveKey() const; // expands short PSK and pads partial keys as firmware does
    std::uint8_t hash() const;
    void validate() const;
};
std::array<std::uint8_t,16> nonce(std::uint32_t from, std::uint64_t packetId);
Bytes cryptPayload(const Bytes &effectiveKey, std::uint32_t from, std::uint64_t packetId, const Bytes &body);
Bytes seal(Header header, const Data &data, const Channel &channel);
struct Decoded {
    RadioPacket radio;
    Data data;
    std::size_t channelIndex = 0;
    Security security = Security::Psk;
    std::optional<Key32> senderPublicKey;
    // PSK AES-CTR provides confidentiality, not sender authentication.
    // Syntactic protobuf success is not proof that the key or sender is genuine.
};
enum class DecodeStatus { Decoded, UnknownChannel, InvalidPayload, AmbiguousChannel };
struct DecodeResult {
    DecodeStatus status = DecodeStatus::UnknownChannel;
    std::optional<Decoded> packet;
};
DecodeResult open(const RadioPacket &radio, const std::vector<Channel> &channels);

// Host must persist nextValue() BEFORE releasing a packet to TX. Never reset this
// sequence under the same sender ID/key; repeating the nonce compromises CTR.
class PacketIdSequence {
public:
    explicit PacketIdSequence(std::uint64_t next);
    std::uint32_t next();
    std::uint64_t nextValue() const { return next_; }
private:
    std::uint64_t next_;
};

enum class RelayMode { Off, Client, Router };
struct NodeConfig {
    std::uint32_t nodeId = 0;
    std::vector<Channel> channels{Channel{}};
    RelayMode relay = RelayMode::Client;
    bool relayUnknownChannels = false;
    bool relayPki = true;
    std::shared_ptr<PkiContext> pki;
    std::uint8_t defaultHopLimit = 3;
    std::uint32_t relayDelayMs = 150, relayJitterMs = 300;
    std::uint32_t ackTimeoutMs = 30000, retryDelayMs = 500;
    int maxRetries = 3;
    std::uint32_t historyTtlMs = 900000;
    std::size_t maxHistory = 2048, maxNodes = 256, maxQueue = 64, maxPending = 16, maxEvents = 128;
    void validate() const;
};
enum class TxKind { Original, Relay, Ack, Retry };
struct Transmission {
    std::uint64_t ticket = 0;
    TxKind kind = TxKind::Original;
    Bytes radioBytes;
};
enum class EventKind {
    Message, Observed, Duplicate, Rejected, UnknownChannel, AmbiguousChannel,
    Sent, Relayed, Acknowledged, NegativeAck, Retrying, TimedOut, TxFailed,
    QueueFull, IdentityConflict, Cancelled
};
struct Event {
    EventKind kind = EventKind::Observed;
    Header header;
    std::optional<Data> data;
    std::optional<std::size_t> channelIndex;
    std::uint32_t requestId = 0, routingError = 0;
    std::string detail;
    Security security = Security::Psk;
    bool authenticated = false;
    std::optional<Key32> senderPublicKey;
};
struct NodeRecord {
    std::uint32_t id = 0;
    std::uint64_t lastSeenMs = 0;
    std::optional<User> user;
    std::optional<Position> position;
};

// No I/O, thread, wall clock or automatic RF transmission. Supply monotonic ms.
// poll() hands out at most one TX; completeTransmission() MUST be called after
// the radio finishes/fails it. Retry timers begin only after actual TX success.
// Bounded managed flooding with PSK/PKI ACK/retry. Learned next-hop routing and
// firmware role parity are separate. Unknown app payloads remain bytes.
class Node {
public:
    using NextPacketId = std::function<std::uint32_t()>;
    Node(NodeConfig config, NextPacketId ids);
    ~Node();
    Node(Node &&) noexcept;
    Node &operator=(Node &&) noexcept;
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;
    std::uint32_t send(const Data &data, std::size_t channel, std::uint32_t to,
                       bool reliable, std::uint64_t nowMs);
    std::uint32_t sendPki(const Data &data, std::uint32_t to, bool reliable, std::uint64_t nowMs);
    void receive(const Bytes &radioBytes, std::uint64_t nowMs);
    std::optional<Transmission> poll(std::uint64_t nowMs, bool channelBusy = false);
    void completeTransmission(std::uint64_t ticket, bool sent, std::uint64_t nowMs);
    void cancel(std::uint32_t packetId, std::uint64_t nowMs);
    std::vector<Event> takeEvents();
    std::vector<NodeRecord> nodes() const;
    std::size_t queuedCount() const;
    std::size_t pendingCount() const;
    std::uint32_t nodeId() const;
    std::uint64_t droppedEventCount() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Meshtastic serial/TCP API envelope only (94 c3, big-endian uint16 length).
// Contents remain opaque ToRadio/FromRadio protobufs; no device I/O or config.
class ApiFramer {
public:
    explicit ApiFramer(std::size_t maxPayload = 512);
    Bytes encode(const Bytes &protobuf) const;
    std::vector<Bytes> push(const std::uint8_t *bytes, std::size_t count);
    std::vector<Bytes> push(const Bytes &bytes) { return push(bytes.data(),bytes.size()); }
    void reset() { buffer_.clear(); }
    std::size_t bufferedBytes() const { return buffer_.size(); }
private:
    std::size_t limit_;
    Bytes buffer_;
};
}
