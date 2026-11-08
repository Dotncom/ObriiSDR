#include "reticulumlora.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace obrii;
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, "Expected validation error");
}
lora::Bytes envelope(int size) {
    lora::Bytes data(static_cast<std::size_t>(size), 0xa5);
    data[0]='O'; data[1]='B'; data[2]='R'; data[3]='N'; data[4]=1;
    return data;
}
int main() {
    try {
        for (int sf : {7, 11}) {
            reticulum::RadioConfig config;
            config.phy.spreadingFactor = sf;
            reticulum::LoRaBearer tx(config), rx(config);
            for (int size : {23, 64, 255}) {
                const auto frame = envelope(size);
                auto iq = tx.modulate(frame);
                check(std::abs(tx.timeOnAirSeconds(frame) - double(iq.size()) / tx.sampleRate()) < 1e-8, "Airtime matches samples");
                std::vector<reticulum::RadioFrame> frames;
                for (std::size_t i=0; i<iq.size(); i+=997) {
                    auto part=rx.receive(iq.data()+i, std::min<std::size_t>(997,iq.size()-i));
                    frames.insert(frames.end(),part.begin(),part.end());
                }
                check(frames.size()==1 && frames[0].data==frame, "Streaming fragment roundtrip");
                rx.reset();
            }
            auto iq=tx.modulate(envelope(23));
            rx.receive(iq.data(),iq.size()/2); rx.reset();
            check(rx.receive(iq).size()==1, "Discontinuity reset");
            rx.reset();
            auto foreign=config.phy; foreign.syncWord=0x2b;
            check(rx.receive(lora::Modem(foreign).modulate(envelope(23))).empty(), "Foreign sync word rejected");
            rejects([&] { tx.modulate(lora::Bytes(255)); });
            rejects([&] { tx.modulate(envelope(256)); });
            rejects([&] { tx.timeOnAirSeconds(lora::Bytes(22)); });
        }
        rejects([] { auto c=reticulum::RadioConfig{}; c.phy.payloadCrc=false; reticulum::LoRaBearer b(c); });
        rejects([] { auto c=reticulum::RadioConfig{}; c.phy.explicitHeader=false; reticulum::LoRaBearer b(c); });
        rejects([] { auto c=reticulum::RadioConfig{}; c.frameMtu=256; reticulum::LoRaBearer b(c); });
        for (int sf : {7, 11}) {
            lora::Config phy;
            phy.spreadingFactor=sf;
            const auto sx127=reticulum::RadioConfig::rnode(phy,reticulum::RNodeModem::Sx127x);
            const auto sx126=reticulum::RadioConfig::rnode(phy,reticulum::RNodeModem::Sx126x);
            check(sx127.phy.preambleSymbols+4==sx126.phy.preambleSymbols, "RNode chip preamble counts");
            check(sx127.phy.syncWord==0x12 && !sx127.phy.invertIq, "RNode sync and IQ");
            for (auto txConfig : {sx127,sx126}) for (auto rxConfig : {sx127,sx126}) {
                reticulum::LoRaBearer tx(txConfig),rx(rxConfig);
                for (int length : {1,2,255}) {
                    lora::Bytes data(length,0xa5); data[0]=0x51;
                    const auto iq=tx.modulate(data);
                    check(rx.receive(iq).size()==1, "RNode inter-chip preamble acquisition");
                    rx.reset();
                    check(std::abs(tx.timeOnAirSeconds(data)-double(iq.size())/tx.sampleRate())<1e-8, "RNode airtime");
                }
            }
        }
        for(auto sfBw : {std::pair<int,int>{9,31250},{10,62500},{11,125000},{12,250000}}) {
            lora::Config phy; phy.spreadingFactor=sfBw.first; phy.bandwidthHz=sfBw.second;
            const auto config=reticulum::RadioConfig::rnode(phy);
            check(!config.phy.lowDataRateOptimization(), "RNode integer LDRO threshold at 16.384ms");
        }
        auto slow=lora::Config{}; slow.spreadingFactor=12; slow.bandwidthHz=125000;
        check(reticulum::RadioConfig::rnode(slow).phy.lowDataRateOptimization(), "RNode LDRO above threshold");
        rejects([] { auto c=reticulum::RadioConfig::rnode(); c.frameMtu=254; reticulum::LoRaBearer b(c); });
        std::cout << "PASS LoRa bearer SF7/SF11, MTU, airtime, chunked IQ, reset, sync isolation and validation\n";
        std::cout << "PASS RNode normal framing, chip profiles, inter-chip RX, header-only trailer and LDRO thresholds\n";
        return 0;
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
