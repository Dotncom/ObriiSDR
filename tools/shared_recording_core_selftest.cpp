#include "recordingformatcore.h"
#include "recordingmanager.h"
#include "playbackmanager.h"
#include "spectrumframerecorder.h"
#include "spectrumiqeventrecorder.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace Core = RecordingFormatCore;
namespace {
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
QByteArray read(const QString &path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "cannot read synthetic capture");
    return file.readAll();
}
void save(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "cannot create synthetic capture");
    check(file.write(bytes) == bytes.size(), "cannot write synthetic capture");
}
quint32 u32(const QByteArray &bytes, int offset) {
    check(offset >= 0 && offset + 4 <= bytes.size(), "missing uint32 field");
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}
QJsonObject json(const QString &path) {
    const auto document = QJsonDocument::fromJson(read(path));
    check(document.isObject(), "invalid metadata JSON");
    return document.object();
}

class FaultBuffer : public QBuffer {
public:
    qint64 allowance = -1;
    bool failSeek = false;
    bool seek(qint64 position) override { return !failSeek && QBuffer::seek(position); }
protected:
    qint64 writeData(const char *data, qint64 size) override {
        if (allowance == 0) { setErrorString("injected write failure"); return -1; }
        const qint64 allowed = allowance < 0 ? size : (std::min)(size, allowance);
        const qint64 written = QBuffer::writeData(data, allowed);
        if (allowance > 0 && written > 0) allowance -= written;
        return written;
    }
};

// Only the small header is resident; payload positions near 4 GiB are logical.
class SparseSink : public QIODevice {
public:
    QByteArray header = QByteArray(256, '\0');
    qint64 extent = 0;
    int writes = 0;
    mutable int sizeQueries = 0;
    const char *lastSpan = nullptr;
    bool isSequential() const override { return false; }
    qint64 size() const override { ++sizeQueries; return extent; }
protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 size) override {
        ++writes;
        lastSpan = data;
        if (pos() < header.size()) {
            const qint64 n = (std::min)(size, qint64(header.size()) - pos());
            std::memcpy(header.data() + pos(), data, std::size_t(n));
        }
        extent = (std::max)(extent, pos() + size);
        return size;
    }
};

const QByteArray Pcm = QByteArray::fromHex("00000100ff7f0080");
const QByteArray LegacyMono = QByteArray::fromHex(
    "524946462c00000057415645666d7420100000000100010080bb000000770100020010006461746108000000") + Pcm;
const QByteArray LegacyFrame = QByteArray::fromHex(
    "46524d31fbffffffffffffffe803000000000000000000000000594000000000008056400000000000805b40"
    "00040000030000000000dcc20000bcc10000803e");

void wave(const QTemporaryDir &dir) {
    QString error;
    QBuffer mono;
    check(mono.open(QIODevice::ReadWrite), "buffer open");
    Core::WaveLayout layout;
    check(Core::beginWave(mono, 48000, 1, 16, {}, layout, &error), "mono WAV header");
    quint64 count = 0;
    check(Core::appendWave(mono, layout, count, Pcm.constData(), Pcm.size(), &error), "mono append");
    check(Core::finalizeWave(mono, layout, count, &error), "mono finalize");
    check(mono.data() == LegacyMono, "mono differs from pre-core PCM16 WAV fixture");
    const auto legacyPath = dir.filePath("legacy-mono.wav");
    save(legacyPath, LegacyMono);
    PlaybackManager::WavInfo info;
    check(PlaybackManager::readWavInfo(legacyPath, info, &error) && info.channels == 1 &&
          info.sampleRate == 48000 && info.dataOffset == 44 && info.dataSize == 8, "legacy mono playback");
    const auto path = dir.filePath("core-mono.wav");
    save(path, mono.data());
    PlaybackManager playback;
    QByteArray replay;
    QObject::connect(&playback, &PlaybackManager::audioFrameReady, [&](const QByteArray &bytes) { replay += bytes; });
    check(playback.start(path, &error), "shared mono playback start");
    while (playback.isPlaying()) QCoreApplication::processEvents();
    check(replay == Pcm, "shared mono playback payload differs");

    for (int parity = 0; parity < 2; ++parity) {
        QByteArray metadata = QJsonDocument(QJsonObject{{"app", "ObriiSDR"}, {"scalePercent", 125},
            {"centerFrequency", 100e6}, {"lab", QJsonObject{{"experiment", "synthetic"}}}}).toJson(QJsonDocument::Compact);
        if ((metadata.size() & 1) != parity) metadata += ' ';
        QBuffer iq;
        iq.open(QIODevice::ReadWrite);
        check(Core::beginWave(iq, 192000, 2, 16, metadata, layout, &error), "metadata WAV header");
        check(layout.headerBytes == quint64(52 + metadata.size() + parity) && layout.blockAlign == 4 &&
              layout.dataSizeFieldOffset == qint64(layout.headerBytes - 4), "metadata WAV offsets");
        check(iq.data().mid(36, 4) == "fbos" && u32(iq.data(), 40) == quint32(metadata.size()) &&
              iq.data().mid(44, metadata.size()) == metadata, "fbos metadata changed");
        if (parity) check(iq.data()[44 + metadata.size()] == '\0', "missing metadata padding");
        count = 0;
        check(Core::appendWave(iq, layout, count, Pcm.constData(), Pcm.size(), &error), "IQ append");
        check(Core::finalizeWave(iq, layout, count, &error), "IQ finalize");
        const auto iqPath = dir.filePath(QString("metadata-%1.wav").arg(parity));
        save(iqPath, iq.data());
        info = {};
        check(PlaybackManager::readWavInfo(iqPath, info, &error) && info.channels == 2 &&
              info.sampleRate == 192000 && info.dataOffset == layout.headerBytes &&
              info.hasRadioSettings && info.radioSettings.centerFrequency == 100e6 &&
              info.hasScalePercent && info.scalePercent == 125, "metadata WAV playback");
        replay.clear();
        QObject::connect(&playback, &PlaybackManager::iqFrameReady,
            [&](const QByteArray &bytes, double rate, int samples) {
                check(rate == 192000 && samples == 2, "IQ replay layout"); replay += bytes;
            });
        check(playback.start(iqPath, &error), "IQ replay start");
        while (playback.isPlaying()) QCoreApplication::processEvents();
        check(replay == Pcm, "IQ replay changed payload");
        QObject::disconnect(&playback, &PlaybackManager::iqFrameReady, nullptr, nullptr);
    }
    QBuffer invalid;
    invalid.open(QIODevice::ReadWrite);
    check(!Core::beginWave(invalid, 0, 1, 16, {}, layout, &error), "zero rate accepted");
    check(!Core::beginWave(invalid, 0xffffffffU, 2, 16, {}, layout, &error), "byte rate overflow accepted");
    check(!Core::beginWave(invalid, 48000, 3, 16, {}, layout, &error), "unsupported channels accepted");
    check(!Core::beginWave(invalid, 48000, 1, 8, {}, layout, &error), "unsupported PCM format accepted");
}

void failuresAndLimits() {
    QString error;
    Core::WaveLayout layout;
    FaultBuffer sink;
    sink.open(QIODevice::ReadWrite);
    sink.allowance = 13;
    check(!Core::beginWave(sink, 48000, 1, 16, {}, layout, &error) && sink.size() == 0 && sink.pos() == 0,
          "partial header did not roll back");
    sink.allowance = -1;
    check(Core::beginWave(sink, 48000, 1, 16, {}, layout), "retry header");
    quint64 bytes = 0;
    check(Core::appendWave(sink, layout, bytes, Pcm.constData(), Pcm.size()), "complete prefix append");
    const auto prefix = sink.data();
    sink.allowance = 3;
    check(!Core::appendWave(sink, layout, bytes, Pcm.constData(), Pcm.size(), &error) &&
          bytes == 8 && sink.data() == prefix && sink.pos() == prefix.size(), "partial PCM committed or leaked bytes");
    sink.allowance = 0;
    check(!Core::appendWave(sink, layout, bytes, Pcm.constData(), Pcm.size(), &error) && sink.data() == prefix,
          "negative write changed prefix");
    sink.allowance = -1;
    check(!Core::appendWave(sink, layout, bytes, "x", 1, &error) && sink.data() == prefix, "unaligned PCM accepted");
    check(Core::finalizeWave(sink, layout, bytes, &error) && sink.data() == LegacyMono, "prefix WAV finalization");
    check(Core::finalizeWave(sink, layout, bytes, &error), "finalization is not idempotent");
    sink.failSeek = true;
    check(!Core::finalizeWave(sink, layout, bytes, &error) && !error.isEmpty(), "seek failure hidden");
    sink.failSeek = false;
    sink.allowance = 2;
    check(!Core::finalizeWave(sink, layout, bytes, &error), "short header patch accepted");
    sink.allowance = -1;
    check(Core::finalizeWave(sink, layout, bytes), "repair injected header patch");
    sink.seek(sink.size());
    sink.write("tail", 4);
    check(Core::finalizeWave(sink, layout, bytes) && sink.data() == LegacyMono, "incomplete trailing payload retained");

    for (int channels : {1, 2}) for (const QByteArray metadata : {QByteArray(), QByteArray("odd")}) {
        SparseSink sparse;
        sparse.open(QIODevice::WriteOnly);
        check(Core::beginWave(sparse, 48000, quint16(channels), 16, metadata, layout), "sparse header");
        const quint64 limit = Core::waveDataLimit(layout);
        bytes = limit - layout.blockAlign;
        sparse.extent = qint64(layout.headerBytes + bytes);
        check(sparse.seek(sparse.extent), "sparse seek");
        check(Core::appendWave(sparse, layout, bytes, Pcm.constData(), layout.blockAlign) && bytes == limit,
              "last aligned RIFF frame rejected");
        const int sizeQueries = sparse.sizeQueries;
        check(sparse.lastSpan == Pcm.constData(), "already-byte PCM was copied");
        const int writes = sparse.writes;
        check(!Core::appendWave(sparse, layout, bytes, Pcm.constData(), layout.blockAlign, &error) &&
              sparse.writes == writes && sparse.sizeQueries == sizeQueries && bytes == limit,
              "RIFF overflow wrote data or queried file size");
        check(Core::finalizeWave(sparse, layout, bytes, &error) &&
              u32(sparse.header, 4) == quint32(layout.headerBytes - 8 + limit) &&
              u32(sparse.header, int(layout.dataSizeFieldOffset)) == limit, "RIFF boundary finalization");
        check(!Core::finalizeWave(sparse, layout, limit + layout.blockAlign, &error), "oversized RIFF finalized");
    }

    const float levels[] = {-110, -23.5f, 0.25f};
    const Core::SpectrumFrameView frame{-5, 1000, 100, 90, 110, 1024, levels, 3};
    for (qint64 allowance : {qint64(3), qint64(52), qint64(55)}) {
        FaultBuffer spectrum;
        spectrum.open(QIODevice::ReadWrite);
        check(Core::beginSpectrum(spectrum, {{"version", 1}}), "failure spectrum header");
        const auto before = spectrum.data();
        spectrum.allowance = allowance;
        check(!Core::appendSpectrum(spectrum, frame, &error) && spectrum.data() == before &&
              spectrum.pos() == before.size(), "partial spectrum frame did not roll back atomically");
    }
    FaultBuffer header;
    header.open(QIODevice::ReadWrite);
    header.allowance = 15;
    check(!Core::beginSpectrum(header, {{"version", 1}}, &error) && header.size() == 0, "partial spectrum metadata retained");

    FaultBuffer raw;
    raw.open(QIODevice::ReadWrite);
    check(Core::checkedAppend(raw, "prefix", 6), "raw prefix append");
    raw.allowance = 2;
    check(!Core::checkedAppend(raw, "tail", 4, &error) && raw.data() == "prefix" && raw.pos() == 6,
          "partial raw IQ retained an incomplete record");

    SparseSink direct;
    direct.open(QIODevice::WriteOnly);
    check(Core::beginWave(direct, 48000, 1, 16, {}, layout), "direct PCM header");
    const int queries = direct.sizeQueries;
    bytes = 0;
    check(Core::appendWave(direct, layout, bytes, Pcm.constData(), Pcm.size()) &&
          direct.lastSpan == Pcm.constData() && direct.sizeQueries == queries,
          "successful PCM append copied or queried file size");
}

void iqEncoding() {
    const float samples[] = {-2, -1, -0.5f, -0.0f, 0, 0.5f, 1, 2};
    QByteArray scratch;
    Core::EncodedIq bytes;
    check(Core::encodeIq(samples, 8, Core::IqEncoding::Ci8, scratch, bytes) &&
          QByteArray(bytes.data, int(bytes.size)) == QByteArray::fromHex("8181c00000407f7f"), "ci8 clamp/round changed");
    check(Core::encodeIq(samples, 8, Core::IqEncoding::Ci16Le, scratch, bytes) &&
          QByteArray(bytes.data, int(bytes.size)) == QByteArray::fromHex("0180018000c0000000000040ff7fff7f"), "ci16 clamp/round/endianness changed");
    const char *storage = scratch.constData();
    check(Core::encodeIq(samples, 8, Core::IqEncoding::Ci16Le, scratch, bytes) &&
          scratch.constData() == storage, "IQ conversion allocated again for equal block size");
    check(Core::encodeIq(samples, 8, Core::IqEncoding::Cf32Le, scratch, bytes) &&
          QByteArray(bytes.data, int(bytes.size)) == QByteArray::fromHex(
              "000000c0000080bf000000bf00000080000000000000003f0000803f00000040"), "CF32 bits or byte order changed");
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    check(bytes.data == reinterpret_cast<const char *>(samples), "CF32 fast path copied IQ");
#endif
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        const float bad[] = {1, invalid};
        for (auto encoding : {Core::IqEncoding::Ci8, Core::IqEncoding::Ci16Le})
            check(!Core::encodeIq(bad, 2, encoding, scratch, bytes) && !bytes.data && !bytes.size,
                  "non-finite quantized IQ exposed an encoded span");
        check(Core::encodeIq(bad, 2, Core::IqEncoding::Ci8, scratch, bytes, nullptr, Core::IqNonFinitePolicy::Zero) &&
              QByteArray(bytes.data, int(bytes.size)) == QByteArray::fromHex("7f00"), "CI8 zero-sanitize policy changed");
        check(Core::encodeIq(bad, 2, Core::IqEncoding::Ci16Le, scratch, bytes, nullptr, Core::IqNonFinitePolicy::Zero) &&
              QByteArray(bytes.data, int(bytes.size)) == QByteArray::fromHex("ff7f0000"), "CI16 zero-sanitize policy changed");
    }
    const quint32 evidenceBits[] = {0x7fc01234U, 0xff800000U, 0x7f800000U, 0x80000000U};
    float evidence[4];
    std::memcpy(evidence, evidenceBits, sizeof(evidence));
    check(Core::encodeIq(evidence, 4, Core::IqEncoding::Cf32Le, scratch, bytes) &&
          QByteArray(bytes.data, int(bytes.size)) == QByteArray::fromHex("3412c07f000080ff0000807f00000080"),
          "CF32 scientific evidence bits changed");
    check(!Core::encodeIq(samples, 7, Core::IqEncoding::Cf32Le, scratch, bytes), "odd IQ components accepted");
    check(!Core::encodeIq(nullptr, 2, Core::IqEncoding::Cf32Le, scratch, bytes), "null IQ accepted");
    check(!Core::encodeIq(samples, std::size_t(std::numeric_limits<int>::max()), Core::IqEncoding::Ci8,
                          scratch, bytes), "oversized IQ accepted");
}

void spectrum(const QTemporaryDir &dir) {
    QString error;
    const float levels[] = {-110, -23.5f, 0.25f};
    const Core::SpectrumFrameView view{-5, 1000, 100, 90, 110, 1024, levels, 3};
    const QByteArray fixture = QByteArray::fromHex("464253504543310a0d000000") + "{\"version\":1}" + LegacyFrame;
    QBuffer sink;
    sink.open(QIODevice::ReadWrite);
    check(Core::beginSpectrum(sink, {{"version", 1}}), "spectrum begin");
    check(Core::appendSpectrum(sink, view, &error), "spectrum append");
    check(sink.data() == fixture, "FBSPEC1 differs from pre-core fixture");
    QByteArray previous;
    QDataStream stream(&previous, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_5_15);
    stream << quint32(0x314d5246U) << qint64(-5) << qint64(1000) << double(100) << double(90)
           << double(110) << qint32(1024) << qint32(3);
    for (float level : levels) {
        quint32 bits; std::memcpy(&bits, &level, 4); stream << bits;
    }
    check(previous == LegacyFrame, "pre-core QDataStream frame fixture mismatch");
    for (const QString name : {QString("legacy.fbspec"), QString("core.fbspec")}) {
        const auto path = dir.filePath(name);
        save(path, name.startsWith("legacy") ? fixture : sink.data());
        SpectrumFrameReader reader;
        SpectrumFrameRecord frame;
        check(reader.open(path, &error) && reader.readNext(frame, &error) && reader.atEnd(), "shared spectrum readback");
        check(frame.elapsedMs == -5 && frame.utcMs == 1000 && frame.centerFrequency == 100 &&
              frame.minFrequency == 90 && frame.maxFrequency == 110 && frame.fftLength == 1024 &&
              frame.magnitudes == std::vector<float>(levels, levels + 3), "spectrum readback changed frame");
    }
    const auto before = sink.data();
    auto bad = view;
    bad.count = 2097153;
    check(!Core::appendSpectrum(sink, bad, &error) && sink.data() == before, "reader bin limit ignored");
    bad = view; bad.maxFrequency = 89;
    check(!Core::appendSpectrum(sink, bad, &error), "invalid spectrum geometry accepted");
}

void guiWrappers(const QTemporaryDir &dir) {
    QString error;
    RadioSettings settings;
    settings.centerFrequency = settings.actualFrequency = settings.listeningFrequency = 100e6;
    settings.sampleRate = 768000;
    const QJsonObject lab{{"experiment", "synthetic"}};
    for (const auto mode : {RecordingManager::Mode::AudioWav, RecordingManager::Mode::ChannelIqWav}) {
        RecordingManager recorder;
        recorder.setLabMetadata(lab);
        recorder.setDisplayScalePercent(137);
        check(recorder.start(mode, settings, &error), "GUI manager start");
        const auto path = recorder.currentFilePath();
        if (mode == RecordingManager::Mode::AudioWav) recorder.appendAudioFrame(Pcm);
        else recorder.appendIqFrame(Pcm, 192000, 2);
        if (mode == RecordingManager::Mode::AudioWav) recorder.appendAudioFrame(QByteArray("x"));
        else recorder.appendIqFrame(Pcm, 384000, 2);
        check(!recorder.isRecording(), "GUI append failure/rate change did not stop recording");
        recorder.stop();
        PlaybackManager::WavInfo info;
        check(PlaybackManager::readWavInfo(path, info, &error) && info.hasRadioSettings &&
              info.scalePercent == 137 && info.dataSize == 8 &&
              info.channels == (mode == RecordingManager::Mode::AudioWav ? 1 : 2), "GUI manager playback/metadata");
        const auto bytes = read(path);
        const auto metadata = QJsonDocument::fromJson(bytes.mid(44, int(u32(bytes, 40)))).object();
        check(metadata.value("lab").toObject() == lab && bytes.mid(int(info.dataOffset)) == Pcm, "GUI lab/payload changed");
        const auto sidecar = QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName() + ".json";
        check(json(sidecar).value("waveHeaderBytes").toDouble() == double(info.dataOffset), "GUI sidecar header size");
        check(QFile::remove(path) && QFile::remove(sidecar), "GUI synthetic capture cleanup");
    }
    for (auto mode : {SpectrumIqEventRecorder::Mode::ChannelIqS16, SpectrumIqEventRecorder::Mode::FullIqS8}) {
        SpectrumIqEventRecorder event;
        event.setExtendedMetadata(lab);
        const auto base = dir.filePath(mode == SpectrumIqEventRecorder::Mode::ChannelIqS16 ? "event16" : "event8");
        check(event.start(base, mode, settings, 192000, 1000, 990, &error), "event recorder start");
        const int samples = mode == SpectrumIqEventRecorder::Mode::ChannelIqS16 ? 2 : 4;
        check(event.appendFrame(Pcm, 192000, samples, 1010) && event.bytesWritten() == 8, "event append");
        check(!event.appendFrame(Pcm, 384000, samples, 1020) && !event.isRecording() && event.bytesWritten() == 8,
              "event rate change accepted or prefix counter changed");
        event.stop();
        const bool channel = mode == SpectrumIqEventRecorder::Mode::ChannelIqS16;
        const auto sidecar = json(base + (channel ? "_channel_iq.json" : "_full_iq_s8.json"));
        check(sidecar.value("scientificMetadata").toObject() == lab && sidecar.value("bytesWritten").toInt() == 8 &&
              sidecar.value("writeErrors").toInt() == 1,
              "event metadata changed");
        if (channel) {
            PlaybackManager::WavInfo info;
            check(PlaybackManager::readWavInfo(event.currentFilePath(), info, &error) && info.channels == 2 &&
                  read(event.currentFilePath()).mid(44) == Pcm, "event WAV playback");
        } else {
            check(read(event.currentFilePath()) == Pcm, "event raw payload changed");
            const auto sig = json(base + "_full_iq_s8.sigmf-meta");
            check(sig.value("global").toObject().value("core:datatype") == "ci8" &&
                  sig.value("captures").toArray().first().toObject().value("core:datetime") == "1970-01-01T00:00:01.000Z",
                  "event SigMF changed");
        }
    }
    SpectrumFrameRecorder recorder;
    recorder.setExtendedMetadata(lab);
    const auto path = dir.filePath("gui-spectrum.fbspec");
    check(recorder.start(settings, 120, 1024, 1005, &error, path), "GUI spectrum start");
    SpectrumFrameRecord frame;
    frame.utcMs = 1000; frame.centerFrequency = 100; frame.minFrequency = 90; frame.maxFrequency = 110;
    frame.fftLength = 1024; frame.magnitudes = {-110, -23.5f, 0.25f};
    check(recorder.appendFrameRecord(frame) && recorder.frameCount() == 1, "GUI spectrum append");
    auto invalid = frame;
    invalid.maxFrequency = 89;
    check(!recorder.appendFrameRecord(invalid) && !recorder.isRecording() && recorder.frameCount() == 1,
          "GUI invalid spectrum frame did not fail closed");
    recorder.stop();
    check(read(path).right(LegacyFrame.size()) == LegacyFrame, "GUI spectrum serialization differs");
    SpectrumFrameReader reader;
    check(reader.open(path, &error) && reader.metadata().value("scientificMetadata").toObject() == lab &&
          reader.readNext(frame, &error) && frame.elapsedMs == -5, "GUI spectrum metadata/readback");
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        check(dir.isValid(), "synthetic test directory unavailable");
        wave(dir); failuresAndLimits(); iqEncoding(); spectrum(dir); guiWrappers(dir);
        std::cout << "shared_recording_core_selftest: PASS (fixtures, playback, metadata, failures, RIFF limits, IQ, GUI)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "shared_recording_core_selftest: FAIL: " << error.what() << '\n';
        return 1;
    }
}
