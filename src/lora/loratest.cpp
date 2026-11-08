#include "loraphy.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
using namespace obrii::lora;
namespace {
int failures=0;
void check(bool value, const std::string &name) {
    if (!value) { ++failures; std::cerr << "FAIL " << name << '\n'; }
}
template<class F> void rejects(F f, const std::string &name) {
    try { f(); check(false,name); } catch(const std::exception &) {}
}
Bytes bytes(int length) {
    Bytes result(length);
    for (int i=0; i<length; ++i) result[i]=std::uint8_t(i*71+13);
    return result;
}
Iq impaired(const Iq &input, const Config &c, double snrDb, double cfoBins, int prefix) {
    std::mt19937 rng(247); std::normal_distribution<float> noise(0,float(0.7*std::pow(10,-snrDb/20)/std::sqrt(2.0)));
    Iq output(prefix+input.size()+c.samplesPerSymbol());
    for (auto &s:output) s={noise(rng),noise(rng)};
    for (std::size_t i=0;i<input.size();++i)
        output[prefix+i] += input[i]*std::polar(1.0f,float(2*3.141592653589793*cfoBins*i/c.samplesPerSymbol()));
    return output;
}
std::vector<Reception> chunked(Receiver &rx, const Iq &iq) {
    std::vector<Reception> packets;
    for (std::size_t i=0;i<iq.size();) {
        const auto count=std::min<std::size_t>(1+(i*23+139)%4093,iq.size()-i);
        auto found=rx.push(iq.data()+i,count);
        packets.insert(packets.end(),found.begin(),found.end());i+=count;
    }
    return packets;
}
}
int main() {
    try {
        int cases=0;
        for (int sf=7;sf<=12;++sf) for(int cr=1;cr<=4;++cr)
        for(bool explicitHeader:{false,true}) for(bool crc:{false,true})
        for(auto ldro:{Ldro::Off,Ldro::On}) for(int length:{0,1,2,3,15,64,255}) {
            Config c;c.spreadingFactor=sf;c.codingRate=cr;c.explicitHeader=explicitHeader;
            c.payloadCrc=crc;c.ldro=ldro;c.implicitPayloadBytes=length;
            const PacketCodec codec(c);const auto data=bytes(length);const auto encoded=codec.encode(data);
            const auto decoded=codec.decode(encoded);
            check(decoded.payload==data && decoded.crcChecked==crc && decoded.correctedCodewords==0,"packet matrix");
            check(encoded.size()==std::size_t(codec.symbolCount(length)),"symbol count"); ++cases;
        }
        std::cout<<"PASS "<<cases<<" codec combinations: SF7..12, CR4/5..4/8, headers, CRC, LDRO, lengths0..255\n";
        // Published independent LoRaPHY.m example, lines 22-24 (MIT, jkadbear).
        // The example has a 9-byte explicit packet, CR4/8, CRC and LDRO on.
        const Symbols reference{2541,1153,673,2397,1189,3509,41,3089,3237,3917,2729,2765,
            1417,2833,1389,801,3197,345,961,745,3101,297,1893,469};
        Config referenceConfig;referenceConfig.spreadingFactor=12;referenceConfig.codingRate=4;referenceConfig.ldro=Ldro::On;
        const Bytes referencePayload{1,2,3,4,5,6,7,8,9};
        check(PacketCodec(referenceConfig).decode(reference).payload==referencePayload,"published independent symbol vector decode");
        const auto referenceEncoded=PacketCodec(referenceConfig).encode(referencePayload);
        // The published example has arbitrary trailing pad nibbles; compare the
        // complete blocks before padding, and decode its entire packet above.
        check(referenceEncoded.size()==reference.size()&&std::equal(referenceEncoded.begin(),referenceEncoded.begin()+16,reference.begin()),
              "published independent symbol vector encode before padding");
        check(PacketCodec::payloadChecksum(referencePayload)==0x2eba,"published independent CRC vector");
        std::cout<<"PASS independent LoRaPHY golden symbols and CRC\n";
        check(PacketCodec::payloadChecksum({})==0,"empty CRC");
        check(PacketCodec::payloadChecksum({0x12})==0x12,"one-byte CRC");
        check(PacketCodec::payloadChecksum({0x12,0x34})==0x1234,"two-byte CRC");
        // Independent CCITT reference: CRC('123456789') = 0x31c3 at init=0.
        const Bytes ccitt{'1','2','3','4','5','6','7','8','9',0x51,0x23};
        check(PacketCodec::payloadChecksum(ccitt)==(0x31c3^0x5123),"CRC polynomial and byte order");
        Config c;c.spreadingFactor=7;c.bandwidthHz=125000;c.preambleSymbols=8;
        check(std::abs(PacketCodec(c).timeOnAirSeconds(20)-0.056576)<1e-12,"known airtime: SF7/BW125/CR4/5/20B =56.576ms");
        for (int sf=7;sf<=12;++sf) for(int os:{1,2,4,8}) {
            c.spreadingFactor=sf;c.oversampling=os;
            Modem m(c);const auto data=bytes(7);const auto iq=m.modulate(data);
            check(m.demodulateAligned(iq).payload==data,"aligned IQ roundtrip");
            for(int symbol:{0,1,31,(1<<sf)-1}) {
                const auto s=m.chirp(symbol);
                const auto peak=m.demodulateSymbol(s.data(),s.size());
                check(peak.bin==symbol&&peak.concentration>0.9999,"chirp/FFT bin and power");
            }
        }
        std::cout<<"PASS IQ modulation/demodulation at SF7..12 and 1x/2x/4x/8x sample rate\n";
        for(int sf:{7,9,11,12}) for(int os:{1,2,4}) for(bool inverted:{false,true}) for(double offset:{-5.23,0.23,8.23}) {
            c=Config{};c.spreadingFactor=sf;c.oversampling=os;c.invertIq=inverted;c.preambleSymbols=8;
            const auto data=bytes(19);const auto iq=impaired(Modem(c).modulate(data),c,12,offset,317);
            Receiver rx(c);const auto packets=chunked(rx,iq);
            check(packets.size()==1&&packets[0].packet.payload==data&&packets[0].packet.crcChecked,
                  "stream acquisition sf="+std::to_string(sf)+" os="+std::to_string(os)+" inverted="+std::to_string(inverted)+" cfo="+std::to_string(offset));
            const double expectedOffset=(inverted?-offset:offset)*c.bandwidthHz/c.bins();
            if(packets.size()==1)check(std::abs(packets[0].frequencyOffsetHz-expectedOffset)<0.02*c.bandwidthHz/c.bins(),
                "CFO estimate within 0.02 bins");
        }
        std::cout<<"PASS chunked acquisition with sample offset, CFO, noise and inverted IQ\n";
        for(int sf:{7,11}) for(bool explicitHeader:{false,true}) for(int length:{0,1,2,3}) {
            c=Config{};c.spreadingFactor=sf;c.explicitHeader=explicitHeader;c.implicitPayloadBytes=length;
            Receiver shortRx(c);const auto exact=Modem(c).modulate(bytes(length));
            const auto complete=chunked(shortRx,exact);
            check(complete.size()==1&&complete[0].packet.payload==bytes(length),"short exact burst without trailing samples");
        }
        for(int sf:{7,11}) for(double snr:{-3.0,0.0}) {
            c=Config{};c.spreadingFactor=sf;
            const auto waveform=impaired(Modem(c).modulate(bytes(25)),c,snr,-3.17,91);
            Receiver noisyRx(c);const auto got=chunked(noisyRx,waveform);
            check(got.size()==1&&got[0].packet.payload==bytes(25),"noisy IQ SNR="+std::to_string(snr));
        }
        c=Config{};c.spreadingFactor=7;c.preambleSymbols=8;c.codingRate=4;
        Receiver rx(c);auto first=Modem(c).modulate(bytes(11));auto second=Modem(c).modulate(bytes(23));
        Iq two(91);two.insert(two.end(),first.begin(),first.end());two.insert(two.end(),second.begin(),second.end());two.resize(two.size()+c.samplesPerSymbol());
        const auto packets=chunked(rx,two);
        check(packets.size()==2&&packets[0].packet.payload==bytes(11)&&packets[1].packet.payload==bytes(23),"back-to-back packets");
        auto wrong=c;wrong.syncWord=0x34;
        check(chunked(rx,Modem(wrong).modulate(bytes(11))).empty(),"foreign sync word rejected");rx.reset();
        Iq silence(1000*c.samplesPerSymbol());
        check(rx.push(silence).empty()&&rx.bufferedSamples()<8*c.samplesPerSymbol(),"silence bounded/no packet");rx.reset();
        const auto packet=PacketCodec(c).encode(bytes(40));
        // One flipped interleaved bit becomes one erroneous Hamming codeword.
        // The first block is always CR4/8 and must recover its header exactly.
        auto damagedHeader=packet;
        auto gray=unsigned(damagedHeader[0]/4);gray^=gray>>1;gray^=1;
        unsigned binary=gray;while(gray>>=1)binary^=gray;
        damagedHeader[0]=std::uint16_t(binary*4+1);
        const auto corrected=PacketCodec(c).decode(damagedHeader);
        check(corrected.payload==bytes(40)&&corrected.correctedCodewords==1,"single-bit header FEC recovery");
        auto broken=packet;broken.pop_back();rejects([&]{PacketCodec(c).decode(broken);},"truncated symbols rejected");
        rejects([&]{PacketCodec(c).decode(Symbols(8,0));},"invalid explicit header rejected");
        broken=packet;broken[0]=c.bins();rejects([&]{PacketCodec(c).decode(broken);},"invalid symbol rejected");
        // At SF7 the first block contains only the explicit header. Replace a middle
        // payload block with another valid FEC block, retaining the original CRC.
        auto changed=bytes(40);changed[12]^=0x80;const auto other=PacketCodec(c).encode(changed);
        broken=packet;std::copy(other.begin()+32,other.begin()+40,broken.begin()+32);
        rejects([&]{PacketCodec(c).decode(broken);},"CRC rejects FEC-valid payload corruption");
        rejects([&]{rx.push(Iq{{std::numeric_limits<float>::quiet_NaN(),0}});},"NaN IQ rejected");
        rejects([&]{auto bad=c;bad.spreadingFactor=6;PacketCodec codec(bad);},"unsupported SF rejected");
        rejects([&]{auto bad=c;bad.bandwidthHz=0;Modem m(bad);},"zero bandwidth rejected");
        rejects([&]{Modem(c).modulate(Bytes(256));},"oversized payload rejected");
        rejects([&]{Modem(c).modulate(bytes(3),-1);},"invalid amplitude rejected");
        rx.push(first.data(),first.size()/2);rx.reset();
        check(chunked(rx,first).size()==1,"reset discards incomplete frame");
        std::cout<<"PASS validation, CRC rejection, reset, sync isolation and bounded history\n";
    } catch(const std::exception &e) { ++failures; std::cerr<<"EXCEPTION "<<e.what()<<'\n'; }
    std::cout<<(failures?"FAILED ":"ALL PASS ")<<failures<<" failures\n";
    return failures?1:0;
}
