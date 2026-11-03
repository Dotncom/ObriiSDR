#include "meshadmin.h"
#include "meshprotobuf_internal.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace obrii::meshtastic {
namespace {
using F=detail::ProtoField;
std::optional<unsigned> wire(AdminTag tag) {
    switch(tag) {
    case AdminTag::ChannelResponse:case AdminTag::OwnerResponse:case AdminTag::ConfigResponse:
    case AdminTag::ModuleConfigResponse:case AdminTag::CannedResponse:case AdminTag::MetadataResponse:
    case AdminTag::RingtoneResponse:case AdminTag::ConnectionStatusResponse:case AdminTag::SetHamMode:
    case AdminTag::RemotePinsResponse:case AdminTag::DeleteFile:case AdminTag::InputEvent:
    case AdminTag::SetOwner:case AdminTag::SetChannel:case AdminTag::SetConfig:case AdminTag::SetModuleConfig:
    case AdminTag::SetCanned:case AdminTag::SetRingtone:case AdminTag::SetPosition:case AdminTag::UiConfigResponse:
    case AdminTag::StoreUiConfig:case AdminTag::AddContact:case AdminTag::KeyVerification:
    case AdminTag::OtaRequest:case AdminTag::SensorConfig:case AdminTag::LockdownAuth:return 2;
    case AdminTag::SetTime:return 5;
    case AdminTag::GetChannel:case AdminTag::GetOwner:case AdminTag::GetConfig:case AdminTag::GetModuleConfig:
    case AdminTag::GetCanned:case AdminTag::GetMetadata:case AdminTag::GetRingtone:case AdminTag::GetConnectionStatus:
    case AdminTag::GetRemotePins:case AdminTag::EnterDfu:case AdminTag::SetScale:case AdminTag::Backup:
    case AdminTag::Restore:case AdminTag::RemoveBackup:case AdminTag::RemoveNode:case AdminTag::SetFavorite:
    case AdminTag::RemoveFavorite:case AdminTag::RemovePosition:case AdminTag::GetUiConfig:case AdminTag::SetIgnored:
    case AdminTag::RemoveIgnored:case AdminTag::ToggleMuted:case AdminTag::GetDisplayFrame:case AdminTag::SetDisplayMirror:
    case AdminTag::BeginEdit:case AdminTag::CommitEdit:case AdminTag::FactoryResetDevice:case AdminTag::RebootOta:
    case AdminTag::ExitSimulator:case AdminTag::Reboot:case AdminTag::Shutdown:case AdminTag::FactoryResetConfig:
    case AdminTag::ResetNodeDb:return 0;
    }
    return {};
}
bool isString(AdminTag tag) {
    return tag==AdminTag::SetCanned||tag==AdminTag::CannedResponse||tag==AdminTag::SetRingtone
        ||tag==AdminTag::RingtoneResponse||tag==AdminTag::DeleteFile;
}
void validate(const AdminMessage &m) {
    const auto w=wire(m.tag);if(!w)throw std::invalid_argument("Unknown admin operation");
    if(!m.sessionPasskey.empty()&&m.sessionPasskey.size()!=8)throw std::invalid_argument("Admin passkey must have eight bytes");
    if(*w!=2&&!m.payload.empty())throw std::invalid_argument("Scalar admin operation has a byte payload");
    if(isString(m.tag)&&!detail::validUtf8(std::string(m.payload.begin(),m.payload.end())))throw std::invalid_argument("Invalid admin UTF-8");
}
std::uint32_t u32(const F &f,unsigned w) {
    if(f.wire!=w||f.value>0xffffffffu)throw std::invalid_argument("Invalid admin protobuf uint32");return std::uint32_t(f.value);
}
}
AdminMessage adminNumber(AdminTag tag,std::uint64_t n) { AdminMessage m;m.tag=tag;m.number=n;validate(m);return m; }
AdminMessage adminSigned(AdminTag tag,std::int32_t n) {
    if(tag!=AdminTag::Reboot&&tag!=AdminTag::Shutdown&&tag!=AdminTag::RebootOta
       &&tag!=AdminTag::FactoryResetConfig&&tag!=AdminTag::FactoryResetDevice)throw std::invalid_argument("Admin operation is not int32");
    return adminNumber(tag,std::uint64_t(std::int64_t(n)));
}
std::int32_t adminSignedValue(const AdminMessage &m) {
    adminSigned(m.tag,0);const auto bits=std::uint32_t(m.number);std::int32_t n;std::memcpy(&n,&bits,4);return n;
}
AdminMessage adminGetConfig(AdminConfigType type) {
    if(std::uint32_t(type)>9)throw std::invalid_argument("Invalid config type");return adminNumber(AdminTag::GetConfig,std::uint32_t(type));
}
AdminMessage adminGetChannel(std::uint32_t index) {
    if(index>7)throw std::invalid_argument("Channel index must be 0..7");return adminNumber(AdminTag::GetChannel,index+1);
}
AdminMessage adminPayload(AdminTag tag,Bytes b) { AdminMessage m;m.tag=tag;m.number=0;m.payload=std::move(b);validate(m);return m; }
Bytes encodeAdmin(const AdminMessage &m) {
    validate(m);std::vector<F> fields{{std::uint32_t(m.tag),*wire(m.tag),m.number,m.payload}};
    if(!m.sessionPasskey.empty())fields.push_back({101,2,0,m.sessionPasskey});
    for(const auto &f:detail::protoFields(m.unknownFields)) {
        if(f.tag==101||wire(AdminTag(f.tag)))throw std::invalid_argument("Unknown admin fields overlap known fields");fields.push_back(f);
    }
    return detail::protoEncode(fields);
}
AdminMessage decodeAdmin(const Bytes &b) {
    AdminMessage m;bool found=false;std::vector<F> unknown;
    for(const auto &f:detail::protoFields(b)) {
        if(f.tag==101) { if(f.wire!=2)throw std::invalid_argument("Invalid admin passkey type");m.sessionPasskey=f.bytes;continue; }
        const auto w=wire(AdminTag(f.tag));
        if(!w) { unknown.push_back(f);continue; }
        if(f.wire!=*w)throw std::invalid_argument("Admin operation has wrong wire type");
        m.tag=AdminTag(f.tag);m.number=f.value;m.payload=f.bytes;found=true;
    }
    if(!found)throw std::invalid_argument("Admin message has no known operation");
    m.unknownFields=detail::protoEncode(unknown);validate(m);return m;
}
std::optional<AdminTag> adminResponseTag(AdminTag tag) {
    switch(tag) {
    case AdminTag::GetChannel:return AdminTag::ChannelResponse;
    case AdminTag::GetOwner:return AdminTag::OwnerResponse;
    case AdminTag::GetConfig:return AdminTag::ConfigResponse;
    case AdminTag::GetModuleConfig:return AdminTag::ModuleConfigResponse;
    case AdminTag::GetCanned:return AdminTag::CannedResponse;
    case AdminTag::GetMetadata:return AdminTag::MetadataResponse;
    case AdminTag::GetRingtone:return AdminTag::RingtoneResponse;
    case AdminTag::GetConnectionStatus:return AdminTag::ConnectionStatusResponse;
    case AdminTag::GetRemotePins:return AdminTag::RemotePinsResponse;
    case AdminTag::GetUiConfig:return AdminTag::UiConfigResponse;
    default:return {};
    }
}
bool adminIsRequest(AdminTag tag) { return adminResponseTag(tag).has_value(); }
bool adminIsResponse(AdminTag tag) {
    for(auto request:{AdminTag::GetChannel,AdminTag::GetOwner,AdminTag::GetConfig,AdminTag::GetModuleConfig,
        AdminTag::GetCanned,AdminTag::GetMetadata,AdminTag::GetRingtone,AdminTag::GetConnectionStatus,AdminTag::GetRemotePins,AdminTag::GetUiConfig})
        if(adminResponseTag(request)==tag)return true;
    return false;
}
Data adminData(const AdminMessage &m,bool response) {
    Data d;d.port=AdminPort;d.payload=encodeAdmin(m);d.wantResponse=response;
    if(encodeData(d).size()>DataMtu-12)throw std::length_error("Admin message exceeds PKI plaintext MTU");return d;
}
Bytes configEnvelope(std::uint32_t type,const Bytes &inner,bool module) {
    if(type>(module?16u:9u))throw std::invalid_argument("Invalid config type");
    detail::protoFields(inner);return detail::protoEncode({{type+1,2,0,inner}});
}
std::uint32_t configEnvelopeType(const Bytes &envelope,bool module) {
    const auto f=detail::protoFields(envelope);
    if(f.size()!=1||f[0].wire!=2||!f[0].tag||f[0].tag>(module?17u:10u))throw std::invalid_argument("Config needs exactly one known payload variant");
    detail::protoFields(f[0].bytes);return f[0].tag-1;
}
Bytes replaceProtoVarint(const Bytes &b,std::uint32_t tag,std::uint64_t value) {
    auto fields=detail::protoFields(b);
    fields.erase(std::remove_if(fields.begin(),fields.end(),[&](const auto &f){return f.tag==tag;}),fields.end());
    fields.push_back({tag,0,value,{}});return detail::protoEncode(fields);
}
Bytes replaceProtoBytes(const Bytes &b,std::uint32_t tag,const Bytes &value) {
    auto fields=detail::protoFields(b);
    fields.erase(std::remove_if(fields.begin(),fields.end(),[&](const auto &f){return f.tag==tag;}),fields.end());
    fields.push_back({tag,2,0,value});return detail::protoEncode(fields);
}
Bytes encodeAdminChannel(const AdminChannel &ch) {
    if(ch.index>7||ch.role>2)throw std::invalid_argument("Invalid admin channel index/role");
    Bytes settings=ch.rawSettings;
    if(ch.settings) {
        if(!ch.settings->name.empty())ch.settings->validate();
        if(ch.settings->psk.size()>32)throw std::invalid_argument("Channel PSK exceeds32 bytes");
        if(ch.settings->name.size()>11)throw std::invalid_argument("Radio channel name exceeds eleven bytes");
        settings=replaceProtoBytes(settings,2,ch.settings->psk);
        settings=replaceProtoBytes(settings,3,Bytes(ch.settings->name.begin(),ch.settings->name.end()));
    }
    std::vector<F> fields;
    if(ch.index)fields.push_back({1,0,ch.index,{}});
    if(ch.settings||!settings.empty())fields.push_back({2,2,0,settings});
    if(ch.role)fields.push_back({3,0,ch.role,{}});
    return detail::protoEncode(fields);
}
AdminChannel decodeAdminChannel(const Bytes &b) {
    AdminChannel ch;
    for(const auto &f:detail::protoFields(b)) {
        if(f.tag==1)ch.index=u32(f,0);
        else if(f.tag==3)ch.role=u32(f,0);
        else if(f.tag==2) {
            if(f.wire!=2)throw std::invalid_argument("Channel settings have wrong wire type");
            ch.rawSettings=f.bytes;Channel c;c.name.clear();c.psk.clear();
            for(const auto &s:detail::protoFields(f.bytes)) {
                if(s.tag==2) { if(s.wire!=2)throw std::invalid_argument("Channel PSK has wrong type");c.psk=s.bytes; }
                if(s.tag==3) { if(s.wire!=2)throw std::invalid_argument("Channel name has wrong type");c.name.assign(s.bytes.begin(),s.bytes.end()); }
            }
            if(!c.name.empty())c.validate();if(c.psk.size()>32)throw std::invalid_argument("Channel PSK exceeds32 bytes");ch.settings=std::move(c);
        }
    }
    if(ch.index>7||ch.role>2)throw std::invalid_argument("Invalid admin channel index/role");return ch;
}
}
