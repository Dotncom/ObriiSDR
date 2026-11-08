#include "loraphy.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace obrii::lora {
namespace {
constexpr double Pi = 3.14159265358979323846;
void fft(std::vector<std::complex<double>> &a) {
    const auto n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        auto bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const auto step = std::polar(1.0, -2 * Pi / length);
        for (std::size_t i = 0; i < n; i += length) {
            std::complex<double> w = 1;
            for (std::size_t j = 0; j < length/2; ++j) {
                const auto u = a[i+j], v = a[i+j+length/2]*w;
                a[i+j] = u+v; a[i+j+length/2] = u-v; w *= step;
            }
        }
    }
}
}
Modem::Modem(Config config) : config_(std::move(config)) {
    config_.validate();
    reference_ = chirp();
    // Internal reference is always the normal-IQ upchirp.
    if (config_.invertIq) for (auto &sample : reference_) sample = std::conj(sample);
}
Iq Modem::chirp(int symbol, bool down) const {
    const int bins = config_.bins();
    if (symbol < 0 || symbol >= bins) throw std::invalid_argument("LoRa chirp symbol out of range");
    Iq samples(config_.samplesPerSymbol());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double t = double(i) / config_.oversampling;
        // Continuous-phase, bandwidth-limited cyclic chirp. The wrap subtracts BW.
        double phase = 2*Pi*(t*t/(2*bins) + (double(symbol)/bins - 0.5)*t
                            - std::max(0.0, t-(bins-symbol)));
        if (down != config_.invertIq) phase = -phase;
        samples[i] = std::complex<float>(float(std::cos(phase)), float(std::sin(phase)));
    }
    return samples;
}
Iq Modem::modulateSymbols(const Symbols &symbols, float amplitude) const {
    if (!std::isfinite(amplitude) || amplitude <= 0 || amplitude > 1)
        throw std::invalid_argument("LoRa amplitude must be finite and in (0,1]");
    if (symbols.size() < 8 || symbols.size() > 1024) throw std::invalid_argument("Invalid LoRa symbol count");
    for (auto s : symbols) if (s >= config_.bins()) throw std::invalid_argument("LoRa symbol out of range");
    const auto n = config_.samplesPerSymbol();
    const auto length = (config_.preambleSymbols + 4 + symbols.size())*n + n/4;
    if (length > 32u*1024*1024) throw std::length_error("LoRa burst exceeds 32M sample limit");
    Iq out; out.reserve(length);
    auto append = [&](const Iq &s, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) out.push_back(s[i]*amplitude);
    };
    const auto up = chirp(), down = chirp(0, true);
    for (int i = 0; i < config_.preambleSymbols; ++i) append(up, n);
    append(chirp((config_.syncWord >> 4)*8), n);
    append(chirp((config_.syncWord & 15)*8), n);
    append(down, n); append(down, n); append(down, n/4);
    for (auto symbol : symbols) append(chirp(symbol), n);
    return out;
}
Iq Modem::modulate(const Bytes &payload, float amplitude) const {
    return modulateSymbols(PacketCodec(config_).encode(payload), amplitude);
}
Peak Modem::demodulateSymbol(const std::complex<float> *samples, std::size_t count,
                            bool down, double cfo) const {
    const auto n = config_.samplesPerSymbol();
    if (!samples || count < n) throw std::invalid_argument("Truncated LoRa IQ symbol");
    if (!std::isfinite(cfo) || std::abs(cfo) > config_.bandwidthHz/2.0)
        throw std::invalid_argument("Invalid LoRa frequency correction");
    const int bins = config_.bins();
    std::vector<std::complex<double>> spectrum(bins);
    double energy = 0;
    for (int i = 0; i < bins; ++i) {
        const auto index = std::size_t(i)*config_.oversampling;
        auto s = std::complex<double>(samples[index]);
        if (config_.invertIq) s = std::conj(s);
        if (!std::isfinite(s.real()) || !std::isfinite(s.imag())) throw std::invalid_argument("Non-finite LoRa IQ sample");
        // CFO is measured in normal-IQ coordinates (after optional conjugation).
        s *= std::polar(1.0, -2*Pi*cfo*i/config_.bandwidthHz);
        const auto ref = std::complex<double>(reference_[index]);
        spectrum[i] = s*(down ? ref : std::conj(ref));
        energy += std::norm(s);
    }
    fft(spectrum);
    Peak peak; double power = 0;
    for (int i = 0; i < bins; ++i) {
        const double p = std::norm(spectrum[i]);
        if (p > power) { power = p; peak.bin = i; }
    }
    peak.concentration = energy > 0 ? std::min(1.0, power/(bins*energy)) : 0;
    return peak;
}
Packet Modem::demodulateAligned(const Iq &burst, double cfo) const {
    const auto n = config_.samplesPerSymbol();
    const auto downStart = (config_.preambleSymbols+2)*n;
    const auto dataStart = downStart + 2*n + n/4;
    if (burst.size() < dataStart + 8*n) throw std::runtime_error("Truncated LoRa burst");
    auto read = [&](std::size_t offset, bool down = false) {
        return demodulateSymbol(burst.data()+offset, burst.size()-offset, down, cfo);
    };
    const auto preamble = read(downStart-3*n);
    const auto sync1 = read(downStart-2*n), sync2 = read(downStart-n), sfd = read(downStart, true);
    if (preamble.bin != 0 || preamble.concentration < 0.1 || sfd.bin != 0 || sfd.concentration < 0.1
        || sync1.bin != (config_.syncWord >> 4)*8 || sync2.bin != (config_.syncWord & 15)*8)
        throw std::runtime_error("LoRa preamble, sync word or SFD mismatch");
    Symbols symbols;
    for (int i = 0; i < 8; ++i) symbols.push_back(std::uint16_t(read(dataStart+i*n).bin));
    PacketCodec codec(config_);
    const auto header = codec.inspectHeader(symbols);
    auto actual = config_; actual.codingRate = header.codingRate; actual.payloadCrc = header.payloadCrc;
    const int count = PacketCodec(actual).symbolCount(header.payloadBytes);
    if (burst.size() != dataStart + count*n) throw std::runtime_error("LoRa burst length mismatch");
    for (int i = 8; i < count; ++i) symbols.push_back(std::uint16_t(read(dataStart+i*n).bin));
    return codec.decode(symbols);
}
}
