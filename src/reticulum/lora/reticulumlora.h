#pragma once
#include "loraphy.h"

namespace obrii::reticulum {
enum class Framing { Obrn, RNode };
enum class RNodeModem { Sx127x, Sx126x };

// Framing must match the sidecar. RNS owns encryption and routing.
struct RadioConfig {
    obrii::lora::Config phy;
    int frameMtu = 255;
    Framing framing = Framing::Obrn;
    RadioConfig();
    // Apply RNode Firmware 1.86 normal-mode sync, CRC, LDRO and preamble rules.
    // Call again after changing SF/BW/CR. SX1280 and SF5/6 are not supported.
    static RadioConfig rnode(obrii::lora::Config phy = {}, RNodeModem modem = RNodeModem::Sx127x);
    void validate() const;
};

struct RadioFrame {
    obrii::lora::Bytes data; // Feed unchanged into the sidecar's rx command.
    std::uint64_t payloadSampleOffset = 0;
    double frequencyOffsetHz = 0;
};

// No GUI, process management, sockets or hardware. One instance per IQ stream.
// Not thread safe. Host owns tuning, channel access, pacing and discontinuities.
class LoRaBearer {
public:
    explicit LoRaBearer(RadioConfig config = {});
    const RadioConfig &config() const { return config_; }
    int sampleRate() const { return config_.phy.sampleRate(); }
    double timeOnAirSeconds(const obrii::lora::Bytes &frame) const;
    obrii::lora::Iq modulate(const obrii::lora::Bytes &frame, float amplitude = 0.7f) const;
    std::vector<RadioFrame> receive(const std::complex<float> *samples, std::size_t count);
    std::vector<RadioFrame> receive(const obrii::lora::Iq &samples) { return receive(samples.data(), samples.size()); }
    void reset(); // Call after retuning, rate changes, IQ gaps or radio offline.
    std::size_t bufferedSamples() const { return receiver_.bufferedSamples(); }
private:
    bool validEnvelope(const obrii::lora::Bytes &frame) const;
    RadioConfig config_;
    obrii::lora::Modem modem_;
    obrii::lora::PacketCodec codec_;
    obrii::lora::Receiver receiver_;
};
}
