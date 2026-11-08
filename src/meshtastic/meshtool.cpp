#include "meshtastic.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
using namespace obrii::meshtastic;
namespace {
unsigned digit(char c) {
    if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;
    throw std::invalid_argument("Invalid hex digit");
}
Bytes fromHex(const std::string &s) {
    if(s.size()%2||s.size()>510)throw std::invalid_argument("Expected at most 255 hex bytes");Bytes b;
    for(std::size_t i=0;i<s.size();i+=2)b.push_back(std::uint8_t(digit(s[i])*16+digit(s[i+1])));return b;
}
std::string toHex(const Bytes &b) {
    std::ostringstream s;s<<std::hex<<std::setfill('0');for(auto byte:b)s<<std::setw(2)<<unsigned(byte);return s.str();
}
std::uint32_t number(const std::string &s,int base=16) {
    if(s.empty()||s[0]=='-')throw std::invalid_argument("Invalid unsigned integer");
    std::size_t used=0;const auto n=std::stoull(s,&used,base);
    if(used!=s.size()||n>std::numeric_limits<std::uint32_t>::max())throw std::invalid_argument("Invalid uint32");return std::uint32_t(n);
}
std::string quote(const std::string &value) {
    std::ostringstream out;out<<'"';
    for(unsigned char c:value) {
        if(c=='"'||c=='\\')out<<'\\'<<c;
        else if(c<32||c==127)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;
        else out<<c;
    }
    out<<'"';return out.str();
}
void help() {
    std::cout<<"ObriiMeshtasticTool encode --from HEX --id HEX --text UTF8 [--to HEX] [--hop 0..7] [--ack]\n"
        "ObriiMeshtasticTool encode --from HEX --id HEX --port DEC --payload HEX [options]\n"
        "ObriiMeshtasticTool decode --radio HEX [--channel NAME] [--psk-file FILE]\n"
        "Default channel: LongFast/public short PSK1. --psk-file reads raw 0..32 bytes.\n"
        "This offline tool produces/parses PHY payload bytes; it never opens a radio.\n"
        "Packet IDs and sender IDs are hexadecimal. Do not reuse an ID/key nonce for new data.\n";
}
}
int main(int argc,char **argv) {
    try {
        if(argc<2||std::string(argv[1])=="--help") { help();return argc<2?1:0; }
        const std::string command=argv[1];if(command!="encode"&&command!="decode")throw std::invalid_argument("Expected encode or decode");
        std::map<std::string,std::string> args;
        for(int i=2;i<argc;++i) {
            const std::string key=argv[i];if(args.count(key))throw std::invalid_argument("Duplicate argument");
            if(key=="--ack") { args[key]="1";continue; }
            if(key!="--from"&&key!="--id"&&key!="--text"&&key!="--to"&&key!="--hop"&&key!="--port"&&key!="--payload"
               &&key!="--channel"&&key!="--psk-file"&&key!="--radio")throw std::invalid_argument("Unknown argument: "+key);
            if(++i>=argc)throw std::invalid_argument("Missing argument value");args[key]=argv[i];
        }
        Channel ch;if(args.count("--channel"))ch.name=args["--channel"];
        if(args.count("--psk-file")) {
            std::ifstream file(args["--psk-file"],std::ios::binary);if(!file)throw std::runtime_error("Cannot open PSK file");
            ch.psk.clear();char c;
            while(file.get(c)) { if(ch.psk.size()==32)throw std::invalid_argument("PSK file exceeds32 bytes");ch.psk.push_back(std::uint8_t(c)); }
            if(file.bad())throw std::runtime_error("Cannot read PSK file");
        }
        ch.validate();
        if(command=="encode") {
            if(!args.count("--from")||!args.count("--id")||args.count("--radio"))throw std::invalid_argument("encode requires --from and --id");
            Data data;
            if(args.count("--text")) {
                if(args.count("--port")||args.count("--payload"))throw std::invalid_argument("Use text OR port/payload");
                data=textMessage(args["--text"]);
            } else {
                if(!args.count("--port")||!args.count("--payload"))throw std::invalid_argument("Expected text or port/payload");
                data.port=number(args["--port"],10);data.payload=fromHex(args["--payload"]);
            }
            Header h;h.from=number(args["--from"]);h.id=number(args["--id"]);h.relayNode=relayId(h.from);
            if(args.count("--to"))h.to=number(args["--to"]);
            if(args.count("--hop")) { const auto hops=number(args["--hop"],10);if(hops>7)throw std::invalid_argument("Hop limit must be 0..7");h.hopLimit=std::uint8_t(hops);h.hopStart=h.hopLimit; }
            h.wantAck=args.count("--ack")&&h.to!=Broadcast;
            std::cout<<toHex(seal(h,data,ch))<<'\n';return 0;
        }
        if(!args.count("--radio"))throw std::invalid_argument("decode requires --radio");
        for(const auto &arg:args)if(arg.first!="--radio"&&arg.first!="--channel"&&arg.first!="--psk-file")throw std::invalid_argument("Unexpected decode argument");
        const auto raw=decodeRadio(fromHex(args["--radio"]));const auto result=open(raw,{ch});
        if(!result.packet) { std::cout<<"{\"decoded\":false,\"status\":"<<int(result.status)<<"}\n";return 2; }
        const auto &p=*result.packet;const auto &h=p.radio.header;
        std::cout<<"{\"decoded\":true,\"authenticated\":false,\"from\":"<<h.from<<",\"to\":"<<h.to<<",\"id\":"<<h.id
            <<",\"port\":"<<p.data.port<<",\"payload_hex\":"<<quote(toHex(p.data.payload));
        if(p.data.port==TextPort)std::cout<<",\"text\":"<<quote(text(p.data));
        std::cout<<"}\n";return 0;
    } catch(const std::exception &e) { std::cerr<<"Meshtastic: "<<e.what()<<'\n';return 1; }
}
