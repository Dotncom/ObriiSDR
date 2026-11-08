#pragma once
#include "regional.h"
#include <utility>

namespace obrii::lorawan {
enum class DeviceClass : std::uint8_t { A=0, B=1, C=2 };
struct Beacon {
    std::uint32_t gpsSeconds=0;
    std::array<std::uint8_t,7> gateway{};
    bool gatewayCrcValid=true;
};
Bytes encodeBeacon(const Beacon &beacon);
// The common CRC is mandatory; corrupt gateway-specific bytes do not prevent
// stationary-device time acquisition. Beacons have no cryptographic MIC.
Beacon decodeBeacon(const Bytes &wire);
std::uint16_t pingOffset(std::uint32_t beaconTime,std::uint32_t devAddr,unsigned periodicity);
struct ClassBConfig {
    std::uint32_t beaconFrequencyHz=869525000,pingFrequencyHz=869525000;
    std::uint8_t pingDataRate=3,periodicity=7;
    std::uint16_t clockErrorPpm=20;
    std::uint32_t toleranceUs=10000;
    void validate() const;
};
// Host monotonic time describes the beginning of the beacon preamble. Never
// restore this anchor across restart: reacquire a real/modelled beacon first.
class ClassBTracker {
public:
    explicit ClassBTracker(ClassBConfig config={});
    const ClassBConfig &config() const { return config_; }
    void configure(ClassBConfig config); // Changing channels/periodicity loses lock.
    void acquire(const Bytes &beacon,std::uint64_t preambleStartUs);
    bool locked(std::uint64_t nowUs) const;
    std::optional<RxWindow> nextPing(std::uint32_t devAddr,std::uint64_t nowUs) const;
    bool acceptsPing(std::uint32_t devAddr,std::uint64_t startUs,std::uint32_t frequency,unsigned rate) const;
    // Blocks beacon reserved + guard intervals, including a complete Class A
    // transaction that would otherwise extend across an upcoming beacon.
    bool allowsTransaction(std::uint64_t beginUs,std::uint64_t endUs) const;
    std::uint64_t nextTransaction(std::uint64_t beginUs,std::uint64_t durationUs=0) const;
    std::optional<std::uint32_t> lastBeaconTime() const { return time_; }
    void reset();
private:
    ClassBConfig config_;
    std::optional<std::uint32_t> time_;
    std::uint64_t anchor_=0;
    RxWindow slot(std::uint32_t addr,std::uint64_t period,unsigned n) const;
};
// Class C RXC uses RX2 parameters outside TX and the higher-priority RX1
// interval. Caller also excludes ongoing packet reception/retuning as needed.
bool acceptsClassC(const Eu868 &,std::uint64_t startUs,std::uint32_t frequency,unsigned rate,
    const std::optional<std::array<RxWindow,2>> &classAWindows,
    std::optional<std::pair<std::uint64_t,std::uint64_t>> transmission={});
}
