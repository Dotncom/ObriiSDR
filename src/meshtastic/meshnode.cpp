#include "meshtastic.h"
#include "meshpki.h"
#include "meshadmin.h"
#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace obrii::meshtastic {
void NodeConfig::validate() const {
    if(nodeId<=3||nodeId==Broadcast)throw std::invalid_argument("Meshtastic node ID is reserved");
    if(channels.empty()||channels.size()>8)throw std::invalid_argument("Configure 1..8 channels");
    for(const auto &channel:channels)channel.validate();
    if(pki)for(const auto &channel:channels)if(channel.enabled&&channel.hash()==0)
        throw std::invalid_argument("Channel hash zero is reserved for PKI on a PKI-enabled node");
    if(defaultHopLimit>7||maxRetries<0||maxRetries>10||!ackTimeoutMs||!retryDelayMs
       ||ackTimeoutMs>86400000||retryDelayMs>86400000||relayDelayMs>86400000||relayJitterMs>86400000)
        throw std::invalid_argument("Invalid Meshtastic timing or hop configuration");
    if(historyTtlMs<std::uint64_t(ackTimeoutMs+retryDelayMs)*(maxRetries+1)||historyTtlMs>86400000)
        throw std::invalid_argument("History TTL must cover the reliable transmission window");
    if(!maxHistory||maxHistory>8192||!maxNodes||maxNodes>4096||!maxQueue||maxQueue>1024
       ||!maxPending||maxPending>maxQueue||!maxEvents||maxEvents>4096)
        throw std::invalid_argument("Invalid Meshtastic bounded queue/cache limits");
    if(relay!=RelayMode::Off&&relay!=RelayMode::Client&&relay!=RelayMode::Router)
        throw std::invalid_argument("Invalid relay mode");
}
struct Node::Impl {
    using Key=std::pair<std::uint32_t,std::uint32_t>;
    struct Seen { RadioPacket packet;std::uint64_t expires=0;Bytes ack;std::uint8_t bestHop=0;std::optional<std::uint64_t> lastRelayTx; };
    struct Scheduled { Transmission tx;RadioPacket packet;std::uint64_t due=0;bool cancelled=false; };
    struct Pending { RadioPacket packet;Bytes wire;std::size_t channel=0;int attempts=0;std::optional<std::uint64_t> deadline;bool pki=false; };
    NodeConfig config;NextPacketId ids;std::uint32_t lastId=0;
    std::uint64_t now=0,nextTicket=1,droppedEvents=0;
    std::map<Key,Seen> seen;
    std::map<std::uint32_t,Seen> own;
    std::map<std::uint32_t,Pending> pending;
    std::map<std::uint32_t,NodeRecord> records;
    std::deque<Scheduled> queue;
    std::optional<Scheduled> active;
    std::deque<Event> events;

    Impl(NodeConfig c,NextPacketId source):config(std::move(c)),ids(std::move(source)) {
        config.validate();if(!ids)throw std::invalid_argument("Packet ID allocator is required");
    }
    void clock(std::uint64_t time) {
        if(time<now||time>std::numeric_limits<std::uint64_t>::max()-1000000000ull)
            throw std::invalid_argument("Meshtastic clock must be monotonic and leave deadline headroom");
        now=time;
        for(auto i=seen.begin();i!=seen.end();)if(i->second.expires<=now)i=seen.erase(i);else ++i;
        for(auto i=own.begin();i!=own.end();)if(i->second.expires<=now)i=own.erase(i);else ++i;
    }
    void emit(EventKind kind,const Header &h,const std::string &detail={},
              std::optional<Data> data={},std::optional<std::size_t> channel={},
              std::uint32_t request=0,std::uint32_t error=0,Security security=Security::Psk,
              bool authenticated=false,std::optional<Key32> senderKey={}) {
        if(events.size()==config.maxEvents) { events.pop_front();++droppedEvents; }
        events.push_back({kind,h,std::move(data),channel,request,error,detail,security,authenticated,senderKey});
    }
    std::uint32_t allocate() {
        const auto id=ids();
        if(!id||id<=lastId)throw std::invalid_argument("Packet ID allocator must be strictly increasing without wrap");
        lastId=id;return id;
    }
    bool full() const { return queue.size()+std::size_t(active.has_value())>=config.maxQueue; }
    void enqueue(Bytes wire,TxKind kind,std::uint64_t due) {
        auto packet=decodeRadio(wire);
        queue.push_back({{nextTicket++,kind,std::move(wire)},std::move(packet),due,false});
    }
    static bool samePayload(const RadioPacket &a,const RadioPacket &b) {
        return a.header.to==b.header.to&&a.header.from==b.header.from&&a.header.id==b.header.id
            &&a.header.channelHash==b.header.channelHash&&a.body==b.body;
    }
    Seen &remember(const RadioPacket &p) {
        if(seen.size()==config.maxHistory) {
            const auto oldest=std::min_element(seen.begin(),seen.end(),[](const auto &a,const auto &b){return a.second.expires<b.second.expires;});
            seen.erase(oldest);
        }
        return seen.emplace(Key{p.header.from,p.header.id},Seen{p,now+config.historyTtlMs,{},p.header.hopLimit}).first->second;
    }
    void rememberOwn(const RadioPacket &p) {
        const auto existing=own.find(p.header.id);
        if(existing!=own.end()) { existing->second.packet=p;existing->second.expires=now+config.historyTtlMs;return; }
        if(own.size()==config.maxHistory) {
            const auto oldest=std::min_element(own.begin(),own.end(),[](const auto &a,const auto &b){return a.second.expires<b.second.expires;});own.erase(oldest);
        }
        own.emplace(p.header.id,Seen{p,now+config.historyTtlMs,{},p.header.hopLimit});
    }
    void removeQueued(std::uint32_t id) {
        queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const auto &s){return s.packet.header.from==config.nodeId&&s.packet.header.id==id;}),queue.end());
    }
    void processAck(const Decoded &p) {
        const auto &h=p.radio.header;const auto &d=p.data;
        if(h.to!=config.nodeId||!d.requestId)return;
        const auto it=pending.find(d.requestId);if(it==pending.end())return;
        const auto &waiting=it->second;
        if(!waiting.attempts||waiting.packet.header.to==Broadcast||waiting.packet.header.to!=h.from)return;
        if(d.port!=RoutingPort) {
            if(waiting.pki&&p.security==Security::Pki) {
                emit(EventKind::Acknowledged,h,"Authenticated application response",d,{},d.requestId,0,p.security,true,p.senderPublicKey);
                removeQueued(d.requestId);pending.erase(it);
            }
            return;
        }
        try {
            const auto routing=decodeRouting(d.payload);
            if(routing.kind!=Routing::Kind::Error)return;
            bool authenticated=p.security==Security::Pki;
            if(waiting.pki) {
                if(!authenticated&&config.pki)authenticated=config.pki->verifyAckProof(h.from,h.from,h.to,d.requestId,routing);
                if(!authenticated) { emit(EventKind::Rejected,h,"Unauthenticated ACK cannot clear a PKI transmission");return; }
            } else if(p.security!=Security::Pki&&waiting.channel!=p.channelIndex)return;
            emit(routing.error?EventKind::NegativeAck:EventKind::Acknowledged,h,
                authenticated?"Pairwise authenticated routing response":"PSK routing response; sender is not cryptographically authenticated",
                d,p.channelIndex,d.requestId,routing.error,p.security,authenticated,
                p.senderPublicKey?p.senderPublicKey:authenticated&&config.pki?config.pki->peerKey(h.from):std::optional<Key32>{});
            removeQueued(d.requestId);pending.erase(it);
        } catch(const std::invalid_argument &) { emit(EventKind::Rejected,h,"Malformed routing response"); }
    }
    void ack(const Decoded &p,Seen &history) {
        const auto &h=p.radio.header;const auto &d=p.data;
        if(h.to!=config.nodeId||!h.wantAck||d.port==RoutingPort)return;
        if(d.port==AdminPort) {
            // Firmware may send an admin getter response reliably. Acknowledge
            // receipt of that response, but never pre-ACK an admin command.
            if(p.security!=Security::Pki||!d.requestId)return;
            try { if(!adminIsResponse(decodeAdmin(d.payload).tag))return; }
            catch(const std::exception &) { return; }
        } else if(d.requestId||d.replyId)return;
        if(full()) { emit(EventKind::QueueFull,h,"ACK queue full");return; }
        if(history.ack.empty()) {
            Routing routing;routing.kind=Routing::Kind::Error;
            if(p.security==Security::Pki) {
                if(!config.pki)return;
                routing.ackProof=config.pki->ackProof(h.from,config.nodeId,h.from,h.id,routing);
            }
            Data response;response.port=RoutingPort;response.payload=encodeRouting(routing);response.requestId=h.id;
            Header reply;reply.from=config.nodeId;reply.to=h.from;reply.id=allocate();
            reply.hopLimit=h.hopStart?config.defaultHopLimit:0;reply.hopStart=reply.hopLimit;
            reply.relayNode=relayId(config.nodeId);
            if(p.security==Security::Pki) {
                auto channel=std::find_if(config.channels.begin(),config.channels.end(),[](const auto &c){return c.enabled;});
                history.ack=channel==config.channels.end()?config.pki->seal(reply,response):seal(reply,response,*channel);
            } else history.ack=seal(reply,response,config.channels[p.channelIndex]);
            rememberOwn(decodeRadio(history.ack));
        }
        const auto ackPacket=decodeRadio(history.ack);
        for(const auto &s:queue)if(s.packet.header.from==config.nodeId&&s.packet.header.id==ackPacket.header.id)return;
        if(active&&active->packet.header.from==config.nodeId&&active->packet.header.id==ackPacket.header.id)return;
        enqueue(history.ack,TxKind::Ack,now);
    }
    void updateRecord(const Decoded &p) {
        const auto id=p.radio.header.from;
        if(!records.count(id)&&records.size()==config.maxNodes) {
            auto oldest=std::min_element(records.begin(),records.end(),[](const auto &a,const auto &b){return a.second.lastSeenMs<b.second.lastSeenMs;});records.erase(oldest);
        }
        auto &r=records[id];r.id=id;r.lastSeenMs=now;
        try {
            if(p.data.port==NodeInfoPort)r.user=decodeUser(p.data.payload);
            if(p.data.port==PositionPort)r.position=decodePosition(p.data.payload);
        } catch(const std::invalid_argument &) { emit(EventKind::Rejected,p.radio.header,"Malformed NodeInfo/Position payload; raw Data retained"); }
    }
    void relay(const RadioPacket &p,bool known) {
        const auto &h=p.header;
        if(config.relay==RelayMode::Off||!h.hopLimit||h.to==config.nodeId||h.from==config.nodeId
            ||(!known&&!config.relayUnknownChannels))return;
        // Learned next-hop routing is not implemented; honor another selected relay.
        if(h.to!=Broadcast&&h.nextHop&&h.nextHop!=relayId(config.nodeId))return;
        if(full()) { emit(EventKind::QueueFull,h,"Relay queue full");return; }
        auto forwarded=p;--forwarded.header.hopLimit;forwarded.header.relayNode=relayId(config.nodeId);
        forwarded.header.nextHop=0; // fallback flooding, no route preference
        const std::uint32_t mix=h.id*2654435761u^h.from^config.nodeId*2246822519u;
        const auto jitter=config.relayJitterMs?mix%(config.relayJitterMs+1):0;
        enqueue(encodeRadio(forwarded),TxKind::Relay,now+config.relayDelayMs+jitter);
    }
    void timers() {
        for(auto it=pending.begin();it!=pending.end();) {
            auto &p=it->second;
            if(!p.deadline||*p.deadline>now) { ++it;continue; }
            if(p.attempts>=config.maxRetries+1) {
                emit(EventKind::TimedOut,p.packet.header,"No confirmation before retry limit",{},p.channel,it->first);
                it=pending.erase(it);continue;
            }
            if(full()) { p.deadline=now+config.retryDelayMs;++it;continue; }
            enqueue(p.wire,TxKind::Retry,now+config.retryDelayMs);p.deadline.reset();
            emit(EventKind::Retrying,p.packet.header,"Retry queued with original packet ID and ciphertext",{},p.channel,it->first);++it;
        }
    }
};
Node::Node(NodeConfig config,NextPacketId ids):impl_(std::make_unique<Impl>(std::move(config),std::move(ids))) {}
Node::~Node()=default;
Node::Node(Node &&) noexcept=default;
Node &Node::operator=(Node &&) noexcept=default;
std::uint32_t Node::send(const Data &d,std::size_t channel,std::uint32_t to,bool reliable,std::uint64_t time) {
    auto &s=*impl_;s.clock(time);
    if(channel>=s.config.channels.size()||!s.config.channels[channel].enabled)throw std::invalid_argument("Invalid local channel index");
    if(to<=3||to==s.config.nodeId)throw std::invalid_argument("Destination is reserved or local");
    if(s.full()||(reliable&&s.pending.size()>=s.config.maxPending))throw std::length_error("Meshtastic transmission queue full");
    encodeData(d); // Validate size before consuming an ID.
    Header h;h.from=s.config.nodeId;h.to=to;h.id=s.allocate();h.hopLimit=s.config.defaultHopLimit;h.hopStart=h.hopLimit;
    if(reliable&&to==Broadcast&&!h.hopLimit)throw std::invalid_argument("Implicit broadcast confirmation needs a relay hop");
    h.wantAck=reliable&&to!=Broadcast;h.relayNode=relayId(s.config.nodeId);
    auto wire=seal(h,d,s.config.channels[channel]);auto packet=decodeRadio(wire);s.rememberOwn(packet);
    if(reliable)s.pending.emplace(h.id,Impl::Pending{packet,wire,channel,0,{}});
    s.enqueue(std::move(wire),TxKind::Original,s.now);return h.id;
}
std::uint32_t Node::sendPki(const Data &d,std::uint32_t to,bool reliable,std::uint64_t time) {
    auto &s=*impl_;s.clock(time);
    if(!s.config.pki||!s.config.pki->peerKey(to))throw std::invalid_argument("PKI destination needs an explicitly pinned key");
    if(to<=3||to==Broadcast||to==s.config.nodeId)throw std::invalid_argument("PKI destination must be a remote unicast node");
    if(s.full()||(reliable&&s.pending.size()>=s.config.maxPending))throw std::length_error("Meshtastic transmission queue full");
    if(encodeData(d).size()>DataMtu-12)throw std::length_error("PKI Data exceeds 227-byte plaintext MTU");
    Header h;h.from=s.config.nodeId;h.to=to;h.id=s.allocate();h.hopLimit=s.config.defaultHopLimit;h.hopStart=h.hopLimit;
    h.wantAck=reliable;h.relayNode=relayId(s.config.nodeId);
    auto wire=s.config.pki->seal(h,d);auto packet=decodeRadio(wire);s.rememberOwn(packet);
    if(reliable)s.pending.emplace(h.id,Impl::Pending{packet,wire,0,0,{},true});
    s.enqueue(std::move(wire),TxKind::Original,s.now);return h.id;
}
void Node::receive(const Bytes &wire,std::uint64_t time) {
    auto &s=*impl_;s.clock(time);RadioPacket radio;
    try { radio=decodeRadio(wire); }
    catch(const std::invalid_argument &e) { s.emit(EventKind::Rejected,{},e.what());return; }
    const auto &h=radio.header;const Impl::Key key{h.from,h.id};
    auto old=s.seen.find(key);
    if(old!=s.seen.end()&&!Impl::samePayload(old->second.packet,radio)) {
        s.emit(EventKind::IdentityConflict,h,"Same sender/packet ID has different destination, channel or ciphertext");return;
    }
    if(h.from==s.config.nodeId) {
        const auto it=s.pending.find(h.id);
        const auto owned=s.own.find(h.id);
        const RadioPacket *sent=it!=s.pending.end()?&it->second.packet:owned!=s.own.end()?&owned->second.packet:nullptr;
        if(!sent||!Impl::samePayload(*sent,radio)) {
            s.emit(EventKind::IdentityConflict,h,"Unknown packet or different ciphertext uses local sender/packet ID");return;
        }
        if(it!=s.pending.end()&&h.to==Broadcast&&h.hopLimit<it->second.packet.header.hopLimit&&!h.viaMqtt) {
            s.emit(EventKind::Relayed,h,"Implicit relay confirmation; not end-to-end delivery",{},it->second.channel,h.id);
            s.removeQueued(h.id);s.pending.erase(it);
        }
        s.emit(EventKind::Duplicate,h);return;
    }
    DecodeResult decoded;
    if(h.channelHash==0&&h.to==s.config.nodeId&&s.config.pki) {
        auto packet=s.config.pki->open(radio,s.config.nodeId);
        if(!packet) { s.emit(EventKind::Rejected,h,"PKI authentication failed or sender is not pinned");return; }
        decoded.status=DecodeStatus::Decoded;decoded.packet=std::move(packet);
    } else decoded=open(radio,s.config.channels);
    const bool known=decoded.status==DecodeStatus::Decoded;
    const bool mayRelay=known||(s.config.relayPki&&h.channelHash==0&&h.to!=Broadcast&&radio.body.size()>=14);
    if(known)s.processAck(*decoded.packet);
    if(old!=s.seen.end()) {
        s.emit(EventKind::Duplicate,h);
        if(known&&decoded.packet->data.port==AdminPort&&h.to==s.config.nodeId) {
            const auto &p=*decoded.packet;
            s.emit(EventKind::Message,h,"Duplicate admin packet; replay cached service response without reapplying",p.data,p.channelIndex,
                0,0,p.security,p.security==Security::Pki,p.senderPublicKey);
        }
        if(known)s.ack(*decoded.packet,old->second); // Lost ACK must not cause duplicate delivery.
        bool upgraded=h.hopLimit>old->second.bestHop;
        auto &q=s.queue;
        q.erase(std::remove_if(q.begin(),q.end(),[&](const auto &scheduled) {
            if(scheduled.tx.kind!=TxKind::Relay||scheduled.packet.header.from!=h.from||scheduled.packet.header.id!=h.id)return false;
            return upgraded||(s.config.relay==RelayMode::Client&&h.hopLimit<=scheduled.packet.header.hopLimit);
        }),q.end());
        if(upgraded) { old->second.bestHop=h.hopLimit;s.relay(radio,mayRelay); }
        else if(h.hopStart&&h.hopStart==h.hopLimit&&old->second.lastRelayTx
                &&s.now>=*old->second.lastRelayTx+s.config.retryDelayMs) {
            // A reliable originator may repeat after its ACK was lost. Allow
            // that attempt through again, while retaining exactly-once app delivery.
            bool alreadyQueued=s.active&&s.active->tx.kind==TxKind::Relay
                &&s.active->packet.header.from==h.from&&s.active->packet.header.id==h.id;
            for(const auto &queued:s.queue)if(queued.tx.kind==TxKind::Relay
                &&queued.packet.header.from==h.from&&queued.packet.header.id==h.id)alreadyQueued=true;
            if(!alreadyQueued)s.relay(radio,mayRelay);
        }
        return;
    }
    auto &history=s.remember(radio);
    if(known) {
        const auto &p=*decoded.packet;s.updateRecord(p);
        s.emit(h.to==Broadcast||h.to==s.config.nodeId?EventKind::Message:EventKind::Observed,h,{},p.data,p.channelIndex,
            0,0,p.security,p.security==Security::Pki,p.senderPublicKey);
        s.ack(p,history);
    } else {
        s.emit(decoded.status==DecodeStatus::UnknownChannel?EventKind::UnknownChannel:
            decoded.status==DecodeStatus::AmbiguousChannel?EventKind::AmbiguousChannel:EventKind::Rejected,
            h,"No unambiguous PSK Data decode; payload may use an unknown key or PKI");
    }
    s.relay(radio,mayRelay);
}
std::optional<Transmission> Node::poll(std::uint64_t time,bool busy) {
    auto &s=*impl_;s.clock(time);s.timers();if(busy||s.active||s.queue.empty())return {};
    auto best=s.queue.end();
    for(auto it=s.queue.begin();it!=s.queue.end();++it)if(it->due<=s.now) {
        if(best==s.queue.end()||(it->tx.kind==TxKind::Ack&&best->tx.kind!=TxKind::Ack)||
            ((it->tx.kind==TxKind::Ack)==(best->tx.kind==TxKind::Ack)&&it->due<best->due))best=it;
    }
    if(best==s.queue.end())return {};
    s.active=std::move(*best);s.queue.erase(best);return s.active->tx;
}
void Node::completeTransmission(std::uint64_t ticket,bool sent,std::uint64_t time) {
    auto &s=*impl_;s.clock(time);
    if(!s.active||s.active->tx.ticket!=ticket)throw std::invalid_argument("Unknown Meshtastic TX ticket");
    auto tx=std::move(*s.active);s.active.reset();
    if(tx.cancelled)return;
    const auto &h=tx.packet.header;
    if(sent&&h.from==s.config.nodeId)s.rememberOwn(tx.packet);
    if(sent&&tx.tx.kind==TxKind::Relay) {
        const auto history=s.seen.find({h.from,h.id});
        if(history!=s.seen.end())history->second.lastRelayTx=s.now;
    }
    const auto waiting=h.from==s.config.nodeId?s.pending.find(h.id):s.pending.end();
    if(waiting!=s.pending.end()) {
        if(!sent) { s.emit(EventKind::TxFailed,h,"Local radio rejected transmission",{},waiting->second.channel,h.id);s.pending.erase(waiting);return; }
        ++waiting->second.attempts;waiting->second.deadline=s.now+s.config.ackTimeoutMs;
    }
    s.emit(sent?EventKind::Sent:EventKind::TxFailed,h,sent?"Radio confirmed local TX completion":"Local TX failed");
}
void Node::cancel(std::uint32_t id,std::uint64_t time) {
    auto &s=*impl_;s.clock(time);s.removeQueued(id);s.pending.erase(id);
    if(s.active&&s.active->packet.header.from==s.config.nodeId&&s.active->packet.header.id==id)s.active->cancelled=true;
    Header h;h.from=s.config.nodeId;h.id=id;s.emit(EventKind::Cancelled,h,"Queued work cancelled; host must abort an already active radio TX");
}
std::vector<Event> Node::takeEvents() {
    auto &q=impl_->events;std::vector<Event> events;
    events.reserve(q.size());while(!q.empty()) { events.push_back(std::move(q.front()));q.pop_front(); }return events;
}
std::vector<NodeRecord> Node::nodes() const {
    std::vector<NodeRecord> nodes;for(const auto &entry:impl_->records)nodes.push_back(entry.second);return nodes;
}
std::size_t Node::queuedCount() const { return impl_->queue.size()+std::size_t(impl_->active.has_value()); }
std::size_t Node::pendingCount() const { return impl_->pending.size(); }
std::uint32_t Node::nodeId() const { return impl_->config.nodeId; }
std::uint64_t Node::droppedEventCount() const { return impl_->droppedEvents; }
}
