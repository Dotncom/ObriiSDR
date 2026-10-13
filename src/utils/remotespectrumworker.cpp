#include "remotespectrumworker.h"
#include "spectrumfftworker.h"
#include "dspstreammanager.h"
#include "spectrumcomputeoptions.h"
#include "spectrumcomputesession.h"
#include "fft.h"
#include "remotefftretrypolicy.h"

#include <QHostAddress>
#include <QTcpSocket>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
constexpr int SliceMs = 25;
constexpr int LeaseMs = 2000;
constexpr int IoMs = 3000;
constexpr int FftMs = 300000;
constexpr int PayloadLimit = SpectrumComputeProtocol::ChunkSamples * 8;
constexpr int PacketLimit = 16 + DspStreamProtocol::MaxJsonBytes + PayloadLimit;

qint64 ticks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
struct Cancelled {};
struct TimelineChanged {};
struct Failure {
    QString message;
    bool unavailable = false;
};
bool integer(const QJsonValue &value, int minimum, int maximum, int &out) {
    if (!value.isDouble()) return false;
    const double v = value.toDouble();
    if (!std::isfinite(v) || v < minimum || v > maximum || std::floor(v) != v) return false;
    out = int(v);
    return true;
}
bool validToken(const QString &token) {
    if (token.size() < 32 || token.size() > 128) return false;
    for (QChar c : token) if (c.unicode() < 33 || c.unicode() > 126) return false;
    return true;
}
bool sameFftSettings(const RadioSettings &a, const RadioSettings &b) {
    return a.deviceIndex == b.deviceIndex && a.clockSource == b.clockSource &&
           a.inputMode == b.inputMode && a.sampleRate == b.sampleRate &&
           a.centerFrequency == b.centerFrequency && a.actualFrequency == b.actualFrequency &&
           a.fftLength == b.fftLength && a.fftWindowType == b.fftWindowType;
}
double sampledCenter(const RadioSettings &settings, const IqBuffer::BlockMetadata &meta) {
    return meta.valid && meta.centerFrequencyHz > 0.0 ? meta.centerFrequencyHz : settings.centerFrequency;
}
}

struct RemoteSpectrumWorker::Impl {
    struct Config {
        RadioSettings settings;
        DspStreamEndpoint endpoint;
        SpectrumComputeOptions options;
        int interval = 1, overlap = 0, localBackend = 0;
        bool operator==(const Config &b) const {
            return sameFftSettings(settings, b.settings) && endpoint == b.endpoint &&
                   options == b.options && interval == b.interval && overlap == b.overlap &&
                   localBackend == b.localBackend;
        }
    } config;

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> stopping{false};
    std::atomic<quint64> generation{1};
    std::atomic<qint64> lease{0};
    bool demanded = false, hasResult = false;
    quint64 requestId = 0, resultLoss = 0, resultEpoch = 0;
    std::unique_ptr<SpectrumFftFrame> latest;
    QString message = QStringLiteral("Remote FFT idle");
    std::thread thread;

    // Below this point the state is confined to the worker thread.
    quint64 activeGeneration = 0, epoch = 0, loss = 0, jobs = 0, gaps = 0, overrunSamples = 0;
    quint64 historyBarrierEpoch = 0, historyBarrier = 0;
    bool boundTimeline = false;
    quint64 uploaded = 0, downloaded = 0;
    double upRate = 0, downRate = 0, dspMs = 0, latencyMs = 0, sampleLagMs = 0;
    Time meterAt = Clock::now(), statusAt{}, errorAt{};
    RemoteFftRetryPolicy retry;
    std::unique_ptr<FFTResult> localFft;
    std::vector<float> localIq;
    quint64 localGeneration = 0, localEpoch = 0, localLoss = 0, localEnd = 0;
    int pipelineChunks = 1;

    void releaseLocalCache() {
        localFft.reset();
        std::vector<float>().swap(localIq);
        localEnd = localEpoch = localLoss = 0;
    }
    void check() const {
        if (stopping.load(std::memory_order_acquire) ||
            generation.load(std::memory_order_acquire) != activeGeneration || ticks() >= lease.load())
            throw Cancelled{};
        if (boundTimeline) {
            const auto d = IqBuffer::discontinuityStats();
            if (d.epoch != epoch || d.skippedSnapshotBlocks != loss) throw TimelineChanged{};
        }
    }
    void pause(int ms) {
        const Time end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            check();
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait_until(lock, (std::min)(end, Clock::now() + std::chrono::milliseconds(SliceMs)));
        }
        check();
    }
    RadioSettings settings() {
        std::lock_guard<std::mutex> lock(mutex);
        return config.settings;
    }
    void report(const QString &text, bool force = false) {
        const auto now = Clock::now();
        if (!force && now - statusAt < std::chrono::milliseconds(250)) return;
        const double seconds = std::chrono::duration<double>(now - meterAt).count();
        if (seconds >= 1.0) {
            upRate = double(uploaded) / seconds / 1e6;
            downRate = double(downloaded) / seconds / 1e6;
            uploaded = downloaded = 0;
            meterAt = now;
        }
        const QString value = QStringLiteral("%1 | upload %2 MB/s | download %3 MB/s | DSP %4 ms | latency %5 ms | gaps %6 | overrun %7 samples | IQ lag %8 ms | IQ window %9 chunks")
            .arg(text).arg(upRate, 0, 'f', 2).arg(downRate, 0, 'f', 2)
            .arg(dspMs, 0, 'f', 1).arg(latencyMs, 0, 'f', 1).arg(gaps).arg(overrunSamples).arg(sampleLagMs, 0, 'f', 1).arg(pipelineChunks);
        std::lock_guard<std::mutex> lock(mutex);
        if (generation.load() == activeGeneration && demanded) message = value;
        statusAt = now;
    }
    void publish(std::unique_ptr<SpectrumFftFrame> frame) {
        check();
        if (frame->valid) {
            const auto source = IqBuffer::stats();
            sampleLagMs = source.epoch == frame->metadata.epoch &&
                          source.totalFloatCount >= frame->metadata.totalFloatCount && frame->settings.sampleRate > 0
                ? double(source.totalFloatCount - frame->metadata.totalFloatCount) * 500.0 / frame->settings.sampleRate : 0;
        }
        std::unique_ptr<SpectrumFftFrame> previous;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto d = IqBuffer::discontinuityStats();
            if (generation.load() != activeGeneration || !demanded || ticks() >= lease.load() ||
                (boundTimeline && (d.epoch != epoch || d.skippedSnapshotBlocks != loss)) ||
                (frame->valid && frame->metadata.epoch != d.epoch)) return;
            frame->generation = activeGeneration;
            frame->requestId = requestId;
            previous = std::move(latest);
            latest = std::move(frame);
            resultEpoch = d.epoch;
            resultLoss = d.skippedSnapshotBlocks;
            hasResult = true;
        }
    }
    void error(const QString &text, bool allocation = false) {
        report(text, true);
        if (Clock::now() - errorAt < std::chrono::seconds(1)) return;
        auto frame = std::make_unique<SpectrumFftFrame>();
        frame->settings = settings();
        frame->error = text;
        frame->badAlloc = allocation;
        publish(std::move(frame));
        errorAt = Clock::now();
    }

    struct Connection {
        Impl &owner;
        const Config &config;
        QTcpSocket socket;
        QByteArray buffer;
        QString job;
        bool connected = false;
        int credit = 1, head = 0, pending = 0;
        bool pipelineAdvertised = false;
        struct Ack { quint64 next; int filled; };
        std::array<Ack, SpectrumComputeProtocol::MaxPipelineChunks> acks{};
        Time nextUpload{};
        explicit Connection(Impl &p, const Config &c) : owner(p), config(c) {
            socket.setReadBufferSize(PacketLimit);
        }
        ~Connection() { socket.abort(); }
        void send(const QJsonObject &header, const QByteArray &payload = {}) {
            owner.check();
            const auto bytes = DspStreamProtocol::encode(header, payload);
            if (bytes.isEmpty() || bytes.size() > PacketLimit || payload.size() > PayloadLimit)
                throw Failure{QStringLiteral("Remote FFT packet exceeds limit")};
            // Charge whole framed packets, not just IQ, to the decimal MB/s budget.
            while (Clock::now() < nextUpload) owner.pause(1 + int((std::min)(qint64(SliceMs),
                qint64(std::chrono::duration_cast<std::chrono::milliseconds>(nextUpload - Clock::now()).count()))));
            nextUpload = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(double(bytes.size()) / (double(config.options.uploadLimitMBps) * 1e6)));
            if (socket.state() != QAbstractSocket::ConnectedState || socket.bytesToWrite() ||
                socket.write(bytes) != bytes.size())
                throw Failure{QStringLiteral("Remote FFT connection unavailable while writing"), true};
            const Time deadline = Clock::now() + std::chrono::milliseconds(IoMs);
            while (socket.bytesToWrite()) {
                owner.check();
                if (Clock::now() >= deadline || socket.state() != QAbstractSocket::ConnectedState)
                    throw Failure{QStringLiteral("Remote FFT write timeout/disconnection"), true};
                socket.waitForBytesWritten(SliceMs);
            }
            owner.uploaded += quint64(bytes.size());
            owner.check();
        }
        void receive(QJsonObject &header, QByteArray &payload, Time deadline) {
            for (;;) {
                owner.check();
                if (Clock::now() >= deadline) throw Failure{QStringLiteral("Remote FFT response timeout"), true};
                if (buffer.size() >= 16 && qFromLittleEndian<quint32>(
                    reinterpret_cast<const uchar *>(buffer.constData() + 12)) > PayloadLimit)
                    throw Failure{QStringLiteral("Remote FFT response payload exceeds limit")};
                QString ignored;
                const int result = DspStreamProtocol::extract(buffer, header, payload, ignored);
                if (result < 0) throw Failure{QStringLiteral("Invalid remote FFT framing/JSON")};
                if (result == 1) {
                    if (header.value("type") == "error") {
                        // Never echo untrusted server text: it may contain session credentials.
                        if (header.value("code") == "authentication_failed")
                            throw Failure{QStringLiteral("Remote FFT authentication rejected; check session token")};
                        if (header.value("code") == "busy")
                            throw Failure{QStringLiteral("Remote FFT worker is busy; retrying after the active session/test"), true};
                        throw Failure{QStringLiteral("Remote main FFT server rejected the request")};
                    }
                    return;
                }
                if (!socket.bytesAvailable()) {
                    if (socket.state() != QAbstractSocket::ConnectedState)
                        throw Failure{QStringLiteral("Remote FFT connection closed"), true};
                    socket.waitForReadyRead(SliceMs);
                    continue;
                }
                const qint64 room = PacketLimit - buffer.size();
                if (room <= 0) throw Failure{QStringLiteral("Remote FFT receive buffer exceeds limit")};
                const QByteArray bytes = socket.read(room);
                owner.downloaded += quint64(bytes.size());
                buffer.append(bytes);
            }
        }
        void expect(const char *type, QJsonObject &header, QByteArray &payload, Time deadline) {
            receive(header, payload, deadline);
            if (header.value("type") != QLatin1String(type) || header.value("job") != job)
                throw Failure{QStringLiteral("Remote FFT returned an unexpected or stale job")};
        }
        Time deadline() const { return Clock::now() + std::chrono::milliseconds(IoMs); }
        void open() {
            if (connected) return;
            QHostAddress address;
            if (!config.endpoint.enabled || !config.endpoint.port || !validToken(config.endpoint.token) ||
                !address.setAddress(config.endpoint.address))
                throw Failure{QStringLiteral("Enable Remote FFT with a numeric IP, port and 32-128 character session token")};
            socket.connectToHost(address, config.endpoint.port);
            const auto end = deadline();
            while (socket.state() != QAbstractSocket::ConnectedState) {
                owner.check();
                if (Clock::now() >= end || socket.state() == QAbstractSocket::UnconnectedState)
                    throw Failure{QStringLiteral("Remote FFT connection unavailable"), true};
                socket.waitForConnected(SliceMs);
            }
            socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
            send({{"type", "hello"}, {"version", 1}, {"token", config.endpoint.token}});
            QJsonObject header;
            QByteArray payload;
            receive(header, payload, deadline());
            int version = 0, fftVersion = 0, maximum = 0;
            if (!payload.isEmpty() || header.value("type") != "hello" ||
                !integer(header.value("version"), 1, 1, version) ||
                !integer(header.value("mainFftVersion"), 1, 1, fftVersion) ||
                !integer(header.value("mainFftMaxLength"), 512, SpectrumComputeProtocol::MaxFftLength, maximum))
                throw Failure{QStringLiteral("Remote server does not advertise compatible main FFT v1 support")};
            if (config.settings.fftLength > maximum)
                throw Failure{QStringLiteral("Requested FFT exceeds remote memory budget (maximum %1 samples)").arg(maximum)};
            pipelineAdvertised = header.contains("mainFftPipelineChunks");
            if (pipelineAdvertised && !integer(header.value("mainFftPipelineChunks"), 1,
                SpectrumComputeProtocol::MaxPipelineChunks, credit))
                throw Failure{QStringLiteral("Invalid remote FFT pipeline capability")};
            owner.pipelineChunks = credit;
            connected = true;
        }
        void begin(const RadioSettings &settings) {
            open();
            drain();
            job = QString::number(owner.activeGeneration) + QLatin1Char('-') + QString::number(++owner.jobs);
            QJsonObject request{{"type", "main_fft_begin"}, {"job", job}, {"length", settings.fftLength},
                {"rate", settings.sampleRate}, {"center", settings.centerFrequency},
                {"window", settings.fftWindowType}, {"backend", config.options.backend},
                {"mode", config.options.mode == SpectrumComputeMode::Continuous ? "continuous" : "windows"}};
            if (pipelineAdvertised) request["pipelineChunks"] = credit;
            send(request);
            QJsonObject header;
            QByteArray payload;
            expect("main_fft_ready", header, payload, deadline());
            if (!payload.isEmpty()) throw Failure{QStringLiteral("Invalid remote FFT ready payload")};
            int accepted = 0;
            if (pipelineAdvertised && !integer(header.value("pipelineChunks"), credit, credit, accepted))
                throw Failure{QStringLiteral("Remote FFT did not confirm the pipeline bound")};
        }
        void iq(const float *samples, int count, quint64 first, int expectedFilled, bool gap) {
            if (count <= 0 || count > SpectrumComputeProtocol::ChunkSamples * 2 || count % 2 ||
                first > (std::numeric_limits<quint64>::max)() - quint64(count / 2))
                throw Failure{QStringLiteral("Invalid remote FFT IQ counters")};
            for (int i = 0; i < count; ++i)
                if (!std::isfinite(samples[i])) throw Failure{QStringLiteral("Non-finite IQ samples; remote FFT stopped")};
            if (pending == credit) acknowledge();
            send({{"type", "main_fft_iq"}, {"job", job}, {"first", QString::number(first)}, {"gap", gap}},
                 SpectrumComputeProtocol::floats(samples, count));
            acks[std::size_t((head + pending) % credit)] = {first + quint64(count / 2), expectedFilled};
            ++pending;
            if (credit == 1) acknowledge();
        }
        void acknowledge() {
            if (!pending) return;
            QJsonObject header;
            QByteArray payload;
            expect("main_fft_ack", header, payload, deadline());
            quint64 next = 0;
            int filled = 0;
            if (!payload.isEmpty() || !SpectrumComputeProtocol::readCounter(header.value("next"), next) ||
                next != acks[std::size_t(head)].next ||
                !integer(header.value("filled"), 0, config.settings.fftLength, filled) || filled != acks[std::size_t(head)].filled)
                throw Failure{QStringLiteral("Invalid remote FFT acknowledgement counters")};
            head = (head + 1) % credit;
            --pending;
        }
        void drain() { while (pending) acknowledge(); }
        std::unique_ptr<SpectrumFftFrame> run(const RadioSettings &settings,
                                             const IqBuffer::BlockMetadata &metadata, Time started) {
            send({{"type", "main_fft_run"}, {"job", job}});
            // TCP orders run after its IQ. The server can start computing while
            // we validate the ACK tail, without a second transform or IQ window.
            drain();
            const Time end = Clock::now() + std::chrono::milliseconds(FftMs);
            QJsonObject header;
            QByteArray payload;
            expect("main_fft_result", header, payload, end);
            int length = 0;
            quint64 next = 0;
            const auto dsp = header.value("dspMs");
            if (!payload.isEmpty() || !integer(header.value("length"), settings.fftLength, settings.fftLength, length) ||
                !SpectrumComputeProtocol::readCounter(header.value("next"), next) ||
                next != metadata.totalFloatCount / 2 || !dsp.isDouble() ||
                !std::isfinite(dsp.toDouble()) || dsp.toDouble() < 0 || dsp.toDouble() > FftMs)
                throw Failure{QStringLiteral("Invalid remote FFT result header/counters")};
            auto frame = std::make_unique<SpectrumFftFrame>();
            // Keep the requested settings for the GUI's configuration match;
            // only the compute/axis center follows the receiver metadata.
            frame->settings = owner.settings();
            owner.check();
            frame->metadata = metadata;
            frame->magnitudes.resize(std::size_t(length));
            std::vector<float> bins;
            for (int offset = 0; offset < length;) {
                expect("main_fft_bins", header, payload, end);
                int receivedOffset = 0;
                if (!integer(header.value("offset"), offset, offset, receivedOffset) ||
                    !SpectrumComputeProtocol::readFloats(payload, bins) || bins.size() > std::size_t(length - offset))
                    throw Failure{QStringLiteral("Invalid remote FFT bins, offsets or non-finite levels")};
                std::copy(bins.begin(), bins.end(), frame->magnitudes.begin() + offset);
                offset += int(bins.size());
            }
            expect("main_fft_done", header, payload, end);
            if (!payload.isEmpty()) throw Failure{QStringLiteral("Invalid remote FFT completion payload")};
            frame->frequencies.resize(std::size_t(length));
            for (int i = 0; i < length; ++i) {
                if (i % SpectrumComputeProtocol::ChunkSamples == 0) owner.check();
                frame->frequencies[std::size_t(i)] = float(settings.centerFrequency +
                    double(i - length / 2) * (settings.sampleRate / length));
            }
            // Main FFT levels are deliberately left in natural, unshifted order.
            frame->valid = true;
            owner.dspMs = dsp.toDouble();
            owner.latencyMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            owner.retry.reset();
            owner.releaseLocalCache();
            return frame;
        }
    };

    void validate(const Config &c) {
        const auto &s = c.settings;
        if (s.inputMode != INPUT_RF) throw Failure{QStringLiteral("Remote main FFT supports RF only; use local FFT for HF/scans")};
        if (c.options.mode != SpectrumComputeMode::Windows && c.options.mode != SpectrumComputeMode::Continuous)
            throw Failure{QStringLiteral("Remote FFT mode is not selected")};
        if (s.fftLength < 512 || s.fftLength > SpectrumComputeProtocol::MaxFftLength ||
            !std::isfinite(s.sampleRate) || s.sampleRate < 1000 || s.sampleRate > 1e9 ||
            !std::isfinite(s.centerFrequency) || s.centerFrequency < 0 || s.centerFrequency > 8e9 ||
            s.fftWindowType < 0 || s.fftWindowType > 4 || c.options.backend < 0 || c.options.backend > 2)
            throw Failure{QStringLiteral("Invalid remote FFT length, rate, center, window or backend")};
        if (c.options.uploadLimitMBps <= 0)
            throw Failure{QStringLiteral("Remote FFT upload budget must be a positive MB/s value")};
    }
    void bind(const IqBuffer::Stats &stats) {
        if (stats.totalFloatCount % 2 || stats.snapshotSize % 2 || stats.snapshotSize > stats.totalFloatCount)
            throw Failure{QStringLiteral("Invalid IQ ring counters")};
        epoch = stats.epoch;
        loss = stats.skippedSnapshotBlocks;
        boundTimeline = true;
        check();
    }
    bool snapshot(std::vector<float> &iq, std::size_t count, quint64 end,
                  RadioSettings &sampled, IqBuffer::BlockMetadata &meta) {
        check();
        sampled = settings();
        if (!IqBuffer::snapshotRecentEndingAt(iq, count, end, nullptr, &meta)) return false;
        check();
        if (iq.size() != count || meta.epoch != epoch || meta.totalFloatCount != end) return false;
        if (meta.tuning || meta.scanIndex >= 0)
            throw Failure{QStringLiteral("Remote FFT paused during tuning/scan; use local FFT")};
        if (meta.valid && (!std::isfinite(meta.centerFrequencyHz) || meta.centerFrequencyHz < 0 || meta.centerFrequencyHz > 8e9))
            throw Failure{QStringLiteral("Invalid sampled IQ center frequency")};
        sampled.centerFrequency = sampledCenter(sampled, meta);
        meta.floatCount = count;
        return true;
    }
    QString needsWindow(quint64 n, quint64 retained) const {
        return QStringLiteral("Window FFT needs %1 samples; only %2 retained. Wait for a full snapshot or select Continuous mode; no truncation/zero-padding")
            .arg(n).arg(retained);
    }

    void remote(const Config &c) {
        Connection connection(*this, c);
        const quint64 n = quint64(c.settings.fftLength), floats = n * 2;
        const bool continuous = c.options.mode == SpectrumComputeMode::Continuous;
        const quint64 hop = (std::max)({quint64(1), n * quint64(100 - c.overlap) / 100,
            quint64(std::ceil(c.settings.sampleRate * double(c.interval) / 1000.0))});
        quint64 cursor = 0, lastFft = 0, windowEnd = 0, floor = 0;
        int filled = 0;
        bool begun = false, firstChunk = true;
        double center = c.settings.centerFrequency;
        std::vector<float> iq;
        auto initial = IqBuffer::stats();
        bind(initial);
        // Joining live processing must not replay an entire old retained history.
        // For FFTs larger than the local ring, retain all available samples to seed it.
        cursor = initial.totalFloatCount - (continuous ? (std::min)(quint64(initial.snapshotSize), floats)
                                                       : quint64(initial.snapshotSize));
        if (historyBarrierEpoch == epoch) cursor = (std::max)(cursor, historyBarrier);
        floor = cursor;
        Time nextWindow{}, started = Clock::now();
        for (;;) {
            check();
            const auto stats = IqBuffer::stats();
            if (stats.epoch != epoch || stats.skippedSnapshotBlocks != loss) throw TimelineChanged{};
            if (stats.totalFloatCount % 2 || stats.snapshotSize % 2 || stats.snapshotSize > stats.totalFloatCount)
                throw Failure{QStringLiteral("Invalid IQ ring counters")};
            const quint64 retained = stats.totalFloatCount - stats.snapshotSize;
            if (continuous && (cursor < retained || cursor > stats.totalFloatCount)) {
                if (cursor < retained) overrunSamples += (retained - cursor) / 2;
                ++gaps;
                cursor = retained;
                begun = false;
                filled = 0;
                lastFft = 0;
                started = Clock::now();
                report(QStringLiteral("Continuous FFT overrun; restarting server history"), true);
            }
            if (!continuous) {
                if (Clock::now() < nextWindow || stats.totalFloatCount == windowEnd) { pause(10); continue; }
                if (stats.snapshotSize < floats || stats.totalFloatCount - floor < floats) {
                    error(needsWindow(n, stats.snapshotSize / 2));
                    pause(100);
                    continue;
                }
                started = Clock::now();
                RadioSettings sampled;
                IqBuffer::BlockMetadata meta;
                if (!snapshot(iq, std::size_t(floats), stats.totalFloatCount, sampled, meta)) { pause(10); continue; }
                nextWindow = Clock::now() + std::chrono::milliseconds(c.interval);
                connection.begin(sampled);
                for (std::size_t offset = 0; offset < iq.size();) {
                    const int count = int((std::min)(std::size_t(SpectrumComputeProtocol::ChunkSamples * 2), iq.size() - offset));
                    connection.iq(iq.data() + offset, count, (meta.totalFloatCount - floats + offset) / 2,
                                  int((offset + std::size_t(count)) / 2), offset == 0);
                    offset += std::size_t(count);
                }
                windowEnd = meta.totalFloatCount;
                publish(connection.run(sampled, meta, started));
                report(QStringLiteral("Remote window FFT"));
                continue;
            }
            if (cursor == stats.totalFloatCount) {
                connection.drain();
                report(QStringLiteral("Remote continuous FFT: waiting for IQ (%1/%2 samples)").arg(filled).arg(n));
                pause(5);
                continue;
            }
            quint64 count = (std::min)(quint64(SpectrumComputeProtocol::ChunkSamples * 2), stats.totalFloatCount - cursor);
            RadioSettings sampled;
            IqBuffer::BlockMetadata meta;
            if (!snapshot(iq, std::size_t(count), cursor + count, sampled, meta)) { pause(1); continue; }
            if (begun && sampled.centerFrequency != center) {
                // A metadata retune without an epoch cannot be spliced into history.
                ++gaps;
                cursor = stats.totalFloatCount;
                begun = false;
                filled = 0;
                lastFft = 0;
                started = Clock::now();
                continue;
            }
            if (!begun) {
                connection.begin(sampled);
                begun = true;
                firstChunk = true;
                center = sampled.centerFrequency;
            }
            filled = int((std::min)(n, quint64(filled) + count / 2));
            connection.iq(iq.data(), int(count), cursor / 2, filled, firstChunk);
            firstChunk = false;
            cursor += count;
            if (filled == int(n) && (!lastFft || cursor - lastFft >= hop * 2)) {
                meta.floatCount = std::size_t(floats);
                publish(connection.run(sampled, meta, started));
                lastFft = cursor;
                started = Clock::now();
            }
            report(QStringLiteral("Remote continuous FFT (%1/%2 samples)").arg(filled).arg(n));
        }
    }

    void fallback(const Config &c, const QString &reason, int retryMs) {
        // The failed Connection has already been destroyed before entering here.
        // Keep the plan and source cursor warm across failed reconnect attempts.
        if (!localFft) localFft = std::make_unique<FFTResult>(true);
        localFft->setBackendPreference(c.localBackend);
        const Time until = Clock::now() + std::chrono::milliseconds(retryMs);
        while (Clock::now() < until) {
            check();
            const auto stats = IqBuffer::stats();
            if (!boundTimeline) bind(stats);
            else if (stats.epoch != epoch || stats.skippedSnapshotBlocks != loss) throw TimelineChanged{};
            const quint64 count = quint64(c.settings.fftLength) * 2;
            const QString label = QStringLiteral("LOCAL FALLBACK: %1; remote retry backoff %2 ms").arg(reason).arg(retryMs);
            report(label, true);
            if (stats.snapshotSize < count || (historyBarrierEpoch == epoch &&
                (stats.totalFloatCount < historyBarrier || stats.totalFloatCount - historyBarrier < count))) {
                error(label + QStringLiteral("; ") + needsWindow(count / 2, stats.snapshotSize / 2));
                pause(100);
                continue;
            }
            if (localEpoch == epoch && localLoss == loss && localEnd == stats.totalFloatCount) { pause(20); continue; }
            auto frame = std::make_unique<SpectrumFftFrame>();
            RadioSettings sampled;
            if (!snapshot(localIq, std::size_t(count), stats.totalFloatCount, sampled, frame->metadata)) { pause(10); continue; }
            const auto start = Clock::now();
            frame->valid = localFft->computeSnapshot(sampled, localIq, frame->frequencies, frame->magnitudes,
                                               &frame->referenceMagnitudes, &frame->metadata);
            if (!frame->valid) throw Failure{QStringLiteral("LOCAL FALLBACK: FFT computation failed")};
            frame->settings = settings();
            check();
            dspMs = latencyMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            localEnd = frame->metadata.totalFloatCount;
            localEpoch = epoch;
            localLoss = loss;
            publish(std::move(frame));
            report(label, true);
            pause(c.interval);
        }
    }
    void run() {
        while (!stopping.load()) {
            Config c;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stopping.load() || (demanded && ticks() < lease.load()); });
                if (stopping.load()) return;
                c = config;
                activeGeneration = generation.load();
            }
            boundTimeline = false;
            if (localGeneration != activeGeneration) {
                releaseLocalCache();
                localGeneration = activeGeneration;
                retry.reset();
                pipelineChunks = 1;
            }
            try {
                validate(c);
                try {
                    remote(c);
                } catch (const Failure &failure) {
                    if (c.options.mode == SpectrumComputeMode::Continuous && boundTimeline) ++gaps;
                    check();
                    const int retryMs = retry.failed(failure.unavailable);
                    if (c.options.fallbackToLocal) fallback(c, failure.message, retryMs);
                    else { error(failure.message); pause(retryMs); }
                }
            } catch (const TimelineChanged &) {
                // The loss counter can advance before the ring is cleared by the
                // next publisher. Never restart from its still-retained old tail.
                const auto stats = IqBuffer::stats();
                if (stats.epoch == epoch) {
                    historyBarrierEpoch = epoch;
                    historyBarrier = stats.totalFloatCount;
                }
                boundTimeline = false;
                ++gaps;
                report(QStringLiteral("IQ epoch/loss changed; restarting remote FFT history"), true);
            } catch (const Cancelled &) {
                releaseLocalCache();
                std::lock_guard<std::mutex> lock(mutex);
                if (generation.load() == activeGeneration && ticks() >= lease.load()) {
                    hasResult = false;
                    message = QStringLiteral("Remote FFT idle: demand lease expired");
                }
            } catch (const Failure &failure) {
                boundTimeline = false;
                try { error(failure.message); pause(1000); } catch (...) {}
            } catch (const std::bad_alloc &) {
                boundTimeline = false;
                try { error(QStringLiteral("Remote FFT memory allocation failed; reduce FFT length"), true); pause(1000); } catch (...) {}
            } catch (...) {
                boundTimeline = false;
                try { error(QStringLiteral("Remote FFT worker failed")); pause(1000); } catch (...) {}
            }
        }
    }
};

RemoteSpectrumWorker::RemoteSpectrumWorker() : impl(std::make_unique<Impl>()) {
    // Start only after every member, including the owning pointer, is initialized.
    impl->thread = std::thread([this] { impl->run(); });
}
RemoteSpectrumWorker::~RemoteSpectrumWorker() {
    impl->stopping.store(true, std::memory_order_release);
    impl->generation.fetch_add(1, std::memory_order_acq_rel);
    impl->lease.store(0);
    impl->wake.notify_all();
    if (impl->thread.joinable()) impl->thread.join();
}
void RemoteSpectrumWorker::request(const RadioSettings &settings, const DspStreamEndpoint &endpoint,
                                   const SpectrumComputeOptions &options, int updateMs,
                                   int overlapPercent, int localBackend) {
    Impl::Config next;
    next.settings = settings;
    next.endpoint = endpoint;
    next.options = options;
    next.interval = (std::max)(1, updateMs);
    next.overlap = (std::clamp)(overlapPercent, 0, 95);
    next.localBackend = normalizedFftBackendPreference(localBackend);
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->demanded || ticks() >= impl->lease.load() || !(impl->config == next)) {
            impl->generation.fetch_add(1, std::memory_order_acq_rel);
            impl->hasResult = false;
            impl->message = QStringLiteral("Remote FFT starting");
        }
        impl->config = std::move(next);
        impl->demanded = true;
        ++impl->requestId;
        const qint64 demandMs = (std::max)(qint64(LeaseMs), qint64(impl->config.interval) * 2 + 1000);
        impl->lease.store(ticks() + demandMs, std::memory_order_release);
    }
    impl->wake.notify_one();
}
bool RemoteSpectrumWorker::takeLatest(SpectrumFftFrame &frame) {
    std::unique_ptr<SpectrumFftFrame> result;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->hasResult || !impl->latest) return false;
        impl->hasResult = false;
        const auto d = IqBuffer::discontinuityStats();
        if (!impl->demanded || ticks() >= impl->lease.load() ||
            impl->latest->generation != impl->generation.load() || d.epoch != impl->resultEpoch ||
            d.skippedSnapshotBlocks != impl->resultLoss) return false;
        result = std::move(impl->latest);
    }
    frame = std::move(*result);
    return true;
}
void RemoteSpectrumWorker::cancel() {
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->generation.fetch_add(1, std::memory_order_acq_rel);
        impl->lease.store(0);
        impl->demanded = false;
        impl->hasResult = false;
        impl->message = QStringLiteral("Remote FFT cancelled");
    }
    impl->wake.notify_all();
}
QString RemoteSpectrumWorker::status() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->message;
}
