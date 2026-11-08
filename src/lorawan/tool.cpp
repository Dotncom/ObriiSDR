// Offline protocol pipe. Keys are read from stdin, never command-line arguments.
#include "lorawan.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

using namespace obrii::lorawan;
namespace {
unsigned nibble(char c) {
    if(c>='0' && c<='9') return unsigned(c-'0');
    if(c>='a' && c<='f') return unsigned(c-'a'+10);
    if(c>='A' && c<='F') return unsigned(c-'A'+10);
    throw Error("Invalid hexadecimal input");
}
Bytes unhex(const std::string &s) {
    if(s=="-") return {};
    if(s.size()%2 || s.size()>1024) throw Error("Invalid hexadecimal size");
    Bytes out; for(std::size_t i=0; i<s.size(); i+=2) out.push_back(std::uint8_t(nibble(s[i])*16+nibble(s[i+1]))); return out;
}
template<std::size_t N> std::array<std::uint8_t,N> array(const std::string &s) {
    const auto b=unhex(s); if(b.size()!=N) throw Error("Wrong key/EUI size");
    std::array<std::uint8_t,N> a{}; std::copy(b.begin(),b.end(),a.begin()); return a;
}
std::string hex(const Bytes &bytes) {
    static constexpr char digits[]="0123456789abcdef"; std::string out;
    for(auto b:bytes) { out+=digits[b>>4]; out+=digits[b&15]; } return out;
}
std::uint32_t number(const std::string &s, int base=10, std::uint32_t maximum=0xffffffff) {
    if(s.empty() || s[0]=='-' || s[0]=='+') throw Error("Invalid unsigned number");
    std::size_t used=0; const auto n=std::stoull(s,&used,base);
    if(used!=s.size() || n>maximum) throw Error("Number exceeds range"); return std::uint32_t(n);
}
std::string command(const std::vector<std::string> &p) {
    if(p.empty()) throw Error("Empty command");
    if(p[0]=="join-request" && p.size()==5) {
        return hex(encodeJoinRequest({array<8>(p[2]),array<8>(p[3]),std::uint16_t(number(p[4],10,65535))},array<16>(p[1])));
    }
    if(p[0]=="join-accept" && p.size()==9) {
        JoinAccept a; a.joinNonce=number(p[2],10,0xffffff); a.netId=number(p[3],16,0xffffff); a.devAddr=number(p[4],16);
        a.rx1Offset=std::uint8_t(number(p[5],10,7)); a.rx2DataRate=std::uint8_t(number(p[6],10,15));
        a.rxDelay=std::uint8_t(number(p[7],10,15)); if(p[8]!="-") a.cfList=array<16>(p[8]);
        return hex(encodeJoinAccept(a,array<16>(p[1])));
    }
    if(p[0]=="data-encode" && p.size()==10) {
        SessionKeys keys{array<16>(p[1]),array<16>(p[2])}; DataFrame f;
        f.type=FrameType(number(p[3],10,5)); f.devAddr=number(p[4],16); f.counter=number(p[5]);
        f.control=std::uint8_t(number(p[6],16,255)); f.options=unhex(p[7]);
        if(p[8]!="-") f.port=std::uint8_t(number(p[8],10,255)); f.payload=unhex(p[9]); return hex(encodeData(f,keys));
    }
    if(p[0]=="data-decode" && p.size()==5) {
        const auto f=decodeData(unhex(p[4]),{array<16>(p[1]),array<16>(p[2])},number(p[3]));
        std::ostringstream out; out<<unsigned(f.type)<<' '<<std::hex<<f.devAddr<<std::dec<<' '<<f.counter<<' '
            <<unsigned(f.control)<<' '<<(f.options.empty()?"-":hex(f.options))<<' '
            <<(f.port ? std::to_string(*f.port):"-")<<' '<<(f.payload.empty()?"-":hex(f.payload)); return out.str();
    }
    if(p[0]=="derive" && p.size()==5) {
        const auto keys=deriveSessionKeys(array<16>(p[1]),number(p[2],10,0xffffff),number(p[3],16,0xffffff),std::uint16_t(number(p[4],10,65535)));
        return hex(Bytes(keys.network.begin(),keys.network.end()))+" "+hex(Bytes(keys.application.begin(),keys.application.end()));
    }
    throw Error("Unknown command or wrong argument count");
}
}
int main(int argc, char **argv) {
    if(argc!=2 || std::string(argv[1])!="--pipe") {
        std::cout<<"Offline LoRaWAN 1.0.4 protocol tool. Use --pipe; one command per stdin line.\n"
            <<"join-request AppKey JoinEUI DevEUI DevNonce\n"
            <<"join-accept AppKey JoinNonce NetID DevAddr RX1Offset RX2DR RXDelay CFList|-\n"
            <<"data-encode NwkSKey AppSKey Type DevAddr FCnt Control FOpts|- FPort|- Payload|-\n"
            <<"data-decode NwkSKey AppSKey FCnt PHYPayload\n"
            <<"derive AppKey JoinNonce NetID DevNonce\n"
            <<"Keys/EUIs/addresses/control/payloads are hex; other numbers decimal. No RF or sockets.\n";
        return argc==1 ? 0:1;
    }
    std::string line; char c;
    while(std::cin.get(c)) {
        if(c!='\n') { if(line.size()>=4096) { std::cerr<<"Command line too large\n"; return 1; } line+=c; continue; }
        if(!line.empty() && line.back()=='\r') line.pop_back();
        try {
            std::istringstream input(line); std::vector<std::string> parts; std::string token;
            while(input>>token) parts.push_back(token);
            const auto result=command(parts); std::cout<<"ok "<<result<<std::endl;
        } catch(const std::exception &) { std::cout<<"error Invalid LoRaWAN command, parameters or authentication"<<std::endl; }
        line.clear();
    }
    if(!line.empty()) { std::cerr<<"Unterminated command\n"; return 1; }
}
