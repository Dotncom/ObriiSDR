#include "dspstreamrecorder.h"
#include "dspstreamprocessor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

QByteArray read(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read recording fixture");
    return file.readAll();
}

QString sidecarPath(const QString &path) {
    require(path.endsWith(".cf32"), "Unexpected IQ extension");
    return path.left(path.size() - 5) + ".json";
}

QJsonObject metadata(const QString &path) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(read(sidecarPath(path)), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "Invalid recording JSON");
    return document.object();
}

quint64 number(const QJsonObject &json, const char *key) {
    bool ok = false;
    const auto value = json.value(QLatin1String(key));
    const quint64 result = value.toString().toULongLong(&ok);
    require(value.isString() && ok, "Missing exact uint64 metadata");
    return result;
}

DspStreamBlock block() {
    DspStreamBlock b;
    b.stream = "private-device-identifier-must-not-be-recorded";
    b.generation = (quint64(1) << 60) + 3;
    b.epoch = (quint64(1) << 60) + 5;
    b.firstSample = (quint64(1) << 60) + 17;
    b.sampleRate = 96000;
    b.centerHz = 100100000;
    b.iq = {0.25f, 0.5f, -0.25f, -0.5f};
    return b;
}

void checkFinal(const QString &path, quint64 first, quint64 count, const char *reason) {
    const auto json = metadata(path);
    require(json.value("finalized").toBool(), "Recording not finalized");
    require(number(json, "sampleCount") == count && number(json, "firstSample") == first &&
        number(json, "endSampleExclusive") == first + count, "Incorrect relative sample range");
    require(number(json, "byteCount") == count * 8 && quint64(read(path).size()) == count * 8, "Incorrect recording byte count");
    require(json.value("stopReason") == QLatin1String(reason), "Incorrect segment stop reason");
}

void bytesAndLifetimeTests() {
    QTemporaryDir dir;
    require(dir.isValid(), "Cannot create recording test directory");
    DspStreamRecorder recorder;
    require(!recorder.active() && recorder.path().isEmpty(), "Recorder starts active");
    auto b = block();
    recorder.append(b, b.iq);
    require(QDir(dir.path()).entryList(QDir::Files).isEmpty(), "Inactive append created files");
    require(recorder.start(dir.path()), "Cannot start recorder");
    const QString path = recorder.path();
    const auto initial = metadata(path);
    require(!initial.value("finalized").toBool() && number(initial, "sampleCount") == 0 &&
        initial.value("firstSample").isNull() && initial.value("sampleRate").isNull(), "Initial sidecar invents metadata");
    require(!recorder.start(dir.path()) && recorder.active() && recorder.path() == path, "Double start replaced active session");
    const std::vector<float> processed{1.0f, -2.0f, 0.5f, -0.0f};
    const QByteArray expected = QByteArray::fromHex("0000803f000000c00000003f00000080");
    b.gap = true;
    recorder.append(b, processed);
    require(recorder.active() && read(path) == expected, "Recorded IQ is not exact processed CF32LE");
    const auto started = metadata(path);
    require(!started.value("finalized").toBool() && number(started, "firstSample") == b.firstSample &&
        number(started, "generation") == b.generation && number(started, "epoch") == b.epoch &&
        started.value("gapBefore").toBool(), "Initial channel metadata lost precision or gap flag");
    b.firstSample += 2;
    b.gap = false;
    recorder.append(b, processed);
    require(recorder.path() == path, "First-block gap flag split a subsequent contiguous block");
    recorder.stop();
    require(!recorder.active() && read(path) == expected + expected, "Contiguous blocks changed bytes");
    checkFinal(path, b.firstSample - 2, 4, "user_stop");
    const auto final = metadata(path);
    require(final.value("format") == "CF32LE" && final.value("samplePositionReference") == "relative_channel" &&
        final.value("sampleRate").toDouble() == b.sampleRate && final.value("centerHz").toDouble() == b.centerHz,
        "Missing recording format or channel metadata");
    const QByteArray jsonBytes = read(sidecarPath(path));
    require(!jsonBytes.contains(b.stream.toUtf8()) && !final.contains("timestamp") && !final.contains("device") &&
        !final.contains("token") && !final.contains("directory"), "Sidecar exposes identifiers or invents absolute time");
    recorder.stop();
    recorder.append(b, processed);
    require(read(sidecarPath(path)) == jsonBytes, "Stopped session changed on append/stop");

    QString destructorPath;
    {
        DspStreamRecorder scoped;
        require(scoped.start(dir.path()), "Cannot start destructor fixture");
        scoped.append(b, b.iq);
        destructorPath = scoped.path();
    }
    checkFinal(destructorPath, b.firstSample, 2, "user_stop");

    require(recorder.start(dir.path()), "Cannot start empty recording");
    recorder.stop();
    const auto empty = metadata(recorder.path());
    require(empty.value("finalized").toBool() && empty.value("firstSample").isNull() &&
        empty.value("endSampleExclusive").isNull() && number(empty, "sampleCount") == 0 &&
        read(recorder.path()).isEmpty(), "Empty recording invents sample positions");
}

void segmentationTests() {
    QTemporaryDir dir;
    require(dir.isValid(), "Cannot create segment test directory");
    DspStreamRecorder recorder;
    require(recorder.start(dir.path()), "Cannot start segment fixture");
    auto b = block();
    recorder.append(b, b.iq);
    const QString sessionId = metadata(recorder.path()).value("sessionId").toString();
    for (int change = 0; change < 8; ++change) {
        const QString previous = recorder.path();
        const quint64 previousFirst = b.firstSample;
        b.firstSample += 2;
        b.gap = false;
        const char *reason = "metadata_change";
        switch (change) {
        case 0: b.gap = true; reason = "gap"; break;
        case 1: b.firstSample += 7; reason = "noncontiguous_sample"; break;
        case 2: b.firstSample -= 5; reason = "noncontiguous_sample"; break;
        case 3: b.sampleRate = 48000; break;
        case 4: b.centerHz += 1000; break;
        case 5: ++b.generation; break;
        case 6: ++b.epoch; break;
        case 7: b.stream = "another-private-channel"; break;
        }
        recorder.append(b, b.iq);
        require(recorder.active() && recorder.path() != previous, "Discontinuity did not create a new segment");
        checkFinal(previous, previousFirst, 2, reason);
        const auto next = metadata(recorder.path());
        require(next.value("startReason") == QLatin1String(reason) && number(next, "firstSample") == b.firstSample &&
            next.value("gapBefore").toBool() == b.gap && next.value("sessionId") == sessionId &&
            next.value("segmentIndex").toInt() == change + 2, "Incorrect new segment metadata");
        require(number(next, "generation") == b.generation && number(next, "epoch") == b.epoch &&
            next.value("sampleRate").toDouble() == b.sampleRate && next.value("centerHz").toDouble() == b.centerHz,
            "Changed channel metadata not preserved");
    }
    const QString last = recorder.path();
    const quint64 lastFirst = b.firstSample;
    b.firstSample += 2;
    recorder.append(b, b.iq);
    require(recorder.path() == last, "Contiguous block unnecessarily split");
    recorder.stop();
    checkFinal(last, lastFirst, 4, "user_stop");
    require(QDir(dir.path()).entryList({"*.cf32"}, QDir::Files).size() == 9, "Unexpected segment count");
}

void limitsAndNoOverwriteTests() {
    QTemporaryDir dir;
    require(dir.isValid(), "Cannot create limit test directory");
    DspStreamRecorder recorder;
    auto b = block();
    require(recorder.start(dir.path(), 27), "Cannot start byte-limit fixture");
    recorder.append(b, b.iq);
    b.firstSample += 2;
    recorder.append(b, b.iq);
    const QString capped = recorder.path();
    require(!recorder.active() && recorder.status().contains("byte limit"), "Byte cap did not end session");
    checkFinal(capped, b.firstSample - 2, 3, "byte_limit");
    const QByteArray savedIq = read(capped), savedJson = read(sidecarPath(capped));
    recorder.append(b, b.iq);
    recorder.stop();
    require(read(capped) == savedIq && read(sidecarPath(capped)) == savedJson, "Capped recording changed");

    require(recorder.start(dir.path(), 40), "Cannot restart recording");
    require(recorder.path() != capped, "Recording reused old filename");
    recorder.append(b, b.iq);
    const QString first = recorder.path();
    const quint64 firstSample = b.firstSample;
    b.firstSample += 2;
    b.gap = true;
    recorder.append(b, b.iq);
    const QString second = recorder.path();
    b.firstSample += 2;
    b.gap = false;
    recorder.append(b, b.iq);
    require(!recorder.active(), "Byte budget reset at segment boundary");
    checkFinal(first, firstSample, 2, "gap");
    checkFinal(second, firstSample + 2, 3, "byte_limit");
    require(read(capped) == savedIq && read(sidecarPath(capped)) == savedJson, "New session overwrote previous pair");
    for (const QString &name : QDir(dir.path()).entryList({"*.cf32"}, QDir::Files)) {
        require(!QUuid(name.mid(11, name.size() - 16)).isNull(), "Recording filename is not UUID-based");
    }

    DspStreamRecorder other;
    require(recorder.start(dir.path(), 8) && other.start(dir.path()), "Cannot start simultaneous recorders");
    const QString oneSample = recorder.path();
    require(oneSample != other.path(), "Simultaneous recorders share a filename");
    recorder.append(b, b.iq);
    require(!recorder.active(), "Minimum byte cap did not stop recording");
    checkFinal(oneSample, b.firstSample, 1, "byte_limit");
    other.append(b, b.iq);
    other.stop();
    checkFinal(other.path(), b.firstSample, 2, "user_stop");
    require(read(capped) == savedIq && read(sidecarPath(capped)) == savedJson, "Simultaneous recorders overwrote an existing pair");

    QTemporaryDir bounded;
    require(bounded.isValid() && recorder.start(bounded.path()), "Cannot start segment-limit fixture");
    b = block();
    b.gap = true;
    for (int i = 0; i < 128; ++i) {
        recorder.append(b, b.iq);
        require(recorder.active(), "Segment limit reached too early");
        b.firstSample += 2;
    }
    const QString last = recorder.path();
    recorder.append(b, b.iq);
    require(!recorder.active() && recorder.status().contains("segment"), "Segment count is unbounded");
    checkFinal(last, b.firstSample - 2, 2, "segment_limit");
    require(QDir(bounded.path()).entryList(QDir::Files).size() == 256, "Segment limit created extra files");
}

void invalidAndIoTests() {
    QTemporaryDir dir;
    require(dir.isValid(), "Cannot create invalid-input directory");
    DspStreamRecorder recorder;
    require(!recorder.start({}) && !recorder.start(".") && !recorder.start(dir.filePath("missing")), "Implicit or missing directory accepted");
    require(!recorder.start(dir.path(), 0) && !recorder.start(dir.path(), 7) &&
        !recorder.start(dir.path(), (std::numeric_limits<quint64>::max)()), "Invalid byte limit accepted");
    for (int invalid = 0; invalid < 11; ++invalid) {
        auto b = block();
        require(recorder.start(dir.path()), "Cannot start invalid-input fixture");
        recorder.append(b, b.iq);
        const quint64 first = b.firstSample;
        const QString path = recorder.path();
        b.firstSample += 2;
        std::vector<float> processed = b.iq;
        switch (invalid) {
        case 0: processed[0] = std::numeric_limits<float>::quiet_NaN(); break;
        case 1: processed[1] = std::numeric_limits<float>::infinity(); break;
        case 2: processed.pop_back(); break;
        case 3: processed.clear(); break;
        case 4: b.sampleRate = 0; break;
        case 5: b.sampleRate = std::numeric_limits<double>::infinity(); break;
        case 6: b.centerHz = std::numeric_limits<double>::quiet_NaN(); break;
        case 7: b.firstSample = (std::numeric_limits<quint64>::max)(); break;
        case 8: b.iq[0] = std::numeric_limits<float>::quiet_NaN(); break;
        case 9: b.iq.resize((DspStreamProtocol::MaxSamples + 1) * 2); processed = b.iq; break;
        case 10: b.stream.clear(); break;
        }
        recorder.append(b, processed);
        require(!recorder.active() && recorder.status().startsWith("Error:"), "Invalid input did not stop with an error");
        checkFinal(path, first, 2, "invalid_block");
    }
    require(!recorder.start(recorder.path()), "File accepted as recording directory");

    require(recorder.start(dir.path()), "Cannot start sidecar-error fixture");
    auto b = block();
    recorder.append(b, b.iq);
    const QString path = recorder.path();
    const QString jsonPath = sidecarPath(path);
    require(QFile::remove(jsonPath) && QDir().mkdir(jsonPath), "Cannot inject sidecar write failure");
    recorder.stop();
    require(!recorder.active() && recorder.status().startsWith("Error:") && recorder.status().contains("sidecar"),
        "Sidecar I/O failure silently reported as success");
    require(read(path).size() == 16 && QFileInfo(jsonPath).isDir(), "Sidecar failure damaged IQ or replaced existing directory");
}
}

bool dspStreamRecorderSelfTest() {
    try {
        bytesAndLifetimeTests();
        segmentationTests();
        limitsAndNoOverwriteTests();
        invalidAndIoTests();
        std::cout << "DSP stream recorder: exact CF32LE, metadata, segmentation, limits and I/O errors passed\n";
        return true;
    } catch (const std::exception &error) {
        std::cerr << "DSP stream recorder FAIL: " << error.what() << '\n';
        return false;
    }
}

#ifdef DSP_STREAM_RECORDER_SELFTEST_MAIN
#include <QCoreApplication>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    return dspStreamRecorderSelfTest() ? 0 : 1;
}
#endif
