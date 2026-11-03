#include "regional.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace obrii::lorawan {
namespace {
std::uint32_t le(const Bytes &b, std::size_t offset, unsigned size) {
    std::uint32_t n=0; for(unsigned i=0; i<size; ++i) n|=std::uint32_t(b.at(offset+i))<<(i*8); return n;
}
int band(std::uint32_t hz) {
    if(hz>=863000000 && hz<865000000) return 2;
    if(hz>=865000000 && hz<=868000000) return 0;
    if(hz>868000000 && hz<=868600000) return 1;
    if(hz>=868700000 && hz<=869200000) return 5;
    if(hz>=869400000 && hz<=869650000) return 3;
    if(hz>=869700000 && hz<=870000000) return 4;
    throw Error("Frequency outside supported EU868 subbands");
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if(b>std::numeric_limits<std::uint64_t>::max()-a) throw Error("Time overflow"); return a+b;
}
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if(b && a>std::numeric_limits<std::uint64_t>::max()/b) throw Error("Time overflow"); return a*b;
}
unsigned commandSize(Direction dir, unsigned cid) {
    if(dir==Direction::Uplink) {
        switch(cid) { case 2:return 0; case 3:return 1; case 4:return 0; case 5:return 1;
            case 6:return 2; case 7:return 1; case 8:return 0; case 9:return 0; case 10:return 1; case 13:return 0; }
    } else {
        switch(cid) { case 2:return 2; case 3:return 4; case 4:return 1; case 5:return 4;
            case 6:return 0; case 7:return 5; case 8:return 1; case 9:return 1; case 10:return 4; case 13:return 5; }
    }
    return 256;
}
void answer(Bytes &b, std::uint8_t cid, std::optional<std::uint8_t> status={}) {
    b.push_back(cid); if(status) b.push_back(*status);
}
}
MacCommands parseMac(Direction dir, const Bytes &bytes) {
    if((dir!=Direction::Uplink && dir!=Direction::Downlink) || bytes.size()>242) throw Error("Invalid MAC command input");
    MacCommands result;
    for(std::size_t at=0; at<bytes.size();) {
        const auto begin=at; const auto cid=bytes[at++]; const auto size=commandSize(dir,cid);
        if(size==256) { result.unparsed.assign(bytes.begin()+begin,bytes.end()); break; }
        if(at+size>bytes.size()) throw Error("Truncated MAC command");
        result.commands.push_back({cid,Bytes(bytes.begin()+at,bytes.begin()+at+size)}); at+=size;
    }
    return result;
}
Eu868::Eu868() {
    for(std::size_t i=0; i<3; ++i) channels[i]={std::uint32_t(868100000+i*200000),0,0,5,true};
}
bool Eu868::validFrequency(std::uint32_t hz) {
    if(hz%100) return false;
    try { (void)band(hz); return true; } catch(const Error &) { return false; }
}
std::size_t Eu868::maxMacPayload(unsigned dr, bool repeater) {
    if(dr>6) throw Error("Unsupported EU868 rate: only LoRa DR0..DR6 are implemented");
    return dr<3 ? 59 : dr==3 ? 123 : repeater ? 230:250;
}
int Eu868::spreadingFactor(unsigned dr) { (void)maxMacPayload(dr); return dr==6 ? 7:12-int(dr); }
int Eu868::bandwidth(unsigned dr) { (void)maxMacPayload(dr); return dr==6 ? 250000:125000; }
int Eu868::txEirpDbm(unsigned index) { if(index>7) throw Error("EU868 power index must be 0..7"); return 16-int(index)*2; }
void Eu868::checkPayload(const Bytes &wire, unsigned dr, bool repeater) {
    if(wire.empty() || wire.size()>maxMacPayload(dr,repeater)+5) throw Error("PHYPayload exceeds EU868 data-rate limit");
}
void Eu868::validate() const {
    (void)maxMacPayload(dataRate,repeaterCompatible); (void)maxMacPayload(rx2DataRate,repeaterCompatible);
    (void)txEirpDbm(txPower);
    if(!nbTrans || nbTrans>15 || dutyExponent>15 || rx1Offset>5 || receiveDelay<1 || receiveDelay>15
       || !validFrequency(rx2FrequencyHz) || stickyAnswers.size()>242) throw Error("Invalid EU868 settings");
    bool any=false;
    for(const auto &c:channels) {
        if(c.frequencyHz && (!validFrequency(c.frequencyHz) || c.minRate>c.maxRate || c.maxRate>6
           || (c.downlinkHz && !validFrequency(c.downlinkHz)))) throw Error("Invalid EU868 channel");
        if(c.enabled && !c.frequencyHz) throw Error("Enabled undefined channel");
        any|=c.enabled && c.minRate<=dataRate && c.maxRate>=dataRate;
    }
    if(!any) throw Error("No enabled EU868 channel supports selected rate");
}
int Eu868::channelIndex(std::uint32_t hz, unsigned rate) const {
    for(std::size_t i=0; i<channels.size(); ++i) {
        const auto &c=channels[i];
        if(c.enabled && c.frequencyHz==hz && rate>=c.minRate && rate<=c.maxRate) return int(i);
    }
    throw Error("Frequency/rate is not an enabled EU868 uplink channel");
}
void Eu868::applyJoin(const JoinAccept &a) {
    if(a.rx1Offset>5 || a.rx2DataRate>6 || a.rxDelay>15) throw Error("Join accept has unsupported EU868 settings");
    Eu868 next;
    next.rx1Offset=a.rx1Offset; next.rx2DataRate=a.rx2DataRate; next.receiveDelay=std::max<std::uint8_t>(1,a.rxDelay);
    next.dataRate=dataRate; next.repeaterCompatible=repeaterCompatible;
    next.adrEnabled=adrEnabled;
    if(a.cfList) {
        const auto &cf=*a.cfList;
        if(cf[15]!=0) throw Error("Only EU868 frequency CFList type 0 is supported");
        for(std::size_t i=0; i<5; ++i) {
            const auto hz=(std::uint32_t(cf[i*3])|std::uint32_t(cf[i*3+1])<<8|std::uint32_t(cf[i*3+2])<<16)*100;
            // Invalid NewChannel parameters are ignored, as required for CFList.
            if(hz && validFrequency(hz)) next.channels[i+3]={hz,0,0,5,true};
        }
    }
    next.validate(); *this=std::move(next);
}
MacResult Eu868::processDownlink(const DataFrame &f, std::uint8_t battery, int snr) {
    if(direction(f.type)!=Direction::Downlink) throw Error("Expected authenticated downlink");
    if(f.port && *f.port==0 && !f.options.empty()) throw Error("MAC commands in both FOpts and FRMPayload");
    const auto parsed=parseMac(Direction::Downlink,f.port && *f.port==0 ? f.payload:f.options);
    Eu868 next=*this; next.stickyAnswers.clear(); next.adrAckCount=0; MacResult result; result.unparsed=parsed.unparsed;
    for(std::size_t i=0; i<parsed.commands.size(); ++i) {
        const auto &command=parsed.commands[i]; const auto &p=command.data; const auto cid=command.cid;
        if(cid==2) { result.linkCheck=std::array<std::uint8_t,2>{p[0],p[1]}; }
        else if(cid==3) {
            Eu868 proposed=next; bool masksOk=true; std::size_t end=i;
            while(end+1<parsed.commands.size() && parsed.commands[end+1].cid==3) ++end;
            for(std::size_t j=i; j<=end; ++j) {
                const auto &q=parsed.commands[j].data; const auto control=(q[3]>>4)&7; const auto mask=le(q,1,2);
                if(control==0) for(std::size_t k=0; k<16; ++k) {
                    if((mask>>k&1) && !proposed.channels[k].frequencyHz) masksOk=false;
                    proposed.channels[k].enabled=(mask>>k&1)!=0;
                }
                else if(control==6) for(auto &c:proposed.channels) c.enabled=c.frequencyHz!=0;
                else masksOk=false;
            }
            const auto &last=parsed.commands[end].data;
            const auto dr=last[0]>>4, power=last[0]&15;
            bool rateOk=dr==15 || (next.adrEnabled ? dr<=6:dr==next.dataRate);
            bool powerOk=power==15 || (next.adrEnabled ? power<=7:power==next.txPower);
            if(next.adrEnabled) {
                if(rateOk && dr!=15) proposed.dataRate=std::uint8_t(dr);
                if(powerOk && power!=15) proposed.txPower=std::uint8_t(power);
                proposed.nbTrans=std::max<std::uint8_t>(1,last[3]&15);
            }
            bool any=false;
            for(const auto &c:proposed.channels) any|=c.enabled && c.frequencyHz && c.minRate<=proposed.dataRate && c.maxRate>=proposed.dataRate;
            masksOk&=any;
            const std::uint8_t status=std::uint8_t((powerOk?4:0)|(rateOk?2:0)|(masksOk?1:0));
            if(status==7 || (!next.adrEnabled && masksOk)) next=std::move(proposed);
            for(std::size_t j=i; j<=end; ++j) answer(result.answers,3,status);
            i=end;
        } else if(cid==4) { next.dutyExponent=p[0]&15; answer(result.answers,4); }
        else if(cid==5) {
            const auto hz=le(p,1,3)*100; const auto offset=(p[0]>>4)&7, dr=p[0]&15;
            const std::uint8_t status=std::uint8_t((offset<=5?4:0)|(dr<=6?2:0)|(validFrequency(hz)?1:0));
            if(status==7) { next.rx1Offset=std::uint8_t(offset); next.rx2DataRate=std::uint8_t(dr); next.rx2FrequencyHz=hz; }
            answer(result.answers,5,status); answer(next.stickyAnswers,5,status);
        } else if(cid==6) {
            answer(result.answers,6,battery); result.answers.push_back(std::uint8_t(std::clamp(snr,-32,31))&63);
        } else if(cid==7) {
            const unsigned index=p[0], minimum=p[4]&15, maximum=p[4]>>4; const auto hz=le(p,1,3)*100;
            bool rateOk=minimum<=maximum && maximum<=6;
            bool freqOk=index>=3 && index<16 && (!hz || validFrequency(hz));
            const std::uint8_t status=std::uint8_t((rateOk?2:0)|(freqOk?1:0));
            if(status==3) next.channels[index]={hz,0,std::uint8_t(minimum),std::uint8_t(maximum),hz!=0};
            answer(result.answers,7,status);
        } else if(cid==8) {
            next.receiveDelay=std::max<std::uint8_t>(1,p[0]&15); answer(result.answers,8); answer(next.stickyAnswers,8);
        } else if(cid==9) { /* EU868 does not implement TXParamSetupReq; no acknowledgment. */ }
        else if(cid==10) {
            const auto index=p[0]; const auto hz=le(p,1,3)*100;
            const bool exists=index<16 && next.channels[index].frequencyHz;
            const std::uint8_t status=std::uint8_t((exists?2:0)|(validFrequency(hz)?1:0));
            if(status==3) next.channels[index].downlinkHz=hz;
            answer(result.answers,10,status); answer(next.stickyAnswers,10,status);
        } else if(cid==13) {
            std::array<std::uint8_t,5> time{}; std::copy(p.begin(),p.end(),time.begin()); result.deviceTime=time;
        }
    }
    // Answers are small bounded command lists; the host truncates on a command
    // boundary to its actual DR capacity, prioritising MAC answers over app data.
    const auto capacity=maxMacPayload(next.dataRate,next.repeaterCompatible)-8;
    if(result.answers.size()>capacity) {
        std::size_t end=0;
        while(end<result.answers.size()) {
            const auto nextEnd=end+1+commandSize(Direction::Uplink,result.answers[end]);
            if(nextEnd>capacity) break;
            end=nextEnd;
        }
        result.answers.resize(end); result.answersTruncated=true;
    }
    if(next.stickyAnswers.size()>242) throw Error("Sticky MAC answer buffer exceeds limit");
    next.validate(); *this=std::move(next); return result;
}
std::uint8_t Eu868::onNewUplink() {
    validate();
    if(!adrEnabled) { adrAckCount=0; return 0; }
    Eu868 next=*this;
    if(next.adrAckCount<std::numeric_limits<std::uint32_t>::max()) ++next.adrAckCount;
    if(next.adrAckCount>=96) next.txPower=0;
    if(next.adrAckCount>=128 && (next.adrAckCount-128)%32==0) {
        if(next.dataRate) --next.dataRate;
        else {
            for(std::size_t i=0; i<3; ++i) next.channels[i].enabled=true;
            next.nbTrans=1;
        }
        bool any=false;
        for(const auto &c:next.channels) any|=c.enabled && c.frequencyHz && c.minRate<=next.dataRate && c.maxRate>=next.dataRate;
        if(!any) for(std::size_t i=0; i<3; ++i) next.channels[i].enabled=true;
    }
    next.validate(); *this=std::move(next);
    return std::uint8_t(0x80|(adrAckCount>=64 ? 0x40:0));
}
std::array<RxWindow,2> receiveWindows(const Eu868 &s, std::uint64_t txEnd, std::uint32_t frequency,
                                    unsigned rate, bool join, std::uint32_t tolerance) {
    s.validate(); if(tolerance>250000) throw Error("RX clock tolerance exceeds 250ms");
    const auto channel=s.channelIndex(frequency,rate); std::array<RxWindow,2> windows{};
    for(unsigned i=0; i<2; ++i) {
        auto &w=windows[i]; w.index=std::uint8_t(i+1);
        w.nominalUs=add(txEnd,std::uint64_t((join?5:s.receiveDelay)+i)*1000000);
        w.frequencyHz=i ? s.rx2FrequencyHz : s.channels[channel].downlinkHz ? s.channels[channel].downlinkHz:frequency;
        w.dataRate=i ? s.rx2DataRate:std::uint8_t(std::max(0,int(rate)-int(s.rx1Offset)));
        const auto detection=std::uint64_t(std::ceil(8.0*(1<<Eu868::spreadingFactor(w.dataRate))*1000000/Eu868::bandwidth(w.dataRate)));
        w.opensUs=w.nominalUs>tolerance ? w.nominalUs-tolerance:0;
        w.closesUs=add(add(w.nominalUs,tolerance),detection);
    }
    return windows;
}
std::uint64_t nextUplinkTime(const std::array<RxWindow,2> &windows, bool confirmed, std::uint32_t delay) {
    if(delay<1000000 || delay>3000000) throw Error("EU868 retry delay must be 1..3 seconds");
    return confirmed ? std::max(windows[1].closesUs,add(windows[1].nominalUs,delay)):windows[1].closesUs;
}
std::uint64_t dutyReady(const DutyState &s, std::uint32_t hz) { return std::max(s.aggregate,s.bands[band(hz)]); }
DutyState reserveDuty(const DutyState &s, std::uint32_t hz, std::uint64_t begin, std::uint64_t airtime, unsigned exponent) {
    if(!airtime || exponent>15 || begin<dutyReady(s,hz)) throw Error("Invalid airtime, duty exponent or premature TX");
    static constexpr unsigned denominators[]{100,100,1000,10,100,1000};
    auto next=s; const auto index=band(hz);
    next.bands[index]=add(begin,multiply(airtime,denominators[index]));
    next.aggregate=add(begin,multiply(airtime,std::uint64_t(1)<<exponent)); return next;
}
DutyState reserveBackoff(const DutyState &s, std::uint32_t hz, std::uint64_t begin, std::uint64_t airtime,
                         std::uint64_t uptime, unsigned exponent) {
    auto next=reserveDuty(s,hz,begin,airtime,exponent);
    const unsigned ratio=uptime<3600000000ULL ? 100 : uptime<39600000000ULL ? 1000:10000;
    next.aggregate=std::max(next.aggregate,add(begin,multiply(airtime,ratio))); return next;
}
}
