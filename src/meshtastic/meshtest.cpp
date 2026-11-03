#include "meshtastic.h"
#include "meshcrypto_internal.h"
#ifdef OBRII_MESHTASTIC_WITH_LORA
#include "meshlora.h"
#endif
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
using namespace obrii::meshtastic;
namespace {
int failures=0;
void check(bool value,const std::string &name) { if(!value) { ++failures;std::cerr<<"FAIL "<<name<<'\n'; } }
template<class F> void rejects(F f,const std::string &name) { try { f();check(false,name); }catch(const std::exception &) {} }
Bytes hex(const std::string &s) {
    if(s.size()%2)throw std::invalid_argument("Odd hex");Bytes out;
    for(std::size_t i=0;i<s.size();i+=2)out.push_back(std::uint8_t(std::stoul(s.substr(i,2),nullptr,16)));return out;
}
bool has(const std::vector<Event> &events,EventKind kind) { return std::any_of(events.begin(),events.end(),[&](const auto &e){return e.kind==kind;}); }
int count(const std::vector<Event> &events,EventKind kind) { return int(std::count_if(events.begin(),events.end(),[&](const auto &e){return e.kind==kind;})); }
NodeConfig config(std::uint32_t id) {
    NodeConfig c;c.nodeId=id;c.relayDelayMs=10;c.relayJitterMs=0;c.ackTimeoutMs=100;c.retryDelayMs=10;c.maxRetries=1;c.historyTtlMs=10000;return c;
}
struct TestNode {
    PacketIdSequence sequence{100};Node node;
    explicit TestNode(NodeConfig c):node(c,[this]{return sequence.next();}) {}
    explicit TestNode(std::uint32_t id):TestNode(config(id)) {}
};
Transmission tx(Node &node,std::uint64_t time) {
    auto t=node.poll(time);if(!t)throw std::runtime_error("Expected TX at "+std::to_string(time));
    node.completeTransmission(t->ticket,true,time);return *t;
}
Header header(std::uint32_t from=0x11223344,std::uint32_t id=0x12345678) {
    Header h;h.from=from;h.id=id;h.relayNode=std::uint8_t(from);return h;
}
void cryptoTests() {
    const auto plaintext=hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    std::array<std::uint8_t,16> iv{};for(int i=0;i<16;++i)iv[i]=std::uint8_t(0xf0+i);
    auto encrypted=plaintext;
    detail::aes128Ctr(hex("2b7e151628aed2a6abf7158809cf4f3c"),iv,encrypted);
    check(encrypted==hex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee"),"NIST SP800-38A AES128 CTR");
    encrypted=plaintext;
    detail::aes256Ctr(hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"),iv,encrypted);
    check(encrypted==hex("601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c52b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6"),"NIST SP800-38A AES256 CTR");
    check(nonce(0x11223344,0x0123456789abcdefull)==std::array<std::uint8_t,16>{0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01,0x44,0x33,0x22,0x11,0,0,0,0},"Meshtastic nonce byte order");
    std::ifstream input(OBRII_MESHTASTIC_FIXTURES);check(bool(input),"Independent .NET AES fixtures present");
    std::string line;int fixtures=0;
    while(std::getline(input,line)) {
        if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty()||line[0]=='#')continue;
        std::vector<std::string> fields;std::istringstream row(line);std::string cell;
        while(std::getline(row,cell,'|'))fields.push_back(cell);
        if(fields.size()!=7)throw std::runtime_error("Bad independent fixture row");
        const auto key=hex(fields[1]),plain=hex(fields[4]),cipher=hex(fields[5]),wire=hex(fields[6]);
        const auto from=std::uint32_t(std::stoul(fields[2],nullptr,16)),id=std::uint32_t(std::stoul(fields[3],nullptr,16));
        check(cryptPayload(key,from,id,plain)==cipher,"Independent .NET ciphertext "+fields[0]);
        Channel ch;ch.psk=key;
        check(seal(header(from,id),textMessage("hello"),ch)==wire,"Independent full radio packet "+fields[0]);
        const auto opened=open(decodeRadio(wire),{ch});
        check(opened.packet&&text(opened.packet->data)=="hello","Independent radio decode "+fields[0]);++fixtures;
    }
    check(fixtures==2,"Both AES128/AES256 independent vectors exercised");
    check(Channel{}.hash()==8,"LongFast default channel hash");
    check(relayId(0x123400)==0xff&&relayId(0x123444)==0x44,"Relay ID zero-byte mapping");
    check(Channel{}.effectiveKey()==hex("d4f1bb3a20290759f0bcffabcf4e6901"),"Short PSK expansion");
    for(int length=0;length<=32;++length) {
        Channel ch;ch.psk=Bytes(length,0x57);const auto k=ch.effectiveKey();
        check(k.size()==std::size_t(length==0?0:length<=16?16:32),"PSK padding length");
        const auto data=encodeData(textMessage("roundtrip"));check(cryptPayload(k,55,78,cryptPayload(k,55,78,data))==data,"PSK CTR symmetry");
    }
    Channel clear;clear.psk={0};check(clear.effectiveKey().empty(),"Explicit no encryption");
    auto wire=seal(header(),textMessage("hello"),Channel{});
    check(open(decodeRadio(wire),{Channel{},Channel{}}).status==DecodeStatus::AmbiguousChannel,"Hash collision cannot select first channel silently");
    Channel foreign;foreign.name="foreign";check(open(decodeRadio(wire),{foreign}).status==DecodeStatus::UnknownChannel,"Unknown channel");
    Channel wrong;wrong.psk=Bytes(16,0);wrong.psk[0]=1;wrong.psk[1]=1;wrong.name="LongFast";
    auto bad=decodeRadio(wire);bad.header.channelHash=wrong.hash();
    check(open(bad,{wrong}).status==DecodeStatus::InvalidPayload,"Wrong key fails protobuf plausibility for fixture");
    rejects([&]{cryptPayload(Bytes(15),1,2,{1});},"Invalid AES key length");
    std::cout<<"PASS NIST crypto and independent full Meshtastic radio vectors\n";
}
void protobufTests() {
    check(encodeData(textMessage("hello"))==hex("0801120568656c6c6f"),"Official Data field-number vector");
    Data all=textMessage(u8"Привіт");all.wantResponse=true;all.dest=0x12345678;all.source=0x87654321;
    all.requestId=19;all.replyId=20;all.emoji=21;all.bitfield=0;all.signature=Bytes(64,17);all.unknownFields=hex("a00601");
    const auto encoded=encodeData(all);const auto parsed=decodeData(encoded);
    check(encodeData(parsed)==encoded&&text(parsed)==u8"Привіт"&&parsed.bitfield.has_value(),"Data protobuf all fields, UTF8 and unknown retention");
    check(decodeData(hex("08010803")).port==3,"Protobuf last singular field wins");
    Routing ack;ack.kind=Routing::Kind::Error;
    check(encodeRouting(ack)==hex("1800")&&decodeRouting(hex("1800")).kind==Routing::Kind::Error,"Explicit Routing NONE oneof value");
    check(decodeRouting({}).kind==Routing::Kind::None,"Empty Routing is not a success ACK");
    User user;user.id="!11223344";user.longName="Obrii";user.shortName="OB";user.role=2;user.publicKey=Bytes(32,17);
    check(encodeUser(decodeUser(encodeUser(user)))==encodeUser(user),"NodeInfo protobuf");
    Position position;position.latitudeE7=0;position.longitudeE7=-700000000;position.altitudeMeters=-17;position.time=123;
    const auto pos=decodePosition(encodePosition(position));
    check(pos.latitudeE7&&*pos.latitudeE7==0&&pos.longitudeE7==position.longitudeE7&&pos.altitudeMeters==position.altitudeMeters,"Position negative int32 and optional zero");
    rejects([&]{decodeData(hex("128080808080808080808002"));},"Varint overflow");
    rejects([&]{decodeData(hex("1206abcd"));},"Truncated bytes");
    rejects([&]{decodeData(hex("0001"));},"Zero tag");
    rejects([&]{decodeData(hex("0a00"));},"Known field wrong wire type");
    rejects([&]{decodeData(hex("0b"));},"Protobuf groups rejected");
    rejects([&]{auto d=all;d.unknownFields=hex("0802");encodeData(d);},"Unknown-field injection blocked");
    rejects([&]{textMessage(std::string("\xc0\x80",2));},"Overlong UTF8 rejected");
    rejects([&]{textMessage(std::string(235,'x'));},"Text must fit full Data overhead");
    check(encodeData(textMessage(std::string(234,'x'))).size()==DataMtu,"Maximum text MTU");
    rejects([&]{auto p=position;p.latitudeE7=910000000;encodePosition(p);},"Coordinates validated");
    rejects([&]{decodeRadio(Bytes(16));},"Header-only radio frame rejected");
    rejects([&]{decodeRadio(Bytes(256));},"Oversize radio frame rejected");
    ApiFramer framer;const Bytes payload{0x94,0xc3,0,0,0x12};const auto frame=framer.encode(payload);
    Bytes stream{1,2,0x94,0,0x94,0xc3,0xff,0xff,7};stream.insert(stream.end(),frame.begin(),frame.end());stream.insert(stream.end(),frame.begin(),frame.end());
    std::vector<Bytes> frames;
    for(auto byte:stream) { auto found=framer.push(&byte,1);frames.insert(frames.end(),found.begin(),found.end()); }
    check(frames==std::vector<Bytes>{payload,payload}&&framer.bufferedBytes()==0,"API framing split bytes, noise, oversize and embedded magic");
    framer.push(frame.data(),3);framer.reset();check(framer.bufferedBytes()==0,"API framing reset");
    std::cout<<"PASS protobuf, payload bounds and streaming API framing\n";
}
void networkTests() {
    TestNode a(0x101),b(0x102),c(0x103);
    const auto id=a.node.send(textMessage("three hops"),0,0x103,true,0);
    const auto original=tx(a.node,0);b.node.receive(original.radioBytes,1);
    check(!b.node.poll(11,true),"Busy channel defers relay");
    const auto relay=tx(b.node,11);const auto relayed=decodeRadio(relay.radioBytes);
    check(relayed.header.hopLimit==2&&relayed.body==decodeRadio(original.radioBytes).body,"Relay decrements hops and preserves ciphertext");
    c.node.receive(relay.radioBytes,12);c.node.receive(relay.radioBytes,13);
    const auto received=c.node.takeEvents();check(count(received,EventKind::Message)==1&&has(received,EventKind::Duplicate),"Duplicate app delivery suppressed");
    const auto ack=tx(c.node,13);b.node.receive(ack.radioBytes,14);const auto returnRelay=tx(b.node,24);a.node.receive(returnRelay.radioBytes,25);
    const auto events=a.node.takeEvents();check(a.node.pendingCount()==0&&has(events,EventKind::Acknowledged),"A-B-C-B-A PSK ACK clears correct pending packet");
    // A lost ACK triggers the same ACK bytes again without repeating app delivery.
    c.node.receive(relay.radioBytes,26);const auto ackAgain=tx(c.node,26);
    check(ackAgain.radioBytes==ack.radioBytes,"Duplicate request reuses identical ACK ciphertext and ID");
    check(!has(c.node.takeEvents(),EventKind::Message),"Lost ACK retry does not redeliver text");
    TestNode lostA(0x151),lostB(0x152),lostC(0x153);
    lostA.node.send(textMessage("lost return hop"),0,0x153,true,0);
    lostB.node.receive(tx(lostA.node,0).radioBytes,0);
    lostC.node.receive(tx(lostB.node,10).radioBytes,10);
    lostB.node.receive(tx(lostC.node,10).radioBytes,10);
    tx(lostB.node,20); // Drop the final B->A ACK, after all nodes recorded it.
    lostA.node.poll(100);lostB.node.receive(tx(lostA.node,110).radioBytes,110);
    lostC.node.receive(tx(lostB.node,120).radioBytes,120);
    lostB.node.receive(tx(lostC.node,120).radioBytes,120);
    lostA.node.receive(tx(lostB.node,130).radioBytes,130);
    check(lostA.node.pendingCount()==0&&count(lostC.node.takeEvents(),EventKind::Message)==1,
          "Lost multihop ACK recovers through duplicate relays without redelivery");
    TestNode broadcast(0x111),relayNode(0x112);
    broadcast.node.send(textMessage("all"),0,Broadcast,true,0);const auto broad=tx(broadcast.node,0);
    check(!decodeRadio(broad.radioBytes).header.wantAck,"Broadcast clears on-air wantAck");
    relayNode.node.receive(broad.radioBytes,1);const auto echoed=tx(relayNode.node,11);broadcast.node.receive(echoed.radioBytes,12);
    check(!broadcast.node.pendingCount()&&has(broadcast.node.takeEvents(),EventKind::Relayed),"Broadcast relay confirmation distinguished from delivery");
    TestNode retry(0x121);retry.node.send(textMessage("retry"),0,0x122,true,0);
    auto held=retry.node.poll(500);check(held&&held->kind==TxKind::Original,"No ACK timer before first actual TX");
    check(!retry.node.poll(1000),"Only one active TX handed out");
    retry.node.completeTransmission(held->ticket,true,1000);check(!retry.node.poll(1099),"ACK deadline starts after TX completion");
    check(!retry.node.poll(1100),"Retry respects delay");const auto retransmit=tx(retry.node,1110);
    check(retransmit.kind==TxKind::Retry&&retransmit.radioBytes==held->radioBytes,"Retry preserves original bytes and ID");
    retry.node.poll(1210);check(!retry.node.pendingCount()&&has(retry.node.takeEvents(),EventKind::TimedOut),"Finite retry timeout");
    TestNode failure(0x131);failure.node.send(textMessage("local failure"),0,0x132,true,0);auto attempt=failure.node.poll(0);
    failure.node.completeTransmission(attempt->ticket,false,1);check(!failure.node.pendingCount()&&has(failure.node.takeEvents(),EventKind::TxFailed),"Local failure does not pretend remote timeout");
    rejects([&]{failure.node.completeTransmission(attempt->ticket,true,2);},"Stale completion ticket rejected");
    rejects([&]{failure.node.poll(1);},"Clock regression rejected");
    a.node.cancel(id,26);
    std::cout<<"PASS three-node forwarding, ACK, duplicate handling, busy radio and retries\n";
}
void edgeTests() {
    auto cfg=config(0x201);cfg.maxHistory=1;cfg.maxNodes=1;cfg.maxEvents=2;cfg.maxQueue=1;cfg.maxPending=1;
    TestNode n(cfg);auto h=header(0x202,100);h.to=cfg.nodeId;h.wantAck=true;
    const auto wire=seal(h,textMessage("hello"),Channel{});n.node.receive(wire,0);tx(n.node,0);n.node.receive(wire,1);tx(n.node,1);
    check(n.node.pendingCount()==0&&n.node.nodes().size()==1,"ACK handling with one-entry cache remains valid");
    for(int i=0;i<20;++i) { h.from=0x300+i;h.id=101+i;n.node.receive(seal(h,textMessage("load"),Channel{}),2+i); }
    check(n.node.nodes().size()==1&&n.node.takeEvents().size()<=2&&n.node.droppedEventCount()>0,"Node/event caches bounded under load");
    TestNode waiting(0x211);const auto packetId=waiting.node.send(textMessage("request"),0,0x212,true,0);tx(waiting.node,0);
    Routing routing;routing.kind=Routing::Kind::Error;Data d;d.port=RoutingPort;d.payload=encodeRouting(routing);d.requestId=packetId;
    auto forged=header(0x213,104);forged.to=0x211;
    waiting.node.receive(seal(forged,d,Channel{}),1);check(waiting.node.pendingCount()==1,"ACK from wrong peer ignored");
    forged.from=0x212;forged.id=105;d.payload={};waiting.node.receive(seal(forged,d,Channel{}),2);
    check(waiting.node.pendingCount()==1,"Empty Routing payload is not an ACK");
    routing.error=32;d.payload=encodeRouting(routing);forged.id=106;waiting.node.receive(seal(forged,d,Channel{}),3);
    check(!waiting.node.pendingCount()&&has(waiting.node.takeEvents(),EventKind::NegativeAck),"Valid PSK NAK clears pending with failure event");
    auto off=config(0x222);off.relay=RelayMode::Off;TestNode noRelay(off);
    auto broad=seal(header(0x223,123),textMessage("hop"),Channel{});noRelay.node.receive(broad,0);check(noRelay.node.queuedCount()==0,"Relay disabled");
    TestNode client(0x224);client.node.receive(broad,0);auto duplicate=decodeRadio(broad);--duplicate.header.hopLimit;duplicate.header.relayNode=0x25;
    client.node.receive(encodeRadio(duplicate),1);check(!client.node.queuedCount(),"Client cancels queued relay after overheard duplicate");
    auto routerConfig=config(0x225);routerConfig.relay=RelayMode::Router;TestNode router(routerConfig);
    router.node.receive(broad,0);router.node.receive(encodeRadio(duplicate),1);check(router.node.queuedCount()==1,"Router keeps its relay");
    auto lower=decodeRadio(broad);lower.header.hopLimit=1;TestNode upgrade(0x226);upgrade.node.receive(encodeRadio(lower),0);upgrade.node.receive(broad,1);
    check(upgrade.node.queuedCount()==1&&decodeRadio(tx(upgrade.node,11).radioBytes).header.hopLimit==2,"Higher-hop duplicate upgrades queued relay without duplicates");
    TestNode cancel(0x230);const auto cancelId=cancel.node.send(textMessage("cancel"),0,0x231,true,0);cancel.node.cancel(cancelId,1);
    check(cancel.node.queuedCount()==0&&cancel.node.pendingCount()==0,"Cancel removes queued reliable work");
    User u;u.id="!00000240";u.longName="Node";u.shortName="N";Data info;info.port=NodeInfoPort;info.payload=encodeUser(u);
    TestNode db(0x241);db.node.receive(seal(header(0x240,200),info,Channel{}),0);
    check(db.node.nodes().size()==1&&db.node.nodes()[0].user&&db.node.nodes()[0].user->longName=="Node","NodeInfo updates bounded node database");
    Position p;p.latitudeE7=500000000;p.longitudeE7=300000000;info.port=PositionPort;info.payload=encodePosition(p);
    db.node.receive(seal(header(0x240,201),info,Channel{}),1);check(db.node.nodes()[0].position&&db.node.nodes()[0].position->latitudeE7==p.latitudeE7,"Position updates node database");
    auto conflict=decodeRadio(broad);conflict.body[0]^=1;client.node.receive(encodeRadio(conflict),2);
    check(has(client.node.takeEvents(),EventKind::IdentityConflict),"ID/ciphertext conflict rejected");
    PacketIdSequence exhausted(0xffffffffu);check(exhausted.next()==0xffffffffu,"Last uint32 packet ID");rejects([&]{exhausted.next();},"Packet ID wrap rejected");
    auto limited=config(0x251);limited.maxQueue=1;limited.maxPending=1;TestNode full(limited);full.node.send(textMessage("one"),0,0x252,true,0);
    rejects([&]{full.node.send(textMessage("two"),0,0x252,true,0);},"Queue limit enforced");
    TestNode zeroRelay(0x500);auto preferred=decodeRadio(broad);preferred.header.to=0x555;preferred.header.nextHop=0xff;
    zeroRelay.node.receive(encodeRadio(preferred),0);
    check(decodeRadio(tx(zeroRelay.node,10).radioBytes).header.relayNode==0xff,"Next-hop preference honors nodes ending in zero");
    auto tiny=config(0x601);tiny.maxHistory=1;tiny.maxQueue=2;tiny.maxPending=2;TestNode eviction(tiny);
    eviction.node.send(textMessage("first"),0,Broadcast,true,0);
    eviction.node.send(textMessage("second"),0,Broadcast,true,0);
    auto first=decodeRadio(tx(eviction.node,0).radioBytes);tx(eviction.node,0);
    --first.header.hopLimit;first.header.relayNode=2;eviction.node.receive(encodeRadio(first),1);
    check(eviction.node.pendingCount()==1&&has(eviction.node.takeEvents(),EventKind::Relayed),"Pending confirmation survives bounded own-history eviction");
    std::cout<<"PASS bounded state, ACK peer checks, relay policies and identity lifecycle\n";
}
void malformedTests() {
    std::mt19937 random(0x2051);
    TestNode receiver(0xabcdef);
    ApiFramer framer;
    for(unsigned i=0;i<10000;++i) {
        Bytes input(random()%256);for(auto &byte:input)byte=std::uint8_t(random());
        try { decodeData(input); }catch(const std::invalid_argument &) {}
        try { decodeUser(input); }catch(const std::invalid_argument &) {}
        try { decodePosition(input); }catch(const std::invalid_argument &) {}
        try { decodeRouting(input); }catch(const std::invalid_argument &) {}
        receiver.node.receive(input,i);framer.push(input);
        check(framer.bufferedBytes()<=516,"Framer remains bounded on malformed bytes");
    }
    check(receiver.node.queuedCount()<=64&&receiver.node.takeEvents().size()<=128,"Malformed radio input preserves bounded state");
    std::cout<<"PASS 10000 malformed input cases through protobuf/radio/API parsers\n";
}
#ifdef OBRII_MESHTASTIC_WITH_LORA
void iqTests() {
    obrii::lora::Config config;config.spreadingFactor=7;
    LoRaRadio radio(config),receiver(config);
    TestNode a(0x401),b(0x402),c(0x403);
    a.node.send(textMessage(u8"Тест IQ"),0,0x403,true,0);
    auto deliver=[&](Node &from,Node &to,std::uint64_t time) {
        const auto outgoing=tx(from,time);const auto iq=radio.modulate(outgoing);int got=0;
        for(std::size_t i=0;i<iq.size();) {
            const auto size=std::min<std::size_t>(1379,iq.size()-i);
            for(const auto &p:receiver.push(iq.data()+i,size)) { to.receive(p.radioBytes,time);++got; }
            i+=size;
        }
        check(got==1,"One complete LoRa/Meshtastic IQ packet per link");
    };
    deliver(a.node,b.node,0);deliver(b.node,c.node,10);deliver(c.node,b.node,10);deliver(b.node,a.node,20);
    const auto events=c.node.takeEvents();bool found=false;
    for(const auto &e:events)if(e.kind==EventKind::Message&&e.data&&e.data->port==TextPort)found=text(*e.data)==u8"Тест IQ";
    check(found&&!a.node.pendingCount()&&has(a.node.takeEvents(),EventKind::Acknowledged),"A-B-C-B-A IQ, AES, UTF8 text and ACK end to end");
    rejects([&]{auto invalid=config;invalid.payloadCrc=false;LoRaRadio bad(invalid);},"Meshtastic PHY requires CRC");
    std::cout<<"PASS complete Meshtastic -> LoRa IQ -> Meshtastic three-node path\n";
}
#endif
}
int main() {
    try { cryptoTests();protobufTests();networkTests();edgeTests();malformedTests();
#ifdef OBRII_MESHTASTIC_WITH_LORA
        iqTests();
#endif
    } catch(const std::exception &e) { ++failures;std::cerr<<"EXCEPTION "<<e.what()<<'\n'; }
    std::cout<<(failures?"FAILED ":"ALL PASS ")<<failures<<" failures\n";return failures?1:0;
}
