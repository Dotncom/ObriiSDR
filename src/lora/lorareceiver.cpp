#include "loraphy.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace obrii::lora {
namespace {
constexpr double Pi = 3.14159265358979323846;
int circularDistance(int a, int b, int bins) {
    const int d = std::abs(a-b);
    return std::min(d, bins-d);
}
}
Receiver::Receiver(Config config) : config_(std::move(config)), modem_(config_), codec_(config_) {
    auto maximum = config_; maximum.codingRate = 4; maximum.payloadCrc = true;
    limit_ = (config_.preambleSymbols + 17 + PacketCodec(maximum).symbolCount(255))*config_.samplesPerSymbol();
    if (limit_ > 32u*1024*1024) throw std::invalid_argument("LoRa receiver configuration exceeds 32M sample limit");
}
void Receiver::reset() {
    buffer_.clear(); scan_ = pending_ = pendingSymbols_ = 0; baseOffset_ = 0; pendingCfo_ = 0;
}
void Receiver::discard(std::size_t count) {
    buffer_.erase(buffer_.begin(), buffer_.begin()+count);
    baseOffset_ += count;
    scan_ = scan_ > count ? scan_-count : 0;
}
std::vector<Reception> Receiver::push(const std::complex<float> *samples, std::size_t count) {
    if (!samples && count) throw std::invalid_argument("Null LoRa IQ buffer");
    // Validate before mutating state, including samples not selected by decimation.
    for (std::size_t i = 0; i < count; ++i)
        if (!std::isfinite(samples[i].real()) || !std::isfinite(samples[i].imag()))
            throw std::invalid_argument("Non-finite LoRa IQ sample; reset after discontinuity");
    std::vector<Reception> result;
    while (count) {
        const auto take = std::min({count, limit_-buffer_.size(), 32*config_.samplesPerSymbol()});
        if (!take) throw std::logic_error("LoRa receiver buffer limit reached");
        buffer_.insert(buffer_.end(), samples, samples+take);
        samples += take; count -= take;
        process(result);
    }
    return result;
}
void Receiver::process(std::vector<Reception> &out) {
    const auto n = config_.samplesPerSymbol();
    const int bins = config_.bins();
    auto peak = [&](std::size_t at, bool down = false, double cfo = 0) {
        return modem_.demodulateSymbol(buffer_.data()+at, buffer_.size()-at, down, cfo);
    };
    while (true) {
        if (pending_) {
            const auto end = pending_ + pendingSymbols_*n;
            if (buffer_.size() < end) return;
            Symbols symbols; symbols.reserve(pendingSymbols_);
            for (std::size_t i = 0; i < pendingSymbols_; ++i)
                symbols.push_back(std::uint16_t(peak(pending_+i*n, false, pendingCfo_).bin));
            try {
                auto packet = codec_.decode(symbols);
                out.push_back({std::move(packet), baseOffset_+pending_, pendingCfo_});
            } catch (const std::runtime_error &) {
                // Invalid FEC/CRC is never delivered as a successful packet.
            }
            pending_ = pendingSymbols_ = 0;
            discard(end); scan_ = 0;
            continue;
        }
        // Retain acquisition history across arbitrary input chunks.
        if (buffer_.size() < scan_+2*n) break;
        const auto a = peak(scan_), b = peak(scan_+n);
        if (a.concentration < 0.12 || b.concentration < 0.12 || circularDistance(a.bin,b.bin,bins) > 1) {
            scan_ += n/2;
            continue;
        }
        auto coarse = std::int64_t(scan_) - std::int64_t(a.bin)*config_.oversampling;
        while (coarse < 0) coarse += n;
        bool needMore = false;
        // Find the up/down transition; the preamble origin need not coincide with input chunks.
        for (int symbol = 4; symbol <= config_.preambleSymbols+2 && !pending_; ++symbol) {
            const auto center = std::size_t(coarse) + symbol*n;
            if (center + 2*n > buffer_.size()) {
                needMore = true; break;
            }
            double bestScore = 0; std::size_t best = 0; double bestCfo = 0;
            const auto coarseDown = peak(center, true);
            if (coarseDown.concentration < 0.12) continue;
            const int downBin = coarseDown.bin > bins/2 ? coarseDown.bin-bins : coarseDown.bin;
            // Upchirp timing and integer CFO are ambiguous. The opposite sweep
            // separates them: the downchirp sees twice the coarse CFO error.
            const auto aligned = std::int64_t(center) + std::int64_t(std::llround(double(downBin)*config_.oversampling/2));
            if (aligned < std::int64_t(4*n+config_.oversampling)) continue;
            for (int delta = -config_.oversampling; delta <= config_.oversampling; ++delta) {
                const auto d = std::size_t(aligned+delta);
                if (d+2*n > buffer_.size()) { needMore=true; continue; }
                const auto down = peak(d, true);
                if (down.concentration < 0.12) continue;
                // Consecutive preamble chirps estimate fractional CFO, modulo one symbol bin.
                std::complex<double> rotation = 0;
                for (std::size_t j = 0; j < n; j += config_.oversampling)
                    rotation += std::conj(std::complex<double>(buffer_[d-4*n+j])) *
                                std::complex<double>(buffer_[d-3*n+j]);
                double cfo = std::arg(rotation)/(2*Pi)*config_.bandwidthHz/bins;
                if (config_.invertIq) cfo = -cfo;
                const auto raw = peak(d-3*n);
                const int integerBin = raw.bin > bins/2 ? raw.bin-bins : raw.bin;
                if (std::abs(integerBin) >= bins/8) continue;
                cfo += double(integerBin)*config_.bandwidthHz/bins;
                const auto pre1 = peak(d-4*n, false, cfo), pre2 = peak(d-3*n, false, cfo);
                const auto s1 = peak(d-2*n, false, cfo), s2 = peak(d-n, false, cfo);
                const auto correctedDown = peak(d, true, cfo), down2 = peak(d+n, true, cfo);
                if (pre1.bin || pre2.bin || correctedDown.bin || down2.bin
                    || s1.bin != (config_.syncWord >> 4)*8 || s2.bin != (config_.syncWord & 15)*8) continue;
                const double score = pre1.concentration+pre2.concentration+correctedDown.concentration
                    +down2.concentration+s1.concentration+s2.concentration;
                if (score > bestScore) { bestScore=score; best=d; bestCfo=cfo; }
            }
            if (!best || bestScore < 0.9) continue;
            const auto data = best + 2*n + n/4;
            if (data+8*n > buffer_.size()) { needMore=true; break; }
            Symbols first;
            for (int i = 0; i < 8; ++i) first.push_back(std::uint16_t(peak(data+i*n, false, bestCfo).bin));
            try {
                const auto header = codec_.inspectHeader(first);
                auto actual = config_; actual.codingRate=header.codingRate; actual.payloadCrc=header.payloadCrc;
                pending_=data; pendingSymbols_=PacketCodec(actual).symbolCount(header.payloadBytes); pendingCfo_=bestCfo;
            } catch (const std::runtime_error &) { /* Continue searching after a false sync. */ }
        }
        if (pending_) continue;
        if (needMore) break;
        scan_ += n/2;
    }
    // Keep four preceding chirps for preamble/CFO checks. No unbounded history.
    if (scan_ > 4*n) discard(scan_-4*n);
}
}
