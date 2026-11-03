#include "classbc.h"
#include <algorithm>
#include <limits>

namespace obrii::lorawan {
namespace {
constexpr std::uint64_t Period=128000000,Reserved=2120000,Guard=3000000,Slot=30000,Holdover=7200000000ULL;
std::uint64_t add(std::uint64_t a,std::uint64_t b){if(b>std::numeric_limits<std::uint64_t>::max()-a)throw Error("Class B time overflow");return a+b;}
std::uint16_t crc(const Bytes &b,std::size_t at,std::size_t end){
    std::uint16_t c=0;for(;at<end;++at){c^=std::uint16_t(b[at])<<8;for(unsigned j=0;j<8;++j)c=std::uint16_t((c<<1)^((c&0x8000)?0x1021:0));}return c;
}
void put(Bytes &b,std::uint32_t n,unsigned size){for(unsigned i=0;i<size;++i)b.push_back(std::uint8_t(n>>(8*i)));}
}
Bytes encodeBeacon(const Beacon &b){
    if(b.gpsSeconds%128)throw Error("Beacon time must be a multiple of 128 GPS seconds");
    Bytes out{0,0};put(out,b.gpsSeconds,4);put(out,crc(out,0,6),2);
    out.insert(out.end(),b.gateway.begin(),b.gateway.end());put(out,crc(out,8,15),2);return out;
}
Beacon decodeBeacon(const Bytes &b){
    if(b.size()!=17 || b[0] || b[1])throw Error("Invalid EU868 beacon length/RFU");
    if(crc(b,0,6)!=(unsigned(b[6])|(unsigned(b[7])<<8)))throw Error("Beacon common CRC mismatch");
    Beacon result;for(unsigned i=0;i<4;++i)result.gpsSeconds|=std::uint32_t(b[i+2])<<(8*i);
    if(result.gpsSeconds%128)throw Error("Unaligned beacon time");
    std::copy_n(b.begin()+8,7,result.gateway.begin());result.gatewayCrcValid=crc(b,8,15)==(unsigned(b[15])|(unsigned(b[16])<<8));return result;
}
std::uint16_t pingOffset(std::uint32_t time,std::uint32_t addr,unsigned periodicity){
    if(periodicity>7 || !addr)throw Error("Invalid Class B periodicity/address");
    Block b{};for(unsigned i=0;i<4;++i){b[i]=std::uint8_t(time>>(8*i));b[4+i]=std::uint8_t(addr>>(8*i));}
    b=aesEncrypt(Key{},b);return std::uint16_t((unsigned(b[0])|(unsigned(b[1])<<8))%(32u<<periodicity));
}
void ClassBConfig::validate() const {
    if(!Eu868::validFrequency(beaconFrequencyHz)||!Eu868::validFrequency(pingFrequencyHz)||pingDataRate>6||periodicity>7
        ||clockErrorPpm>1000||!toleranceUs||toleranceUs>250000)throw Error("Invalid EU868 Class B configuration");
}
ClassBTracker::ClassBTracker(ClassBConfig c):config_(c){c.validate();}
void ClassBTracker::reset(){time_.reset();anchor_=0;}
void ClassBTracker::configure(ClassBConfig c){c.validate();config_=c;reset();}
void ClassBTracker::acquire(const Bytes &b,std::uint64_t start){
    const auto beacon=decodeBeacon(b);
    if(time_){
        const std::uint32_t delta=beacon.gpsSeconds-*time_;
        if(!delta||delta>=0x80000000U||start<anchor_)throw Error("Replayed/backwards Class B beacon");
        const auto elapsed=start-anchor_,expected=std::uint64_t(delta)*1000000;
        const auto error=elapsed>expected?elapsed-expected:expected-elapsed;
        if(error>std::uint64_t(config_.toleranceUs)*2+expected*config_.clockErrorPpm/1000000)
            throw Error("Beacon GPS/host time discontinuity; reacquire explicitly");
    }
    (void)add(start,Holdover+Period);time_=beacon.gpsSeconds;anchor_=start;
}
bool ClassBTracker::locked(std::uint64_t now) const{return time_ && now>=anchor_ && now-anchor_<Holdover;}
RxWindow ClassBTracker::slot(std::uint32_t addr,std::uint64_t period,unsigned n) const{
    const auto time=std::uint32_t(*time_+std::uint32_t(period*128));
    const auto offset=pingOffset(time,addr,config_.periodicity);
    const auto nominal=add(add(anchor_,period*Period),Reserved+(offset+std::uint64_t(n)*(32u<<config_.periodicity))*Slot);
    const auto drift=(nominal-anchor_)*config_.clockErrorPpm/1000000;
    const auto margin=std::uint64_t(config_.toleranceUs)+drift;
    // Preamble detection window; retain the full packet after detection.
    const auto symbols=std::uint64_t(8)*(std::uint64_t(1)<<Eu868::spreadingFactor(config_.pingDataRate))*1000000/Eu868::bandwidth(config_.pingDataRate);
    return {nominal,nominal>margin?nominal-margin:0,add(nominal,std::max(Slot,symbols)+margin),config_.pingFrequencyHz,config_.pingDataRate,3};
}
std::optional<RxWindow> ClassBTracker::nextPing(std::uint32_t addr,std::uint64_t now) const {
    if(!locked(now))return {};
    const auto period=(now-anchor_)/Period;
    for(auto p=period;p<=period+1;++p)for(unsigned n=0;n<(128u>>config_.periodicity);++n){
        auto w=slot(addr,p,n);if(w.closesUs>=now && w.nominalUs<anchor_+Holdover)return w;
    }return {};
}
bool ClassBTracker::acceptsPing(std::uint32_t addr,std::uint64_t start,std::uint32_t hz,unsigned dr) const {
    if(!locked(start)||hz!=config_.pingFrequencyHz||dr!=config_.pingDataRate)return false;
    const auto period=(start-anchor_)/Period;
    for(auto p=period?period-1:0;p<=period;++p)for(unsigned n=0;n<(128u>>config_.periodicity);++n)
        if(slot(addr,p,n).containsStart(start))return true;
    return false;
}
bool ClassBTracker::allowsTransaction(std::uint64_t begin,std::uint64_t end) const{
    if(end<begin)throw Error("Backwards Class B transaction");
    if(!locked(begin))return true;
    const auto p=(begin-anchor_)/Period;
    const auto base=add(anchor_,p*Period);
    if(begin<base+Reserved)return false;
    return end<base+Period-Guard;
}
std::uint64_t ClassBTracker::nextTransaction(std::uint64_t begin,std::uint64_t duration) const {
    if(duration>=Period-Reserved-Guard)throw Error("Class B transaction exceeds beacon window");
    if(allowsTransaction(begin,add(begin,duration)))return begin;
    const auto base=add(anchor_,((begin-anchor_)/Period)*Period);
    return begin<base+Reserved?base+Reserved:add(base,Period+Reserved);
}
bool acceptsClassC(const Eu868 &r,std::uint64_t start,std::uint32_t hz,unsigned dr,
    const std::optional<std::array<RxWindow,2>> &windows,std::optional<std::pair<std::uint64_t,std::uint64_t>> tx){
    if(tx && start>=tx->first && start<=tx->second)return false;
    if(windows && (*windows)[0].containsStart(start))return hz==(*windows)[0].frequencyHz && dr==(*windows)[0].dataRate;
    return hz==r.rx2FrequencyHz && dr==r.rx2DataRate;
}
}
