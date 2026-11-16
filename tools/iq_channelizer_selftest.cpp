#include "iqchannelizer.h"
#include "channelizerutils.h"
#include "iqbuffer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>

void iqBufferWithSnapshotLockedForTest(const std::function<void()> &action);
void iqBufferWithPublisherLockedForTest(const std::function<void()> &action);

struct IqChannelizerTestAccess {
    static void configure(IqChannelizer &channel, int factor) { channel.configureFrontDecimator(factor); }
    static bool push(IqChannelizer &channel, std::complex<float> sample, std::complex<float> &output) {
        return channel.pushFrontDecimatorSample(sample, output);
    }
};

namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
class ReferenceFrontDecimator {
public:
    explicit ReferenceFrontDecimator(int factor) : factor_(factor) {
        for (auto &row : delay_) row.resize(factor);
        scale_ = static_cast<float>(1.0 / (double(factor)*factor*factor));
    }
    bool push(std::complex<float> sample, std::complex<float> &output) {
        if (factor_ <= 1) { output = sample; return true; }
        for (int stage = 0; stage < 3; ++stage) {
            const auto previous = delay_[stage][index_];
            delay_[stage][index_] = sample;
            sum_[stage] += sample - previous;
            sample = sum_[stage];
        }
        index_ = (index_ + 1) % factor_;
        if (++count_ < factor_) return false;
        count_ = 0;
        output = sample * scale_;
        return true;
    }
private:
    int factor_, index_ = 0, count_ = 0;
    float scale_ = 1;
    std::array<std::vector<std::complex<float>>, 3> delay_;
    std::array<std::complex<float>, 3> sum_ = {};
};
void testFrontFilter() {
    for (int factor : {1, 2, 3, 4, 7, 8, 16, 32, 64}) {
        for (float amplitude : {1.0f, 1.0e-5f, 1.0e-10f}) {
            IqChannelizer channel;
            IqChannelizerTestAccess::configure(channel, factor);
            ReferenceFrontDecimator reference(factor);
            std::mt19937 random(733);
            std::uniform_real_distribution<float> noise(-0.2f, 0.2f);
            for (int n = 0; n < 32768; ++n) {
                const std::complex<float> input(amplitude*(0.1f+std::cos(n*0.009f)+noise(random)),
                                                 amplitude*(std::sin(n*0.009f)+noise(random)));
                std::complex<float> actual, expected;
                const bool ready = IqChannelizerTestAccess::push(channel, input, actual);
                require(ready == reference.push(input, expected), "Front decimator sample cadence changed");
                if (ready) require(std::abs(actual-expected) <= amplitude*3.0e-6f,
                                   "Front decimator filter/amplitude changed");
            }
        }
    }
}
void testChunking() {
    for (double rate : {2048000.0, 20000000.0, 50000000.0}) {
        RadioSettings settings;
        settings.sampleRate = rate;
        settings.centerFrequency = 100000000;
        settings.listeningFrequency = settings.centerFrequency + rate*0.095;
        settings.modulationType = MOD_NFM;
        settings.bandwidth = 16000;
        const int count = int(rate*0.02);
        std::vector<float> input(std::size_t(count)*2);
        for (int n = 0; n < count; ++n) {
            const double phase = 6.283185307179586*(0.095+1000.0/rate)*n;
            input[2*n] = float(1e-4*std::cos(phase));
            input[2*n+1] = float(1e-4*std::sin(phase));
        }
        IqChannelizer whole, chunked;
        std::vector<float> reference, part, actual;
        const auto result = whole.processFloatIq(input.data(), input.size(), settings, reference, false);
        require(result.valid, "Whole channelizer result invalid");
        for (std::size_t position = 0; position < input.size();) {
            const std::size_t chunk = std::min(std::size_t(7142), input.size()-position);
            const auto partial = chunked.processFloatIq(input.data()+position, chunk, settings, part, false);
            require(partial.valid && partial.outputRate == result.outputRate, "Chunk rate changed");
            actual.insert(actual.end(), part.begin(), part.end());
            position += chunk;
        }
        require(actual.size() == reference.size(), "Channelizer duration depends on chunking");
        double maximumError = 0;
        for (std::size_t n = 0; n < actual.size(); ++n) {
            require(std::isfinite(actual[n]), "Nonfinite channel output");
            maximumError = std::max(maximumError, double(std::abs(actual[n]-reference[n])));
        }
        std::cout << "Chunk comparison rate=" << rate << " maximumError=" << maximumError << '\n';
        require(maximumError < 5e-7, "Chunking changed channelizer waveform");
    }
}
void testNonfiniteInput() {
    const std::array<std::uint32_t, 16> patterns{{0, 0x80000000U, 0x7f800000U, 0xff800000U,
        0x7fc12345U, 0xffc12345U, 0x7f800001U, 0xff800001U, 1, 0x80000001U,
        0x007fffffU, 0x807fffffU, 0x00800000U, 0x80800000U, 0x3f800000U, 0xbf800000U}};
    std::vector<float> input(65536), sanitized, actual, expected;
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = float(0.03 * std::sin(i * 0.009));
    for (std::size_t i = 0; i < patterns.size(); ++i)
        std::memcpy(&input[500 + i * 991], &patterns[i], sizeof(float));
    sanitized = input;
    for (auto &sample : sanitized) if (!std::isfinite(sample)) sample = 0.0f;
    for (double rate : {192000.0, 50000000.0}) {
        for (bool agc : {false, true}) {
            RadioSettings settings;
            settings.sampleRate = rate;
            settings.centerFrequency = 100e6;
            settings.listeningFrequency = settings.centerFrequency + rate * 0.075;
            settings.modulationType = MOD_NFM;
            settings.bandwidth = 12500;
            IqChannelizer tested, reference;
            for (int block = 0; block < 3; ++block) {
                require(tested.processFloatIq(input.data(), input.size(), settings, actual, agc).valid &&
                    reference.processFloatIq(sanitized.data(), sanitized.size(), settings, expected, agc).valid,
                    "Special-float channelizer configuration failed");
                require(!actual.empty() && actual.size() == expected.size() &&
                    std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(float)) == 0,
                    "NaN/Inf sanitation differs from std::isfinite reference");
            }
        }
    }
    std::cout << "Binary32 sanitation: signed NaN/Inf, zero and subnormals bit-exact PASS\n";
}
void testIndependentQueues() {
    IqBuffer::clear();
    IqBuffer::setSampleRateEstimate(50000000);
    std::vector<float> block(512*1024, 1e-4f);
    constexpr int blocks = 40;
    for (int n = 0; n < blocks; ++n) require(IqBuffer::publish(block.data(), block.size()), "IQ publish failed");
    const auto stats = IqBuffer::stats();
    require(stats.droppedQueuedBlocks > 0 && stats.skippedSnapshotBlocks == 0,
            "Audio overflow must not imply visual-snapshot loss");
    require(stats.totalFloatCount == std::uint64_t(blocks)*block.size(), "Audio drop removed FFT samples");
    std::vector<float> snapshot;
    require(IqBuffer::snapshotRecent(snapshot, 64) && snapshot.size() == 64 &&
            std::all_of(snapshot.begin(), snapshot.end(), [](float value) { return value == 1e-4f; }),
            "Independent FFT tail corrupted by audio overflow");
    IqBuffer::clear();
}

// Hold the reader lock deterministically; the deadline also turns an accidental
// blocking publish into a test failure instead of a deadlocked test process.
class HeldBufferLock {
public:
    explicit HeldBufferLock(bool publisher = false)
        : worker_([this, publisher] {
            const auto action = [this] {
                entered_.store(true);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!released_.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!released_.load()) timedOut_.store(true);
            };
            if (publisher) iqBufferWithPublisherLockedForTest(action);
            else iqBufferWithSnapshotLockedForTest(action);
        }) {
        while (!entered_.load()) std::this_thread::yield();
    }
    ~HeldBufferLock() { release(); }
    void release() {
        released_.store(true);
        if (worker_.joinable()) worker_.join();
    }
    bool timedOut() const { return timedOut_.load(); }
private:
    std::atomic_bool entered_{false}, released_{false}, timedOut_{false};
    std::thread worker_;
};

void publishSnapshotBlock(int number, std::uint64_t epoch = 0, std::size_t count = 16) {
    const std::vector<float> block(count, float(number));
    IqBuffer::BlockMetadata metadata;
    metadata.valid = true;
    metadata.scanIndex = number;
    metadata.centerFrequencyHz = 100000000.0 + number;
    require(IqBuffer::publish(block.data(), block.size(), false, true, epoch, &metadata),
            "Snapshot-only publish rejected");
}

void requireSnapshotBlocks(int first, int last, std::size_t blockSize = 16) {
    std::vector<float> snapshot;
    IqBuffer::BlockMetadata metadata;
    require(IqBuffer::snapshot(snapshot, nullptr, &metadata), "Snapshot missing");
    require(snapshot.size() == std::size_t(last-first+1)*blockSize, "Snapshot span contains a gap or stale data");
    for (int number = first; number <= last; ++number) {
        const auto offset = std::size_t(number-first)*blockSize;
        for (std::size_t n = 0; n < blockSize; ++n)
            require(snapshot[offset+n] == float(number), "Deferred snapshot order/data changed");
    }
    require(metadata.valid && metadata.scanIndex == last &&
            metadata.centerFrequencyHz == 100000000.0 + last && metadata.floatCount == blockSize,
            "Deferred snapshot metadata changed");
}

void testDeferredSnapshots() {
    IqBuffer::clear(701);
    {
        HeldBufferLock reader;
        for (int number = 1; number <= 3; ++number) publishSnapshotBlock(number, 701);
        reader.release();
        require(!reader.timedOut(), "Publish blocked behind an FFT reader");
    }
    publishSnapshotBlock(4, 701);
    requireSnapshotBlocks(1, 4);
    const auto stats = IqBuffer::discontinuityStats();
    require(stats.deferredSnapshotBlocks == 3 && stats.skippedSnapshotBlocks == 0,
            "Brief reader contention must defer, not discard, IQ");
    require(IqBuffer::stats().totalFloatCount == 64, "Deferred samples not counted exactly once");
    std::vector<float> historical;
    IqBuffer::BlockMetadata metadata;
    require(IqBuffer::snapshotRecentEndingAt(historical, 32, 48, nullptr, &metadata) &&
            historical.size() == 32 && historical.front() == 2 && historical.back() == 3 &&
            metadata.totalFloatCount == 48, "Deferred historical FFT endpoint changed");

    IqBuffer::clear(702);
    publishSnapshotBlock(0, 702);
    {
        HeldBufferLock reader;
        for (int number = 1; number <= 10; ++number) publishSnapshotBlock(number, 702);
        reader.release();
        require(!reader.timedOut(), "Overflow path waited for the reader");
    }
    publishSnapshotBlock(11, 702);
    requireSnapshotBlocks(3, 11);
    const auto overflow = IqBuffer::discontinuityStats();
    require(overflow.deferredSnapshotBlocks == 10 && overflow.skippedSnapshotBlocks == 2,
            "Deferred queue overflow accounting changed");
    require(!IqBuffer::snapshotRecentEndingAt(historical, 16, 16),
            "FFT window crossed the dropped snapshot interval");

    IqBuffer::clear(703);
    publishSnapshotBlock(0, 703);
    {
        HeldBufferLock reader;
        publishSnapshotBlock(1, 703);
    }
    {
        HeldBufferLock publisher(true);
        publishSnapshotBlock(2, 703);
        publisher.release();
        require(!publisher.timedOut(), "Concurrent publisher path blocked");
    }
    publishSnapshotBlock(3, 703);
    requireSnapshotBlocks(3, 3);
    require(IqBuffer::discontinuityStats().skippedSnapshotBlocks == 2,
            "A lost publisher must discard the older deferred tail too");

    IqBuffer::clear(704);
    {
        HeldBufferLock reader;
        publishSnapshotBlock(1, 704);
    }
    IqBuffer::clear(705);
    const std::array<float, 2> stale = {1, 1};
    require(!IqBuffer::publish(stale.data(), stale.size(), false, true, 704),
            "Previous-epoch deferred data accepted after retune");
    publishSnapshotBlock(2, 705);
    requireSnapshotBlocks(2, 2);
    const auto reset = IqBuffer::discontinuityStats();
    require(reset.epoch == 705 && reset.deferredSnapshotBlocks == 0 && reset.skippedSnapshotBlocks == 0,
            "Retune retained a deferred queue or old loss counters");

    IqBuffer::clear(706);
    publishSnapshotBlock(0, 706);
    {
        HeldBufferLock reader;
        publishSnapshotBlock(1, 706);
        publishSnapshotBlock(2, 706, 4*1024*1024+2);
        reader.release();
        require(!reader.timedOut(), "Oversized snapshot path waited for the reader");
    }
    publishSnapshotBlock(3, 706);
    requireSnapshotBlocks(3, 3);
    require(IqBuffer::discontinuityStats().skippedSnapshotBlocks == 2,
            "Oversized block did not enforce the deferred memory limit");

    IqBuffer::clear(707);
    constexpr std::size_t largeBlockSize = 1024*1024;
    {
        HeldBufferLock reader;
        for (int number = 1; number <= 5; ++number) publishSnapshotBlock(number, 707, largeBlockSize);
        reader.release();
        require(!reader.timedOut(), "Memory-limited snapshot path waited for the reader");
    }
    publishSnapshotBlock(6, 707, largeBlockSize);
    requireSnapshotBlocks(2, 6, largeBlockSize);
    require(IqBuffer::discontinuityStats().skippedSnapshotBlocks == 1,
            "Deferred float-budget overflow accounting changed");
    IqBuffer::clear();
    std::cout << "Deferred FFT snapshots: contention, FIFO, overflow, epoch and memory bounds PASS\n";
}

void testConcurrentSnapshots() {
    IqBuffer::clear(708);
    constexpr std::size_t blockSize = 32*1024;
    constexpr int blocks = 256;
    std::atomic_bool complete{false}, coherent{true};
    std::atomic<int> snapshots{0};
    std::thread reader([&] {
        std::vector<float> snapshot;
        do {
            if (!IqBuffer::snapshotRecent(snapshot, 512*1024)) continue;
            for (std::size_t n = 1; n < snapshot.size(); ++n) {
                if (snapshot[n] != snapshot[n-1] + 1) coherent.store(false);
            }
            snapshots.fetch_add(1);
        } while (!complete.load());
    });
    bool accepted = true;
    std::vector<float> block(blockSize);
    for (int number = 0; number < blocks; ++number) {
        for (std::size_t n = 0; n < blockSize; ++n)
            block[n] = float(std::size_t(number)*blockSize+n);
        accepted = IqBuffer::publish(block.data(), block.size(), false, true, 708) && accepted;
        if (number == 0) {
            while (snapshots.load() == 0) std::this_thread::yield();
        }
        std::this_thread::yield();
    }
    complete.store(true);
    reader.join();
    require(accepted && coherent.load() && snapshots.load() > 0,
            "Concurrent FFT reader saw reordered samples or a spliced IQ gap");
    IqBuffer::clear();
    std::cout << "Concurrent chunked FFT snapshots: PASS\n";
}
void benchmark() {
    RadioSettings settings;
    settings.sampleRate = 50000000;
    settings.centerFrequency = 161496000;
    settings.listeningFrequency = 166246000;
    settings.modulationType = MOD_NFM;
    settings.bandwidth = 16000;
    std::vector<float> input(2*1000000, 1e-4f), output;
    IqChannelizer channel;
    channel.processFloatIq(input.data(), input.size(), settings, output, false);
    const auto start = std::chrono::steady_clock::now();
    for (int n = 0; n < 5; ++n) channel.processFloatIq(input.data(), input.size(), settings, output, false);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    std::cout << "50 MS/s channelizer: 100 ms of input processed in " << ms << " ms\n";
}
}
static void testWideFmAt(double rate, double offset) {
    RadioSettings settings;
    settings.inputMode = INPUT_RF;
    settings.modulationType = MOD_WFM;
    settings.bandwidth = 200000;
    settings.sampleRate = rate;
    settings.centerFrequency = 102.287174e6;
    settings.listeningFrequency = settings.centerFrequency + offset;
    IqChannelizer::Options referenceOptions;
    referenceOptions.outputRateHz = channelizerTargetRate(settings);
    referenceOptions.cutoffHz = channelizerCutoff(settings, referenceOptions.outputRateHz);
    std::vector<float> iq(262147 * 2);
    const double carrierStep = 6.283185307179586 * offset / rate;
    for (std::size_t n = 0; n < iq.size() / 2; ++n) {
        const double phase = (carrierStep + 0.004) * n + 0.8 * std::sin(0.0005 * n);
        iq[2*n] = float(0.03 * std::cos(phase) + 0.21 * std::sin(0.6 * n));
        iq[2*n+1] = float(0.03 * std::sin(phase) + 0.21 * std::cos(0.6 * n));
    }
    iq[307] = std::numeric_limits<float>::quiet_NaN();
    iq[17020] = std::numeric_limits<float>::infinity();
    RadioSettings referenceSettings = settings;
    referenceSettings.listeningFrequency = settings.centerFrequency;
    auto baseband = iq;
    if (offset != 0.0) {
        // Use a double-precision oscillator as the reference. The legacy
        // scalar oscillator can accumulate phase error over a long callback.
        for (std::size_t n = 0; n < iq.size() / 2; ++n) {
            const double i = std::isfinite(iq[2*n]) ? iq[2*n] : 0.0;
            const double q = std::isfinite(iq[2*n+1]) ? iq[2*n+1] : 0.0;
            const double c = std::cos(carrierStep * n), s = std::sin(carrierStep * n);
            baseband[2*n] = float(i*c + q*s);
            baseband[2*n+1] = float(q*c - i*s);
        }
    }
    IqChannelizer reference, whole, chunked;
    std::vector<float> expected, actual, piece, assembled;
    require(reference.processFloatIq(baseband.data(), baseband.size(), referenceSettings,
                                     expected, false, &referenceOptions).valid,
            "WFM scalar reference failed");
    require(whole.processFloatIq(iq.data(), iq.size(), settings, actual, false).valid &&
            !actual.empty() && actual.size() == expected.size(), "WFM front decimation cadence changed");
    // A strong near-channel tone also exercises roundoff in the scalar rolling sums.
    const float referenceTolerance = offset == 0.0 ? 5e-6f : 1.5e-5f;
    for (std::size_t n = 0; n < actual.size(); ++n)
        require(std::abs(actual[n] - expected[n]) < referenceTolerance, "WFM block front filter changed response");
    for (std::size_t position = 0; position < iq.size();) {
        const auto count = (std::min)(std::size_t(4738), iq.size() - position);
        require(chunked.processFloatIq(iq.data() + position, count, settings, piece, false).valid,
                "WFM irregular chunk rejected");
        assembled.insert(assembled.end(), piece.begin(), piece.end());
        position += count;
    }
    require(assembled.size() == actual.size(), "WFM frame count depends on IQ block boundaries");
    const float chunkTolerance = offset == 0.0 ? 0.0f : 2e-6f;
    for (std::size_t n = 0; n < actual.size(); ++n)
        require(std::abs(assembled[n] - actual[n]) <= chunkTolerance, "WFM phase depends on IQ block boundaries");
}

static void testWideFm() {
    for (const double rate : {28e6, 56e6, 112e6}) {
        testWideFmAt(rate, 0.0);
#if defined(__aarch64__) && defined(__ARM_NEON)
        testWideFmAt(rate, -5.3e6);
        testWideFmAt(rate, 8.1e6);
#endif
    }
}

int main() {
    try {
        testFrontFilter(); testChunking(); testWideFm(); testNonfiniteInput(); testIndependentQueues();
        testDeferredSnapshots(); testConcurrentSnapshots(); benchmark();
        std::cout << "IQ channelizer/independent queue tests: PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
