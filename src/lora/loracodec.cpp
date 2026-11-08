// Coding conventions adapted from LoRaPHY (MIT), jkadbear, 2020-2022.
// See LICENSE.LoRaPHY. Receiver error reporting and C++ API are Obrii additions.
#include "loraphy.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace obrii::lora {
namespace {
int parity(unsigned value) {
    int result = 0;
    while (value) { result ^= value & 1; value >>= 1; }
    return result;
}
int distance(unsigned a, unsigned b) {
    unsigned x = a ^ b; int result = 0;
    while (x) { result += x & 1; x >>= 1; }
    return result;
}
unsigned codeword(unsigned nibble, int cr) {
    if (cr == 1) return nibble | (parity(nibble) << 4);
    const unsigned full = nibble | (parity(nibble & 7) << 4)
        | (parity(nibble & 14) << 5) | (parity(nibble & 11) << 6)
        | (parity(nibble & 13) << 7);
    return full & ((1u << (cr + 4)) - 1);
}
std::uint8_t decodeWord(unsigned word, int cr, int &corrected) {
    int best = 9, candidate = -1; bool tie = false;
    for (unsigned n = 0; n < 16; ++n) {
        const int d = distance(word, codeword(n, cr));
        if (d < best) { best = d; candidate = int(n); tie = false; }
        else if (d == best) tie = true;
    }
    // CR 4/5 and 4/6 detect errors, but cannot unambiguously correct one bit.
    if (tie || best > (cr >= 3 ? 1 : 0)) throw std::runtime_error("LoRa FEC error");
    corrected += best != 0;
    return std::uint8_t(candidate);
}
std::array<std::uint8_t, 5> headerNibbles(const Header &h) {
    std::array<std::uint8_t, 5> n{std::uint8_t(h.payloadBytes >> 4),
        std::uint8_t(h.payloadBytes & 15), std::uint8_t(2 * h.codingRate + h.payloadCrc), 0, 0};
    // Twelve header information bits, MSB first; five checksum equations.
    constexpr unsigned masks[] = {0xf00, 0x8e1, 0x49a, 0x257, 0x12f};
    const unsigned bits = (n[0] << 8) | (n[1] << 4) | n[2];
    n[3] = std::uint8_t(parity(bits & masks[0]));
    for (int i = 1; i < 5; ++i) n[4] |= std::uint8_t(parity(bits & masks[i]) << (4-i));
    return n;
}
Bytes whiten(Bytes data) {
    unsigned reg = 255;
    for (auto &byte : data) {
        byte ^= std::uint8_t(reg);
        reg = ((reg << 1) | parity(reg & 0xb8)) & 255;
    }
    return data;
}
int symbolsFor(const Config &c, const Header &h) {
    const int ppm = c.spreadingFactor - 2 * c.lowDataRateOptimization();
    const int numerator = 2 * h.payloadBytes - c.spreadingFactor + 7
        + 4 * h.payloadCrc - 5 * !c.explicitHeader;
    return 8 + std::max(0, (numerator + ppm - 1) / ppm) * (h.codingRate + 4);
}
unsigned ungray(unsigned value) {
    unsigned binary = value;
    while (value >>= 1) binary ^= value;
    return binary;
}
void interleave(const Bytes &words, std::size_t start, int ppm, int cr,
                bool reduced, int bins, Symbols &out) {
    for (int col = 0; col < cr + 4; ++col) {
        unsigned s = 0;
        for (int row = 0; row < ppm; ++row)
            s |= ((words[start + (row + col) % ppm] >> col) & 1u) << row;
        out.push_back(std::uint16_t((ungray(s) * (reduced ? 4 : 1) + 1) % bins));
    }
}
Bytes deinterleave(const Symbols &symbols, std::size_t start, int ppm, int cr,
                   bool reduced, int bins, int &corrected) {
    Bytes words(ppm, 0);
    for (int col = 0; col < cr + 4; ++col) {
        unsigned s = symbols[start + col];
        s = reduced ? s / 4 : (s + bins - 1) % bins;
        s ^= s >> 1;
        for (int row = 0; row < ppm; ++row)
            words[(row + col) % ppm] |= ((s >> row) & 1u) << col;
    }
    for (auto &word : words) word = decodeWord(word, cr, corrected);
    return words;
}
Header parseHeader(const Config &c, const Bytes &nibbles) {
    if (!c.explicitHeader) return {c.implicitPayloadBytes, c.codingRate, c.payloadCrc};
    const Header h{nibbles[0] * 16 + nibbles[1], nibbles[2] >> 1, bool(nibbles[2] & 1)};
    if (h.codingRate < 1 || h.codingRate > 4) throw std::runtime_error("Invalid LoRa header coding rate");
    const auto expected = headerNibbles(h);
    if (!std::equal(expected.begin(), expected.end(), nibbles.begin()))
        throw std::runtime_error("LoRa header checksum error");
    return h;
}
void checkSymbols(const Config &c, const Symbols &symbols) {
    if (symbols.size() < 8 || symbols.size() > 1024) throw std::runtime_error("Invalid LoRa symbol count");
    if (std::any_of(symbols.begin(), symbols.end(), [&](auto s){ return s >= c.bins(); }))
        throw std::runtime_error("LoRa symbol out of range");
}
}
void Config::validate() const {
    if (spreadingFactor < 7 || spreadingFactor > 12) throw std::invalid_argument("LoRa SF must be 7..12");
    if (bandwidthHz != 31250 && bandwidthHz != 62500 && bandwidthHz != 125000
        && bandwidthHz != 250000 && bandwidthHz != 500000)
        throw std::invalid_argument("Unsupported LoRa bandwidth");
    if (codingRate < 1 || codingRate > 4) throw std::invalid_argument("LoRa coding rate must be 1..4");
    if (oversampling != 1 && oversampling != 2 && oversampling != 4 && oversampling != 8)
        throw std::invalid_argument("LoRa oversampling must be 1,2,4,8");
    if (preambleSymbols < 6 || preambleSymbols > 1024) throw std::invalid_argument("LoRa preamble must be 6..1024 symbols");
    if (implicitPayloadBytes < 0 || implicitPayloadBytes > 255) throw std::invalid_argument("LoRa implicit length must be 0..255");
    if (ldro != Ldro::Auto && ldro != Ldro::Off && ldro != Ldro::On) throw std::invalid_argument("Invalid LDRO mode");
}
bool Config::lowDataRateOptimization() const {
    validate();
    return ldro == Ldro::On || (ldro == Ldro::Auto && double(1 << spreadingFactor) / bandwidthHz > 0.016);
}
int Config::sampleRate() const { validate(); return bandwidthHz * oversampling; }
int Config::bins() const { validate(); return 1 << spreadingFactor; }
std::size_t Config::samplesPerSymbol() const { return std::size_t(bins()) * oversampling; }
PacketCodec::PacketCodec(Config config) : config_(std::move(config)) { config_.validate(); }
int PacketCodec::symbolCount(int length) const {
    if (length < 0 || length > 255) throw std::invalid_argument("LoRa payload must be 0..255 bytes");
    return symbolsFor(config_, {length, config_.codingRate, config_.payloadCrc});
}
double PacketCodec::timeOnAirSeconds(int length) const {
    return (config_.preambleSymbols + 4.25 + symbolCount(length)) * config_.bins() / config_.bandwidthHz;
}
std::uint16_t PacketCodec::payloadChecksum(const Bytes &data) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i + 2 < data.size(); ++i) {
        crc ^= std::uint16_t(data[i]) << 8;
        for (int b = 0; b < 8; ++b) crc = std::uint16_t((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    if (!data.empty()) crc ^= data.back();
    if (data.size() > 1) crc ^= std::uint16_t(data[data.size()-2]) << 8;
    return crc;
}
Symbols PacketCodec::encode(const Bytes &payload) const {
    if (payload.size() > 255) throw std::invalid_argument("LoRa payload exceeds 255 bytes");
    if (!config_.explicitHeader && payload.size() != std::size_t(config_.implicitPayloadBytes))
        throw std::invalid_argument("LoRa implicit payload length mismatch");
    const int count = symbolCount(int(payload.size()));
    const int ppm = config_.spreadingFactor - 2 * config_.lowDataRateOptimization();
    const int capacity = config_.spreadingFactor - 2 + (count-8)/(config_.codingRate+4)*ppm;
    Bytes nibbles;
    if (config_.explicitHeader) {
        const auto header = headerNibbles({int(payload.size()), config_.codingRate, config_.payloadCrc});
        nibbles.assign(header.begin(), header.end());
    }
    Bytes bytes = whiten(payload);
    if (config_.payloadCrc) {
        const auto crc = payloadChecksum(payload);
        bytes.push_back(std::uint8_t(crc)); bytes.push_back(std::uint8_t(crc >> 8));
    }
    for (auto b : bytes) { nibbles.push_back(b & 15); nibbles.push_back(b >> 4); }
    if (nibbles.size() > std::size_t(capacity)) throw std::logic_error("LoRa coding capacity mismatch");
    nibbles.resize(capacity, 15);
    for (std::size_t i = 0; i < nibbles.size(); ++i)
        nibbles[i] = std::uint8_t(codeword(nibbles[i], i < std::size_t(config_.spreadingFactor-2) ? 4 : config_.codingRate));
    Symbols symbols; symbols.reserve(count);
    interleave(nibbles, 0, config_.spreadingFactor-2, 4, true, config_.bins(), symbols);
    for (int i = config_.spreadingFactor-2; i < capacity; i += ppm)
        interleave(nibbles, i, ppm, config_.codingRate, config_.lowDataRateOptimization(), config_.bins(), symbols);
    return symbols;
}
Header PacketCodec::inspectHeader(const Symbols &symbols) const {
    checkSymbols(config_, symbols);
    int corrected = 0;
    return parseHeader(config_, deinterleave(symbols, 0, config_.spreadingFactor-2, 4, true, config_.bins(), corrected));
}
Packet PacketCodec::decode(const Symbols &symbols) const {
    checkSymbols(config_, symbols);
    Packet packet;
    Bytes nibbles = deinterleave(symbols, 0, config_.spreadingFactor-2, 4, true, config_.bins(), packet.correctedCodewords);
    packet.header = parseHeader(config_, nibbles);
    const int expected = symbolsFor(config_, packet.header);
    if (symbols.size() != std::size_t(expected)) throw std::runtime_error("LoRa packet symbol count mismatch");
    if (config_.explicitHeader) nibbles.erase(nibbles.begin(), nibbles.begin()+5);
    const int ppm = config_.spreadingFactor - 2 * config_.lowDataRateOptimization();
    for (int i = 8; i < expected; i += packet.header.codingRate+4) {
        const auto block = deinterleave(symbols, i, ppm, packet.header.codingRate,
            config_.lowDataRateOptimization(), config_.bins(), packet.correctedCodewords);
        nibbles.insert(nibbles.end(), block.begin(), block.end());
    }
    Bytes bytes;
    for (std::size_t i = 0; i + 1 < nibbles.size(); i += 2) bytes.push_back(nibbles[i] | (nibbles[i+1] << 4));
    const auto length = std::size_t(packet.header.payloadBytes);
    if (bytes.size() < length + 2 * packet.header.payloadCrc) throw std::runtime_error("Truncated LoRa payload");
    packet.payload = whiten(Bytes(bytes.begin(), bytes.begin()+length));
    if (packet.header.payloadCrc) {
        const auto received = bytes[length] | (bytes[length+1] << 8);
        if (payloadChecksum(packet.payload) != received) throw std::runtime_error("LoRa payload CRC error");
        packet.crcChecked = true;
    }
    return packet;
}
}
