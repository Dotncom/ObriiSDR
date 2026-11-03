// Packet coding follows the MIT-licensed LoRaPHY reference; see NOTICE and LICENSE.LoRaPHY.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace obrii::lora {
using Bytes = std::vector<std::uint8_t>;
using Symbols = std::vector<std::uint16_t>;
using Iq = std::vector<std::complex<float>>;

enum class Ldro { Auto, Off, On };
struct Config {
    int spreadingFactor = 11;
    int bandwidthHz = 250000;
    int codingRate = 1; // 1..4 means 4/5..4/8
    int oversampling = 2; // sample rate = bandwidth * oversampling; 1,2,4,8
    int preambleSymbols = 16;
    std::uint8_t syncWord = 0x2b; // Meshtastic; callers can select another network
    bool explicitHeader = true;
    bool payloadCrc = true;
    bool invertIq = false;
    Ldro ldro = Ldro::Auto;
    int implicitPayloadBytes = 0; // required RX length in implicit mode
    void validate() const; // throws invalid_argument
    bool lowDataRateOptimization() const;
    int sampleRate() const;
    int bins() const;
    std::size_t samplesPerSymbol() const;
};

struct Header {
    int payloadBytes = 0;
    int codingRate = 1;
    bool payloadCrc = false;
};
struct Packet {
    Bytes payload;
    Header header;
    bool crcChecked = false; // false means CRC absent, not a CRC failure
    int correctedCodewords = 0;
};

// Byte/symbol codec: no GUI, Qt, hardware, network or Meshtastic dependency.
class PacketCodec {
public:
    explicit PacketCodec(Config config = {});
    const Config &config() const { return config_; }
    int symbolCount(int payloadBytes) const;
    double timeOnAirSeconds(int payloadBytes) const;
    Symbols encode(const Bytes &payload) const;
    Header inspectHeader(const Symbols &firstEight) const;
    Packet decode(const Symbols &symbols) const; // rejects bad header/FEC/CRC/truncation
    static std::uint16_t payloadChecksum(const Bytes &payload);
private:
    Config config_;
};

struct Peak {
    int bin = 0;
    double concentration = 0; // strongest FFT-bin power / total FFT power
};
class Modem {
public:
    explicit Modem(Config config = {});
    const Config &config() const { return config_; }
    Iq chirp(int symbol = 0, bool down = false) const;
    Iq modulateSymbols(const Symbols &symbols, float amplitude = 0.7f) const;
    Iq modulate(const Bytes &payload, float amplitude = 0.7f) const;
    // Aligned symbol API for integration with an external synchronizer.
    Peak demodulateSymbol(const std::complex<float> *samples, std::size_t count,
                          bool down = false, double frequencyOffsetHz = 0) const;
    Packet demodulateAligned(const Iq &burst, double frequencyOffsetHz = 0) const;
private:
    Config config_;
    Iq reference_;
};

struct Reception {
    Packet packet;
    std::uint64_t payloadSampleOffset = 0;
    double frequencyOffsetHz = 0;
};
// Initial streaming synchronizer for single-channel, known SF/BW signals.
// Accepts arbitrary chunk sizes; buffers at most one maximum-size PHY packet plus acquisition.
// reset() MUST be called after IQ discontinuity, sample-rate or channel change.
// Not thread safe. SF/BW scanning, sample-clock drift and collision decoding are not implemented.
class Receiver {
public:
    explicit Receiver(Config config = {});
    std::vector<Reception> push(const std::complex<float> *samples, std::size_t count);
    std::vector<Reception> push(const Iq &samples) { return push(samples.data(), samples.size()); }
    void reset();
    std::size_t bufferedSamples() const { return buffer_.size(); }
private:
    void process(std::vector<Reception> &output);
    void discard(std::size_t count);
    Config config_;
    Modem modem_;
    PacketCodec codec_;
    Iq buffer_;
    std::size_t scan_ = 0;
    std::size_t pending_ = 0;
    std::size_t pendingSymbols_ = 0;
    double pendingCfo_ = 0;
    std::uint64_t baseOffset_ = 0;
    std::size_t limit_ = 0;
};
}
