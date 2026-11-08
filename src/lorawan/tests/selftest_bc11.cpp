#include "version11.h"
#include "classbc.h"
#ifdef OBRII_LORAWAN_WITH_LORA
#include "lorawanlora.h"
#endif
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
using namespace obrii::lorawan;
namespace {
void check(bool c,const char *why){if(!c)throw Error(why);}
template<class F>void reject(F f){bool bad=false;try{f();}catch(const std::exception &){bad=true;}check(bad,"Expected rejection");}
Bytes unhex(const std::string &s){Bytes b;if(s=="-")return b;for(std::size_t i=0;i<s.size();i+=2)b.push_back(std::uint8_t(std::stoul(s.substr(i,2),nullptr,16)));return b;}
template<std::size_t N>std::array<std::uint8_t,N> fixed(const std::string &s){const auto b=unhex(s);check(b.size()==N,"Fixture size");std::array<std::uint8_t,N> a{};std::copy(b.begin(),b.end(),a.begin());return a;}
std::string hex(const Key &k){const char *h="0123456789abcdef";std::string s;for(auto b:k){s+=h[b>>4];s+=h[b&15];}return s;}
void vectors(){
    std::ifstream file(OBRII_LORAWAN_VECTORS11);check(bool(file),"Independent 1.1 fixtures missing");std::string line;unsigned cases=0;
    while(std::getline(file,line)){
        if(line.empty()||line[0]=='#')continue;
        const auto tab=line.find('\t');std::istringstream in(line.substr(0,tab));std::vector<std::string> p;std::string v;while(in>>v)p.push_back(v);
        const auto expected=line.substr(tab+1);auto n=[&](std::size_t i,int base=10){return std::uint32_t(std::stoull(p.at(i),nullptr,base));};Bytes wire;
        if(p[0]=="data11"){
            SessionKeys k;k.version=Version::V1_1;k.network=fixed<16>(p[1]);k.servingIntegrity=fixed<16>(p[2]);k.networkEncryption=fixed<16>(p[3]);k.application=fixed<16>(p[4]);
            DataFrame f;f.type=FrameType(n(5));f.devAddr=n(6,16);f.counter=n(7);f.control=std::uint8_t(n(8,16));f.options=unhex(p[9]);if(p[10]!="-")f.port=std::uint8_t(n(10));f.payload=unhex(p[11]);
            const DataContext c{std::uint8_t(n(12)),std::uint8_t(n(13)),std::uint16_t(n(14))};wire=encodeData(f,k,c);
            const auto decoded=decodeData(unhex(expected),k,f.counter,c);check(decoded.payload==f.payload&&decoded.options==f.options&&decoded.port==f.port&&decoded.control==f.control,"Independent 1.1 decode");
            auto bad=wire;bad.back()^=1;reject([&]{decodeData(bad,k,f.counter,c);});
            if(direction(f.type)==Direction::Uplink){auto wrong=c;++wrong.txChannel;reject([&]{decodeData(wire,k,f.counter,wrong);});}
            if(f.control&0x20){auto wrong=c;++wrong.confirmedCounter;reject([&]{decodeData(wire,k,f.counter,wrong);});}
        }else if(p[0]=="js11"){
            const auto k=deriveJoinServerKeys(fixed<16>(p[1]),fixed<8>(p[2]));check(hex(k.encryption)+" "+hex(k.integrity)==expected,"Independent JS keys");
        }else if(p[0]=="derive11"){
            const auto k=deriveSessionKeys11(fixed<16>(p[1]),fixed<16>(p[2]),n(3),fixed<8>(p[4]),std::uint16_t(n(5)));
            check(hex(k.network)+" "+hex(k.servingIntegrity)+" "+hex(k.networkEncryption)+" "+hex(k.application)==expected,"Independent 1.1 session keys");
        }else if(p[0]=="accept11"){
            Credentials c;c.version=Version::V1_1;c.nwkKey=fixed<16>(p[1]);c.appKey=fixed<16>(p[2]);c.joinEui=fixed<8>(p[3]);c.devEui=fixed<8>(p[4]);
            JoinAccept a;a.optNeg=true;a.joinNonce=n(6);a.netId=0x13;a.devAddr=0x26011bda;a.rx1Offset=2;a.rx2DataRate=3;a.rxDelay=5;
            wire=encodeJoinAccept11(a,c,std::uint16_t(n(5)),std::uint8_t(n(7)));
            const auto decoded=decodeJoinAccept11(unhex(expected),c,std::uint16_t(n(5)),std::uint8_t(n(7)));check(decoded.optNeg&&decoded.joinNonce==a.joinNonce,"Independent OptNeg join accept");
            reject([&]{decodeJoinAccept11(wire,c,std::uint16_t(n(5)+1),std::uint8_t(n(7)));});
        }else if(p[0]=="rejoin11"){
            RejoinRequest r;r.joinEui=fixed<8>(p[2]);r.devEui=fixed<8>(p[3]);r.counter=std::uint16_t(n(4));r.type=std::uint8_t(n(5));r.netId=0x13;
            const auto key=fixed<16>(p[1]);wire=encodeRejoinRequest(r,key);const auto decoded=decodeRejoinRequest(unhex(expected),key);
            check(decoded.type==r.type&&decoded.devEui==r.devEui&&decoded.counter==r.counter,"Independent rejoin codec");
        }else if(p[0]=="ping")check(pingOffset(n(1),n(2,16),n(3))==std::stoul(expected),"Independent Class B ping offset");
        else throw Error("Unknown 1.1 fixture");
        if(!wire.empty())check(wire==unhex(expected),"Independent 1.1 air bytes");++cases;
    }
    check(cases==2076,"1.1 fixture count");std::cout<<"PASS 2076 independent 1.1/Class B fixtures, FOpts erratum, MIC, rejoin and derivation\n";
}
void sessions(){
    Credentials c;c.version=Version::V1_1;c.nwkKey.fill(0x31);c.appKey.fill(0x73);c.joinEui[0]=1;c.devEui[0]=2;
    DeviceJoinState ds;JoinServerState ns;
    OtaaDevice dev(c,{},[&](auto s){ds=loadDeviceJoin(saveDeviceJoin(s));});JoinServer net(c,{},[&](auto s){ns=loadJoinServer(saveJoinServer(s));});
    JoinAccept a;a.devAddr=0x26011bda;a.netId=0x13;const auto request=dev.request();
    reject([&]{decodeJoinRequest(request,c.appKey);});const auto response=net.accept(request,a);
    reject([&]{decodeJoinAccept(response.wire,c.nwkKey);});dev.accept(response.wire);
    check(ds.activated&&ns.activated&&ds.activated->keys.version==Version::V1_1,"1.1 activation persisted");
    auto d=*ds.activated,n=*ns.activated;d.nextTransmit=65535;n.nextTransmit=65535;n.nextApplicationTransmit=0;
    Session device(Role::Device,d,[&](auto s){d=loadSession(saveSession(s));});Session network(Role::Network,n,[&](auto s){n=loadSession(saveSession(s));});
    device.setNbTrans(3);network.setNbTrans(3);
    DataFrame up;up.type=FrameType::ConfirmedUp;up.port=1;up.payload={1,2,3};up.options={11,1};
    auto wire=device.transmit(up,{5,0,0});check(network.receive(wire,{}, {5,0,0}).frame.counter==65535,"1.1 uplink counter");
    auto repeated=device.repeatUplink(DataContext{4,1,0});check(repeated!=wire&&std::equal(wire.begin(),wire.end()-4,repeated.begin()),"1.1 channel repeat changes only MIC");
    reject([&]{network.receive(repeated,{}, {5,0,0});});check(network.receive(repeated,{}, {4,1,0}).duplicate,"Cross-channel duplicate authenticated without delivery");
    DataFrame down;down.type=FrameType::ConfirmedDown;down.port=2;down.payload={9};down.options={11,1};
    const auto app=network.transmit(down);check(device.receive(app).frame.counter==0,"AFCntDown independent of NFCntDown");
    check(d.lastApplicationReceived==0&&!d.lastReceived&&!d.confirmedTransmitted,"ACK tied to pending confirmed uplink");
    reject([&]{device.receive(app);});
    up.type=FrameType::UnconfirmedUp;const auto ack=device.transmit(up,{3,2,0});check(network.receive(ack,{}, {3,2,0}).frame.control&0x20,"Confirmed downlink acknowledgement");
    down.type=FrameType::UnconfirmedDown;down.port=0;down.options.clear();down.payload={13};
    const auto mac=network.transmit(down);check(device.receive(mac).frame.counter==65535,"NFCntDown separate domain");
    check(d.lastReceived==65535&&d.lastApplicationReceived==0,"Both receive domains preserved");
    Session resumed(Role::Device,loadSession(saveSession(d)),[&](auto s){d=s;});
    reject([&]{resumed.receive(app);});reject([&]{resumed.receive(mac);});
    const auto next=network.transmit(down);check(resumed.receive(next).frame.counter==65536,"NFCntDown rollover after restart");
    auto stale=d;stale.nextTransmit=0;reject([&]{dev.updateSession(stale);});
    auto exhausted=n;exhausted.lastTransmitted.clear();exhausted.nextApplicationTransmit=0x100000000ULL;
    Session full(Role::Network,exhausted,[](auto){});down.port=1;reject([&]{full.transmit(down);});
    Session ambiguous(Role::Device,d,[](auto){throw Error("ambiguous write");});reject([&]{ambiguous.transmit(up);});reject([&]{ambiguous.transmit(up);});
    std::cout<<"PASS 1.1 OTAA, split FCntDown, ACK binding, cross-channel NbTrans, restart, replay and persistence failure\n";
}
void classes(){
    const auto published=unhex("0000000002cca27e00012000008103de55");const auto beacon=decodeBeacon(published);
    check(beacon.gpsSeconds==0xcc020000&&beacon.gatewayCrcValid&&encodeBeacon(beacon)==published,"Published LoRa Alliance beacon CRC");
    auto bad=published;bad[3]^=1;reject([&]{decodeBeacon(bad);});bad=published;bad.back()^=1;check(!decodeBeacon(bad).gatewayCrcValid,"Common beacon time survives gateway CRC loss");
    for(unsigned periodicity=0;periodicity<8;++periodicity){
        ClassBConfig cfg;cfg.periodicity=std::uint8_t(periodicity);ClassBTracker t(cfg);t.acquire(encodeBeacon({128}),1000000);
        check(t.locked(1000000)&&!t.locked(7201000000ULL),"Class B holdover bound");
        auto now=1000000ULL;unsigned count=0;
        while(auto w=t.nextPing(0x26011bda,now)){
            if(w->nominalUs>=129000000)break;
            check(t.acceptsPing(0x26011bda,w->nominalUs,w->frequencyHz,w->dataRate),"Ping receive filter");
            check(!t.acceptsPing(0x26011bda,w->nominalUs,868100000,w->dataRate),"Ping wrong channel");
            now=w->closesUs+1;++count;
        }check(count==(128u>>periodicity),"Class B slot count");
        check(!t.allowsTransaction(1000000,2000000)&&!t.allowsTransaction(124000000,128000000),"Beacon reserved/guard priority");
        reject([&]{t.acquire(encodeBeacon({128}),129000000);});reject([&]{t.acquire(encodeBeacon({256}),140000000);});
        t.acquire(encodeBeacon({256}),129000000);check(t.locked(129000000),"Class B tracking");
        t.reset();check(!t.nextPing(0x26011bda,129000000),"Restart requires beacon reacquisition");
    }
    Eu868 region;const auto windows=receiveWindows(region,1000000,868100000,5);
    check(acceptsClassC(region,1100000,869525000,0,windows),"Class C before RX1");
    check(!acceptsClassC(region,windows[0].nominalUs,869525000,0,windows),"Class C RX1 priority");
    check(acceptsClassC(region,windows[0].nominalUs,868100000,5,windows),"Class C keeps RX1");
    check(!acceptsClassC(region,1100000,869525000,0,windows,std::make_pair(1000000,1200000)),"Class C half duplex");
    std::mt19937 random(11);for(unsigned i=0;i<5000;++i){Bytes b(random()%256);for(auto &v:b)v=std::uint8_t(random());try{decodeBeacon(b);}catch(const Error &){}try{decodeRejoinRequest(b,{});}catch(const Error &){}}
#ifdef OBRII_LORAWAN_WITH_LORA
    const auto c=beaconPhyConfig(1);check(!c.explicitHeader&&!c.payloadCrc&&!c.invertIq&&c.preambleSymbols==10,"Beacon PHY");
    const auto iq=obrii::lora::Modem(c).modulate(published);obrii::lora::Receiver receiver(c);std::vector<obrii::lora::Reception> packets;
    for(std::size_t at=0;at<iq.size();){const auto n=(std::min)(std::size_t(137),iq.size()-at);const auto found=receiver.push(iq.data()+at,n);packets.insert(packets.end(),found.begin(),found.end());at+=n;}
    check(packets.size()==1&&packets[0].packet.payload==published,"Class B beacon through implicit LoRa IQ");
#endif
    std::cout<<"PASS beacon CRC, all ping periodicities, holdover, guard priority, Class C RX1/TX exclusion and bounded malformed frames\n";
}
}
int main(){try{vectors();sessions();classes();return 0;}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
