#include "meshadmin.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace obrii::meshtastic {
struct AdminClient::Impl {
    struct Pending {
        std::uint32_t peer=0,local=0;Key32 key{};AdminTag operation=AdminTag::GetOwner;
        std::uint64_t selector=0;std::optional<std::uint64_t> deadline;
    };
    struct Token { Key32 key{};Bytes bytes;std::uint64_t expires=0; };
    std::shared_ptr<PkiContext> pki;std::size_t limit;std::uint32_t timeout;std::uint64_t now=0;
    std::map<std::uint32_t,Pending> pending;std::map<std::uint32_t,Token> tokens;
    Impl(std::shared_ptr<PkiContext> p,std::size_t n,std::uint32_t t):pki(std::move(p)),limit(n),timeout(t) {
        if(!pki||!n||n>256||!t||t>3600000)throw std::invalid_argument("Invalid admin client configuration");
    }
    ~Impl() { for(auto &entry:tokens)secureErase(entry.second.bytes.data(),entry.second.bytes.size()); }
    void clock(std::uint64_t time) {
        if(time<now||time>std::numeric_limits<std::uint64_t>::max()-7200000)throw std::invalid_argument("Invalid admin client clock");now=time;
        for(auto it=tokens.begin();it!=tokens.end();) {
            const auto key=pki->peerKey(it->first);
            if(now>=it->second.expires||!key||!constantTimeEqual(*key,it->second.key)) {
                secureErase(it->second.bytes.data(),it->second.bytes.size());it=tokens.erase(it);
            } else ++it;
        }
    }
};
AdminClient::AdminClient(std::shared_ptr<PkiContext> p,std::size_t n,std::uint32_t t):impl_(std::make_unique<Impl>(std::move(p),n,t)) {}
AdminClient::~AdminClient()=default;
std::uint32_t AdminClient::request(Node &node,std::uint32_t peer,AdminMessage m,std::uint64_t time) {
    auto &s=*impl_;s.clock(time);const auto key=s.pki->peerKey(peer);
    if(!key)throw std::invalid_argument("Admin peer needs a pinned public key");
    if(adminIsResponse(m.tag))throw std::invalid_argument("Admin client cannot request a response variant");
    if(s.pending.size()==s.limit)throw std::length_error("Admin pending requests full");
    if(adminIsRequest(m.tag))m.sessionPasskey.clear();
    else {
        const auto token=s.tokens.find(peer);
        if(token==s.tokens.end())throw std::invalid_argument("Read admin settings/session key before a mutation");
        m.sessionPasskey=token->second.bytes;
    }
    auto data=adminData(m,adminIsRequest(m.tag));
    const auto id=node.sendPki(data,peer,true,time);
    s.pending.emplace(id,Impl::Pending{peer,node.nodeId(),*key,m.tag,m.number,{}});return id;
}
std::optional<AdminClientResult> AdminClient::handle(const Event &e,std::uint64_t time) {
    auto &s=*impl_;s.clock(time);
    // Consume host TX lifecycle events as well as received Message events.
    if(e.kind==EventKind::Sent||e.kind==EventKind::TxFailed||e.kind==EventKind::TimedOut||e.kind==EventKind::Cancelled) {
        const auto it=s.pending.find(e.header.id);if(it==s.pending.end())return {};
        if(e.kind==EventKind::Sent) { it->second.deadline=s.now+s.timeout;return {}; }
        const auto peer=it->second.peer;s.pending.erase(it);
        return AdminClientResult{peer,e.header.id,e.kind==EventKind::TimedOut?3u:4u,{},false};
    }
    if((e.kind!=EventKind::Message&&e.kind!=EventKind::Acknowledged&&e.kind!=EventKind::NegativeAck)||!e.data||!e.data->requestId)return {};
    const auto it=s.pending.find(e.data->requestId);if(it==s.pending.end())return {};
    const auto &p=it->second;const auto pin=s.pki->peerKey(p.peer);
    if(e.header.from!=p.peer||e.header.to!=p.local||!e.authenticated||!e.senderPublicKey||!pin
       ||!constantTimeEqual(*e.senderPublicKey,p.key)||!constantTimeEqual(*pin,p.key))return {};
    AdminClientResult out;out.peer=p.peer;out.requestId=it->first;
    if(e.data->port==RoutingPort) {
        Routing r;
        try { r=decodeRouting(e.data->payload); }catch(const std::exception &) { return {}; }
        if(r.kind!=Routing::Kind::Error)return {};
        // Getters require their specific response, not a bare success ACK.
        if(!r.error&&adminIsRequest(p.operation))return {};
        out.error=r.error;out.success=r.error==0;
        if(r.error==36) {
            auto token=s.tokens.find(p.peer);if(token!=s.tokens.end()) { secureErase(token->second.bytes.data(),token->second.bytes.size());s.tokens.erase(token); }
        }
    } else if(e.data->port==AdminPort&&e.security==Security::Pki) {
        try {
            auto response=decodeAdmin(e.data->payload);
            if(adminResponseTag(p.operation)!=response.tag)return {};
            if(response.tag==AdminTag::ConfigResponse&&configEnvelopeType(response.payload)!=p.selector)return {};
            if(response.tag==AdminTag::ModuleConfigResponse&&configEnvelopeType(response.payload,true)!=p.selector)return {};
            if(response.tag==AdminTag::ChannelResponse&&decodeAdminChannel(response.payload).index+1!=p.selector)return {};
            if(response.sessionPasskey.size()==8) {
                if(!s.tokens.count(p.peer)&&s.tokens.size()==s.limit) {
                    const auto oldest=std::min_element(s.tokens.begin(),s.tokens.end(),[](const auto &a,const auto &b){return a.second.expires<b.second.expires;});
                    secureErase(oldest->second.bytes.data(),oldest->second.bytes.size());s.tokens.erase(oldest);
                }
                auto &token=s.tokens[p.peer];secureErase(token.bytes.data(),token.bytes.size());
                token={p.key,response.sessionPasskey,s.now+150000};
            }
            out.response=std::move(response);out.success=true;
        } catch(const std::exception &) { return {}; }
    } else return {};
    s.pending.erase(it);return out;
}
std::vector<AdminClientResult> AdminClient::expire(Node &node,std::uint64_t time) {
    auto &s=*impl_;s.clock(time);std::vector<AdminClientResult> results;
    for(auto it=s.pending.begin();it!=s.pending.end();) {
        if(it->second.deadline&&*it->second.deadline<=s.now) {
            results.push_back({it->second.peer,it->first,3,{},false});node.cancel(it->first,time);it=s.pending.erase(it);
        } else ++it;
    }
    return results;
}
void AdminClient::forgetPeer(Node &node,std::uint32_t peer,std::uint64_t time) {
    auto &s=*impl_;s.clock(time);
    for(auto it=s.pending.begin();it!=s.pending.end();)if(it->second.peer==peer) { node.cancel(it->first,time);it=s.pending.erase(it); }else ++it;
    auto token=s.tokens.find(peer);if(token!=s.tokens.end()) { secureErase(token->second.bytes.data(),token->second.bytes.size());s.tokens.erase(token); }
}
bool AdminClient::hasSession(std::uint32_t peer,std::uint64_t time) const {
    const auto &s=*impl_;if(time<s.now)throw std::invalid_argument("Invalid admin client clock");
    const auto key=s.pki->peerKey(peer);const auto token=s.tokens.find(peer);
    return key&&token!=s.tokens.end()&&token->second.expires>time&&constantTimeEqual(*key,token->second.key);
}
}
