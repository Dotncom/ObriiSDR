#pragma once
#include "lorawan.h"

namespace obrii::lorawan {
struct Channel {
    std::uint32_t frequencyHz=0, downlinkHz=0;
    std::uint8_t minRate=0, maxRate=5;
    bool enabled=false;
};
struct MacCommand { std::uint8_t cid=0; Bytes data; };
struct MacCommands { std::vector<MacCommand> commands; Bytes unparsed; };
MacCommands parseMac(Direction direction, const Bytes &bytes);
struct MacResult {
    Bytes answers;
    Bytes unparsed;
    bool answersTruncated=false;
    std::optional<std::array<std::uint8_t,2>> linkCheck; // Margin, gateway count.
    std::optional<std::array<std::uint8_t,5>> deviceTime; // GPS seconds LE + 1/256 second.
};
struct Eu868 {
    std::array<Channel,16> channels{};
    std::uint8_t dataRate=0, txPower=0, nbTrans=1, dutyExponent=0;
    std::uint8_t rx1Offset=0, rx2DataRate=0, receiveDelay=1;
    std::uint32_t rx2FrequencyHz=869525000;
    bool repeaterCompatible=false;
    bool adrEnabled=true;
    std::uint32_t adrAckCount=0;
    Bytes stickyAnswers;
    Eu868();
    void validate() const;
    void applyJoin(const JoinAccept &accept);
    // Call only after MIC/counter checks, for every accepted Class A downlink,
    // including one with no MAC commands. Returned answers precede app traffic.
    MacResult processDownlink(const DataFrame &frame, std::uint8_t battery=255, int snrDb=0);
    std::uint8_t onNewUplink(); // FCtrl ADR/ADRACKReq; call once per new FCnt, not per repeat.
    int channelIndex(std::uint32_t frequency, unsigned rate) const;
    static bool validFrequency(std::uint32_t frequency);
    static std::size_t maxMacPayload(unsigned rate, bool repeaterCompatible=false);
    static int spreadingFactor(unsigned rate);
    static int bandwidth(unsigned rate);
    static int txEirpDbm(unsigned powerIndex);
    static void checkPayload(const Bytes &wire, unsigned rate, bool repeaterCompatible=false);
};
struct RxWindow {
    std::uint64_t nominalUs=0, opensUs=0, closesUs=0;
    std::uint32_t frequencyHz=0;
    std::uint8_t dataRate=0, index=0;
    bool containsStart(std::uint64_t packetStartUs) const { return packetStartUs>=opensUs && packetStartUs<=closesUs; }
};
std::array<RxWindow,2> receiveWindows(const Eu868 &settings, std::uint64_t txEndUs,
    std::uint32_t uplinkFrequencyHz, unsigned uplinkRate, bool join=false,
    std::uint32_t clockToleranceUs=10000);
std::uint64_t nextUplinkTime(const std::array<RxWindow,2> &windows, bool confirmed,
                            std::uint32_t randomRetryDelayUs=2000000);
struct DutyState { std::array<std::uint64_t,6> bands{}; std::uint64_t aggregate=0; };
std::uint64_t dutyReady(const DutyState &state, std::uint32_t frequencyHz);
// Returns a reservation to persist before RF. Conservatively consume even if TX fails.
DutyState reserveDuty(const DutyState &state, std::uint32_t frequencyHz,
                      std::uint64_t txBeginUs, std::uint64_t airtimeUs, unsigned dutyExponent=0);
DutyState reserveBackoff(const DutyState &state, std::uint32_t frequencyHz,
                        std::uint64_t txBeginUs, std::uint64_t airtimeUs, std::uint64_t uptimeUs,
                        unsigned dutyExponent=0);
Bytes saveRegion(const Eu868 &settings);
Eu868 loadRegion(const Bytes &blob);
Bytes saveDuty(const DutyState &state);
DutyState loadDuty(const Bytes &blob);
}
