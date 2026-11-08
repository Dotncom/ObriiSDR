#include "lorawan.h"
#include "regional.h"
#ifdef OBRII_LORAWAN_WITH_LORA
#include "lorawanlora.h"
#endif
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>

using namespace obrii::lorawan;
void check(bool condition, const char *why) { if(!condition) throw Error(why); }
template<class F> void rejects(F action) {
    bool rejected=false; try { action(); } catch(const std::exception &) { rejected=true; }
    check(rejected,"Expected rejection");
}
Bytes unhex(const std::string &s) {
    Bytes b; for(std::size_t i=0; i<s.size(); i+=2) b.push_back(std::uint8_t(std::stoul(s.substr(i,2),nullptr,16))); return b;
}
template<std::size_t N> std::array<std::uint8_t,N> fixed(const std::string &s) {
    const auto b=unhex(s); check(b.size()==N,"Fixture size"); std::array<std::uint8_t,N> a{}; std::copy(b.begin(),b.end(),a.begin()); return a;
}
std::string hex(const Bytes &b) {
    static constexpr char chars[]="0123456789abcdef"; std::string s;
    for(auto v:b) { s+=chars[v>>4]; s+=chars[v&15]; } return s;
}
void vectors() {
    std::ifstream file(OBRII_LORAWAN_VECTORS); check(bool(file),"Independent fixture file missing");
    std::string line; unsigned cases=0;
    while(std::getline(file,line)) {
        if(line.empty() || line[0]=='#') continue;
        const auto tab=line.find('\t'); check(tab!=std::string::npos,"Fixture separator");
        const auto expected=line.substr(tab+1); std::istringstream in(line.substr(0,tab));
        std::vector<std::string> p; std::string token; while(in>>token) p.push_back(token);
        auto value=[&](std::size_t i,int base=10) { return std::uint32_t(std::stoull(p.at(i),nullptr,base)); };
        auto bytes=[&](std::size_t i) { return p.at(i)=="-" ? Bytes{}:unhex(p.at(i)); };
        Bytes wire;
        if(p[0]=="data-encode") {
            SessionKeys keys{fixed<16>(p[1]),fixed<16>(p[2])}; DataFrame f;
            f.type=FrameType(value(3)); f.devAddr=value(4,16); f.counter=value(5); f.control=std::uint8_t(value(6,16));
            f.options=bytes(7); if(p[8]!="-") f.port=std::uint8_t(value(8)); f.payload=bytes(9);
            wire=encodeData(f,keys);
            const auto decoded=decodeData(unhex(expected),keys,f.counter);
            check(decoded.type==f.type && decoded.devAddr==f.devAddr && decoded.counter==f.counter && decoded.control==f.control
                && decoded.options==f.options && decoded.port==f.port && decoded.payload==f.payload,"Independent data decode");
            auto bad=wire; bad.back()^=1; rejects([&] { decodeData(bad,keys,f.counter); });
        } else if(p[0]=="join-request") {
            const auto key=fixed<16>(p[1]); JoinRequest j{fixed<8>(p[2]),fixed<8>(p[3]),std::uint16_t(value(4))};
            wire=encodeJoinRequest(j,key); const auto decoded=decodeJoinRequest(unhex(expected),key);
            check(decoded.joinEui==j.joinEui && decoded.devEui==j.devEui && decoded.devNonce==j.devNonce,"Independent JoinRequest decode");
        } else if(p[0]=="join-accept") {
            const auto key=fixed<16>(p[1]); JoinAccept a;
            a.joinNonce=value(2); a.netId=value(3,16); a.devAddr=value(4,16); a.rx1Offset=std::uint8_t(value(5));
            a.rx2DataRate=std::uint8_t(value(6)); a.rxDelay=std::uint8_t(value(7)); if(p[8]!="-") a.cfList=fixed<16>(p[8]);
            wire=encodeJoinAccept(a,key); const auto decoded=decodeJoinAccept(unhex(expected),key);
            check(decoded.joinNonce==a.joinNonce && decoded.netId==a.netId && decoded.devAddr==a.devAddr
                && decoded.rx1Offset==a.rx1Offset && decoded.rx2DataRate==a.rx2DataRate && decoded.rxDelay==a.rxDelay
                && decoded.cfList==a.cfList,"Independent JoinAccept decode");
        } else if(p[0]=="derive") {
            const auto keys=deriveSessionKeys(fixed<16>(p[1]),value(2),value(3,16),std::uint16_t(value(4)));
            check(hex(Bytes(keys.network.begin(),keys.network.end()))+" "+hex(Bytes(keys.application.begin(),keys.application.end()))==expected,"Independent session keys");
        } else throw Error("Unknown fixture operation");
        if(p[0]!="derive") check(hex(wire)==expected,"Independent wire encode");
        ++cases;
    }
    check(cases==2837,"Independent vector count");
    std::cout<<"PASS "<<cases<<" independent Go LoRaWAN vectors, both directions, ports, FOpts, FCnt32 and joins\n";
}
Credentials credentials() {
    return {fixed<8>("0102030405060708"),fixed<8>("8090a0b0c0d0e0f0"),fixed<16>("00112233445566778899aabbccddeeff")};
}
void sessions() {
    static_assert(!std::is_copy_constructible<Session>::value,"Sessions cannot be copied");
    DeviceJoinState savedDevice; JoinServerState savedServer;
    OtaaDevice device(credentials(),{},[&](const auto &s) { savedDevice=loadDeviceJoin(saveDeviceJoin(s)); });
    JoinServer server(credentials(),{},[&](const auto &s) { savedServer=loadJoinServer(saveJoinServer(s)); });
    const auto request=device.request(); check(savedDevice.nextDevNonce==1 && savedDevice.pendingDevNonce==0,"Persist before join TX");
    JoinAccept parameters; parameters.netId=0x13; parameters.devAddr=0x26011bda;
    const auto response=server.accept(request,parameters);
    rejects([&] { server.accept(request,parameters); });
    auto bad=response.wire; bad[4]^=1; rejects([&] { device.accept(bad); });
    check(savedDevice.pendingDevNonce==0,"Failed MIC does not consume pending join");
    rejects([&] { device.accept(response.wire,[](const auto &) { throw Error("Region refused parameters"); }); });
    check(!savedDevice.activated,"Region validation precedes activation commit");
    device.accept(response.wire,[&](const auto &) { rejects([&] { device.request(); }); });
    check(savedDevice.activated->keys.application==savedServer.activated->keys.application,"OTAA derived keys agree");
    rejects([&] { device.accept(response.wire); });
    auto ds=*device.state().activated; ds.maxUplinkCopies=3; device.updateSession(ds);
    auto ns=*server.state().activated; ns.maxUplinkCopies=3; server.updateSession(ns);
    Session d(Role::Device,*device.state().activated,[&](const auto &s) { device.updateSession(s); });
    Session n(Role::Network,*server.state().activated,[&](const auto &s) { server.updateSession(s); });
    DataFrame up; up.type=FrameType::ConfirmedUp; up.port=1; up.payload={'h','e','l','l','o'};
    const auto wire=d.transmit(up); const auto received=n.receive(wire);
    check(received.frame.counter==0 && received.frame.payload==up.payload && !received.duplicate,"First uplink FCnt0");
    check(savedDevice.activated->nextTransmit==1,"Live counters persisted in join record");
    Session stale(Role::Device,ds,[&](const auto &s) { device.updateSession(s); });
    auto changed=up; changed.payload={'x'};
    rejects([&] { stale.transmit(changed); }); // A stale owner cannot reuse a reserved FCnt with different plaintext.
    for(unsigned i=0; i<2; ++i) {
        check(d.repeatUplink()==wire,"Repetition preserves ciphertext and FCnt");
        const auto duplicate=n.receive(wire); check(duplicate.duplicate && duplicate.frame.payload.empty(),"Uplink dedup suppresses app data");
    }
    rejects([&] { d.repeatUplink(); }); rejects([&] { n.receive(wire); });
    DataFrame down; down.type=FrameType::ConfirmedDown; down.port=3; down.payload={'w','o','r','l','d'};
    const auto reply=n.transmit(down); const auto delivery=d.receive(reply);
    check(delivery.frame.counter==0 && delivery.frame.payload==down.payload && (delivery.frame.control&0x20),"First downlink and ACK");
    rejects([&] { d.receive(reply); }); rejects([&] { n.repeatUplink(); });
    DataFrame ack; const auto ackWire=d.transmit(ack);
    const auto ackReceived=n.receive(ackWire); check(ackReceived.frame.control&0x20,"Confirmed downlink ACK");
    OtaaDevice rebootedDevice(credentials(),savedDevice,[&](const auto &s) { savedDevice=s; });
    JoinServer rebootedServer(credentials(),savedServer,[&](const auto &s) { savedServer=s; });
    check(rebootedDevice.state().activated->nextTransmit==2,"Reboot preserves ABP/OTAA FCnt");
    Session rebootedNetwork(Role::Network,*rebootedServer.state().activated,[&](const auto &s) { rebootedServer.updateSession(s); });
    check(rebootedNetwork.receive(ackWire).duplicate,"Reboot preserves uplink dedup cache");
    const auto newRequest=rebootedDevice.request(); check(decodeJoinRequest(newRequest,credentials().appKey).devNonce==1,"Reboot preserves DevNonce");
    const auto newAccept=rebootedServer.accept(newRequest,parameters); rebootedDevice.accept(newAccept.wire);
    check(rebootedDevice.state().lastJoinNonce==1,"JoinNonce increments");
    rejects([&] { rebootedDevice.updateSession(d.state()); });
    // A store may have committed before reporting failure. The old instance
    // must become unusable; only a reload may continue with the reserved FCnt.
    SessionState baseline; baseline.devAddr=1; SessionState committed;
    Session failure(Role::Device,baseline,[&](const auto &s) { committed=s; throw Error("Storage failed after commit"); });
    rejects([&] { failure.transmit({}); }); rejects([&] { failure.transmit({}); });
    Session recovered(Role::Device,committed,[](const auto &) {});
    check(wireCounter(recovered.transmit({}))==1,"Ambiguous persistence failure cannot reuse FCnt");
    SessionState wrap; wrap.devAddr=1; wrap.nextTransmit=65535;
    Session sender(Role::Device,wrap,[](const auto &) {});
    wrap.role=Role::Network; wrap.nextTransmit=0;
    Session receiver(Role::Network,wrap,[](const auto &) {});
    check(receiver.receive(sender.transmit({})).frame.counter==65535,"Counter near rollover");
    check(receiver.receive(sender.transmit({})).frame.counter==65536,"16-bit wire rollover reconstructs FCnt32");
    auto exhausted=baseline; exhausted.nextTransmit=0x100000000ULL;
    Session end(Role::Device,exhausted,[](const auto &) {}); rejects([&] { end.transmit({}); });
    DeviceJoinState nonceEnd; nonceEnd.nextDevNonce=65536;
    OtaaDevice noNonce(credentials(),nonceEnd,[](const auto &) {}); rejects([&] { noNonce.request(); });
    // Snapshot CRC, size and schema fail closed rather than regenerating keys.
    auto blob=saveSession(committed);
    for(std::size_t i=0; i<blob.size(); ++i) { auto corrupt=blob; corrupt[i]^=1; rejects([&] { loadSession(corrupt); }); }
    rejects([&] { loadSession(Bytes(2049)); });
    std::cout<<"PASS OTAA/ABP, persistent counters/nonces, ACKs, NbTrans, replay, reboot and storage failure\n";
}
DataFrame commandFrame(Bytes commands, bool portZero=false) {
    DataFrame f; f.type=FrameType::UnconfirmedDown;
    if(portZero) { f.port=0; f.payload=std::move(commands); } else f.options=std::move(commands); return f;
}
Bytes frequencyCommand(std::uint8_t cid, std::uint8_t first, std::uint32_t hz) {
    const auto n=hz/100; return {cid,first,std::uint8_t(n),std::uint8_t(n>>8),std::uint8_t(n>>16)};
}
void regions() {
    Eu868 eu; eu.validate();
    check(Eu868::maxMacPayload(0)==59 && Eu868::maxMacPayload(3)==123 && Eu868::maxMacPayload(5)==250,"EU868 limits");
    rejects([] { Eu868::maxMacPayload(7); });
    auto result=eu.processDownlink(commandFrame({2,9,2,6,13,1,0,0,0,128}),150,-40);
    check(result.linkCheck && (*result.linkCheck)[0]==9 && result.deviceTime,"LinkCheck and DeviceTime answers");
    check(result.answers==Bytes({6,150,32}),"DevStatus sign/clamp");
    result=eu.processDownlink(commandFrame({3,0x50,7,0,2}));
    check(result.answers==Bytes({3,7}) && eu.dataRate==5 && eu.nbTrans==2,"LinkADRReq");
    result=eu.processDownlink(commandFrame({3,0x70,7,0,1}));
    check(result.answers==Bytes({3,5}) && eu.dataRate==5,"Unsupported FSK rate NACK");
    result=eu.processDownlink(commandFrame({3,0x40,0,0,1}));
    check(result.answers==Bytes({3,6}) && eu.dataRate==5,"Empty channel mask NACK");
    result=eu.processDownlink(commandFrame({3,0x50,1,0,1,3,0x41,6,0,3}));
    check(result.answers==Bytes({3,7,3,7}) && eu.dataRate==4 && eu.txPower==1 && eu.nbTrans==3,"Atomic LinkADR block");
    auto newChannel=frequencyCommand(7,3,867100000); newChannel.push_back(0x50);
    result=eu.processDownlink(commandFrame(newChannel)); check(result.answers==Bytes({7,3}) && eu.channels[3].enabled,"NewChannelReq");
    result=eu.processDownlink(commandFrame(frequencyCommand(10,3,867300000)));
    check(result.answers==Bytes({10,3}) && eu.channels[3].downlinkHz==867300000,"DlChannelReq");
    result=eu.processDownlink(commandFrame(frequencyCommand(5,0x23,869525000)));
    check(result.answers==Bytes({5,7}) && eu.rx1Offset==2 && eu.rx2DataRate==3 && !eu.stickyAnswers.empty(),"RXParamSetupReq");
    eu.processDownlink(commandFrame({})); check(eu.stickyAnswers.empty(),"Valid downlink clears previous sticky answers");
    result=eu.processDownlink(commandFrame({8,3,4,5,9,0}));
    check(result.answers==Bytes({8,4}) && eu.receiveDelay==3 && eu.dutyExponent==5,"RXTiming/DutyCycle and EU868 TXParam ignore");
    const auto before=eu.rx1Offset; rejects([&] { eu.processDownlink(commandFrame({5,2})); }); check(eu.rx1Offset==before,"Truncated command transactional");
    result=eu.processDownlink(commandFrame({4,2,0x80,4,3}));
    check(eu.dutyExponent==2 && result.unparsed==Bytes({0x80,4,3}),"Unknown CID stops parsing");
    Eu868 slow;
    result=slow.processDownlink(commandFrame(Bytes(100,6),true));
    check(result.answersTruncated && result.answers.size()==51 && parseMac(Direction::Uplink,result.answers).commands.size()==17,"MAC priority truncates at command boundary");
    auto windows=receiveWindows(slow,1000000,868100000,5);
    check(windows[0].nominalUs==2000000 && windows[1].nominalUs==3000000 && windows[0].dataRate==5 && windows[1].dataRate==0,"Class A windows");
    check(nextUplinkTime(windows,true,1000000)==4000000,"Confirmed retry timeout");
    check(receiveWindows(slow,1000000,868100000,5,true)[0].nominalUs==6000000,"Join accept windows");
    rejects([&] { receiveWindows(slow,std::numeric_limits<std::uint64_t>::max(),868100000,5); });
    rejects([&] { receiveWindows(slow,0,869000000,5); });
    DutyState duty; duty=reserveDuty(duty,868100000,1000000,100000);
    check(dutyReady(duty,868300000)==11000000,"Shared EU868 duty subband");
    check(dutyReady(duty,869525000)==1100000,"Independent 10-percent subband");
    rejects([&] { reserveDuty(duty,868300000,10000000,100000); });
    rejects([&] { reserveDuty(duty,868650000,20000000,100000); });
    check(reserveBackoff({},868100000,0,100000,0).aggregate==10000000,"First-hour join backoff");
    check(reserveBackoff({},868100000,0,100000,3600000000ULL).aggregate==100000000,"Ten-hour join backoff");
    check(reserveBackoff({},868100000,0,100000,39600000000ULL).aggregate==1000000000,"Long-term join backoff");
    Eu868 adr; adr.dataRate=1; adr.txPower=3; adr.nbTrans=3; adr.channels[1].enabled=false;
    for(unsigned i=1; i<=160; ++i) {
        const auto ctrl=adr.onNewUplink();
        check(bool(ctrl&0x40)==(i>=64),"ADR ACK threshold");
        if(i==96) check(adr.txPower==0 && adr.dataRate==1,"ADR power restoration before rate backoff");
        if(i==128) check(adr.dataRate==0 && adr.nbTrans==3,"ADR rate fallback");
    }
    check(adr.nbTrans==1 && adr.channels[1].enabled,"ADR default channel restoration");
    adr.processDownlink(commandFrame({})); check(adr.adrAckCount==0,"ADR reset on accepted downlink");
    adr.adrEnabled=false;
    result=adr.processDownlink(commandFrame({3,0x51,1,0,5}));
    check(result.answers==Bytes({3,1}) && adr.dataRate==0 && adr.txPower==0 && adr.nbTrans==1 && !adr.channels[1].enabled,"ADR-off channel mask accepted independently");
    check(adr.onNewUplink()==0,"ADR disabled uplink control");
    check(saveRegion(loadRegion(saveRegion(eu)))==saveRegion(eu),"Region settings/sticky state survive restart");
    check(saveDuty(loadDuty(saveDuty(duty)))==saveDuty(duty),"Duty reservations survive restart");
    std::cout<<"PASS EU868 channels, MAC commands, atomic ADR, sticky answers, payload limits, RX windows and duty reservations\n";
}
void fuzz() {
    std::mt19937 random(178);
    for(unsigned i=0; i<5000; ++i) {
        Bytes input(random()%300);
        for(auto &b:input) b=std::uint8_t(random());
        try { (void)decodeData(input,{},random()); } catch(const std::exception &) {}
        try { (void)decodeJoinAccept(input,{}); } catch(const std::exception &) {}
        try { (void)decodeJoinRequest(input,{}); } catch(const std::exception &) {}
        try { (void)loadSession(input); } catch(const std::exception &) {}
        try { (void)loadDeviceJoin(input); } catch(const std::exception &) {}
        try { (void)loadJoinServer(input); } catch(const std::exception &) {}
        try { (void)loadRegion(input); } catch(const std::exception &) {}
        try { (void)loadDuty(input); } catch(const std::exception &) {}
        // Exercise body bounds with a valid unkeyed checksum, rather than only
        // rejecting random bytes at the state magic/CRC checks.
        auto crafted=input;
        if(crafted.size()<9) crafted.resize(9);
        crafted[0]='O'; crafted[1]='W'; crafted[2]='L'; crafted[3]=1;
        crafted[4]=std::uint8_t("SDNRT"[i%5]);
        std::uint32_t checksum=0xffffffff;
        for(std::size_t j=0; j<crafted.size()-4; ++j) {
            checksum^=crafted[j];
            for(unsigned bit=0; bit<8; ++bit) checksum=(checksum>>1)^((checksum&1)?0xedb88320:0);
        }
        checksum=~checksum;
        for(unsigned j=0; j<4; ++j) crafted[crafted.size()-4+j]=std::uint8_t(checksum>>(8*j));
        try { (void)loadSession(crafted); } catch(const std::exception &) {}
        try { (void)loadDeviceJoin(crafted); } catch(const std::exception &) {}
        try { (void)loadJoinServer(crafted); } catch(const std::exception &) {}
        try { (void)loadRegion(crafted); } catch(const std::exception &) {}
        try { (void)loadDuty(crafted); } catch(const std::exception &) {}
        if(input.size()>242) input.resize(242);
        try { (void)parseMac(Direction::Downlink,input); } catch(const std::exception &) {}
        Eu868 region;
        try { (void)region.processDownlink(commandFrame(input,true)); } catch(const std::exception &) {}
    }
    std::cout<<"PASS 5000 malformed frame/state/MAC input cases\n";
}
#ifdef OBRII_LORAWAN_WITH_LORA
Bytes iqRoundtrip(const Bytes &wire, unsigned rate, Direction dir) {
    LoRaBearer tx(rate,dir),rx(rate,dir); const auto iq=tx.modulate(wire);
    check(std::abs(tx.airtimeSeconds(wire)-double(iq.size())/tx.config().sampleRate())<1e-8,"PHY airtime");
    std::vector<RadioPacket> received;
    for(std::size_t at=0; at<iq.size(); at+=997) {
        auto part=rx.receive(iq.data()+at,std::min<std::size_t>(997,iq.size()-at)); received.insert(received.end(),part.begin(),part.end());
    }
    check(received.size()==1 && received[0].wire==wire,"Streaming LoRaWAN IQ"); return received[0].wire;
}
void iqTests() {
    SessionKeys keys{};
    for(unsigned dr=0; dr<=6; ++dr) for(const auto dir:{Direction::Uplink,Direction::Downlink}) {
        DataFrame frame; frame.type=dir==Direction::Uplink ? FrameType::UnconfirmedUp:FrameType::UnconfirmedDown;
        frame.devAddr=0x26011bda; frame.port=1; frame.payload.resize(Eu868::maxMacPayload(dr)-8);
        for(std::size_t i=0; i<frame.payload.size(); ++i) frame.payload[i]=std::uint8_t(i*13);
        const auto wire=iqRoundtrip(encodeData(frame,keys),dr,dir);
        check(decodeData(wire,keys,0).payload==frame.payload,"IQ packet decrypt");
        const auto phy=phyConfig(dr,dir); check(phy.syncWord==0x34 && phy.preambleSymbols==8 && phy.invertIq==(dir==Direction::Downlink),"LoRaWAN direction PHY");
    }
    DeviceJoinState savedD; JoinServerState savedN;
    OtaaDevice d(credentials(),{},[&](const auto &s) { savedD=s; });
    JoinServer n(credentials(),{},[&](const auto &s) { savedN=s; });
    JoinAccept params; params.devAddr=0x26011bda; params.netId=0x13;
    const auto response=n.accept(iqRoundtrip(d.request(),5,Direction::Uplink),params);
    d.accept(iqRoundtrip(response.wire,5,Direction::Downlink));
    Session node(Role::Device,*d.state().activated,[&](const auto &s) { d.updateSession(s); });
    Session network(Role::Network,*n.state().activated,[&](const auto &s) { n.updateSession(s); });
    DataFrame up; up.type=FrameType::ConfirmedUp; up.port=2; up.payload={1,2,3,4};
    check(network.receive(iqRoundtrip(node.transmit(up),5,Direction::Uplink)).frame.payload==up.payload,"OTAA uplink through IQ");
    DataFrame down; down.type=FrameType::UnconfirmedDown;
    check(node.receive(iqRoundtrip(network.transmit(down),5,Direction::Downlink)).frame.control&0x20,"OTAA ACK through inverted IQ without PHY CRC");
    DataFrame foreign; foreign.port=1; foreign.payload={1,2,3};
    auto wrongPhy=phyConfig(5,Direction::Uplink); wrongPhy.codingRate=4;
    LoRaBearer gateway(5,Direction::Uplink);
    check(gateway.receive(obrii::lora::Modem(wrongPhy).modulate(encodeData(foreign,{}))).empty(),"Foreign coding rate filtered");
    std::cout<<"PASS all EU868 LoRa rates DR0..6, maximum payloads, inverted downlink, OTAA and confirmed ACK through IQ\n";
}
#endif
int main() {
    try {
        const auto key=fixed<16>("2b7e151628aed2a6abf7158809cf4f3c");
        check(aesEncrypt(key,fixed<16>("6bc1bee22e409f96e93d7e117393172a"))==fixed<16>("3ad77bb40d7a3660a89ecaf32466ef97"),"NIST AES128");
        check(cmac(key,{})==fixed<16>("bb1d6929e95937287fa37d129b756746"),"RFC4493 empty CMAC");
        check(cmac(key,unhex("6bc1bee22e409f96e93d7e117393172a"))==fixed<16>("070a16b46b4d4144f79bdd9dd04a287c"),"RFC4493 one-block CMAC");
        const auto join=decodeJoinAccept(unhex("20493eeb51fba2116f810edb3742975142"),fixed<16>("00112233445566778899aabbccddeeff"));
        check(join.joinNonce==5704647 && join.netId==0x221101 && join.devAddr==0x02031980,"Published independent join vector");
        check(encodeJoinAccept(join,fixed<16>("00112233445566778899aabbccddeeff"))==unhex("20493eeb51fba2116f810edb3742975142"),"Join accept AES-decrypt direction");
        std::cout<<"PASS AES128, RFC4493 CMAC and independent LoRaWAN join accept\n";
        vectors(); sessions(); regions(); fuzz();
#ifdef OBRII_LORAWAN_WITH_LORA
        iqTests();
#endif
        return 0;
    } catch(const std::exception &e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
