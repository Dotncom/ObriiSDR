#pragma once
#include "meshpki.h"
#include <map>
#include <set>
namespace obrii::meshtastic {
enum class AdminTag:std::uint32_t {
    GetChannel=1,ChannelResponse=2,GetOwner=3,OwnerResponse=4,GetConfig=5,ConfigResponse=6,
    GetModuleConfig=7,ModuleConfigResponse=8,GetCanned=10,CannedResponse=11,GetMetadata=12,MetadataResponse=13,
    GetRingtone=14,RingtoneResponse=15,GetConnectionStatus=16,ConnectionStatusResponse=17,
    SetHamMode=18,GetRemotePins=19,RemotePinsResponse=20,EnterDfu=21,DeleteFile=22,SetScale=23,
    Backup=24,Restore=25,RemoveBackup=26,InputEvent=27,SetOwner=32,SetChannel=33,SetConfig=34,
    SetModuleConfig=35,SetCanned=36,SetRingtone=37,RemoveNode=38,SetFavorite=39,RemoveFavorite=40,
    SetPosition=41,RemovePosition=42,SetTime=43,GetUiConfig=44,UiConfigResponse=45,StoreUiConfig=46,
    SetIgnored=47,RemoveIgnored=48,ToggleMuted=49,GetDisplayFrame=50,SetDisplayMirror=51,
    BeginEdit=64,CommitEdit=65,AddContact=66,KeyVerification=67,FactoryResetDevice=94,
    RebootOta=95,ExitSimulator=96,Reboot=97,Shutdown=98,FactoryResetConfig=99,ResetNodeDb=100,
    OtaRequest=102,SensorConfig=103,LockdownAuth=104
};
enum class AdminConfigType:std::uint32_t {
    Device=0,Position=1,Power=2,Network=3,Display=4,LoRa=5,Bluetooth=6,Security=7,SessionKey=8,DeviceUi=9
};
struct AdminMessage {
    AdminTag tag=AdminTag::GetOwner;
    std::uint64_t number=1;
    Bytes payload; // nested protobuf or UTF-8 bytes, depending on tag
    Bytes sessionPasskey;
    Bytes unknownFields;
};
AdminMessage adminNumber(AdminTag tag,std::uint64_t value=1);
AdminMessage adminSigned(AdminTag tag,std::int32_t value);
std::int32_t adminSignedValue(const AdminMessage &message);
AdminMessage adminGetConfig(AdminConfigType type);
AdminMessage adminGetChannel(std::uint32_t zeroBasedIndex);
AdminMessage adminPayload(AdminTag tag,Bytes payload);
Bytes encodeAdmin(const AdminMessage &message);
AdminMessage decodeAdmin(const Bytes &protobuf);
bool adminIsRequest(AdminTag tag);
bool adminIsResponse(AdminTag tag);
std::optional<AdminTag> adminResponseTag(AdminTag request);
Data adminData(const AdminMessage &message,bool wantResponse=true);

// Full Config/ModuleConfig oneof envelope; inner payload remains versioned
// protobuf bytes. type is AdminMessage.ConfigType / ModuleConfigType (zero based).
Bytes configEnvelope(std::uint32_t type,const Bytes &inner,bool module=false);
std::uint32_t configEnvelopeType(const Bytes &envelope,bool module=false);
Bytes replaceProtoVarint(const Bytes &message,std::uint32_t field,std::uint64_t value);
Bytes replaceProtoBytes(const Bytes &message,std::uint32_t field,const Bytes &value);
struct AdminChannel {
    std::uint32_t index=0,role=0;
    std::optional<Channel> settings;
    Bytes rawSettings; // retains unsupported/per-channel module fields
};
Bytes encodeAdminChannel(const AdminChannel &channel);
AdminChannel decodeAdminChannel(const Bytes &protobuf);

// State for virtual nodes or a snapshot returned by the host adapter. All
// mutations are validated then atomically passed to commit before publication.
struct AdminState {
    User owner;
    std::map<std::uint32_t,Bytes> configs,moduleConfigs,channels;
    Bytes metadata,connectionStatus,remotePins,uiConfig;
    std::string cannedMessages,ringtone;
    std::optional<Position> fixedPosition;
};
struct AdminPolicy {
    std::uint32_t localNodeId=0;
    std::vector<Key32> adminKeys; // explicit authorized keys; at most three
    std::size_t maxSessions=8,maxRequestsPerSession=256;
    std::uint32_t sessionTtlMs=300000;
};
struct AdminResult {
    Data reply; // PKI send back to request.from, with a fresh packet ID
    std::uint32_t to=0;
    bool applied=false,replayed=false;
    std::uint32_t error=0;
};
class AdminServer {
public:
    using Commit=std::function<bool(const AdminState &)>;
    // Action callback handles authorized hardware/process operations. It returns
    // Routing.Error (0 success, 4 unavailable, 32 bad request...). No shell is run.
    using Action=std::function<std::uint32_t(const AdminMessage &)>;
    AdminServer(AdminPolicy policy,AdminState state,std::shared_ptr<CryptoProvider> crypto,
                Commit commit={},Action action={});
    ~AdminServer();
    AdminServer(const AdminServer &)=delete;
    AdminServer &operator=(const AdminServer &)=delete;
    std::optional<AdminResult> handle(const Event &verifiedMessage,std::uint64_t nowMs);
    const AdminState &state() const;
    bool transactionOpen() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
struct AdminClientResult {
    std::uint32_t peer=0,requestId=0,error=0;
    std::optional<AdminMessage> response;
    bool success=false;
};
// Uses Node::sendPki and consumes authenticated Message events. A response must
// match sender, pinned key, request ID and expected operation before token reuse.
class AdminClient {
public:
    explicit AdminClient(std::shared_ptr<PkiContext> pki,std::size_t maxPending=32,std::uint32_t responseTimeoutMs=120000);
    ~AdminClient();
    AdminClient(const AdminClient &)=delete;
    AdminClient &operator=(const AdminClient &)=delete;
    std::uint32_t request(Node &node,std::uint32_t peer,AdminMessage message,std::uint64_t nowMs);
    std::optional<AdminClientResult> handle(const Event &verifiedMessage,std::uint64_t nowMs);
    std::vector<AdminClientResult> expire(Node &node,std::uint64_t nowMs);
    void forgetPeer(Node &node,std::uint32_t peer,std::uint64_t nowMs);
    bool hasSession(std::uint32_t peer,std::uint64_t nowMs) const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
