#include "dspstreamprocessor.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok, const char *reason) { if (!ok) throw std::runtime_error(reason); }
DspStreamBlock input(std::size_t count = 513) {
    DspStreamBlock b;
    b.stream = "gnu"; b.generation = 7; b.epoch = 11;
    b.firstSample = (std::uint64_t(1)<<60)+123;
    b.sampleRate = 96000; b.centerHz = 100000000; b.returnIq = true;
    b.iq.assign(count*2, 0.25f);
    return b;
}
void unpack(const DspStreamBlock &b, const DspStreamResult &r, QJsonObject &j, QByteArray &p) {
    auto wire = DspStreamProtocol::response(b, r); QString error;
    require(DspStreamProtocol::extract(wire, j, p, error) == 1, "response framing");
}
void run() {
    auto b = input(); DspStreamProcessor native;
    auto r = native.process(b);
    require(r.error.isEmpty() && r.sampleRate == b.sampleRate && r.firstSample == b.firstSample &&
            r.sampleCount == 513 && r.iq && *r.iq == b.iq && r.gap, "native metadata or samples changed");
    QJsonObject j; QByteArray p; DspStreamResult decoded;
    unpack(b, r, j, p);
    require(!j.contains("output") && DspStreamProtocol::decodeResponse(b, j, p, decoded) &&
            decoded.sampleRate == b.sampleRate && decoded.firstSample == b.firstSample, "legacy response");
    for (int i = 1; i < 8; ++i) {
        b.stream = QString::number(i);
        require(native.process(b).error.isEmpty(), "eight stream admission");
    }
    b.stream = "ninth";
    require(!native.process(b).error.isEmpty(), "ninth stream admitted");

    for (const auto ratio : {std::pair<int, int>{1, 2}, {2, 1}, {3, 2}, {2, 3}, {64, 33}}) {
        b = input(); const int n = ratio.first, d = ratio.second;
        r = {}; r.sampleRate = b.sampleRate*n/d; r.originFirstSample = b.firstSample;
        r.rateNumerator = n; r.rateDenominator = d; r.inputReserve = 2*d; r.gap = true;
        r.sampleCount = (513-r.inputReserve)/d*n;
        r.iq = std::make_shared<const std::vector<float>>(r.sampleCount*2, 0.5f);
        unpack(b, r, j, p);
        require(DspStreamProtocol::decodeResponse(b, j, p, decoded) && decoded.sampleRate == r.sampleRate &&
                decoded.firstSample == 0 && decoded.originFirstSample == b.firstSample && decoded.gap &&
                decoded.iq && decoded.iq->size() == r.sampleCount*2, "rational metadata roundtrip");
        const auto good = j.value("output").toObject();
        for (const QString key : {QString("sampleCount"), QString("rateNumerator"), QString("inputReserve"), QString("sampleRate")}) {
            auto bad = good; bad[key] = -1; j["output"] = bad;
            require(!DspStreamProtocol::decodeResponse(b, j, p, decoded) && !decoded.iq && decoded.spectra.empty(),
                    "invalid output metadata published");
        }
        auto bad = good; bad["firstSample"] = "1"; j["output"] = bad;
        require(!DspStreamProtocol::decodeResponse(b, j, p, decoded), "stale output counter");
        bad = good; bad["originFirstSample"] = QString::number(qulonglong(b.firstSample+1)); j["output"] = bad;
        require(!DspStreamProtocol::decodeResponse(b, j, p, decoded), "future origin");
        j["output"] = good; j["iqBytes"] = p.size()+8;
        require(!DspStreamProtocol::decodeResponse(b, j, p, decoded), "wrong output payload length");
        unpack(b, r, j, p);
        auto nan = std::vector<float>(r.sampleCount*2, 0.0f); nan[0] = std::numeric_limits<float>::quiet_NaN();
        require(!DspStreamProtocol::decodeResponse(b, j, DspStreamProtocol::floats(nan), decoded), "nonfinite output");

        b.firstSample += 513; r.firstSample = r.sampleCount; r.gap = false;
        r.sampleCount = (1026-r.inputReserve)/d*n-r.firstSample;
        r.iq = std::make_shared<const std::vector<float>>(r.sampleCount*2, 0.5f);
        unpack(b, r, j, p);
        require(DspStreamProtocol::decodeResponse(b, j, p, decoded) && decoded.firstSample == r.firstSample && !decoded.gap,
                "continuous rational counter");
    }
    b = input(1); r = {}; r.sampleRate = 48000; r.rateNumerator = 1; r.rateDenominator = 2;
    r.inputReserve = 66; r.originFirstSample = b.firstSample; r.gap = true;
    r.iq = std::make_shared<const std::vector<float>>();
    unpack(b, r, j, p);
    require(DspStreamProtocol::decodeResponse(b, j, p, decoded) && decoded.iq && decoded.iq->empty() && decoded.gap,
            "empty startup output");
    b = input(2048); r.sampleCount = (2048-66)/2;
    r.iq = std::make_shared<const std::vector<float>>(r.sampleCount*2, 0.0f);
    b.ffts.append({"fft", 512, 0, 20});
    DspBranchFrame frame; frame.id = "fft"; frame.levels.assign(512, -100);
    frame.binWidthHz = r.sampleRate/512; frame.rbwHz = frame.binWidthHz; r.spectra = {frame};
    unpack(b, r, j, p);
    require(DspStreamProtocol::decodeResponse(b, j, p, decoded) && decoded.spectra.size() == 1 &&
            decoded.spectra[0].sampleRateHz == r.sampleRate && decoded.spectra[0].sequence == r.sampleCount,
            "processed FFT rate/counter");
    auto spectra = j.value("spectra").toArray(); auto wrong = spectra[0].toObject();
    wrong["binWidthHz"] = b.sampleRate/512; spectra[0] = wrong; j["spectra"] = spectra;
    require(!DspStreamProtocol::decodeResponse(b, j, p, decoded) && !decoded.iq, "input-rate FFT accepted");
    b = input(1); b.firstSample = UINT64_MAX-2; b.ffts.clear();
    r = {}; r.sampleRate = b.sampleRate*2; r.rateNumerator = 2; r.rateDenominator = 1;
    r.inputReserve = 1; r.sampleCount = 2; r.iq = std::make_shared<const std::vector<float>>(4, 0.0f);
    unpack(b, r, j, p);
    require(!DspStreamProtocol::decodeResponse(b, j, p, decoded), "output counter overflow accepted");
}
void decodeFixture(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= 2*(DspStreamProtocol::MaxPayloadBytes+65552), "fixture size/read");
    auto bytes = file.readAll(); QJsonObject j; QByteArray p; QString error;
    DspStreamBlock b; DspStreamResult r;
    require(DspStreamProtocol::extract(bytes, j, p, error) == 1 &&
            DspStreamProtocol::decodeRequest(j, p, b, error), "Python work packet");
    require(DspStreamProtocol::extract(bytes, j, p, error) == 1 && bytes.isEmpty() &&
            DspStreamProtocol::decodeResponse(b, j, p, r) && r.error.isEmpty(), "Python GNU output packet");
    require(r.iq && r.iq->size() == r.sampleCount*2 && r.sampleRate > 0, "Python output metadata");
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc == 2) decodeFixture(QString::fromLocal8Bit(argv[1])); else run();
        std::cout << "GNU rational protocol self-test: PASS\n"; return 0;
    }
    catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
