#include "loraphy.h"
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
using namespace obrii::lora;
namespace {
int number(const std::string &s, int base=10) {
    std::size_t used=0; const int value=std::stoi(s,&used,base);
    if(used!=s.size())throw std::invalid_argument("Invalid integer: "+s);
    return value;
}
Bytes fromHex(const std::string &text) {
    if(text.size()%2||text.size()>510)throw std::invalid_argument("Payload must be 0..255 hex-encoded bytes");
    auto digit=[](char ch)->int {
        if(ch>='0'&&ch<='9')return ch-'0';
        if(ch>='a'&&ch<='f')return ch-'a'+10;
        if(ch>='A'&&ch<='F')return ch-'A'+10;
        throw std::invalid_argument("Invalid hex digit");
    };
    Bytes bytes;
    for(std::size_t i=0;i<text.size();i+=2)bytes.push_back(std::uint8_t(digit(text[i])*16+digit(text[i+1])));
    return bytes;
}
std::string toHex(const Bytes &bytes) {
    std::ostringstream out;out<<std::hex<<std::setfill('0');
    for(auto b:bytes)out<<std::setw(2)<<unsigned(b);
    return out.str();
}
// Explicit little-endian CF32; do not assume std::complex object layout or host endianness.
void writeFloat(std::ostream &out, float f) {
    static_assert(sizeof(float)==4,"CF32 needs 32-bit float");
    std::uint32_t bits;std::memcpy(&bits,&f,4);
    std::array<char,4> bytes{};
    for(int i=0;i<4;++i)bytes[i]=char(bits>>(8*i));
    out.write(bytes.data(),4);
}
float readFloat(const char *bytes) {
    std::uint32_t bits=0;
    for(int i=0;i<4;++i)bits|=std::uint32_t(std::uint8_t(bytes[i]))<<(8*i);
    float f;std::memcpy(&f,&bits,4);return f;
}
void help() {
    std::cout<<"ObriiLoRaTool encode --hex HEX --output FILE.cf32 [options]\n"
        "ObriiLoRaTool decode --input FILE.cf32 [options]\n"
        "Options: --sf 7..12 --bw 31250|62500|125000|250000|500000 --cr 1..4\n"
        "         --os 1|2|4|8 --preamble 6..1024 --sync HEXBYTE --ldro auto|on|off\n"
        "         --implicit BYTES --no-crc --invert-iq\n"
        "Defaults: SF11 BW250000 CR4/5 OS2 preamble16 sync2b explicit CRC LDROauto.\n"
        "Files: raw little-endian float32 I,Q; sample rate=BW*OS. No RF transmission.\n";
}
}
int main(int argc,char **argv) {
    try {
        if(argc<2||std::string(argv[1])=="--help") { help();return argc<2?1:0; }
        const std::string command=argv[1];
        if(command!="encode"&&command!="decode")throw std::invalid_argument("Expected encode or decode");
        Config c;std::map<std::string,std::string> args;
        for(int i=2;i<argc;++i) {
            const std::string key=argv[i];
            if(args.count(key))throw std::invalid_argument("Duplicate option: "+key);
            if(key=="--no-crc"||key=="--invert-iq") { args[key]="1";continue; }
            if(key!="--hex"&&key!="--input"&&key!="--output"&&key!="--sf"&&key!="--bw"
                &&key!="--cr"&&key!="--os"&&key!="--preamble"&&key!="--sync"&&key!="--ldro"&&key!="--implicit")
                throw std::invalid_argument("Unknown option: "+key);
            if(++i>=argc)throw std::invalid_argument("Missing option value: "+key);
            args[key]=argv[i];
        }
        if(args.count("--sf"))c.spreadingFactor=number(args["--sf"]);
        if(args.count("--bw"))c.bandwidthHz=number(args["--bw"]);
        if(args.count("--cr"))c.codingRate=number(args["--cr"]);
        if(args.count("--os"))c.oversampling=number(args["--os"]);
        if(args.count("--preamble"))c.preambleSymbols=number(args["--preamble"]);
        if(args.count("--sync")) {
            const int sync=number(args["--sync"],16);
            if(sync<0||sync>255)throw std::invalid_argument("Sync word must fit one byte");
            c.syncWord=std::uint8_t(sync);
        }
        if(args.count("--ldro")) {
            const auto value=args["--ldro"];
            if(value!="auto"&&value!="on"&&value!="off")throw std::invalid_argument("Invalid LDRO mode");
            c.ldro=value=="auto"?Ldro::Auto:value=="on"?Ldro::On:Ldro::Off;
        }
        if(args.count("--implicit")) { c.explicitHeader=false;c.implicitPayloadBytes=number(args["--implicit"]); }
        c.payloadCrc=!args.count("--no-crc");c.invertIq=args.count("--invert-iq")!=0;c.validate();
        if(command=="encode") {
            if(!args.count("--hex")||!args.count("--output")||args.count("--input"))
                throw std::invalid_argument("encode needs --hex and --output");
            const auto payload=fromHex(args["--hex"]);const auto iq=Modem(c).modulate(payload);
            std::ofstream file(args["--output"],std::ios::binary|std::ios::trunc);
            if(!file)throw std::runtime_error("Cannot open output file");
            for(auto s:iq) { writeFloat(file,s.real());writeFloat(file,s.imag()); }
            file.flush();if(!file)throw std::runtime_error("IQ output write failed");
            std::cout<<"samples="<<iq.size()<<" sample_rate="<<c.sampleRate()<<" airtime_s="
                     <<std::setprecision(12)<<PacketCodec(c).timeOnAirSeconds(int(payload.size()))<<'\n';
            return 0;
        }
        if(!args.count("--input")||args.count("--hex")||args.count("--output"))
            throw std::invalid_argument("decode needs --input");
        std::ifstream file(args["--input"],std::ios::binary);if(!file)throw std::runtime_error("Cannot open input file");
        Receiver receiver(c);std::array<char,32768> block{};std::size_t found=0;
        while(file) {
            file.read(block.data(),block.size());const auto count=std::size_t(file.gcount());
            if(count%8)throw std::runtime_error("Truncated CF32 sample at end of file");
            Iq iq(count/8);
            for(std::size_t i=0;i<iq.size();++i)iq[i]={readFloat(block.data()+8*i),readFloat(block.data()+8*i+4)};
            for(const auto &rx:receiver.push(iq)) {
                ++found;
                std::cout<<"payload_hex="<<toHex(rx.packet.payload)<<" crc="<<(rx.packet.crcChecked?"ok":"absent")
                    <<" cr=4/"<<rx.packet.header.codingRate+4<<" corrected="<<rx.packet.correctedCodewords
                    <<" cfo_hz="<<rx.frequencyOffsetHz<<" payload_sample="<<rx.payloadSampleOffset<<'\n';
            }
        }
        if(file.bad())throw std::runtime_error("IQ input read failed");
        return found?0:2;
    } catch(const std::exception &e) { std::cerr<<"LoRa: "<<e.what()<<'\n';return 1; }
}
