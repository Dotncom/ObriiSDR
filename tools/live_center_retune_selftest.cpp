#include "livecenterretune.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

static int checks;
static void require(bool condition, const char *message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
static bool near(double a, double b) { return std::abs(a-b) < 0.01; }

static void burst(bool continuous, int usbMs) {
    double confirmed=120e6, desired=confirmed, inFlightTarget=0;
    int inputAt=-1000, sentAt=-1000, completesAt=-1, commands=0, finalAt=-1;
    bool queued=false;
    std::vector<double> targets;
    for (int ms=0; ms<5000; ++ms) {
        // A continuous497ms wheel/drag gesture, ending between grid points.
        if (ms<=497) {
            desired=120e6+ms*100000.0;
            inputAt=ms;queued=true;
        }
        if (completesAt==ms) {
            confirmed=inFlightTarget;completesAt=-1;
            if (near(confirmed,169.7e6)) finalAt=ms;
        }
        if (queued && completesAt<0) {
            const auto d=liveCenterRetuneDecision(continuous,desired,confirmed,50e6,5.0,
                                                ms-inputAt,ms-sentAt,160);
            if (d.send) {
                if (!continuous) require(ms>=557,"Safe mode tuned during movement");
                require(d.targetHz<=desired+0.01,"Retune overshot the selected frequency");
                targets.push_back(d.targetHz);++commands;
                inFlightTarget=d.targetHz;sentAt=ms;completesAt=ms+usbMs;
                queued=!near(d.targetHz,desired);
            } else if (d.waitMs==0) queued=false;
        }
    }
    require(near(confirmed,169.7e6),"Final target did not reach exact selected frequency");
    require(finalAt>=557 && finalAt<=557+2*usbMs,"Intermediate targets accumulated after gesture");
    require(continuous ? commands>=2 : commands==1,"Incorrect number of safe/continuous commands");
    if (continuous && usbMs==600) require(commands<=2,"Slow USB replayed intermediate wheel targets");
    if (continuous) require(targets.front()<169.7e6,"Continuous mode waited until gesture ended");
}

int main() {
    try {
        auto decide=[](bool smooth, double desired, double actual, double rate, double percent,
                       int inputAge, int cmdAge, int frameWaitMs = 0) {
            return liveCenterRetuneDecision(smooth, desired, actual, rate, percent,
                                            inputAge, cmdAge, 160, false, frameWaitMs);
        };
        auto d=decide(false,170e6,120e6,50e6,5,0,1000);
        require(!d.send && d.waitMs==60,"Safe mode must remain the quiet default");
        d=decide(false,170e6,120e6,50e6,5,59,1000);
        require(!d.send && d.waitMs==1,"Safe quiet deadline changed");
        d=decide(false,169.7e6,120e6,50e6,5,60,1000);
        require(d.send && near(d.targetHz,169.7e6),"Safe final value was quantized");
        d=decide(true,122.4e6,120e6,50e6,5,10,1000);
        require(!d.send && d.waitMs==50,"Sub-step movement must accumulate");
        d=decide(true,122.5e6,120e6,50e6,5,0,1000);
        require(d.send && near(d.targetHz,122.5e6),"5 percent of50MSps must be2.5MHz");
        d=decide(true,126.1e6,120e6,50e6,5,0,1000);
        require(d.send && near(d.targetHz,125e6),"Positive motion did not follow the sample-rate grid");
        d=decide(true,113.9e6,120e6,50e6,5,0,1000);
        require(d.send && near(d.targetHz,115e6),"Reverse motion did not follow the sample-rate grid");
        d=decide(true,122.4e6,120e6,50e6,5,60,1000);
        require(d.send && near(d.targetHz,122.4e6),"Final sub-step residual was lost");
        d=decide(true,125e6,120e6,50e6,5,0,7);
        require(!d.send && d.waitMs==33,"Continuous command-rate guard ignored");
        d = decide(true, 126.1e6, 120e6, 50e6, 5, 3, 45, 33);
        require(!d.send && d.waitMs == 33, "Next step discarded the pending display frame");
        d = decide(true, 126.1e6, 120e6, 50e6, 5, 3, 45);
        require(d.send && near(d.targetHz, 125e6), "Retune did not resume after the frame wait");
        d = decide(true, 126.1e6, 120e6, 50e6, 5, 50, 45, 90);
        require(!d.send && d.waitMs == 10, "Frame wait delayed the quiet-input deadline");
        d = decide(true, 126.1e6, 120e6, 50e6, 5, 60, 55, 80);
        require(d.send && near(d.targetHz, 126.1e6), "Final frequency waited for a display frame");
        d=decide(true,121.3e6,120e6,20e6,5,0,1000);
        require(d.send && near(d.targetHz,121e6),"Step used zoom span rather than sample rate");
        d=decide(true,120e6,120e6,50e6,5,100,1000);
        require(!d.send && !d.waitMs,"Returning to confirmed frequency issued redundant command");
        d=decide(true,125e6,120e6,0,5,100,1000);
        require(!d.send,"Invalid sample rate produced a command");
        d=decide(true,std::numeric_limits<double>::quiet_NaN(),120e6,50e6,5,100,1000);
        require(!d.send,"Nonfinite target produced a command");
        d=liveCenterRetuneDecision(false,120e6,120e6,50e6,5,60,1000,160,true);
        require(d.send && near(d.targetHz,120e6),"Calibration offset change lost a same-center retune");
        int agileCalls=0,standardCalls=0;
        double sent=0,actual=0;
        const auto agile=[&](double hz){++agileCalls;sent=hz;return 0;};
        const auto standard=[&](double hz,double *achieved){
            ++standardCalls;sent=hz;*achieved=145e6;return 0;
        };
        int rc=runLiveFobosRetune(false,145000100,250,actual,agile,standard);
        require(rc==0 && standardCalls==1 && agileCalls==0,"Standard retune routed to Agile");
        require(near(sent,145000350),"Standard retune omitted calibration offset");
        require(near(actual,144999750),"Standard achieved frequency or inverse calibration lost");
        rc=runLiveFobosRetune(true,145000100,-250,actual,agile,standard);
        require(rc==0 && standardCalls==1 && agileCalls==1,"Agile retune routed to Standard");
        require(near(sent,144999850) && near(actual,145000100),"Agile calibration changed logical frequency");
        rc=runLiveFobosRetune(false,145000100,250,actual,agile,
            [](double,double *achieved){*achieved=1;return -7;});
        require(rc==-7 && near(actual,145000100),"Failed Standard retune published an unconfirmed frequency");
        burst(false,6);burst(true,6);burst(true,600);
        std::cout<<"PASS: "<<checks<<" retune policy checks, including wheel/drag bursts and slow USB\n";
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
