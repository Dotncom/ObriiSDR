#include "meshadmin.h"
#include "meshprotobuf_internal.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace obrii::meshtastic {
namespace {
void validateState(const AdminState &s) {
    encodeUser(s.owner);
    if(s.configs.size()>10||s.moduleConfigs.size()>17||s.channels.size()>8)throw std::invalid_argument("Admin state exceeds config/channel limits");
    for(const auto &p:s.configs)if(configEnvelopeType(p.second)!=p.first)throw std::invalid_argument("Config key/type mismatch");
    for(const auto &p:s.moduleConfigs)if(configEnvelopeType(p.second,true)!=p.first)throw std::invalid_argument("Module config key/type mismatch");
    int primary=0;
    for(const auto &p:s.channels) { const auto ch=decodeAdminChannel(p.second);if(ch.index!=p.first)throw std::invalid_argument("Channel index mismatch");primary+=ch.role==1; }
    if(primary>1)throw std::invalid_argument("Multiple primary channels");
    for(const auto *b:{&s.metadata,&s.connectionStatus,&s.remotePins,&s.uiConfig})detail::protoFields(*b);
    if(s.fixedPosition)encodePosition(*s.fixedPosition);
    if(s.cannedMessages.size()>200||s.ringtone.size()>200||!detail::validUtf8(s.cannedMessages)||!detail::validUtf8(s.ringtone))
        throw std::invalid_argument("Invalid admin text state");
}
Data routingReply(std::uint32_t request,std::uint32_t error) {
    Routing routing;routing.kind=Routing::Kind::Error;routing.error=error;
    Data out;out.port=RoutingPort;out.requestId=request;out.payload=encodeRouting(routing);return out;
}
Bytes hidePrivateKey(Bytes envelope) {
    auto fields=detail::protoFields(envelope);
    if(fields.size()==1&&fields[0].tag==8) {
        auto security=detail::protoFields(fields[0].bytes);
        security.erase(std::remove_if(security.begin(),security.end(),[](const auto &f){return f.tag==2;}),security.end());
        fields[0].bytes=detail::protoEncode(security);envelope=detail::protoEncode(fields);
    }
    return envelope;
}
bool whitespace(const std::string &s) { return !s.empty()&&std::all_of(s.begin(),s.end(),[](unsigned char c){return c<=32;}); }
}
struct AdminServer::Impl {
    struct Cached { Key32 hash{};AdminResult result; };
    struct Session {
        Key32 key{};Bytes token;std::uint64_t issued=0;
        std::map<std::uint32_t,Cached> requests;
        ~Session() {
            secureErase(token.data(),token.size());
            for(auto &r:requests)secureErase(r.second.result.reply.payload.data(),r.second.result.reply.payload.size());
        }
    };
    struct Edit { std::uint32_t peer=0;Key32 key{};AdminState staged; };
    AdminPolicy policy;AdminState live;std::shared_ptr<CryptoProvider> crypto;Commit commit;Action action;
    std::map<std::uint32_t,Session> sessions;std::optional<Edit> edit;std::uint64_t now=0;
    Impl(AdminPolicy p,AdminState s,std::shared_ptr<CryptoProvider> c,Commit save,Action act)
        :policy(std::move(p)),live(std::move(s)),crypto(std::move(c)),commit(std::move(save)),action(std::move(act)) {
        if(!crypto||policy.localNodeId<=3||policy.localNodeId==Broadcast||policy.adminKeys.size()>3
           ||!policy.maxSessions||policy.maxSessions>64||!policy.maxRequestsPerSession||policy.maxRequestsPerSession>1024
           ||policy.sessionTtlMs<1000||policy.sessionTtlMs>300000)throw std::invalid_argument("Invalid admin server policy");
        validateState(live);
    }
    void clock(std::uint64_t t) {
        if(t<now||t>std::numeric_limits<std::uint64_t>::max()-1000000)throw std::invalid_argument("Invalid admin clock");now=t;
        for(auto it=sessions.begin();it!=sessions.end();) {
            if(now>=it->second.issued+policy.sessionTtlMs) {
                if(edit&&edit->peer==it->first)edit.reset();it=sessions.erase(it);
            } else ++it;
        }
    }
    bool authorized(const Key32 &key) const {
        bool match=false;for(const auto &admin:policy.adminKeys)match|=constantTimeEqual(admin,key);return match;
    }
    AdminResult result(const Event &event,std::uint32_t error) const {
        return {routingReply(event.header.id,error),event.header.from,false,false,error};
    }
    AdminState &view(std::uint32_t peer) { return edit&&edit->peer==peer?edit->staged:live; }
    bool save(AdminState candidate) {
        validateState(candidate);if(commit&&!commit(candidate))return false;live=std::move(candidate);return true;
    }
    AdminMessage get(const AdminMessage &m,std::uint32_t peer) {
        auto &s=view(peer);Bytes payload;const auto response=adminResponseTag(m.tag);
        if(!response)throw std::invalid_argument("Not an admin getter");
        switch(m.tag) {
        case AdminTag::GetOwner:payload=encodeUser(s.owner);break;
        case AdminTag::GetConfig:case AdminTag::GetModuleConfig: {
            const bool module=m.tag==AdminTag::GetModuleConfig;
            if(m.number>(module?16u:9u))throw std::invalid_argument("Invalid config selector");
            const auto type=std::uint32_t(m.number);auto &map=module?s.moduleConfigs:s.configs;
            const auto it=map.find(type);payload=it==map.end()?configEnvelope(type,{},module):it->second;
            if(!module&&type==7)payload=hidePrivateKey(std::move(payload));break;
        }
        case AdminTag::GetChannel: {
            if(m.number<1||m.number>8)throw std::invalid_argument("Channel getter uses index+1");
            const auto index=std::uint32_t(m.number-1);const auto it=s.channels.find(index);
            if(it==s.channels.end()) { AdminChannel ch;ch.index=index;payload=encodeAdminChannel(ch); }else payload=it->second;
            break;
        }
        case AdminTag::GetMetadata:payload=s.metadata;break;
        case AdminTag::GetConnectionStatus:payload=s.connectionStatus;break;
        case AdminTag::GetRemotePins:payload=s.remotePins;break;
        case AdminTag::GetUiConfig:payload=s.uiConfig;break;
        case AdminTag::GetCanned:payload.assign(s.cannedMessages.begin(),s.cannedMessages.end());break;
        case AdminTag::GetRingtone:payload.assign(s.ringtone.begin(),s.ringtone.end());break;
        default:throw std::invalid_argument("Unsupported getter");
        }
        return adminPayload(*response,std::move(payload));
    }
    std::uint32_t mutate(const AdminMessage &m,const Event &event) {
        const auto peer=event.header.from;const auto key=*event.senderPublicKey;
        if(edit&&(edit->peer!=peer||!constantTimeEqual(edit->key,key)))return 33;
        if(m.tag==AdminTag::BeginEdit) {
            if(edit)return 32;edit=Edit{peer,key,live};return 0;
        }
        if(m.tag==AdminTag::CommitEdit) {
            if(!edit)return 32;
            if(!save(edit->staged))return 4;edit.reset();return 0;
        }
        auto candidate=view(peer);bool stateChange=true;
        switch(m.tag) {
        case AdminTag::SetOwner: {
            const auto user=decodeUser(m.payload);
            if(whitespace(user.longName)||whitespace(user.shortName))return 32;
            if((!user.id.empty()&&user.id!=candidate.owner.id)||(!user.publicKey.empty()&&user.publicKey!=candidate.owner.publicKey))return 32;
            if(!user.longName.empty())candidate.owner.longName=user.longName;
            if(!user.shortName.empty())candidate.owner.shortName=user.shortName;
            candidate.owner.licensed=user.licensed;break;
        }
        case AdminTag::SetConfig:case AdminTag::SetModuleConfig: {
            const bool module=m.tag==AdminTag::SetModuleConfig;const auto type=configEnvelopeType(m.payload,module);
            auto body=m.payload;
            // Security getters deliberately omit the private key. A read/edit/write
            // cycle must preserve it unless field2 was explicitly supplied.
            if(!module&&type==7&&candidate.configs.count(7)) {
                auto wrapper=detail::protoFields(body);auto next=detail::protoFields(wrapper[0].bytes);bool hasPrivate=false;
                for(const auto &f:next)if(f.tag==2)hasPrivate=true;
                if(!hasPrivate)for(const auto &f:detail::protoFields(detail::protoFields(candidate.configs[7])[0].bytes))if(f.tag==2)next.push_back(f);
                wrapper[0].bytes=detail::protoEncode(next);body=detail::protoEncode(wrapper);
            }
            (module?candidate.moduleConfigs:candidate.configs)[type]=std::move(body);break;
        }
        case AdminTag::SetChannel: {
            const auto ch=decodeAdminChannel(m.payload);
            if(ch.role==1)for(auto &entry:candidate.channels)if(entry.first!=ch.index) {
                auto previous=decodeAdminChannel(entry.second);if(previous.role==1) { previous.role=2;entry.second=encodeAdminChannel(previous); }
            }
            candidate.channels[ch.index]=m.payload;break;
        }
        case AdminTag::SetPosition:candidate.fixedPosition=decodePosition(m.payload);break;
        case AdminTag::RemovePosition:candidate.fixedPosition.reset();break;
        case AdminTag::SetCanned:candidate.cannedMessages.assign(m.payload.begin(),m.payload.end());break;
        case AdminTag::SetRingtone:candidate.ringtone.assign(m.payload.begin(),m.payload.end());break;
        case AdminTag::StoreUiConfig:detail::protoFields(m.payload);candidate.uiConfig=m.payload;break;
        default:stateChange=false;break;
        }
        if(stateChange) {
            validateState(candidate);
            if(edit)edit->staged=std::move(candidate);else if(!save(std::move(candidate)))return 4;
            return 0;
        }
        // An open transaction is not implicitly committed by a reboot/action.
        if(edit)return 32;
        if(m.tag==AdminTag::GetDisplayFrame||m.tag==AdminTag::SetDisplayMirror||m.tag==AdminTag::LockdownAuth)return 32;
        return action?action(m):4;
    }
};
AdminServer::AdminServer(AdminPolicy p,AdminState s,std::shared_ptr<CryptoProvider> c,Commit commit,Action action)
    :impl_(std::make_unique<Impl>(std::move(p),std::move(s),std::move(c),std::move(commit),std::move(action))) {}
AdminServer::~AdminServer()=default;
const AdminState &AdminServer::state() const { return impl_->live; }
bool AdminServer::transactionOpen() const { return impl_->edit.has_value(); }
std::optional<AdminResult> AdminServer::handle(const Event &e,std::uint64_t time) {
    auto &s=*impl_;s.clock(time);
    if(e.kind!=EventKind::Message||!e.data||e.data->port!=AdminPort||e.header.to!=s.policy.localNodeId)return {};
    if(e.security!=Security::Pki||!e.authenticated||!e.senderPublicKey)return s.result(e,33);
    if(!s.authorized(*e.senderPublicKey))return s.result(e,37);
    AdminMessage message;
    try { message=decodeAdmin(e.data->payload); }catch(const std::exception &) { return s.result(e,32); }
    if(adminIsResponse(message.tag))return {}; // Never execute an unsolicited response as a command.
    const bool getter=adminIsRequest(message.tag);
    auto it=s.sessions.find(e.header.from);
    if(it!=s.sessions.end()&&!constantTimeEqual(it->second.key,*e.senderPublicKey)) {
        if(s.edit&&s.edit->peer==e.header.from)s.edit.reset();s.sessions.erase(it);it=s.sessions.end();
    }
    if(it==s.sessions.end()) {
        if(!getter)return s.result(e,36);
        if(s.sessions.size()==s.policy.maxSessions)return s.result(e,38);
        Bytes token;
        try { token=s.crypto->random(8); }catch(const std::exception &) { return s.result(e,4); }
        if(token.size()!=8)return s.result(e,4);
        it=s.sessions.try_emplace(e.header.from).first;it->second.key=*e.senderPublicKey;
        it->second.issued=s.now;it->second.token=std::move(token);
    }
    auto &session=it->second;
    const auto digest=s.crypto->sha256(e.data->payload);const auto cached=session.requests.find(e.header.id);
    if(cached!=session.requests.end()) {
        if(!constantTimeEqual(cached->second.hash,digest))return s.result(e,32);
        auto replay=cached->second.result;replay.replayed=true;replay.applied=false;return replay;
    }
    if(session.requests.size()>=s.policy.maxRequestsPerSession)return s.result(e,38);
    AdminResult result=s.result(e,0);
    if(!getter&&(message.sessionPasskey.size()!=8||session.token.size()!=8||!constantTimeEqual(message.sessionPasskey,session.token)))result=s.result(e,36);
    else try {
        if(getter) {
            auto response=s.get(message,e.header.from);response.sessionPasskey=session.token;
            result.reply=adminData(response,false);result.reply.requestId=e.header.id;
            // request_id adds protobuf overhead; validate the final PKI budget.
            if(encodeData(result.reply).size()>DataMtu-12)throw std::length_error("Admin response too large");
        } else {
            const auto error=s.mutate(message,e);result=s.result(e,error);result.applied=error==0;
        }
    } catch(const std::length_error &) { result=s.result(e,7); }
      catch(const std::invalid_argument &) { result=s.result(e,32); }
      catch(const std::exception &) { result=s.result(e,4); }
    session.requests.emplace(e.header.id,Impl::Cached{digest,result});return result;
}
}
