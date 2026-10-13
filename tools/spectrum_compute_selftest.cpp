#include "diagnosticlogging.h"
#include "dspstreammanager.h"
#include "dspstreamtransport_p.h"
#include "fft.h"
#include "remotespectrumworker.h"
#include "remotefftretrypolicy.h"
#include "spectrumcomputesession.h"
#include "spectrumfftworker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

// Standalone main; legacy receiver symbols come from fft_test_globals.cpp.
namespace {
using SpectrumComputeProtocol::ChunkSamples;
constexpr double TwoPi = 6.28318530717958647692;
int sessionFrames = 0;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs = 10000, const std::function<void()> &renew = {}) {
    QElapsedTimer timer;
    timer.start();
    qint64 nextRenewal = 250;
    do {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        if (renew && timer.elapsed() >= nextRenewal) {
            renew();
            nextRenewal = timer.elapsed() + 250;
        }
        QThread::msleep(5);
    } while (timer.elapsed() < timeoutMs);
    return predicate();
}

std::vector<float> makeIq(int count, int offset = 0) {
    std::vector<float> iq(static_cast<std::size_t>(count) * 2);
    for (int i = 0; i < count; ++i) {
        const double t = i + offset;
        iq[2 * i] = static_cast<float>(0.35 * std::cos(0.071 * t) + 0.11 * std::sin(0.397 * t));
        iq[2 * i + 1] = static_cast<float>(0.29 * std::sin(0.071 * t) - 0.09 * std::cos(0.213 * t));
    }
    return iq;
}

RadioSettings radioSettings(int length = 777, int window = FFT_WINDOW_HANN) {
    RadioSettings settings;
    settings.inputMode = INPUT_RF;
    settings.fftLength = length;
    settings.fftWindowType = window;
    settings.sampleRate = 96000.0;
    settings.centerFrequency = 100100000.0;
    return settings;
}

void compare(const std::vector<float> &actual, const std::vector<float> &expected,
             float tolerance = 0.0001f) {
    require(actual.size() == expected.size(), "FFT vector length differs");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        require(std::isfinite(actual[i]) && std::isfinite(expected[i]) &&
                    std::abs(actual[i] - expected[i]) <= tolerance,
                "remote/session bins differ from direct FFT snapshot");
    }
}

std::vector<float> directLevels(const RadioSettings &settings, const std::vector<float> &iq) {
    FFTResult fft(true);
    fft.setBackendPreference(FFT_BACKEND_CPU_FFTW);
    std::vector<float> frequencies, levels;
    require(fft.computeSnapshot(settings, iq, frequencies, levels), "direct reference FFT failed");
    return levels;
}

struct Packet {
    QJsonObject header;
    QByteArray payload;
};

struct Session {
    SpectrumComputeSession session{64};
    QString job = QStringLiteral("snapshot-test");
    std::vector<Packet> replies;

    bool call(const QJsonObject &header, const QByteArray &payload = {}) {
        replies.clear();
        return session.handle(header, payload, [this](const QJsonObject &h, const QByteArray &p) {
            replies.push_back({h, p});
            return true;
        });
    }

    QJsonObject beginRequest(const RadioSettings &settings, bool continuous = false) const {
        return {{"type", "main_fft_begin"}, {"job", job}, {"length", settings.fftLength},
                {"rate", settings.sampleRate}, {"center", settings.centerFrequency},
                {"window", settings.fftWindowType}, {"backend", FFT_BACKEND_CPU_FFTW},
                {"mode", continuous ? "continuous" : "windows"}};
    }

    void begin(const RadioSettings &settings, bool continuous = false) {
        require(call(beginRequest(settings, continuous)), "main_fft_begin rejected");
        require(replies.size() == 1 && replies[0].header.value("type") == "main_fft_ready" &&
                    replies[0].header.value("job") == job && replies[0].payload.isEmpty(),
                "invalid main_fft_ready reply");
    }

    int append(const float *iq, int count, quint64 first, bool gap = false) {
        require(call({{"type", "main_fft_iq"}, {"job", job}, {"first", QString::number(first)},
                      {"gap", gap}}, SpectrumComputeProtocol::floats(iq, count * 2)),
                "main_fft_iq rejected");
        require(replies.size() == 1 && replies[0].header.value("type") == "main_fft_ack" &&
                    replies[0].header.value("job") == job && replies[0].payload.isEmpty(),
                "invalid main_fft_ack reply");
        quint64 next = 0;
        require(SpectrumComputeProtocol::readCounter(replies[0].header.value("next"), next) &&
                    next == first + static_cast<quint64>(count), "sample counter lost precision");
        return replies[0].header.value("filled").toInt(-1);
    }

    void upload(const std::vector<float> &iq, quint64 first = 0, int chunkSamples = ChunkSamples) {
        const int count = static_cast<int>(iq.size() / 2);
        for (int offset = 0; offset < count;) {
            const int n = (std::min)(chunkSamples, count - offset);
            append(iq.data() + 2 * offset, n, first + offset);
            offset += n;
        }
    }

    std::vector<float> run(int length, quint64 next) {
        require(call({{"type", "main_fft_run"}, {"job", job}}), "main_fft_run rejected");
        require(replies.size() >= 3 && replies.front().header.value("type") == "main_fft_result" &&
                    replies.front().header.value("length").toInt() == length &&
                    replies.front().payload.isEmpty(), "invalid result header");
        quint64 actualNext = 0;
        require(SpectrumComputeProtocol::readCounter(replies.front().header.value("next"), actualNext) &&
                    actualNext == next, "result sample counter differs");
        const double dspMs = replies.front().header.value("dspMs").toDouble(-1);
        require(std::isfinite(dspMs) && dspMs >= 0, "invalid result DSP time");
        std::vector<float> result;
        for (std::size_t i = 0; i < replies.size(); ++i) {
            const auto &packet = replies[i];
            require(packet.header.value("job") == job, "stale result job");
            if (i == 0 || i + 1 == replies.size()) continue;
            require(packet.header.value("type") == "main_fft_bins" &&
                        packet.header.value("offset").toInt(-1) == static_cast<int>(result.size()),
                    "result chunks are missing, reordered or overlapping");
            std::vector<float> bins;
            require(SpectrumComputeProtocol::readFloats(packet.payload, bins), "invalid bin payload");
            result.insert(result.end(), bins.begin(), bins.end());
        }
        require(replies.back().header.value("type") == "main_fft_done" &&
                    replies.back().payload.isEmpty() && result.size() == static_cast<std::size_t>(length),
                "incomplete FFT result");
        ++sessionFrames;
        return result;
    }

    void reject(const QJsonObject &header, const QByteArray &payload = {}) {
        require(!call(header, payload), "invalid session request accepted");
        require(replies.size() == 1 && replies[0].header.value("type") == "error" &&
                    !replies[0].header.value("error").toString().isEmpty() && replies[0].payload.isEmpty(),
                "session failure did not return a bounded error");
    }
};

void windowSessionTests() {
    Session session;
    const quint64 first = (quint64(1) << 60) + 17;
    for (int length : {512, 777, 4096, ChunkSamples * 2 + 7}) {
        const auto iq = makeIq(length);
        for (int window = FFT_WINDOW_RECTANGULAR; window <= FFT_WINDOW_FLAT_TOP; ++window) {
            const auto settings = radioSettings(length, window);
            session.begin(settings);
            session.append(iq.data(), 1, first);
            session.upload(std::vector<float>(iq.begin() + 2, iq.end()), first + 1, ChunkSamples - 1);
            compare(session.run(length, first + length), directLevels(settings, iq));
            compare(session.run(length, first + length), directLevels(settings, iq));
        }
    }
    std::cout << "Window sessions: five windows, arbitrary lengths, chunked results and 64-bit counters passed\n";
}

void continuousSessionTests() {
    const auto settings = radioSettings();
    Session session;
    session.begin(settings, true);
    auto history = makeIq(settings.fftLength);
    require(session.append(history.data(), 300, 0) == 300, "partial ring count differs");
    session.append(history.data() + 600, settings.fftLength - 300, 300);
    quint64 next = settings.fftLength;
    compare(session.run(settings.fftLength, next), directLevels(settings, history));
    for (int count : {201, 503, 17, settings.fftLength + 321}) {
        const auto chunk = makeIq(count, static_cast<int>(next));
        require(session.append(chunk.data(), count, next) == settings.fftLength, "ring lost full window");
        next += count;
        history.insert(history.end(), chunk.begin(), chunk.end());
        compare(session.run(settings.fftLength, next), directLevels(settings, history));
    }
    for (bool explicitGap : {false, true}) {
        if (!explicitGap) next += 91;
        const auto fresh = makeIq(settings.fftLength, static_cast<int>(next));
        require(session.append(fresh.data(), 113, next, explicitGap) == 113,
                "gap retained old ring history");
        session.append(fresh.data() + 226, settings.fftLength - 113, next + 113);
        next += settings.fftLength;
        compare(session.run(settings.fftLength, next), directLevels(settings, fresh));
    }
    session.job = QStringLiteral("retuned-session");
    auto changed = radioSettings(1001, FFT_WINDOW_FLAT_TOP);
    changed.sampleRate = 192000;
    changed.centerFrequency += 250000;
    session.begin(changed, true);
    require(session.append(history.data(), 100, 0) == 100, "reconfigure retained prior history");
    session.reject({{"type", "main_fft_run"}, {"job", session.job}});
    std::cout << "Continuous sessions: wrap, oversized chunks, explicit/implicit gaps and reconfiguration passed\n";
}

void invalidSessionTests() {
    require(SpectrumComputeProtocol::maxLengthForBudget(0) == 1048576 &&
                SpectrumComputeProtocol::maxLengthForBudget(64) == 1048576 &&
                SpectrumComputeProtocol::maxLengthForBudget(8192) == SpectrumComputeProtocol::MaxFftLength,
            "FFT memory budget bounds changed");
    {
        Session session;
        session.begin(radioSettings(SpectrumComputeProtocol::maxLengthForBudget(64)));
        const std::vector<float> values{1.0f, -2.0f, 0.5f};
        const auto bytes = SpectrumComputeProtocol::floats(values.data(), 3);
        require(bytes == QByteArray::fromHex("0000803f000000c00000003f"),
                "float payload is not little-endian IEEE754");
        std::vector<float> decoded;
        require(SpectrumComputeProtocol::readFloats(bytes, decoded) && decoded == values,
                "float codec roundtrip changed data");
        require(SpectrumComputeProtocol::floats(values.data(), -1).isEmpty() &&
                    SpectrumComputeProtocol::floats(values.data(), ChunkSamples * 2 + 1).isEmpty(),
                "out-of-range float serialization accepted");
    }
    for (const auto &change : std::vector<std::pair<QString, QJsonValue>>{
             {"length", 0}, {"length", 511}, {"length", 1048577},
             {"length", SpectrumComputeProtocol::MaxFftLength + 1},
             {"rate", 999}, {"rate", 1e9 + 1}, {"center", -1}, {"center", 8e9 + 1},
             {"window", -1}, {"window", 5}, {"backend", -1}, {"backend", 3},
             {"job", ""}, {"job", QString(81, 'a')}, {"mode", "invalid"}}) {
        Session session;
        auto request = session.beginRequest(radioSettings());
        request[change.first] = change.second;
        session.reject(request);
    }
    {
        Session session;
        session.reject(session.beginRequest(radioSettings()), QByteArray("payload"));
        session.reject({{"type", "main_fft_run"}, {"job", session.job}});
    }
    const auto iq = makeIq(777);
    const auto payload = SpectrumComputeProtocol::floats(iq.data(), static_cast<int>(iq.size()));
    for (const QJsonValue &counter : {QJsonValue(-1), QJsonValue(42), QJsonValue("-1"),
                                     QJsonValue("1.5"), QJsonValue("+1"), QJsonValue(""),
                                     QJsonValue("18446744073709551616")}) {
        Session session;
        session.begin(radioSettings());
        session.reject({{"type", "main_fft_iq"}, {"job", session.job}, {"first", counter}}, payload);
    }
    std::vector<float> nonfinite{std::numeric_limits<float>::quiet_NaN(), 0.0f};
    for (const auto &bad : std::vector<QByteArray>{QByteArray(), QByteArray(4, '\0'), QByteArray(9, '\0'),
             QByteArray(ChunkSamples * 8 + 8, '\0'), SpectrumComputeProtocol::floats(nonfinite.data(), 2)}) {
        Session session;
        session.begin(radioSettings());
        session.reject({{"type", "main_fft_iq"}, {"job", session.job}, {"first", "0"}}, bad);
    }
    {
        Session session;
        session.begin(radioSettings());
        session.reject({{"type", "main_fft_iq"}, {"job", session.job},
                        {"first", QString::number((std::numeric_limits<quint64>::max)())}}, payload);
    }
    for (bool continuous : {false, true}) {
        Session session;
        session.begin(radioSettings(), continuous);
        session.append(iq.data(), 776, 0);
        session.reject({{"type", "main_fft_run"}, {"job", session.job}});
    }
    for (bool gap : {false, true}) {
        Session session;
        session.begin(radioSettings());
        session.append(iq.data(), 100, 0);
        session.reject({{"type", "main_fft_iq"}, {"job", session.job},
                        {"first", gap ? "100" : "101"}, {"gap", gap}}, payload);
    }
    {
        Session session;
        session.begin(radioSettings());
        session.upload(iq);
        session.reject({{"type", "main_fft_iq"}, {"job", session.job}, {"first", "777"}}, payload);
    }
    for (const auto &command : std::vector<QJsonObject>{
             {{"type", "main_fft_run"}, {"job", "stale"}},
             {{"type", "main_fft_unknown"}, {"job", "snapshot-test"}}}) {
        Session session;
        session.begin(radioSettings());
        session.reject(command);
    }
    {
        Session session;
        session.begin(radioSettings());
        session.upload(iq);
        session.reject({{"type", "main_fft_run"}, {"job", session.job}}, QByteArray("payload"));
    }
    {
        Session session;
        session.begin(radioSettings());
        session.upload(iq);
        int replies = 0;
        require(!session.session.handle({{"type", "main_fft_run"}, {"job", session.job}}, {},
                    [&replies](const QJsonObject &, const QByteArray &) { return ++replies < 2; }) &&
                    replies == 2, "failed result sink was ignored");
    }
    std::cout << "Session validation: budget, incomplete windows, stale jobs, gaps, malformed IQ and reply failure passed\n";
}

struct LoopbackServer {
    DspComputeServer server;
    DspStreamEndpoint endpoint;

    LoopbackServer() {
        endpoint.enabled = true;
        endpoint.address = QStringLiteral("127.0.0.1");
        endpoint.token = QString(64, 'a');
        require(server.start(endpoint.address, 0, endpoint.token, 64), "loopback server start failed");
        require(waitUntil([this] { return server.port() != 0; }, 3000), "loopback server bind failed");
        endpoint.port = server.port();
    }
};

IqBuffer::Stats publishIq(const RadioSettings &settings, const std::vector<float> &iq,
                         quint64 epoch, bool clear = true) {
    if (clear) IqBuffer::clear(epoch);
    IqBuffer::BlockMetadata metadata;
    metadata.valid = true;
    metadata.centerFrequencyHz = settings.centerFrequency;
    require(IqBuffer::publish(iq.data(), iq.size(), true, true, epoch, &metadata),
            "queued receiver IQ publish failed");
    return IqBuffer::stats();
}

void checkAudioQueue(const IqBuffer::Stats &before) {
    const auto after = IqBuffer::stats();
    require(after.queuedBlocks == before.queuedBlocks &&
                after.queuedFloatCount == before.queuedFloatCount &&
                after.droppedQueuedBlocks == before.droppedQueuedBlocks &&
                after.droppedQueuedFloats == before.droppedQueuedFloats,
            "remote spectrum request consumed or dropped the audio queue");
}

void checkQueuedIq(const std::vector<float> &expected) {
    std::vector<float> queued;
    require(IqBuffer::popBlock(queued) && queued == expected, "audio queue payload was changed");
}

DspStreamBlock ddcTone() {
    DspStreamBlock block;
    block.stream = QStringLiteral("parallel-ddc-channel");
    block.generation = 1;
    block.epoch = 9;
    block.sampleRate = 96000;
    block.centerHz = 100100000;
    block.returnIq = true;
    block.ffts.append({"ddc-fft", 1024, FFT_WINDOW_HANN, 20});
    block.iq.resize(4096 * 2);
    for (int i = 0; i < 4096; ++i) {
        const double phase = TwoPi * 1500.0 * i / block.sampleRate;
        block.iq[2 * i] = static_cast<float>(0.5 * std::cos(phase));
        block.iq[2 * i + 1] = static_cast<float>(0.5 * std::sin(phase));
    }
    return block;
}

void checkDdc(const DspStreamResult &result, const DspStreamBlock &block) {
    require(result.error.isEmpty() && result.iq && *result.iq == block.iq &&
                result.spectra.size() == 1, "second-socket DDC stream failed");
    require(result.sampleRate == block.sampleRate && result.firstSample == block.firstSample &&
                result.sampleCount == block.iq.size() / 2 && result.rateNumerator == 1 &&
                result.rateDenominator == 1 && result.inputReserve == 0,
            "manager changed native DDC output metadata");
    const auto &spectrum = result.spectra.front();
    require(!spectrum.levels.empty() && spectrum.frequencies.size() == spectrum.levels.size(),
            "DDC spectrum incomplete");
    const auto peak = std::max_element(spectrum.levels.begin(), spectrum.levels.end());
    require(std::abs(*peak + 6.0206f) < 0.01f &&
                std::abs(spectrum.frequencies[peak - spectrum.levels.begin()] - 100101500.0) < 10,
            "DDC tone amplitude/frequency changed while main FFT was active");
}

// A protocol peer that stops at main_fft_run, making cancellation/stale-reply
// checks deterministic. Successful offloads always use the real compute server.
struct HeldPeer {
    QTcpServer server;
    QTcpSocket *socket = nullptr;
    QByteArray buffer;
    QString job, error;
    quint64 next = 0;
    int length = 0;
    bool runSeen = false;
    DspStreamEndpoint endpoint;

    HeldPeer() {
        require(server.listen(QHostAddress::LocalHost, 0), "controlled peer bind failed");
        endpoint.enabled = true;
        endpoint.address = QStringLiteral("127.0.0.1");
        endpoint.port = server.serverPort();
        endpoint.token = QString(64, 'b');
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            if (socket) {
                auto *extra = server.nextPendingConnection();
                extra->abort();
                extra->deleteLater();
                return;
            }
            socket = server.nextPendingConnection();
            socket->setReadBufferSize(DspStreamProtocol::MaxPayloadBytes + DspStreamProtocol::MaxJsonBytes + 16);
            QObject::connect(socket, &QTcpSocket::readyRead, &server, [this] { receive(); });
            receive();
        });
    }

    void send(const QJsonObject &header) {
        const auto packet = DspStreamProtocol::encode(header);
        if (!socket || socket->write(packet) != packet.size()) error = QStringLiteral("controlled peer write failed");
    }

    void receive() {
        buffer.append(socket->readAll());
        while (error.isEmpty()) {
            QJsonObject header;
            QByteArray payload;
            const int parsed = DspStreamProtocol::extract(buffer, header, payload, error);
            if (parsed <= 0) return;
            const QString type = header.value("type").toString();
            if (type == "hello") {
                send({{"type", "hello"}, {"version", 1}, {"mainFftVersion", 1}, {"mainFftMaxLength", 1048576}});
            } else if (type == "main_fft_begin") {
                job = header.value("job").toString();
                length = header.value("length").toInt();
                send({{"type", "main_fft_ready"}, {"job", job}});
            } else if (type == "main_fft_iq") {
                quint64 first = 0;
                if (!SpectrumComputeProtocol::readCounter(header.value("first"), first)) {
                    error = QStringLiteral("worker sent an invalid sample counter");
                    return;
                }
                next = first + static_cast<quint64>(payload.size() / 8);
                send({{"type", "main_fft_ack"}, {"job", job}, {"next", QString::number(next)},
                      {"filled", length}});
            } else if (type == "main_fft_run") {
                runSeen = true;
            } else {
                error = QStringLiteral("worker sent an unexpected command");
            }
        }
    }

    void sendStaleResult() {
        send({{"type", "main_fft_result"}, {"job", job + "-stale"}, {"length", length},
              {"next", QString::number(next)}, {"dspMs", 0.0}});
    }
};

QString workerStatus(const RemoteSpectrumWorker &worker) { return worker.status(); }

QString workerStatus(const SpectrumFftWorker &worker) {
    return worker.remoteEnabled() ? worker.remoteStatus()
        : QStringLiteral("local FFT; resultReady=%1; workOutstanding=%2")
              .arg(worker.resultReady()).arg(worker.workOutstanding());
}

template <typename Worker>
SpectrumFftFrame awaitFrame(Worker &worker, const char *message, const std::function<void()> &renew = {}) {
    SpectrumFftFrame frame;
    const int timeoutMs = IqBuffer::stats().snapshotSize == 0 ? 1500 : 10000;
    if (!waitUntil([&] { return worker.takeLatest(frame); }, timeoutMs, renew)) {
        const auto iq = IqBuffer::stats();
        throw std::runtime_error(QStringLiteral("%1 | status: %2 | retained=%3 samples, total=%4 floats, epoch=%5, audioBlocks=%6")
            .arg(QString::fromLatin1(message), workerStatus(worker))
            .arg(qulonglong(iq.snapshotSize / 2)).arg(qulonglong(iq.totalFloatCount))
            .arg(qulonglong(iq.epoch)).arg(qulonglong(iq.queuedBlocks)).toStdString());
    }
    return frame;
}

void checkFrame(const SpectrumFftFrame &frame, const RadioSettings &settings,
                const std::vector<float> &iq, const IqBuffer::Stats &published,
                double sampledCenter = -1.0) {
    const double center = sampledCenter >= 0.0 ? sampledCenter : settings.centerFrequency;
    if (!frame.valid || !frame.error.isEmpty()) {
        throw std::runtime_error("worker frame failed: " + frame.error.toStdString());
    }
    require(!frame.badAlloc && frame.requestId != 0 && frame.settings.fftLength == settings.fftLength &&
                frame.settings.fftWindowType == settings.fftWindowType &&
                frame.settings.sampleRate == settings.sampleRate &&
                frame.settings.centerFrequency == settings.centerFrequency,
            "worker frame has stale settings");
    require(frame.metadata.valid && frame.metadata.epoch == published.epoch &&
                frame.metadata.totalFloatCount == published.totalFloatCount &&
                frame.metadata.centerFrequencyHz == center,
            "worker frame has stale receiver metadata");
    compare(frame.magnitudes, directLevels(settings, iq));
    require(frame.frequencies.size() == frame.magnitudes.size() && frame.referenceMagnitudes.empty(),
            "RF frame axis/reference dimensions differ");
    for (int i = 0; i < settings.fftLength; ++i) {
        require(frame.frequencies[i] == static_cast<float>(center +
                    (i - settings.fftLength / 2) * (settings.sampleRate / settings.fftLength)),
                "worker frame frequency axis changed");
    }
    checkAudioQueue(published);
}

template <typename Worker>
void expectNoFrame(Worker &worker, const char *message) {
    SpectrumFftFrame frame;
    require(!waitUntil([&] { return worker.takeLatest(frame); }, 150), message);
}

SpectrumComputeOptions remoteOptions(SpectrumComputeMode mode = SpectrumComputeMode::Windows) {
    SpectrumComputeOptions options;
    options.mode = mode;
    options.backend = FFT_BACKEND_CPU_FFTW;
    return options;
}

void requestRemote(RemoteSpectrumWorker &worker, const RadioSettings &settings,
                   const DspStreamEndpoint &endpoint, const SpectrumComputeOptions &options) {
    worker.request(settings, endpoint, options, 0, 0, FFT_BACKEND_CPU_FFTW);
}

SpectrumFftFrame awaitRemoteFrame(RemoteSpectrumWorker &worker, const RadioSettings &settings,
                                  const DspStreamEndpoint &endpoint, const SpectrumComputeOptions &options,
                                  const char *message) {
    return awaitFrame(worker, message, [&] { requestRemote(worker, settings, endpoint, options); });
}

void remoteLoopbackTests() {
    LoopbackServer loopback;
    DspStreamManager ddc;
    ddc.configure(loopback.endpoint);
    auto block = ddcTone();
    checkDdc(ddc.process(block, true), block);

    RemoteSpectrumWorker worker;
    auto options = remoteOptions();
    options.uploadLimitMBps = 1;
    auto settings = radioSettings(ChunkSamples * 2 + 7, FFT_WINDOW_BLACKMAN_HARRIS);
    auto iq = makeIq(settings.fftLength);
    auto published = publishIq(settings, iq, 901);
    const auto checkRemoteFrame = [&](const char *phase, const std::vector<float> &source,
                                      double sampledCenter = -1.0) {
        QElapsedTimer timer;
        timer.start();
        try {
            checkFrame(awaitRemoteFrame(worker, settings, loopback.endpoint, options, phase),
                       settings, source, published, sampledCenter);
        } catch (const std::exception &error) {
            const auto stats = IqBuffer::stats();
            throw std::runtime_error(QStringLiteral(
                "%1 | phase: %2 | worker.status: %3 | server.status: %4"
                " | length=%5, mode=%6, uploadLimit=%7 MB/s, awaitMs=%8"
                " | retained=%9 samples, total=%10 floats, epoch=%11, audioBlocks=%12")
                .arg(QString::fromUtf8(error.what()), QString::fromLatin1(phase),
                     worker.status(), loopback.server.status())
                .arg(settings.fftLength).arg(static_cast<int>(options.mode)).arg(options.uploadLimitMBps)
                .arg(timer.elapsed()).arg(qulonglong(stats.snapshotSize / 2))
                .arg(qulonglong(stats.totalFloatCount)).arg(qulonglong(stats.epoch))
                .arg(qulonglong(stats.queuedBlocks)).toStdString());
        }
    };
    requestRemote(worker, settings, loopback.endpoint, options);
    ++block.generation;
    block.firstSample += block.iq.size() / 2;
    checkDdc(ddc.process(block, true), block);
    checkRemoteFrame("chunked remote window (1 MB/s)", iq);
    ++block.generation;
    block.firstSample += block.iq.size() / 2;
    checkDdc(ddc.process(block, true), block);
    checkQueuedIq(iq);

    settings = radioSettings(1001, FFT_WINDOW_FLAT_TOP);
    settings.sampleRate = 192000;
    settings.centerFrequency += 500000;
    iq = makeIq(settings.fftLength, 101);
    published = publishIq(settings, iq, 902);
    options.uploadLimitMBps = 100;
    requestRemote(worker, settings, loopback.endpoint, options);
    checkRemoteFrame("retuned remote window", iq);
    checkQueuedIq(iq);

    auto sampled = settings;
    sampled.centerFrequency += 250000;
    iq = makeIq(settings.fftLength, 157);
    published = publishIq(sampled, iq, 903);
    requestRemote(worker, settings, loopback.endpoint, options);
    checkRemoteFrame("sampled-center correction", iq, sampled.centerFrequency);
    checkQueuedIq(iq);

    options.mode = SpectrumComputeMode::Continuous;
    settings = radioSettings(777, FFT_WINDOW_HAMMING);
    const auto oldHistory = makeIq(SpectrumComputeProtocol::ChunkSamples * 3, 17);
    publishIq(settings, oldHistory, 904);
    checkQueuedIq(oldHistory);
    iq = makeIq(settings.fftLength, 211);
    published = publishIq(settings, iq, 904, false);
    requestRemote(worker, settings, loopback.endpoint, options);
    checkRemoteFrame("initial continuous remote FFT", iq);
    checkQueuedIq(iq);
    const auto next = makeIq(settings.fftLength, 211 + settings.fftLength);
    published = publishIq(settings, next, 904, false);
    requestRemote(worker, settings, loopback.endpoint, options);
    checkRemoteFrame("rolling continuous remote FFT", next);
    checkQueuedIq(next);
    worker.cancel();
    expectNoFrame(worker, "continuous worker published after cancel");
    std::cout << "Remote loopback: chunked/arbitrary FFT, retune, continuous tail, second-socket DDC and audio queue passed\n";
}

void remoteFailureTests() {
    LoopbackServer loopback;
    const auto settings = radioSettings();
    const auto iq = makeIq(settings.fftLength);
    auto options = remoteOptions();
    auto wrongToken = loopback.endpoint;
    wrongToken.token = QString(64, 'x');
    auto disabled = loopback.endpoint;
    disabled.enabled = false;
    quint64 epoch = 951;
    for (const auto &endpoint : {wrongToken, disabled}) {
        RemoteSpectrumWorker worker;
        options.fallbackToLocal = false;
        const auto published = publishIq(settings, iq, epoch++);
        requestRemote(worker, settings, endpoint, options);
        const auto frame = awaitRemoteFrame(worker, settings, endpoint, options,
                                           "remote failure did not reach takeLatest");
        require(!frame.valid && !frame.error.isEmpty(), "disabled/unauthenticated worker returned valid FFT");
        checkAudioQueue(published);
        checkQueuedIq(iq);
    }
    {
        RemoteSpectrumWorker worker;
        options.fallbackToLocal = true;
        const auto published = publishIq(settings, iq, epoch++);
        requestRemote(worker, settings, wrongToken, options);
        checkFrame(awaitRemoteFrame(worker, settings, wrongToken, options,
                                    "authentication local fallback timed out"), settings, iq, published);
        const auto status = worker.status();
        require(status.contains("LOCAL FALLBACK") && status.contains("authentication", Qt::CaseInsensitive),
                "authentication fallback did not show its local label and reason");
        checkQueuedIq(iq);
    }
    loopback.server.stop();
    {
        RemoteSpectrumWorker worker;
        options.fallbackToLocal = false;
        const auto published = publishIq(settings, iq, epoch++);
        requestRemote(worker, settings, loopback.endpoint, options);
        const auto frame = awaitRemoteFrame(worker, settings, loopback.endpoint, options,
                                           "disconnected worker did not report failure");
        require(!frame.valid && !frame.error.isEmpty(), "disconnected worker silently used local FFT");
        checkAudioQueue(published);
        checkQueuedIq(iq);
    }
    {
        RemoteSpectrumWorker worker;
        options.fallbackToLocal = true;
        const auto published = publishIq(settings, iq, epoch++);
        requestRemote(worker, settings, loopback.endpoint, options);
        checkFrame(awaitRemoteFrame(worker, settings, loopback.endpoint, options,
                                    "explicit local fallback timed out"), settings, iq, published);
        require(worker.status().contains("LOCAL FALLBACK"), "fallback was not identified in worker status");
        checkQueuedIq(iq);
    }
    std::cout << "Remote failure reporting, disabled endpoint and explicit local fallback passed\n";
}

void remoteCancellationTests() {
    LoopbackServer loopback;
    auto settings = radioSettings();
    auto iq = makeIq(settings.fftLength);
    const auto options = remoteOptions();
    quint64 epoch = 971;
    for (bool cancelFirst : {false, true}) {
        HeldPeer held;
        RemoteSpectrumWorker worker;
        auto published = publishIq(settings, iq, epoch++);
        requestRemote(worker, settings, held.endpoint, options);
        require(waitUntil([&] { return held.runSeen || !held.error.isEmpty(); }, 10000,
                          [&] { requestRemote(worker, settings, held.endpoint, options); }) && held.runSeen &&
                    held.error.isEmpty(), "remote request did not reach the cancellation barrier");
        if (cancelFirst) {
            QElapsedTimer timer;
            timer.start();
            worker.cancel();
            require(timer.elapsed() < 1500, "cancel blocked on remote I/O");
            expectNoFrame(worker, "cancelled frame reached takeLatest");
        }
        checkAudioQueue(published);
        checkQueuedIq(iq);
        settings = radioSettings(1001, FFT_WINDOW_BLACKMAN_HARRIS);
        settings.centerFrequency += epoch * 10000;
        iq = makeIq(settings.fftLength, static_cast<int>(epoch));
        published = publishIq(settings, iq, epoch++);
        requestRemote(worker, settings, loopback.endpoint, options);
        checkFrame(awaitRemoteFrame(worker, settings, loopback.endpoint, options,
                                    "replacement request after cancel/reconfigure timed out"),
                   settings, iq, published);
        expectNoFrame(worker, "superseded request published an old frame");
        checkQueuedIq(iq);
    }
    {
        HeldPeer held;
        RemoteSpectrumWorker worker;
        const auto published = publishIq(settings, iq, epoch++);
        requestRemote(worker, settings, held.endpoint, options);
        require(waitUntil([&] { return held.runSeen || !held.error.isEmpty(); }, 10000,
                          [&] { requestRemote(worker, settings, held.endpoint, options); }) && held.runSeen &&
                    held.error.isEmpty(), "remote request never reached stale-reply barrier");
        held.sendStaleResult();
        const auto frame = awaitRemoteFrame(worker, settings, held.endpoint, options,
                                           "stale reply did not terminate the request");
        require(!frame.valid && !frame.error.isEmpty(), "stale job reply was accepted");
        checkAudioQueue(published);
        checkQueuedIq(iq);
    }
    std::cout << "Remote cancellation, in-flight config replacement and stale-reply rejection passed\n";
}

void wrapperToggleTests() {
    LoopbackServer loopback;
    SpectrumFftWorker worker;
    auto settings = radioSettings();
    auto iq = makeIq(settings.fftLength);
    auto published = publishIq(settings, iq, 801);
    worker.request(settings, FFT_BACKEND_CPU_FFTW);
    checkFrame(awaitFrame(worker, "initial wrapper local FFT timed out"), settings, iq, published);
    checkQueuedIq(iq);

    SpectrumComputeOptions remote;
    remote.mode = SpectrumComputeMode::Windows;
    remote.backend = FFT_BACKEND_CPU_FFTW;
    worker.configureRemote(loopback.endpoint, remote);
    require(worker.remoteEnabled(), "wrapper did not enable remote mode");
    settings = radioSettings(1001, FFT_WINDOW_FLAT_TOP);
    settings.centerFrequency += 250000;
    iq = makeIq(settings.fftLength, 23);
    published = publishIq(settings, iq, 802);
    worker.request(settings, FFT_BACKEND_CPU_FFTW);
    checkFrame(awaitFrame(worker, "wrapper remote FFT timed out",
                          [&] { worker.request(settings, FFT_BACKEND_CPU_FFTW); }), settings, iq, published);
    checkQueuedIq(iq);

    SpectrumComputeOptions local;
    worker.configureRemote(loopback.endpoint, local);
    require(!worker.remoteEnabled(), "wrapper did not restore local mode");
    expectNoFrame(worker, "remote result leaked through a local-mode toggle");
    settings = radioSettings(1024, FFT_WINDOW_HAMMING);
    iq = makeIq(settings.fftLength, 46);
    published = publishIq(settings, iq, 803);
    worker.request(settings, FFT_BACKEND_CPU_FFTW);
    checkFrame(awaitFrame(worker, "restored wrapper local FFT timed out"), settings, iq, published);
    checkQueuedIq(iq);

    HeldPeer held;
    worker.configureRemote(held.endpoint, remote);
    published = publishIq(settings, iq, 804);
    worker.request(settings, FFT_BACKEND_CPU_FFTW);
    require(waitUntil([&] { return held.runSeen || !held.error.isEmpty(); }, 10000,
                      [&] { worker.request(settings, FFT_BACKEND_CPU_FFTW); }) && held.runSeen,
            "wrapper request never reached controlled remote peer");
    require(held.error.isEmpty(), "controlled peer rejected wrapper protocol");
    QElapsedTimer timer;
    timer.start();
    worker.configureRemote(loopback.endpoint, local);
    require(timer.elapsed() < 1500, "remote-to-local toggle blocked on network I/O");
    expectNoFrame(worker, "cancelled wrapper remote frame was published");
    checkAudioQueue(published);
    checkQueuedIq(iq);
    settings = radioSettings(1537, FFT_WINDOW_BLACKMAN_HARRIS);
    settings.centerFrequency += 500000;
    iq = makeIq(settings.fftLength, 91);
    published = publishIq(settings, iq, 805);
    worker.request(settings, FFT_BACKEND_CPU_FFTW);
    checkFrame(awaitFrame(worker, "local FFT after in-flight remote cancellation timed out"),
               settings, iq, published);
    expectNoFrame(worker, "old remote frame arrived after the replacement local frame");
    checkQueuedIq(iq);
    std::cout << "Wrapper Local -> Remote -> Local, in-flight toggle and stale-frame suppression passed\n";
}

#include "remote_fft_pipeline_tests.h"

void loopbackBenchmark() {
    LoopbackServer loopback;
    quint64 epoch = 2001;
    std::cout << "Native TCP loopback only; FFTW; 1 cold + 9 warm frames; no LAN/GPU claim\n";
    for (auto mode : {SpectrumComputeMode::Windows, SpectrumComputeMode::Continuous}) {
        for (int length : {65536, 1048576}) {
            for (int budget : {100, 2000}) {
                RemoteSpectrumWorker worker;
                auto options = remoteOptions(mode);
                options.uploadLimitMBps = budget;
                const auto settings = radioSettings(length);
                const auto iq = makeIq(length);
                const auto reference = directLevels(settings, iq);
                std::vector<double> warm;
                double cold = 0;
                for (int i = 0; i < 10; ++i) {
                    const auto published = publishIq(settings, iq, epoch, i == 0);
                    QElapsedTimer timer;
                    timer.start();
                    requestRemote(worker, settings, loopback.endpoint, options);
                    SpectrumFftFrame frame;
                    qint64 renewAt = 250;
                    while (!worker.takeLatest(frame) && timer.elapsed() < 30000) {
                        QCoreApplication::processEvents();
                        if (timer.elapsed() >= renewAt) {
                            requestRemote(worker, settings, loopback.endpoint, options);
                            renewAt = timer.elapsed() + 250;
                        }
                        QThread::msleep(1);
                    }
                    const double ms = timer.nsecsElapsed() / 1e6;
                    require(frame.valid && frame.metadata.totalFloatCount == published.totalFloatCount,
                            "benchmark failed or returned a stale frame");
                    compare(frame.magnitudes, reference);
                    checkAudioQueue(published);
                    checkQueuedIq(iq);
                    if (i == 0) cold = ms;
                    else warm.push_back(ms);
                }
                std::sort(warm.begin(), warm.end());
                std::cout << (mode == SpectrumComputeMode::Windows ? "windows" : "continuous")
                          << " N=" << length << " cap_MBps=" << budget << " cold_ms=" << cold
                          << " warm_median_ms=" << warm[4] << " warm_p95_ms=" << warm.back() << '\n';
                worker.cancel();
                ++epoch;
            }
        }
    }
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("spectrum-compute-selftest"));
    QStandardPaths::setTestModeEnabled(true);
    setFobosVerboseLoggingEnabled(false);
    try {
        if (app.arguments().contains(QStringLiteral("--benchmark"))) {
            loopbackBenchmark();
            return 0;
        }
        windowSessionTests();
        continuousSessionTests();
        invalidSessionTests();
        pipelineSessionTests();
        pipelineWorkerTests();
        pipelineFailureTests();
        fallbackRetryTests();
        fallbackRecoveryTests();
        managerMetadataCoexistenceTest();
        remoteLoopbackTests();
        remoteFailureTests();
        remoteCancellationTests();
        wrapperToggleTests();
        std::cout << "Spectrum compute self-test passed: " << sessionFrames << " session frames\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
