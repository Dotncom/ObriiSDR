#include "dspboardqpskreceiver.h"
#include "dspgraphmodel.h"
#include "dspiqsourceregistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {
using Sample = std::complex<float>;
constexpr float Tau = 6.2831853071795864769f;
constexpr int Sps = 4;
constexpr int TrainingSymbols = 64;
constexpr int ProbeSymbols = 24;
constexpr int HalfTaps = 4 * Sps;
constexpr float MaxCarrier = Tau * 0.40f;
constexpr std::size_t InputBatch = 4096;
constexpr std::array<Sample, 4> Points{{{1, 1}, {-1, 1}, {-1, -1}, {1, -1}}};

const auto &training() {
    static const auto result = [] {
        std::array<Sample, TrainingSymbols> symbols{};
        const auto &bytes = DspBoardQpsk::preamble();
        Q_ASSERT(bytes.size() * DspBoardQpsk::SymbolsPerByte == TrainingSymbols);
        for (int i = 0; i < TrainingSymbols; ++i)
            symbols[i] = Points[(quint8(bytes.at(i / 4)) >> (6 - 2 * (i % 4))) & 3U];
        return symbols;
    }();
    return result;
}

const auto &matchedTaps() {
    static const auto taps = [] {
        std::array<float, 2 * HalfTaps + 1> result{};
        for (int i = -HalfTaps; i <= HalfTaps; ++i)
            result[i + HalfTaps] = float(SignalLabGenerator::rootRaisedCosine(double(i) / Sps, 0.35) / Sps);
        return result;
    }();
    return taps;
}
}

struct DspBoardQpskReceiver::State {
    double sampleRate = 0, symbolRate = 0;
    qint64 inputIndex = 0;
    double nextResample = 0;
    Sample previousInput{};
    std::deque<Sample> samples;
    qint64 base = 0, scan = 0;

    bool locked = false;
    double nextSymbol = 0, timingRate = 0;
    float carrier = 0, phase = 0, power = 0;
    Sample previousSymbol{};
    bool havePreviousSymbol = false;
    int symbolsInByte = 0;
    unsigned byte = 0;
    QByteArray frame;
    int frameSize = DspBoardQpsk::HeaderBytes;

    qint64 end() const { return base + qint64(samples.size()); }
    Sample at(qint64 index) const { return samples[std::size_t(index - base)]; }
    Sample interpolate(double position) const {
        const auto index = qint64(position);
        return at(index) + float(position - index) * (at(index + 1) - at(index));
    }
    void discardBefore(qint64 position) {
        while (base < position && !samples.empty()) {
            samples.pop_front();
            ++base;
        }
    }

    struct Candidate { float score = 0, carrier = 0; };
    template <typename Read>
    Candidate estimate(Read read, int count) const {
        const auto &known = training();
        Sample previous = read(0) * std::conj(known[0]);
        Sample sum{};
        float left = 0, right = 0;
        for (int k = 1; k < count; ++k) {
            const Sample value = read(k) * std::conj(known[k]);
            sum += value * std::conj(previous);
            left += std::norm(previous);
            right += std::norm(value);
            previous = value;
        }
        const float scale = std::sqrt(left * right);
        return scale > 1e-12f ? Candidate{std::abs(sum) / scale, std::arg(sum)} : Candidate{};
    }

    bool acquire() {
        while (scan + TrainingSymbols * Sps + 3 < end()) {
            const auto start = scan++;
            const auto coarse = estimate([&](int k) { return at(start + k * Sps); }, ProbeSymbols);
            if (coarse.score < 0.70f || std::abs(coarse.carrier) > MaxCarrier + 0.18f) continue;
            double best = double(start);
            Candidate fine;
            // Refine the fractional sample phase around the best nearby sample.
            for (int step = -8; step <= 8; ++step) {
                const double candidate = double(start) + step * 0.25;
                if (candidate < base) continue;
                const auto match = estimate([&](int k) { return interpolate(candidate + k * Sps); }, TrainingSymbols);
                if (match.score > fine.score) { best = candidate; fine = match; }
            }
            if (fine.score < 0.76f) continue;

            std::array<Sample, TrainingSymbols> wiped{};
            float energy = 0;
            for (int k = 0; k < TrainingSymbols; ++k) {
                wiped[k] = interpolate(best + k * Sps) * std::conj(training()[k]);
                energy += std::norm(wiped[k]);
            }
            float frequency = fine.carrier;
            for (const int lag : {8, 24}) {
                Sample sum{};
                for (int k = lag; k < TrainingSymbols; ++k) sum += wiped[k] * std::conj(wiped[k - lag]);
                frequency += std::remainder(std::arg(sum) - frequency * lag, Tau) / lag;
            }
            if (std::abs(frequency) > MaxCarrier) continue;
            Sample coherent{};
            for (int k = 0; k < TrainingSymbols; ++k)
                coherent += wiped[k] * std::polar(1.0f, -frequency * k);
            if (std::norm(coherent) < 0.55f * TrainingSymbols * energy) continue;

            locked = true;
            nextSymbol = best + TrainingSymbols * Sps;
            carrier = frequency;
            phase = std::remainder(std::arg(coherent) + carrier * TrainingSymbols, Tau);
            power = std::max(energy / (2 * TrainingSymbols), 1e-12f);
            timingRate = 0;
            havePreviousSymbol = false;
            frame.clear();
            frameSize = DspBoardQpsk::HeaderBytes;
            byte = 0;
            symbolsInByte = 0;
            return true;
        }
        discardBefore(std::max(base, scan - 2));
        return false;
    }

    Sample matched(double time, float angle) const {
        const auto &taps = matchedTaps();
        const Sample rotation = std::polar(1.0f, -carrier / Sps);
        Sample oscillator = std::polar(1.0f, -angle + carrier * HalfTaps / Sps);
        Sample value{};
        const auto index = qint64(time);
        const float frac = float(time - index);
        for (int tap = -HalfTaps; tap <= HalfTaps; ++tap) {
            const Sample a = at(index + tap);
            value += (a + frac * (at(index + tap + 1) - a)) * oscillator * taps[tap + HalfTaps];
            oscillator *= rotation;
        }
        return value;
    }

    void searchAgain() {
        locked = false;
        scan = std::max(base, qint64(nextSymbol) - Sps);
    }

    void demodulate(QVector<DspBoardDecodedPayload> &packets) {
        while (locked && qint64(nextSymbol) + HalfTaps + 1 < end()) {
            const Sample value = matched(nextSymbol, phase);
            const unsigned bits = DspBoardQpsk::symbolBits(value);
            const float phaseError = std::clamp(std::arg(value * std::conj(Points[bits])), -0.7f, 0.7f);

            double timingError = 0;
            power = std::max(0.98f * power + 0.02f * std::norm(value), 1e-12f);
            if (havePreviousSymbol) {
                // Gardner timing error from adjacent symbols and their midpoint.
                const Sample mid = matched(nextSymbol - Sps * 0.5, phase - 0.5f * carrier);
                timingError = std::clamp(double(std::real((previousSymbol - value) * std::conj(mid)) / power), -1.0, 1.0);
                timingRate = std::clamp(timingRate + 0.0006 * timingError, -0.025, 0.025);
            }
            // A decision-directed carrier loop follows the residual after acquisition.
            carrier = std::clamp(carrier + 0.0015f * phaseError, -MaxCarrier, MaxCarrier);
            phase = std::remainder(phase + carrier + 0.09f * phaseError, Tau);
            previousSymbol = value;
            havePreviousSymbol = true;
            nextSymbol += Sps + timingRate + 0.045 * timingError;
            discardBefore(qint64(nextSymbol) - HalfTaps - Sps);

            byte = (byte << 2) | bits;
            if (++symbolsInByte != DspBoardQpsk::SymbolsPerByte) continue;
            frame.append(char(byte));
            byte = 0;
            symbolsInByte = 0;
            if (frame.size() == DspBoardQpsk::HeaderBytes) {
                const int body = DspBoardQpsk::bodyBytes(frame);
                if (body == 0) searchAgain();
                else frameSize += body;
            } else if (frame.size() == frameSize) {
                auto packet = DspBoardQpsk::decodeFrame(frame.left(DspBoardQpsk::HeaderBytes),
                                                        frame.mid(DspBoardQpsk::HeaderBytes));
                if (packet.ok()) packets.append(std::move(packet));
                searchAgain();
            }
        }
    }

    void append(const float *iq, std::size_t count) {
        const double spacing = sampleRate / (Sps * symbolRate);
        for (std::size_t i = 0; i < count; ++i, ++inputIndex) {
            const float re = iq[2 * i], im = iq[2 * i + 1];
            const Sample value = std::isfinite(re) && std::isfinite(im)
                ? Sample(std::clamp(re, -16.0f, 16.0f), std::clamp(im, -16.0f, 16.0f)) : Sample{};
            if (inputIndex == 0) previousInput = value;
            while (nextResample <= double(inputIndex)) {
                samples.push_back(previousInput + float(nextResample - double(inputIndex - 1)) * (value - previousInput));
                nextResample += spacing;
            }
            previousInput = value;
        }
    }
};

DspBoardQpskReceiver::DspBoardQpskReceiver() : state_(std::make_unique<State>()) {}
DspBoardQpskReceiver::~DspBoardQpskReceiver() = default;
void DspBoardQpskReceiver::reset() { *state_ = State{}; }

bool DspBoardQpskReceiver::supports(double sampleRate, double symbolRate) {
    return symbolRate >= 100 && symbolRate <= 96000 && sampleRate <= 384000 &&
           sampleRate >= 2.5 * symbolRate;
}

QVector<DspBoardDecodedPayload> DspBoardQpskReceiver::feed(const float *iq, std::size_t complexCount,
                                                         double sampleRate, double symbolRate) {
    QVector<DspBoardDecodedPayload> packets;
    if (!iq || !supports(sampleRate, symbolRate)) { reset(); return packets; }
    auto &s = *state_;
    if (s.sampleRate != sampleRate || s.symbolRate != symbolRate) {
        reset();
        s.sampleRate = sampleRate;
        s.symbolRate = symbolRate;
    }
    for (std::size_t start = 0; start < complexCount; start += InputBatch) {
        s.append(iq + start * 2, std::min(InputBatch, complexCount - start));
        while (s.locked || s.acquire()) {
            s.demodulate(packets);
            if (s.locked) break;
        }
    }
    return packets;
}

struct DspBoardPacketWorker::State {
    std::mutex mutex;
    std::condition_variable wake;
    QVector<DspBoardPacketRoute> routes;
    QHash<QString, DspBoardDecodedPayload> ready;
    quint64 revision = 0;
    bool stopping = false;
    std::thread thread;

    struct Cursor {
        explicit Cursor(const DspBoardPacketRoute &configuration)
            : route(configuration), source(dspPacketBranchSource(route.branchId)) {}
        DspBoardPacketRoute route;
        DspSourceIdentity source;
        DspBoardQpskReceiver receiver;
        std::uint64_t epoch = 0, end = 0;
        bool rateError = false;
    };

    void run() {
        quint64 current = 0;
        std::vector<std::unique_ptr<Cursor>> cursors;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                const auto changed = [&] { return stopping || current != revision; };
                if (cursors.empty()) wake.wait(lock, changed);
                else wake.wait_for(lock, std::chrono::milliseconds(6), changed);
                if (stopping) return;
                if (current != revision) {
                    current = revision;
                    cursors.clear();
                    for (const auto &route : routes) cursors.push_back(std::make_unique<Cursor>(route));
                }
            }
            for (auto &cursor : cursors) {
                DspIqSourceSnapshot snapshot;
                if (!DspIqSourceRegistry::instance().snapshotSince(
                        cursor->source, cursor->epoch, cursor->end, 16384, snapshot)) continue;
                if (snapshot.gap || snapshot.epoch != cursor->epoch) cursor->receiver.reset();
                cursor->epoch = snapshot.epoch;
                cursor->end = snapshot.endFloatCount;
                const double symbols = cursor->route.symbolRate;
                DspBoardDecodedPayload packet;
                if (!DspBoardQpskReceiver::supports(snapshot.sampleRateHz, symbols)) {
                    if (cursor->rateError) continue;
                    cursor->receiver.reset();
                    cursor->rateError = true;
                    packet.error = QStringLiteral("DDC rate %1 S/s must be at least 2.5 × symbol rate and at most 384 kS/s")
                                       .arg(snapshot.sampleRateHz, 0, 'f', 0);
                } else {
                    auto packets = cursor->receiver.feed(snapshot.iq.data(), snapshot.iq.size() / 2,
                                                         snapshot.sampleRateHz, symbols);
                    if (packets.isEmpty()) {
                        if (!cursor->rateError) continue;
                        packet.error = QStringLiteral("Searching for QPSK preamble");
                    } else packet = std::move(packets.last());
                    cursor->rateError = false;
                }
                std::lock_guard<std::mutex> lock(mutex);
                if (revision == current) ready.insert(cursor->route.decoderId, std::move(packet));
            }
        }
    }
};

DspBoardPacketWorker::DspBoardPacketWorker() : state_(std::make_unique<State>()) {}
DspBoardPacketWorker::~DspBoardPacketWorker() {
    auto &s = *state_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stopping = true;
    }
    s.wake.notify_one();
    if (s.thread.joinable()) s.thread.join();
    DspIqSourceRegistry::instance().removeSubscriptions(&s);
}

void DspBoardPacketWorker::configure(const QVector<DspBoardPacketRoute> &routes) {
    auto &s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.routes == routes) return;
    QSet<QString> sources;
    for (const auto &route : routes) sources.insert(dspPacketBranchSource(route.branchId).key());
    DspIqSourceRegistry::instance().setSubscriptions(&s, sources);
    s.routes = routes;
    s.ready.clear();
    ++s.revision;
    if (!s.thread.joinable()) s.thread = std::thread([state = &s]() { state->run(); });
    s.wake.notify_one();
}

QHash<QString, DspBoardDecodedPayload> DspBoardPacketWorker::takePackets() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    QHash<QString, DspBoardDecodedPayload> packets;
    packets.swap(state_->ready);
    return packets;
}
