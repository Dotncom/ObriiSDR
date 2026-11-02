#include "cliruntimerecorder.h"
#include "channelizerutils.h"
#include "iqchannelizer.h"
#include "spectrumframerecorder.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
QJsonObject slot(const CliRuntimeRecorder &r, const QString &type) {
    for (const auto &v : r.status().value("recordings").toArray())
        if (v.toObject().value("type").toString() == type) return v.toObject();
    return {};
}
void until(const std::function<bool()> &predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate()) {
        check(timer.elapsed() < 10000, "worker did not complete within test timeout");
        QThread::msleep(1);
    }
}
void idle(CliRuntimeRecorder &r) {
    until([&] { return !r.active() && !r.status().value("workerRunning").toBool(); });
    r.command({{"operation", "status"}}, {}); // Reap the already-finished thread.
    check(r.status().value("queueBytes").toDouble() == 0, "queue bytes leaked");
    check(r.status().value("queueBlocks").toDouble() == 0, "queue blocks leaked");
}
void start(CliRuntimeRecorder &r, const QString &type, const QString &path, const RadioSettings &s) {
    check(r.command({{"operation", "start"}, {"type", type}, {"path", path}}, s).value("ok").toBool(), "start rejected");
}
QJsonObject stop(CliRuntimeRecorder &r, const QString &type = {}) {
    const auto result = r.command({{"operation", "stop"}, {"type", type}}, {});
    check(result.value("ok").toBool(), "stop rejected"); return result;
}
QByteArray read(const QString &path) {
    QFile f(path); check(f.open(QIODevice::ReadOnly), "cannot read capture"); return f.readAll();
}
void create(const QString &path, const QByteArray &bytes) {
    QFile f(path); check(f.open(QIODevice::WriteOnly | QIODevice::NewOnly), "cannot create test sentinel");
    check(f.write(bytes) == bytes.size(), "cannot write test sentinel");
}
QJsonObject json(const QString &path) {
    const auto doc = QJsonDocument::fromJson(read(path)); check(doc.isObject(), "invalid JSON sidecar"); return doc.object();
}
quint32 u32(const QByteArray &b, int offset) {
    check(offset >= 0 && offset + 4 <= b.size(), "short 32-bit field");
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + offset));
}
quint16 u16(const QByteArray &b, int offset) {
    check(offset >= 0 && offset + 2 <= b.size(), "short 16-bit field");
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(b.constData() + offset));
}
float f32(const QByteArray &b, int offset) {
    quint32 bits = u32(b, offset); float v; std::memcpy(&v, &bits, 4); return v;
}
void wav(const QByteArray &b, int channels, int rate, int payload) {
    check(b.size() == payload + 44 && b.left(4) == "RIFF" && b.mid(8, 8) == "WAVEfmt " && b.mid(36, 4) == "data", "bad WAV chunks");
    check(u32(b, 4) == quint32(payload + 36) && u32(b, 40) == quint32(payload), "wrong final WAV sizes");
    check(u16(b, 20) == 1 && u16(b, 22) == channels && u16(b, 34) == 16, "wrong WAV PCM format");
    check(u32(b, 24) == quint32(rate) && u32(b, 28) == quint32(rate * channels * 2) && u16(b, 32) == channels * 2, "wrong WAV rate/alignment");
}
RadioSettings settings() {
    RadioSettings s; s.sampleRate = 768000; s.centerFrequency = s.actualFrequency = 100e6;
    s.listeningFrequency = 100e6 + 12000; s.bandwidth = 5000; s.modulationType = MOD_USB;
    s.fftLength = 1024; return s;
}
std::vector<float> tone(int n, const RadioSettings &s, int offset = 0) {
    std::vector<float> v(std::size_t(n) * 2);
    const double frequency = s.listeningFrequency - s.centerFrequency + 1000;
    for (int i = 0; i < n; ++i) {
        const double phase = 6.28318530717958647692 * frequency * (i + offset) / s.sampleRate;
        v[std::size_t(i) * 2] = float(0.5 * std::cos(phase));
        v[std::size_t(i) * 2 + 1] = float(0.5 * std::sin(phase));
    }
    return v;
}

void captures(const QTemporaryDir &dir) {
    const auto s = settings();
    CliRuntimeRecorder r;
    check(!r.active() && !r.needsIq() && !r.needsAudio() && !r.needsSpectrum(), "inactive demand");
    check(!r.submitIq(nullptr, 0, s) && !r.submitAudio("x") && !r.submitSpectrum({}, 0, 0, s), "inactive submission");
    check(!r.status().value("workerRunning").toBool(), "inactive worker started");
    const QString cf = dir.filePath("all.cf32"), ci = dir.filePath("all.iq8");
    const QString ch = dir.filePath("channel.wav"), au = dir.filePath("audio.wav"), sp = dir.filePath("spectrum.fbspec");
    start(r, "cf32", cf, s); start(r, "ci8", ci, s); start(r, "channel", ch, s);
    start(r, "audio", au, s); start(r, "spectrum", sp, s);
    check(r.status().value("activeTypes").toInt() == 5 && r.needsIq() && r.needsAudio() && r.needsSpectrum(), "five-slot demand");
    const auto data = tone(32768, s);
    check(r.submitIq(data.data(), data.size() / 2, s), "IQ rejected");
    QByteArray pcm;
    for (int i = 0; i < 480; ++i) {
        const qint16 v = qint16(std::lround(10000 * std::sin(6.283185307179586 * i / 48)));
        const quint16 le = qToLittleEndian(quint16(v)); pcm.append(reinterpret_cast<const char *>(&le), 2);
    }
    check(r.submitAudio(pcm), "audio rejected");
    std::vector<float> levels(1024, -110); levels[17] = -23.5f;
    const double first = s.centerFrequency - s.sampleRate * 0.5, bin = s.sampleRate / 1024;
    const auto utc = QDateTime::currentMSecsSinceEpoch();
    check(r.submitSpectrum(levels, first, bin, s, false, utc), "spectrum rejected");
    stop(r, "cf32");
    check(r.active(), "other recordings lost on single stop");
    until([&] { return slot(r, "cf32").value("state") == "completed"; });
    check(r.status().value("workerRunning").toBool(), "worker stopped while other slots active");
    check(r.submitAudio(pcm), "other type did not continue");
    const auto second = tone(8192, s, 32768);
    check(r.submitIq(second.data(), second.size() / 2, s), "second IQ rejected");
    levels[18] = -31;
    check(r.submitSpectrum(levels, first, bin, s, false, utc + 12), "second spectrum rejected");
    const auto stopping = stop(r);
    check(stopping.value("active").toBool() && !r.needsIq() && !r.needsAudio() && !r.needsSpectrum(), "stopping must stay active without accepting input");
    idle(r);
    for (const auto type : {"cf32", "ci8", "channel", "audio", "spectrum"})
        check(slot(r, type).value("state") == "completed", "capture failed");
    const auto raw = read(cf);
    check(raw.size() == int(data.size() * 4), "cf32 byte count");
    for (std::size_t i = 0; i < data.size(); ++i) check(f32(raw, int(i * 4)) == data[i], "cf32 not exact");
    const auto i8 = read(ci);
    check(i8.size() == int(data.size() + second.size()), "ci8 byte count");
    for (int i = 0; i < i8.size(); ++i) {
        const auto &v = i < int(data.size()) ? data : second;
        const int index = i < int(data.size()) ? i : i - int(data.size());
        check(qint8(i8[i]) == qint8(std::lround(v[std::size_t(index)] * 127)), "ci8 quantization");
    }
    wav(read(au), 1, 48000, pcm.size() * 2);
    check(read(au).mid(44) == pcm + pcm, "PCM payload changed");
    IqChannelizer reference; reference.reset();
    std::vector<float> out1, out2;
    const auto result = reference.processFloatIq(data.data(), data.size(), s, out1, false);
    reference.processFloatIq(second.data(), second.size(), s, out2, false);
    const auto channel = read(ch);
    wav(channel, 2, int(result.outputRate), int((out1.size() + out2.size()) * 2));
    for (std::size_t i = 0; i < out1.size() + out2.size(); ++i) {
        const float v = i < out1.size() ? out1[i] : out2[i - out1.size()];
        check(qint16(u16(channel, 44 + int(i * 2))) == qint16(std::lround(v * 32767)), "channel IQ differs from shared channelizer");
    }
    double power = 0, crossRe = 0, crossIm = 0;
    for (std::size_t i = 512; i + 3 < out1.size(); i += 2) {
        const double re = out1[i], im = out1[i + 1], nextRe = out1[i + 2], nextIm = out1[i + 3];
        power += re * re + im * im;
        crossRe += re * nextRe + im * nextIm; crossIm += re * nextIm - im * nextRe;
    }
    const double hz = std::atan2(crossIm, crossRe) * result.outputRate / 6.283185307179586;
    check(std::abs(hz - 1000) < 2 && power > 1, "analytic channel tone lost or not mixed to 1 kHz");
    for (const auto &path : {cf, ci}) {
        const auto m = json(path + ".json"); const auto sig = json(path + ".sigmf-meta");
        const quint64 n = path == cf ? data.size() / 2 : (data.size() + second.size()) / 2;
        check(m.value("sampleCount").toDouble() == double(n) && m.value("state") == "completed", "raw final counts");
        check(m.value("sourceSampleRate").toDouble() == s.sampleRate && m.value("actualFrequency").toDouble() == s.actualFrequency, "actual source metadata");
        const auto global = sig.value("global").toObject();
        check(global.value("core:datatype") == (path == cf ? "cf32_le" : "ci8") &&
              global.value("core:sample_rate").toDouble() == s.sampleRate &&
              global.value("core:dataset") == QFileInfo(path).fileName() &&
              global.value("obrii:sample_count").toDouble() == double(n), "SigMF final metadata");
        check(sig.value("captures").toArray().at(0).toObject().value("core:frequency").toDouble() == s.centerFrequency, "SigMF center");
    }
    check(json(au + ".json").value("sampleCount").toInt() == 960, "audio final sample count");
    check(json(ch + ".json").value("sampleCount").toDouble() == double((out1.size() + out2.size()) / 2), "channel final sample count");
    SpectrumFrameRecording recording; QString error;
    check(SpectrumFrameRecorder::loadFile(sp, recording, &error), "existing spectrum reader rejected capture");
    check(recording.frames.size() == 2 && recording.frames[0].magnitudes[17] == -23.5f &&
          recording.frames[0].magnitudes[18] == -110 && recording.frames[1].magnitudes == levels, "spectrum bins changed");
    check(recording.frames[0].minFrequency == first && recording.frames[0].maxFrequency == first + bin * 1023 &&
          recording.frames[1].utcMs == utc + 12, "spectrum geometry/time changed");
    check(json(sp + ".json").value("framesWritten").toInt() == 2 &&
          json(sp + ".json").value("sampleCount").toInt() == 2048, "spectrum final counts");
    check(r.takeNotifications().size() == 5 && r.takeNotifications().isEmpty(), "completion notifications not bounded/drained");
}

void collisionsAndLimits(const QTemporaryDir &dir) {
    const auto s = settings();
    for (const QString suffix : {QString(), QString(".json"), QString(".sigmf-meta")}) {
        CliRuntimeRecorder r;
        const QString path = dir.filePath("collision" + (suffix.isEmpty() ? QString("data") : suffix) + ".cf32");
        create(path + suffix, "preserve-me");
        start(r, "cf32", path, s); idle(r);
        check(slot(r, "cf32").value("state") == "failed" && !slot(r, "cf32").value("error").toString().isEmpty(), "collision not visible");
        check(read(path + suffix) == "preserve-me", "existing file overwritten");
        if (!suffix.isEmpty()) check(!QFileInfo::exists(path), "new data file leaked after sidecar collision");
    }
    CliRuntimeRecorder cap(1024 * 1024, 64, 1);
    start(cap, "cf32", dir.filePath("cap.cf32"), s);
    check(!cap.command({{"operation", "start"}, {"type", "cf32"}, {"path", dir.filePath("duplicate")}}, s).value("ok").toBool(), "duplicate active type allowed");
    check(!cap.command({{"operation", "start"}, {"type", "audio"}, {"path", dir.filePath("cap.wav")}}, s).value("ok").toBool(), "active cap ignored");
    cap.shutdown(); check(!cap.active(), "shutdown did not drain");
    start(cap, "audio", dir.filePath("cap-after.wav"), s); stop(cap); idle(cap);
    check(cap.needsIq() == false, "inactive IQ demand after reuse");
    CliRuntimeRecorder invalid;
    auto bad = s; bad.sampleRate = std::numeric_limits<double>::quiet_NaN();
    check(!invalid.command({{"operation", "start"}, {"type", "cf32"}, {"path", dir.filePath("invalid")}}, bad).value("ok").toBool(), "invalid source accepted");
    check(!invalid.command({{"operation", "start"}, {"type", "audio"}}, s).value("ok").toBool(), "missing path accepted");
    check(!invalid.status().value("workerRunning").toBool(), "rejected start created worker");
    start(invalid, "cf32", dir.filePath("missing-parent/capture.cf32"), s); idle(invalid);
    check(slot(invalid, "cf32").value("state") == "failed", "open failure not exposed");
}

void failures(const QTemporaryDir &dir) {
    const auto s = settings();
    const auto data = tone(1024, s);
    CliRuntimeRecorder tiny(128);
    const QString path = dir.filePath("overflow.cf32");
    start(tiny, "cf32", path, s); start(tiny, "audio", dir.filePath("overflow-unrelated.wav"), s);
    check(!tiny.submitIq(data.data(), 1024, s), "oversized queue block accepted");
    check(!slot(tiny, "cf32").value("error").toString().isEmpty() && tiny.needsAudio(), "overflow failed receiver/unrelated recording");
    check(tiny.status().value("queueBytes").toDouble() <= 128, "queue exceeded budget");
    stop(tiny); idle(tiny);
    check(slot(tiny, "cf32").value("state") == "failed" && QFileInfo(path).size() == 0, "overflow not failed closed");
    check(json(path + ".json").value("state") == "failed", "overflow failure not persisted");
    for (int change = 0; change < 6; ++change) {
        CliRuntimeRecorder r;
        const QString p = dir.filePath(QString("changed-%1.cf32").arg(change));
        start(r, "cf32", p, s);
        check(r.submitIq(data.data(), 1024, s), "prefix IQ rejected");
        auto actual = s;
        if (change == 0) actual.sampleRate *= 2;
        if (change == 1) actual.centerFrequency += 1;
        if (change == 2) ++actual.lnaGain;
        if (change == 3) actual.actualFrequency += 1;
        if (change == 4) ++actual.inputMode;
        check(!r.submitIq(data.data(), 1024, actual, change == 5), "changed/discontinuous data accepted");
        idle(r);
        check(slot(r, "cf32").value("state") == "failed" && QFileInfo(p).size() == 1024 * 8, "complete prefix not retained");
        check(json(p + ".json").value("sampleCount").toInt() == 1024 && json(p + ".json").value("state") == "failed", "failed prefix metadata");
    }
    CliRuntimeRecorder audio;
    start(audio, "audio", dir.filePath("audio-guard.wav"), s);
    check(audio.needsIq() && !audio.needsSpectrum(), "audio source guard demand");
    auto actual = s; actual.listeningFrequency += 10;
    audio.submitIq(nullptr, 0, actual);
    idle(audio); check(slot(audio, "audio").value("state") == "failed", "audio-only source change missed");
    CliRuntimeRecorder channel;
    start(channel, "channel", dir.filePath("channel-guard.wav"), s);
    actual = s; actual.bandwidth *= 2;
    check(!channel.submitIq(data.data(), 1024, actual), "channel width change accepted");
    idle(channel); check(slot(channel, "channel").value("state") == "failed", "channel change not failed");
    CliRuntimeRecorder geometry;
    start(geometry, "spectrum", dir.filePath("geometry.fbspec"), s);
    check(geometry.submitSpectrum({-10, -20}, 99e6, 1000, s), "initial geometry rejected");
    check(!geometry.submitSpectrum({-10, -20}, 99e6, 1001, s), "geometry change accepted");
    idle(geometry); check(slot(geometry, "spectrum").value("state") == "failed", "geometry failure missing");
    CliRuntimeRecorder pcm;
    start(pcm, "audio", dir.filePath("unaligned.wav"), s);
    check(!pcm.submitAudio("x"), "unaligned PCM accepted");
    idle(pcm); check(slot(pcm, "audio").value("state") == "failed", "PCM error missing");
    CliRuntimeRecorder nonfinite;
    start(nonfinite, "ci8", dir.filePath("nan.iq8"), s);
    const float nan[] = {std::numeric_limits<float>::quiet_NaN(), 0};
    check(nonfinite.submitIq(nan, 1, s), "nonfinite block not queued for worker validation");
    idle(nonfinite); check(slot(nonfinite, "ci8").value("state") == "failed", "nonfinite IQ silently replaced");

    CliRuntimeRecorder shared(1024 * 8 + 512);
    start(shared, "cf32", dir.filePath("shared.cf32"), s);
    start(shared, "ci8", dir.filePath("shared.iq8"), s);
    check(shared.submitIq(data.data(), 1024, s), "one shared IQ block charged more than once");
    stop(shared); idle(shared);
    check(slot(shared, "cf32").value("sampleCount").toInt() == 1024 &&
          slot(shared, "ci8").value("sampleCount").toInt() == 1024, "shared formats lost samples");

    CliRuntimeRecorder noBlocks(1024 * 1024, 0);
    start(noBlocks, "audio", dir.filePath("no-blocks.wav"), s);
    check(!noBlocks.submitAudio(QByteArray(2, '\0')), "block-count cap ignored");
    idle(noBlocks); check(slot(noBlocks, "audio").value("state") == "failed", "block-count overflow failure missing");

    auto largeFft = s; largeFft.fftLength = 50000000;
    CliRuntimeRecorder sourceFft;
    const QString largePath = dir.filePath("large-source-fft.fbspec");
    start(sourceFft, "spectrum", largePath, largeFft);
    check(sourceFft.submitSpectrum({-10, -20}, 99e6, 1000, largeFft), "source FFT above stored-bin limit rejected");
    stop(sourceFft); idle(sourceFft);
    SpectrumFrameRecording recording; QString error;
    check(SpectrumFrameRecorder::loadFile(largePath, recording, &error) &&
          recording.frames.front().fftLength == 50000000, "large source FFT incompatible with reader");
    start(sourceFft, "spectrum", dir.filePath("too-many-bins.fbspec"), largeFft);
    check(!sourceFft.submitSpectrum(std::vector<float>(2097153, -100), 99e6, 1, largeFft), "reader bin limit ignored");
    idle(sourceFft); check(slot(sourceFft, "spectrum").value("state") == "failed", "reader limit failure missing");
}

void concurrent(const QTemporaryDir &dir) {
    CliRuntimeRecorder r(8 * 1024 * 1024, 1024);
    const auto s = settings();
    const QString path = dir.filePath("concurrent.wav");
    start(r, "audio", path, s);
    const QByteArray pcm(128, char(0x31));
    std::thread a([&] { for (int i = 0; i < 100; ++i) check(r.submitAudio(pcm), "concurrent submit A"); });
    std::thread b([&] { for (int i = 0; i < 100; ++i) check(r.submitAudio(pcm), "concurrent submit B"); });
    std::thread status([&] { for (int i = 0; i < 100; ++i) { r.active(); r.needsAudio(); r.status(); } });
    a.join(); b.join(); status.join();
    stop(r); idle(r); wav(read(path), 1, 48000, 200 * pcm.size());
    check(json(path + ".json").value("framesWritten").toInt() == 200, "concurrent frames lost");
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir dir; check(dir.isValid(), "temporary directory unavailable");
        captures(dir); collisionsAndLimits(dir); failures(dir); concurrent(dir);
        std::cout << "cli_runtime_recording_selftest: PASS (synthetic captures, guards, queue, concurrency)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "cli_runtime_recording_selftest: FAIL: " << e.what() << '\n'; return 1;
    }
}
