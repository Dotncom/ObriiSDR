#include "adcoverloadguard.h"
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
int main() {
    try {
        for (bool enabled : {false,true}) for (bool eligible : {false,true}) {
            AdcOverloadGuard guard;
            guard.update(1,0,0,20,enabled,eligible,0);
            int decisions=0;
            for (int i=1;i<=4;++i) {
                auto r=guard.update(1,i*100000,i*200,20,enabled,eligible,i*250);
                if (r.requestedVga>=0) { ++decisions; require(r.requestedVga==19,"Unexpected VGA step"); }
            }
            require(decisions==(enabled&&eligible?1:0),"Enable/eligibility/settling gate failed");
        }
        AdcOverloadGuard guard;
        guard.update(1,0,0,1,true,true,0);
        auto a=guard.update(1,100000,2000,1,true,true,800);
        require(a.valid && a.requestedVga<0,"Single spike changed VGA");
        a=guard.update(1,200000,4000,1,true,true,1050);
        require(a.requestedVga==0,"Severe overload must clamp to minimum VGA");
        a=guard.update(1,300000,6000,1,true,true,1300);
        require(a.requestedVga<0,"Cooldown failed");
        a=guard.update(1,400000,8000,0,true,true,2400);
        require(a.requestedVga<0,"VGA went below zero");
        guard.reset();
        guard.update(2,100,5,20,true,true,2500);
        a=guard.update(2,200100,5,20,true,true,3300);
        require(a.valid && a.railPercent==0 && a.requestedVga<0,"Quiet input raised gain");
        a=guard.update(2,200100,5,20,true,true,3550);
        require(!a.valid && a.requestedVga<0,"Stale counters generated a decision");
        a=guard.update(2,300100,200005,20,true,true,3800);
        require(!a.valid,"Invalid counters accepted");
        guard.reset();
        guard.update(3,0xffff0000u,0,20,true,true,4000);
        a=guard.update(3,0xffff0000u+100000u,200,20,true,true,4800);
        require(a.valid && a.railPercent==0.2,"Counter wrap broke interval");
        a=guard.update(3,0xffff0000u+200000u,400,20,true,true,5050);
        require(a.requestedVga==19,"Counter wrap broke overload decision");
        guard.reset();
        guard.update(4,0,0,20,true,true,6000);
        a=guard.update(4,100000,5,20,true,true,6800);
        a=guard.update(4,200000,10,20,true,true,7050);
        require(a.requestedVga<0,"Below-threshold rails changed gain");
        a=guard.update(4,300000,21,20,true,true,7300);
        a=guard.update(4,400000,32,20,true,true,7550);
        require(a.requestedVga==19,"Low sustained saturation was missed");
        std::cout<<"PASS: opt-in/eligibility, settling, sustained saturation, cooldown, minimum VGA, stale/invalid data, generation and counter wrap\n";
        return 0;
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
