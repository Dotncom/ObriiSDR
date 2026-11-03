#include "meshadmin.h"
#include "meshprotobuf_internal.h"
#ifdef OBRII_MESHTASTIC_WITH_LORA
#include "meshlora.h"
#endif
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace obrii::meshtastic;
namespace {
int failures=0;
void check(bool result,const std::string &message) { if(!result) { ++failures;std::cerr<<"FAIL "<<message<<'\n'; } }
template<class F>void rejects(F f,const std::string &message) { try { f();check(false,message); }catch(const std::exception &) {} }
Bytes hex(const std::string &s) { Bytes b;for(std::size_t i=0;i<s.size();i+=2)b.push_back(std::uint8_t(std::stoul(s.substr(i,2),nullptr,16)));return b; }
Key32 key(const Bytes &b) { if(b.size()!=32)throw std::runtime_error("Bad test key");Key32 out{};std::copy(b.begin(),b.end(),out.begin());return out; }
std::map<std::string,Bytes> vectors() {
    std::ifstream file(OBRII_MESHTASTIC_PKI_FIXTURES);if(!file)throw std::runtime_error("PKI fixtures missing");
    std::map<std::string,Bytes> out;std::string line;
    while(std::getline(file,line)) {
        if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty()||line[0]=='#')continue;
        const auto bar=line.find('|');out[line.substr(0,bar)]=hex(line.substr(bar+1));
    }
    return out;
}
class FixtureCrypto:public CryptoProvider {
    std::shared_ptr<CryptoProvider> native_=platformCrypto();
public:
    Bytes random(std::size_t n) override { return n==4?hex("f0e1d2c3"):native_->random(n); }
    Key32 x25519(const Key32 &a,const Key32 &b) override { return native_->x25519(a,b); }
    Key32 sha256(const Bytes &b) override { return native_->sha256(b); }
    Key32 hmacSha256(const Key32 &k,const Bytes &b) override { return native_->hmacSha256(k,b); }
    Bytes ccmSeal(const Key32 &k,const Bytes &n,const Bytes &b) override { return native_->ccmSeal(k,n,b); }
    std::optional<Bytes> ccmOpen(const Key32 &k,const Bytes &n,const Bytes &b) override { return native_->ccmOpen(k,n,b); }
};
class BrokenRandom:public FixtureCrypto {
public:
    Bytes random(std::size_t n) override { if(n==8)throw std::runtime_error("Test RNG failure");return FixtureCrypto::random(n); }
};
struct Ids { std::uint32_t n=100;std::uint32_t next(){return n++;} };
NodeConfig cfg(std::uint32_t id,std::shared_ptr<PkiContext> pki={}) {
    NodeConfig c;c.nodeId=id;c.pki=std::move(pki);c.relayDelayMs=10;c.relayJitterMs=0;c.ackTimeoutMs=100;c.retryDelayMs=10;c.maxRetries=2;c.historyTtlMs=10000;return c;
}
Transmission tx(Node &n,std::uint64_t now) {
    auto out=n.poll(now);if(!out)throw std::runtime_error("Expected PKI TX at "+std::to_string(now));n.completeTransmission(out->ticket,true,now);return *out;
}
bool has(const std::vector<Event> &events,EventKind kind) { return std::any_of(events.begin(),events.end(),[&](const auto &e){return e.kind==kind;}); }
std::shared_ptr<PkiContext> context(const std::shared_ptr<CryptoProvider> &p,const Bytes &privateKey) { return std::make_shared<PkiContext>(p,key(privateKey)); }
void primitiveTests(const std::map<std::string,Bytes> &v) {
    auto p=platformCrypto();const auto a=key(v.at("alice_private")),b=key(v.at("bob_private"));
    check(publicKey(*p,a)==key(v.at("alice_public")),"RFC7748 Alice public key");
    check(publicKey(*p,b)==key(v.at("bob_public")),"RFC7748 Bob public key");
    check(p->x25519(a,key(v.at("bob_public")))==key(v.at("shared")),"RFC7748 shared secret / byte order");
    check(p->sha256(v.at("shared"))==key(v.at("aes_key")),"Independent SHA256 shared AES key");
    check(p->sha256(Bytes{'a','b','c'})==key(hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")),"SHA256 abc");
    const auto cipher=p->ccmSeal(key(v.at("aes_key")),v.at("nonce"),v.at("plaintext"));
    check(cipher==v.at("cipher_tag"),"Independent AES256-CCM ciphertext/tag");
    check(p->ccmOpen(key(v.at("aes_key")),v.at("nonce"),cipher)==v.at("plaintext"),"CCM authenticated open");
    auto bad=cipher;bad.back()^=1;check(!p->ccmOpen(key(v.at("aes_key")),v.at("nonce"),bad),"CCM tag failure never exposes plaintext");
    const auto empty=p->ccmSeal(key(v.at("aes_key")),v.at("nonce"),{});
    check(p->ccmOpen(key(v.at("aes_key")),v.at("nonce"),empty)==Bytes{},"Zero-length CCM payload");
    rejects([&]{p->x25519(a,Key32{});},"Low-order X25519 public key rejected");
    Key32 low{};low[0]=1;rejects([&]{p->x25519(a,low);},"X25519 point1 rejected");
    auto generated=generatePrivateKey(*p);check(publicKey(*p,generated)!=Key32{},"CSPRNG-generated keypair");
    auto fixture=std::make_shared<FixtureCrypto>();auto alice=context(fixture,v.at("alice_private")),bob=context(fixture,v.at("bob_private"));
    alice->pinPeer(0x55667788,bob->publicKey());bob->pinPeer(0x11223344,alice->publicKey());
    Header h;h.from=0x11223344;h.to=0x55667788;h.id=0x12345678;h.wantAck=true;h.relayNode=0x44;
    const auto wire=alice->seal(h,textMessage("hello"));check(wire==v.at("radio"),"Independent complete Meshtastic PKI wire vector");
    const auto packet=bob->open(decodeRadio(wire),h.to);
    check(packet&&packet->security==Security::Pki&&packet->senderPublicKey==alice->publicKey()&&text(packet->data)=="hello","Pinned peer authenticated decode");
    for(std::size_t i=16;i<wire.size();++i) { auto tampered=wire;tampered[i]^=1;check(!bob->open(decodeRadio(tampered),h.to),"Every ciphertext/tag/extra-nonce byte authenticated"); }
    Routing r;r.kind=Routing::Kind::Error;
    check(bob->ackProof(0x11223344,h.to,h.from,h.id,r)==v.at("ack_proof"),"Independent pairwise ACK HMAC vector");
    r.ackProof=v.at("ack_proof");check(alice->verifyAckProof(h.to,h.to,h.from,h.id,r),"ACK proof verification");
    r.error=32;check(!alice->verifyAckProof(h.to,h.to,h.from,h.id,r),"ACK cannot be flipped into NAK");
    rejects([&]{alice->pinPeer(h.to,alice->publicKey());},"Pinned key replacement rejected");
    rejects([&]{alice->seal(h,textMessage(std::string(223,'x')));},"PKI MTU includes tag and extra nonce");
    std::cout<<"PASS native X25519/SHA256/CCM/HMAC and independent PKI wire fixtures\n";
}
void pkiNodeTests(const std::map<std::string,Bytes> &v) {
    auto p=platformCrypto();auto pa=context(p,v.at("alice_private")),pc=context(p,v.at("bob_private"));
    pa->pinPeer(0x103,pc->publicKey());pc->pinPeer(0x101,pa->publicKey());Ids ai,bi,ci;
    Node a(cfg(0x101,pa),[&]{return ai.next();}),b(cfg(0x102),[&]{return bi.next();}),c(cfg(0x103,pc),[&]{return ci.next();});
    const auto id=a.sendPki(textMessage("private"),0x103,true,0);const auto wire=tx(a,0).radioBytes;b.receive(wire,0);
    check(!has(b.takeEvents(),EventKind::Message),"Intermediate node cannot decrypt PKI");
    const auto forwarded=tx(b,10).radioBytes;auto damaged=forwarded;damaged.back()^=1;c.receive(damaged,10);
    check(!has(c.takeEvents(),EventKind::Message),"Tampered PKI packet rejected before history insertion");
    c.receive(forwarded,11);const auto received=c.takeEvents();
    bool authenticated=false;for(const auto &e:received)if(e.kind==EventKind::Message)authenticated=e.authenticated&&e.security==Security::Pki&&text(*e.data)=="private";
    check(authenticated,"Valid packet succeeds after forged packet with same ID");
    Routing fake;fake.kind=Routing::Kind::Error;Data ack;ack.port=RoutingPort;ack.requestId=id;ack.payload=encodeRouting(fake);
    Header spoof;spoof.from=0x103;spoof.to=0x101;spoof.id=999;
    a.receive(seal(spoof,ack,Channel{}),11);check(a.pendingCount()==1,"PSK success ACK cannot downgrade PKI pending");
    const auto actualAck=tx(c,11).radioBytes;b.receive(actualAck,11);a.receive(tx(b,21).radioBytes,21);
    bool proof=false;for(const auto &e:a.takeEvents())if(e.kind==EventKind::Acknowledged)proof=e.authenticated;
    check(!a.pendingCount()&&proof,"PKI request accepts pairwise-proof PSK ACK across relay");
    User malicious;malicious.publicKey.assign(pa->publicKey().begin(),pa->publicKey().end());Data info;info.port=NodeInfoPort;info.payload=encodeUser(malicious);
    spoof.to=Broadcast;spoof.id=1000;a.receive(seal(spoof,info,Channel{}),22);
    check(pa->peerKey(0x103)==pc->publicKey(),"Untrusted NodeInfo cannot change a pin");
    auto invalid=cfg(0x104,pa);invalid.channels[0].name="AA";invalid.channels[0].psk={0};rejects([&]{invalid.validate();},"PKI channel-hash0 collision rejected");
#ifdef OBRII_MESHTASTIC_WITH_LORA
    obrii::lora::Config phy;phy.spreadingFactor=7;LoRaRadio air(phy),rx(phy);
    const auto iq=air.modulate(wire);std::vector<AirPacket> packets;
    for(std::size_t i=0;i<iq.size();) {
        const auto n=std::min<std::size_t>(997,iq.size()-i);auto found=rx.push(iq.data()+i,n);packets.insert(packets.end(),found.begin(),found.end());i+=n;
    }
    check(packets.size()==1&&pc->open(decodeRadio(packets[0].radioBytes),0x103),"PKI ciphertext survives complete LoRa IQ path");
#endif
    std::cout<<"PASS PKI node/relay/LoRa integration, tampering and ACK downgrade checks\n";
}
struct Harness {
    std::shared_ptr<CryptoProvider> crypto=platformCrypto();
    std::shared_ptr<PkiContext> pa,pc;Ids ai,ci;
    Node a,c;AdminClient client;std::unique_ptr<AdminServer> server;
    std::uint64_t time=0;int commits=0,actions=0;bool failCommit=false;
    std::vector<AdminResult> handled;
    Harness(const std::map<std::string,Bytes> &v):pa(context(crypto,v.at("alice_private"))),pc(context(crypto,v.at("bob_private"))),
        a(cfg(0x201,pa),[this]{return ai.next();}),c(cfg(0x203,pc),[this]{return ci.next();}),client(pa) {
        pa->pinPeer(0x203,pc->publicKey());pc->pinPeer(0x201,pa->publicKey());
        AdminState state;state.owner.id="!00000203";state.owner.longName="Original";state.owner.shortName="O";
        state.owner.publicKey.assign(pc->publicKey().begin(),pc->publicKey().end());
        AdminPolicy policy;policy.localNodeId=0x203;policy.adminKeys={pa->publicKey()};
        server=std::make_unique<AdminServer>(policy,state,crypto,[this](const auto &){if(failCommit)return false;++commits;return true;},
            [this](const AdminMessage &m){if(m.tag==AdminTag::Reboot||m.tag==AdminTag::Shutdown){++actions;return 0u;}return 4u;});
    }
    void lifecycle() { for(const auto &e:a.takeEvents())client.handle(e,time); }
    void service(const Bytes &wire) {
        c.receive(wire,time);
        for(const auto &e:c.takeEvents())if(auto result=server->handle(e,time)) {
            handled.push_back(*result);c.sendPki(result->reply,result->to,false,time);
        }
    }
    AdminClientResult response(bool drop=false) {
        const auto answer=tx(c,time);if(drop)return {};
        a.receive(answer.radioBytes,time);std::optional<AdminClientResult> result;
        for(const auto &e:a.takeEvents())if(auto r=client.handle(e,time))result=*r;
        if(!result)throw std::runtime_error("Expected correlated admin response");return *result;
    }
    AdminClientResult exchange(AdminMessage m) {
        ++time;client.request(a,0x203,std::move(m),time);const auto out=tx(a,time);lifecycle();service(out.radioBytes);return response();
    }
    Event event(std::uint32_t id,AdminMessage message) {
        Event e;e.kind=EventKind::Message;e.header.from=0x201;e.header.to=0x203;e.header.id=id;
        e.security=Security::Pki;e.authenticated=true;e.senderPublicKey=pa->publicKey();e.data=adminData(message);return e;
    }
};
void adminTests(const std::map<std::string,Bytes> &v) {
    check(encodeAdmin(adminNumber(AdminTag::GetConfig,0))==hex("2800"),"ConfigType0 oneof presence");
    check(encodeAdmin(adminNumber(AdminTag::GetChannel,1))==hex("0801"),"Channel getter index+1 wire");
    check(encodeAdmin(adminNumber(AdminTag::Reboot,5))==hex("880605"),"Official reboot field97 wire");
    check(adminSignedValue(decodeAdmin(encodeAdmin(adminSigned(AdminTag::Reboot,-1))))==-1,"Negative reboot cancellation int32");
    check(adminGetChannel(0).number==1&&adminGetConfig(AdminConfigType::SessionKey).number==8,"Typed admin selectors");
    auto msg=adminNumber(AdminTag::BeginEdit);msg.sessionPasskey=Bytes(8,1);
    check(encodeAdmin(decodeAdmin(encodeAdmin(msg)))==encodeAdmin(msg),"Admin passkey field101 roundtrip");
    AdminChannel channel;channel.index=2;channel.role=2;channel.settings=Channel{"test",{1},true};
    check(decodeAdminChannel(encodeAdminChannel(channel)).settings->name=="test","Typed channel settings");
    rejects([&]{decodeAdmin(hex("2a00"));},"Admin operation wrong wire type");
    Harness h(v);auto owner=h.exchange(adminNumber(AdminTag::GetOwner));
    check(owner.success&&owner.response&&decodeUser(owner.response->payload).longName=="Original"&&h.client.hasSession(0x203,h.time),"PKI get-owner obtains correlated admin session");
    User change;change.longName="Updated";auto set=h.exchange(adminPayload(AdminTag::SetOwner,encodeUser(change)));
    check(set.success&&h.server->state().owner.longName=="Updated"&&h.commits==1,"Authorized set-owner committed before ACK");
    check(h.exchange(adminNumber(AdminTag::BeginEdit)).success&&h.server->transactionOpen(),"Begin transaction");
    change.longName="Staged";check(h.exchange(adminPayload(AdminTag::SetOwner,encodeUser(change))).success,"Stage owner change");
    check(h.server->state().owner.longName=="Updated"&&h.commits==1,"Staged changes not published or persisted early");
    const auto lora=configEnvelope(5,replaceProtoVarint({},4,11));
    check(h.exchange(adminPayload(AdminTag::SetConfig,lora)).success,"Stage LoRa config envelope");
    h.failCommit=true;check(h.exchange(adminNumber(AdminTag::CommitEdit)).error==4&&h.server->transactionOpen(),"Failed host commit preserves old state and staged transaction");
    h.failCommit=false;check(h.exchange(adminNumber(AdminTag::CommitEdit)).success,"Commit transaction retry with fresh request ID");
    check(h.server->state().owner.longName=="Staged"&&h.server->state().configs.at(5)==lora&&h.commits==2,"Atomic owner/config publication");
    auto config=h.exchange(adminNumber(AdminTag::GetConfig,5));check(config.response&&config.response->payload==lora,"Config readback after commit");
    check(h.exchange(adminNumber(AdminTag::Reboot,3)).success&&h.actions==1,"Authorized reboot dispatches a host action without executing an OS command");
    Bytes security=replaceProtoBytes({},1,Bytes(h.pc->publicKey().begin(),h.pc->publicKey().end()));security=replaceProtoBytes(security,2,Bytes(32,17));
    check(h.exchange(adminPayload(AdminTag::SetConfig,configEnvelope(7,security))).success,"Security config write uses authenticated admin path");
    const auto redacted=h.exchange(adminNumber(AdminTag::GetConfig,7));
    bool privateLeaked=false;for(const auto &f:detail::protoFields(detail::protoFields(redacted.response->payload)[0].bytes))privateLeaked|=f.tag==2;
    check(!privateLeaked,"Security getter never returns device private key");
    const auto edited=configEnvelope(7,replaceProtoVarint(detail::protoFields(redacted.response->payload)[0].bytes,4,1));
    check(h.exchange(adminPayload(AdminTag::SetConfig,edited)).success,"Read/edit/write of redacted security config");
    bool preserved=false;for(const auto &f:detail::protoFields(detail::protoFields(h.server->state().configs.at(7))[0].bytes))if(f.tag==2)preserved=f.bytes==Bytes(32,17);
    check(preserved,"Omitted private key is preserved during config update");
    // Lost response: transport repeats request; service replays its cached result.
    ++h.time;change.longName="Once";h.client.request(h.a,0x203,adminPayload(AdminTag::SetOwner,encodeUser(change)),h.time);
    auto sent=tx(h.a,h.time);h.lifecycle();h.service(sent.radioBytes);h.response(true);
    const auto committed=h.commits;h.time+=100;h.a.poll(h.time);h.time+=10;
    sent=tx(h.a,h.time);h.lifecycle();h.service(sent.radioBytes);const auto recovered=h.response();
    check(recovered.success&&h.commits==committed&&h.handled.back().replayed,"Lost admin response recovers without executing mutation twice");
    auto replay=h.event(9000,adminNumber(AdminTag::GetOwner));auto token=h.server->handle(replay,++h.time);
    const auto pass=decodeAdmin(token->reply.payload).sessionPasskey;
    auto mutation=adminNumber(AdminTag::Shutdown,10);mutation.sessionPasskey=pass;
    auto command=h.event(9001,mutation);auto first=h.server->handle(command,++h.time);auto repeated=h.server->handle(command,++h.time);
    check(first->applied&&repeated->replayed&&!repeated->applied&&h.actions==2,"Admin action replay cache prevents repeated shutdown");
    auto bad=command;bad.header.id=9002;bad.data->payload.back()^=1;
    check(h.server->handle(bad,++h.time)->error!=0&&h.actions==2,"Wrong session passkey rejected");
    auto unauth=h.event(9003,adminNumber(AdminTag::GetOwner));unauth.authenticated=false;unauth.security=Security::Psk;
    check(h.server->handle(unauth,++h.time)->error==33,"PSK admin is not authorized");
    unauth.authenticated=true;unauth.security=Security::Pki;unauth.senderPublicKey=h.pc->publicKey();
    check(h.server->handle(unauth,++h.time)->error==37,"PKI key outside admin allowlist rejected");
    h.time=400000;check(h.server->handle(command,h.time)->error==36&&h.actions==2,"Expired admin session cannot replay a mutation");
    check(!h.client.hasSession(0x203,h.time),"Client session cache expires");
    rejects([&]{h.client.request(h.a,0x203,adminNumber(AdminTag::Reboot,1),h.time);},"Mutation requires fresh session handshake");
    Harness expired(v);auto initial=expired.exchange(adminNumber(AdminTag::GetOwner));
    const auto oldToken=initial.response->sessionPasskey;expired.exchange(adminNumber(AdminTag::BeginEdit));
    change.longName="Never committed";expired.exchange(adminPayload(AdminTag::SetOwner,encodeUser(change)));
    auto commit=adminNumber(AdminTag::CommitEdit);commit.sessionPasskey=oldToken;
    auto expiredResult=expired.server->handle(expired.event(8000,commit),400000);
    check(expiredResult->error==36&&!expired.server->transactionOpen()&&expired.server->state().owner.longName=="Original", "Expired transaction rolls back staged state");
    AdminPolicy policy;policy.localNodeId=0x203;policy.adminKeys={h.pa->publicKey()};AdminState state;
    auto broken=std::make_shared<BrokenRandom>();AdminServer noRng(policy,state,broken);
    check(noRng.handle(h.event(1,adminNumber(AdminTag::GetOwner)),0)->error==4,"RNG failure cannot create a session");
    check(noRng.handle(h.event(2,adminNumber(AdminTag::Reboot,1)),1)->error==36,"RNG failure never grants an empty admin token");
    policy.maxRequestsPerSession=1;AdminServer bounded(policy,state,h.crypto);
    auto firstRead=bounded.handle(h.event(3,adminNumber(AdminTag::GetOwner)),0);
    auto limitCommand=adminNumber(AdminTag::Reboot,1);limitCommand.sessionPasskey=decodeAdmin(firstRead->reply.payload).sessionPasskey;
    check(bounded.handle(h.event(4,limitCommand),1)->error==38,"Replay cache refuses overflow instead of evicting live command IDs");
    Harness timeout(v);timeout.client.request(timeout.a,0x203,adminNumber(AdminTag::GetOwner),0);
    tx(timeout.a,0);timeout.lifecycle();check(timeout.client.expire(timeout.a,120001).size()==1&&!timeout.a.pendingCount(),"Admin response timeout cancels transport work");
    Harness correlation(v);const auto requestId=correlation.client.request(correlation.a,0x203,adminNumber(AdminTag::GetConfig,5),0);
    auto response=correlation.event(7000,adminPayload(AdminTag::OwnerResponse,encodeUser(User{})));
    response.header.from=0x203;response.header.to=0x201;response.senderPublicKey=correlation.pc->publicKey();response.data->requestId=requestId;
    check(!correlation.client.handle(response,0)&&!correlation.client.hasSession(0x203,0),"Wrong response operation cannot establish an admin session");
    response.data=adminData(adminPayload(AdminTag::ConfigResponse,configEnvelope(5,{})),false);response.data->requestId=requestId;
    response.header.to=0x999;check(!correlation.client.handle(response,0),"Response for another recipient ignored");
    correlation.client.forgetPeer(correlation.a,0x203,0);check(!correlation.a.queuedCount(),"Forgetting admin peer cancels queued commands");
    Harness reliableReply(v);reliableReply.client.request(reliableReply.a,0x203,adminNumber(AdminTag::GetOwner),0);
    const auto query=tx(reliableReply.a,0);reliableReply.lifecycle();reliableReply.c.receive(query.radioBytes,0);
    for(const auto &event:reliableReply.c.takeEvents())if(auto answer=reliableReply.server->handle(event,0))
        reliableReply.c.sendPki(answer->reply,answer->to,true,0);
    reliableReply.a.receive(tx(reliableReply.c,0).radioBytes,0);
    for(const auto &event:reliableReply.a.takeEvents())reliableReply.client.handle(event,0);
    check(reliableReply.a.queuedCount()==1,"Reliable firmware admin response gets a receipt ACK");
    reliableReply.c.receive(tx(reliableReply.a,0).radioBytes,0);
    check(!reliableReply.c.pendingCount(),"Pairwise authenticated ACK completes reliable admin response");
    std::mt19937 random(402);AdminServer parserServer(policy,state,h.crypto);
    for(unsigned i=0;i<5000;++i) {
        Bytes input(random()%240);for(auto &byte:input)byte=std::uint8_t(random());
        try { decodeAdmin(input); }catch(const std::exception &) {}
        try { decodeAdminChannel(input); }catch(const std::exception &) {}
        try { configEnvelopeType(input); }catch(const std::exception &) {}
        auto fuzz=h.event(i+10000,adminNumber(AdminTag::GetOwner));fuzz.data->payload=std::move(input);
        parserServer.handle(fuzz,i);
    }
    check(!parserServer.transactionOpen(),"Malformed authenticated admin payloads never open a transaction without a token");
    std::cout<<"PASS authenticated admin client/server, transactions, authorization, replay and expiry\n";
}
#ifdef OBRII_MESHTASTIC_WITH_LORA
void adminIqTests(const std::map<std::string,Bytes> &v) {
    Harness h(v);Ids ids;Node relay(cfg(0x202),[&]{return ids.next();});
    obrii::lora::Config config;config.spreadingFactor=7;LoRaRadio mod(config),demod(config);
    auto air=[&](const Bytes &wire) {
        const auto wave=mod.modulate(wire);const auto decoded=demod.push(wave);
        if(decoded.size()!=1)throw std::runtime_error("Expected one IQ admin packet");return decoded[0].radioBytes;
    };
    auto exchange=[&](AdminMessage m) {
        ++h.time;h.client.request(h.a,0x203,std::move(m),h.time);
        const auto request=tx(h.a,h.time);h.lifecycle();relay.receive(air(request.radioBytes),h.time);
        h.time+=10;h.service(air(tx(relay,h.time).radioBytes));
        relay.receive(air(tx(h.c,h.time).radioBytes),h.time);h.time+=10;
        h.a.receive(air(tx(relay,h.time).radioBytes),h.time);
        std::optional<AdminClientResult> result;for(const auto &event:h.a.takeEvents())if(auto r=h.client.handle(event,h.time))result=*r;
        if(!result)throw std::runtime_error("Expected IQ admin response");return *result;
    };
    check(exchange(adminNumber(AdminTag::GetOwner)).success,"PKI admin session handshake across three LoRa IQ nodes");
    User owner;owner.longName="IQ admin";
    check(exchange(adminPayload(AdminTag::SetOwner,encodeUser(owner))).success&&h.server->state().owner.longName=="IQ admin", "PKI admin settings write across three LoRa IQ nodes");
    check(!has(relay.takeEvents(),EventKind::Message),"Relay never sees admin plaintext or session token");
    std::cout<<"PASS A-B-C-B-A PKI admin handshake and mutation through LoRa IQ\n";
}
#endif
}
int main() {
    try { const auto v=vectors();primitiveTests(v);pkiNodeTests(v);adminTests(v);
#ifdef OBRII_MESHTASTIC_WITH_LORA
        adminIqTests(v);
#endif
    }
    catch(const std::exception &e) { ++failures;std::cerr<<"EXCEPTION "<<e.what()<<'\n'; }
    std::cout<<(failures?"FAILED ":"ALL PASS ")<<failures<<" failures\n";return failures?1:0;
}
