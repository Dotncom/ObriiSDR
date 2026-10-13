#include "dspstreammanager.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

bool dspStreamRecorderSelfTest();
bool dspConnectionProbeSelfTest();

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
DspStreamBlock tone(std::size_t count = 4096) {
    DspStreamBlock b; b.stream = "channel"; b.generation = 1; b.epoch = 9;
    b.sampleRate = 96000; b.centerHz = 100100000; b.returnIq = true;
    b.ffts.append({"fft", 2048, FFT_WINDOW_HANN, 20}); b.iq.resize(count*2);
    for (std::size_t i = 0; i < count; ++i) {
        const double p = 6.28318530717958647692*1500*i/b.sampleRate;
        b.iq[i*2] = float(0.5*std::cos(p)); b.iq[i*2+1] = float(0.5*std::sin(p));
    }
    return b;
}
void checkTone(const DspStreamResult &r) {
    require(r.error.isEmpty() && r.spectra.size() == 1, "Tone result missing");
    const auto &f = r.spectra.front();
    const auto peak = std::max_element(f.levels.begin(), f.levels.end());
    require(std::abs(*peak+6.0206) < 0.01 && std::abs(f.frequencies[peak-f.levels.begin()]-100101500) < 10, "Tone amplitude/frequency wrong");
    require(f.rbwHz > 70 && f.rbwHz < 71, "ENBW/RBW incorrect");
}
void codecTests() {
    auto b = tone(); auto packet = DspStreamProtocol::encode(DspStreamProtocol::request(b), DspStreamProtocol::floats(b.iq));
    QByteArray partial, payload; QJsonObject json; QString error;
    for (int i = 0; i < packet.size()-1; ++i) {
        partial.append(packet[i]);
        require(DspStreamProtocol::extract(partial, json, payload, error) == 0, "Fragmentation parser consumed incomplete data");
    }
    partial.append(packet.back());
    require(DspStreamProtocol::extract(partial, json, payload, error) == 1 && partial.isEmpty(), "Complete packet rejected");
    DspStreamBlock decoded;
    require(DspStreamProtocol::decodeRequest(json, payload, decoded, error) && decoded.iq == b.iq, "IQ wire roundtrip changed samples");
    b.firstSample = (std::uint64_t(1)<<60)+17;
    require(DspStreamProtocol::decodeRequest(DspStreamProtocol::request(b), payload, decoded, error) && decoded.firstSample == b.firstSample, "64-bit sample clock lost precision");
    json["firstSample"] = -1;
    require(!DspStreamProtocol::decodeRequest(json, payload, decoded, error), "Negative sample position accepted");
    partial = QByteArray("OBDSP001", 8)+QByteArray(8, char(0xff));
    require(DspStreamProtocol::extract(partial, json, payload, error) == -1, "Oversized packet accepted");
    partial = QByteArray("WRONG001", 8)+QByteArray(8, char(0));
    require(DspStreamProtocol::extract(partial, json, payload, error) == -1, "Bad protocol signature accepted");
    b.iq[0] = std::numeric_limits<float>::quiet_NaN();
    require(!DspStreamProtocol::validate(b).isEmpty(), "Nonfinite samples accepted");
    b = tone(); b.ffts[0].length = 1500;
    require(!DspStreamProtocol::validate(b).isEmpty(), "Invalid FFT length accepted");
    std::cout << "Framing, finite bounds, metadata and 64-bit sample clock: passed\n";
}
void processorTests() {
    DspStreamProcessor processor;
    auto b = tone(); auto r = processor.process(b); checkTone(r);
    QByteArray response = DspStreamProtocol::response(b, r), payload;
    QJsonObject json; QString error; DspStreamResult decoded;
    require(DspStreamProtocol::extract(response, json, payload, error) == 1 &&
        DspStreamProtocol::decodeResponse(b, json, payload, decoded) && decoded.iq && *decoded.iq == b.iq, "Result roundtrip failed");
    checkTone(decoded);
    json["generation"] = "0";
    require(!DspStreamProtocol::decodeResponse(b, json, payload, decoded), "Stale generation accepted");
    processor.clear(); b = tone(1024);
    require(processor.process(b).spectra.empty(), "FFT emitted before full observation");
    b.firstSample += 1024; checkTone(processor.process(b));
    b.firstSample += 1024; b.gap = true;
    require(processor.process(b).spectra.empty(), "Gap retained old FFT history");
    b.gap = false; b.firstSample += 1024; checkTone(processor.process(b));
    b.firstSample += 1024; ++b.epoch;
    require(processor.process(b).spectra.empty(), "Epoch retained old FFT history");
    b.firstSample += 5000;
    require(processor.process(b).spectra.empty(), "Noncontiguous sample positions retained history");
    processor.clear(); b = tone(1024);
    require(processor.process(b).spectra.empty(), "Unexpected initial FFT");
    b.returnIq = false; b.firstSample += 1024;
    checkTone(processor.process(b));
    b.stream = "second"; b.firstSample = 0;
    require(processor.process(b).error.isEmpty(), "Second stream rejected");
    b.stream = "third";
    require(!processor.process(b).error.isEmpty(), "Third stream exceeded the history budget");
    processor.clear(); b = tone(4096);
    for (int i = 0; i < 19; ++i) {
        b.firstSample = std::uint64_t(i)*4096;
        r = processor.process(b);
        require(r.error.isEmpty(), "Ring buffer wrap failed");
    }
    QThread::msleep(25); b.firstSample += 4096;
    checkTone(processor.process(b));
    std::cout << "Window/FFT calibration, returned IQ, gaps, epochs and stale replies: passed\n";
}
void networkTests(const DspStreamEndpoint &endpoint, bool native = false) {
    DspStreamManager manager; manager.configure(endpoint);
    auto b = tone(); const auto remote = manager.process(b, true); checkTone(remote);
    require(remote.iq && *remote.iq == b.iq, "External IQ differs");
    auto bad = endpoint; bad.token = QString(64, 'x');
    manager.configure(bad);
    const auto rejected = manager.process(b, true);
    require(!rejected.error.isEmpty(), "Invalid session token accepted");
    if (native) require(rejected.error.contains("rejected the session token"), "Native token rejection reason was lost");
    require(manager.process(b, true).error == rejected.error, "Retry hid the authentication failure");
    manager.configure(endpoint); b.generation++;
    checkTone(manager.process(b, true));
    manager.clear();
    std::cout << "Authenticated external FFT/IQ, rejected wrong token and reconnect: passed\n";
}
void benchmark() {
    constexpr int rounds = 2400;
    auto b = tone(4096); b.returnIq = false; b.ffts.clear();
    DspStreamProcessor direct; DspStreamManager manager;
    const auto run = [&](bool managed) {
        QElapsedTimer timer; timer.start();
        for (int i = 0; i < rounds; ++i) {
            b.firstSample = std::uint64_t(i)*4096;
            const auto r = managed ? manager.process(b, false) : direct.process(b);
            require(r.error.isEmpty(), "Benchmark stream failed");
        }
        return timer.nsecsElapsed()/1e6;
    };
    for (const bool returnIq : {false, true}) {
        b.returnIq = returnIq;
        run(false); run(true);
        std::vector<double> base, routed;
        for (int repeat = 0; repeat < 7; ++repeat) {
            if (repeat%2) { routed.push_back(run(true)); base.push_back(run(false)); }
            else { base.push_back(run(false)); routed.push_back(run(true)); }
        }
        std::sort(base.begin(), base.end()); std::sort(routed.begin(), routed.end());
        std::cout << "Local routing median (7 alternating runs, returnIQ=" << returnIq << "): " << rounds*4096
            << " complex samples, processor=" << base[3] << " ms, manager=" << routed[3]
            << " ms, ratio=" << routed[3]/base[3] << " (not a hardware RX benchmark)\n";
    }
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        codecTests(); processorTests(); require(dspStreamRecorderSelfTest(), "Recorder tests failed");
        require(dspConnectionProbeSelfTest(), "Connection probe tests failed");
        DspStreamEndpoint endpoint; endpoint.enabled = true;
        if (argc == 3) {
            endpoint.address = QString::fromLocal8Bit(argv[1]); endpoint.port = quint16(QString::fromLocal8Bit(argv[2]).toUInt());
            endpoint.token = qEnvironmentVariable("OBRII_DSP_TOKEN"); networkTests(endpoint);
        } else {
            endpoint.token = QString(64, 'a'); DspComputeServer server;
            require(server.start("127.0.0.1", 0, endpoint.token), "Server start rejected");
            for (int i = 0; i < 100 && !server.port(); ++i) QThread::msleep(10);
            endpoint.port = server.port(); require(endpoint.port != 0, "Server bind failed");
            networkTests(endpoint, true);
            DspStreamManager disconnected; disconnected.configure(endpoint);
            auto b = tone(); checkTone(disconnected.process(b, true));
            QElapsedTimer timer; timer.start(); server.stop();
            require(timer.elapsed() < 1000 && !server.running(), "Server stop not bounded");
            std::cout << "Compute worker stop: " << timer.elapsed() << " ms\n";
            timer.restart(); b.firstSample += b.iq.size()/2;
            const auto lost = disconnected.process(b, true);
            require(!lost.error.isEmpty() && timer.elapsed() < 2500,
                    "Disconnected compute worker did not fail within its deadline");
            require(disconnected.process(b, true).error == lost.error, "Retry hid the disconnection failure");
            DspStreamManager unavailable; unavailable.configure(endpoint);
            const auto refused = unavailable.process(b, true);
            require(refused.error.contains("socket error") && refused.error.contains(QString::number(endpoint.port)),
                    "TCP error lost the endpoint or system error");
            require(unavailable.process(b, true).error == refused.error, "Retry hid the TCP failure");
            b.gap = true; checkTone(disconnected.process(b, false));
            std::cout << "Disconnected external worker leaves local processing available: passed\n";
        }
        benchmark(); std::cout << "DSP stream self-test passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
