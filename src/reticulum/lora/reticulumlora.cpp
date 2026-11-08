#include "reticulumlora.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace obrii::reticulum {
RadioConfig::RadioConfig() {
    phy.syncWord = 0x12; // Private LoRa sync word; not a network/authentication key.
}

RadioConfig RadioConfig::rnode(obrii::lora::Config phy, RNodeModem modem) {
    phy.validate();
    if (modem != RNodeModem::Sx127x && modem != RNodeModem::Sx126x)
        throw std::invalid_argument("Unsupported RNode modem profile");
    RadioConfig config;
    config.framing = Framing::RNode;
    config.phy = phy;
    config.phy.syncWord = 0x12; // Also corresponds to SX126x's 0x1424 register value.
    config.phy.explicitHeader = true;
    config.phy.payloadCrc = true;
    config.phy.invertIq = false;
    // RNode's radio drivers use integer milliseconds >16, not LoRa PHY's
    // generic >16ms rule. In particular, a 16.384ms symbol uses LDRO OFF.
    const int symbolMs = (1 << phy.spreadingFactor) / (phy.bandwidthHz / 1000);
    config.phy.ldro = symbolMs > 16 ? obrii::lora::Ldro::On : obrii::lora::Ldro::Off;
    const double symbolSeconds = double(1 << phy.spreadingFactor) / phy.bandwidthHz;
    const double bitrate = phy.spreadingFactor * (4.0 / (4 + phy.codingRate)) / symbolSeconds;
    const double targetSeconds = bitrate > 30000 ? 0.006 : 0.024;
    const int logicalPreamble = std::max(18, static_cast<int>(std::ceil(targetSeconds / symbolSeconds)));
    // The SX127x driver subtracts four before writing its preamble register;
    // the SX126x driver programs the logical count directly.
    config.phy.preambleSymbols = logicalPreamble - (modem == RNodeModem::Sx127x ? 4 : 0);
    config.validate();
    return config;
}

void RadioConfig::validate() const {
    phy.validate();
    if (frameMtu < 64 || frameMtu > 255 || !phy.explicitHeader || !phy.payloadCrc)
        throw std::invalid_argument("Reticulum bearer needs MTU64..255, explicit LoRa header and CRC");
    if (framing != Framing::Obrn && framing != Framing::RNode)
        throw std::invalid_argument("Invalid Reticulum radio framing");
    if (framing == Framing::RNode && (frameMtu != 255 || phy.syncWord != 0x12 || phy.invertIq))
        throw std::invalid_argument("Standard RNode framing needs MTU255, sync0x12 and normal IQ");
}

namespace {
RadioConfig validated(RadioConfig config) { config.validate(); return config; }
}

LoRaBearer::LoRaBearer(RadioConfig config)
    : config_(validated(config)), modem_(config_.phy), codec_(config_.phy), receiver_(config_.phy) {}

bool LoRaBearer::validEnvelope(const obrii::lora::Bytes &frame) const {
    if (config_.framing == Framing::RNode)
        return !frame.empty() && frame.size() <= 255;
    // Full length/index/CRC32 validation belongs to the sidecar reassembler.
    return frame.size() > 22 && frame.size() <= static_cast<std::size_t>(config_.frameMtu)
        && frame[0] == 'O' && frame[1] == 'B' && frame[2] == 'R' && frame[3] == 'N' && frame[4] == 1;
}

double LoRaBearer::timeOnAirSeconds(const obrii::lora::Bytes &frame) const {
    if (!validEnvelope(frame)) throw std::invalid_argument("Invalid Reticulum radio envelope for selected framing");
    return codec_.timeOnAirSeconds(static_cast<int>(frame.size()));
}

obrii::lora::Iq LoRaBearer::modulate(const obrii::lora::Bytes &frame, float amplitude) const {
    if (!validEnvelope(frame)) throw std::invalid_argument("Invalid Reticulum radio envelope for selected framing");
    return modem_.modulate(frame, amplitude);
}

std::vector<RadioFrame> LoRaBearer::receive(const std::complex<float> *samples, std::size_t count) {
    std::vector<RadioFrame> frames;
    for (auto &reception : receiver_.push(samples, count)) {
        if (reception.packet.crcChecked && validEnvelope(reception.packet.payload))
            frames.push_back({std::move(reception.packet.payload), reception.payloadSampleOffset, reception.frequencyOffsetHz});
    }
    return frames;
}

void LoRaBearer::reset() { receiver_.reset(); }
}
