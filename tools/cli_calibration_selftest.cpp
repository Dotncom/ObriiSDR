#include "clicalibration.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace {
QJsonArray array(const std::vector<float> &values) {
    QJsonArray result;
    for (float v : values) result.append(v);
    return result;
}

QJsonObject point(double frequency, double amplitude, const QJsonObject &conditions = {}) {
    return {{"frequencyHz", frequency}, {"frequencyOffsetHz", amplitude * 2},
            {"amplitudeOffsetDb", amplitude}, {"uncertaintyDb", 0.25},
            {"note", "test reference"}, {"conditions", conditions}};
}

QJsonObject batch(const std::vector<float> &frequencies, const std::vector<float> &cold,
                  const std::vector<float> &hot) {
    QJsonArray coldFrames, hotFrames;
    for (int i = 0; i < 8; ++i) { coldFrames.append(array(cold)); hotFrames.append(array(hot)); }
    return {{"operation", "yfactor.compute"}, {"frequenciesHz", array(frequencies)},
            {"coldFramesDbfs", coldFrames}, {"hotFramesDbfs", hotFrames},
            {"coldTemperatureKelvin", 290}, {"hotTemperatureKelvin", 2900}};
}

bool near(const QJsonValue &value, double expected, double tolerance = 0.002) {
    return value.isDouble() && std::abs(value.toDouble() - expected) < tolerance;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    int checks = 0, failures = 0;
    const auto check = [&](bool passed, const char *label) {
        ++checks;
        if (!passed) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    };
    CliCalibration cli;
    const auto command = [&](const char *operation) { return cli.command({{"operation", operation}}); };
    const auto ok = [](const QJsonObject &reply) { return reply.value("ok").toBool(); };
    const auto query = [&](double hz) { return cli.command({{"operation", "table.correction"}, {"frequencyHz", hz}, {"levelDbfs", -80}}); };
    ReceiverNoiseBaselineKey key;
    key.receiver.backendId = "test";
    key.receiver.sampleRateHz = 4e6;
    key.receiver.inputMode = 0;
    key.centerFrequencyHz = 100e6;
    key.fftLength = 4;
    key.fftWindowType = 1;
    QJsonObject keyObject{{"receiver", key.receiver.toJson()}, {"centerFrequencyHz", key.centerFrequencyHz},
                         {"fftLength", 4}, {"fftWindowType", 1}};
    const auto select = [&](const QJsonObject &k) { return cli.command({{"operation", "profile.select"}, {"key", k}}); };
    const std::vector<float> f{98e6f, 99e6f, 100e6f, 101e6f};
    const std::vector<float> cold(4, -100);
    const std::vector<float> hot(4, static_cast<float>(-100 + 10 * std::log10(5.5)));

    check(!cli.capturing() && cli.submitSpectrum({}, {}, {}).isEmpty(), "idle is no-op even on invalid frames");
    check(ok(command("status")) && !ok(query(100e6)), "selection required");
    check(ok(select(keyObject)), "profile selection");
    check(!query(100e6).value("valid").toBool() && query(100e6).value("amplitudeOffsetDb").isNull(), "empty table validity");
    QJsonObject specific = key.receiver.toJson();
    specific.remove("input");
    const QJsonArray rows{point(98e6, 1), point(101e6, 4), point(98e6, 10, specific), point(101e6, 16, specific)};
    check(ok(cli.command({{"operation", "table.load"}, {"calibrationTable", rows}})), "GUI-shaped table load");
    auto correction = query(99.5e6);
    check(correction.value("valid").toBool() && near(correction.value("amplitudeOffsetDb"), 13) &&
          near(correction.value("frequencyOffsetHz"), 26) && near(correction.value("uncertaintyDb"), 0.25), "specificity and interpolation");
    check(near(correction.value("correctedLevelDbfs"), -67) && correction.value("unit") == "dBFS" &&
          !correction.value("absolutePowerCalibrated").toBool(), "correction preserves unit, not dBm");
    check(near(query(1).value("amplitudeOffsetDb"), 10) && near(query(1e9).value("amplitudeOffsetDb"), 16), "core endpoint hold behavior");
    QJsonObject otherKey = keyObject;
    auto otherReceiver = key.receiver.toJson();
    otherReceiver.insert("backend", "other");
    otherKey.insert("receiver", otherReceiver);
    check(ok(select(otherKey)) && near(query(99.5e6).value("amplitudeOffsetDb"), 2.5), "wildcard fallback");
    check(ok(select(keyObject)), "reselect specific profile");
    auto badPoint = point(100e6, 3);
    badPoint.insert("uncertaintyDb", -1);
    check(!ok(cli.command({{"operation", "table.load"}, {"calibrationTable", QJsonArray{badPoint}}})) &&
          near(query(99.5e6).value("amplitudeOffsetDb"), 13), "invalid load is transactional");
    badPoint = point(100e6, 3);
    badPoint.insert("frequencyHz", "100000000");
    check(!ok(cli.command({{"operation", "table.load"}, {"calibrationTable", QJsonArray{badPoint}}})), "numeric strings rejected");
    check(!ok(cli.command({{"operation", "table.correction"}, {"frequencyHz", 100e6}, {"unit", "dBm"}})), "no dBm relabeling");
    QJsonArray tooMany;
    for (int i = 0; i < 4097; ++i) tooMany.append(point(i, 0));
    check(!ok(cli.command({{"operation", "table.load"}, {"calibrationTable", tooMany}})), "table size bound");
    auto malformedKey = keyObject;
    malformedKey.insert("fftLength", 4.5);
    check(!ok(select(malformedKey)), "fractional key rejected");

    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory");
    const QString path = directory.filePath("table.json");
    check(ok(cli.command({{"operation", "table.export"}, {"path", path}})), "table export");
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "read exported table");
    const auto exported = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    check(exported.value("calibrationTable").toArray().size() == 4 && exported.size() == 3, "export is calibration-only");
    check(!ok(cli.command({{"operation", "table.export"}, {"path", path}})), "export never overwrites");
    check(ok(command("table.clear")) && ok(cli.command({{"operation", "table.load"}, {"path", path}})) &&
          near(query(99.5e6).value("amplitudeOffsetDb"), 13), "table file roundtrip");
    check(!ok(cli.command({{"operation", "table.load"}, {"path", path}, {"calibrationTable", rows}})), "ambiguous input rejected");
    check(!ok(cli.command({{"operation", "table.load"}, {"version", 2}, {"calibrationTable", rows}})), "unsupported table version rejected");
    const QString oversizedPath = directory.filePath("oversized.json");
    QFile oversizedFile(oversizedPath);
    check(oversizedFile.open(QIODevice::WriteOnly) && oversizedFile.write(QByteArray(4 * 1024 * 1024 + 1, ' ')) == 4 * 1024 * 1024 + 1,
          "oversized file fixture");
    oversizedFile.close();
    check(!ok(cli.command({{"operation", "table.load"}, {"path", oversizedPath}})) &&
          near(query(99.5e6).value("amplitudeOffsetDb"), 13), "file input size bound is transactional");

    const auto compute = batch(f, cold, hot);
    check(ok(cli.command(compute)), "Y-factor compute from actual frame arrays");
    auto report = command("yfactor.read");
    auto first = report.value("rows").toArray().first().toObject();
    check(ok(report) && report.value("validBins").toInt() == 4 && near(first.value("noiseFigureDb"), 3.0103) &&
          near(first.value("equivalentNoiseTemperatureKelvin"), 290, 0.01), "Y-factor analytic result");
    QJsonObject apply{{"operation", "yfactor.apply"}, {"frequenciesHz", array(f)}, {"levelsDbfs", array(cold)}};
    auto applied = cli.command(apply);
    check(ok(applied) && near(applied.value("levelsDb").toArray().first(), 0) &&
          applied.value("unit") == "dB re cold input", "input-referred cold normalization");
    auto hotApply = apply;
    hotApply.insert("levelsDbfs", array(hot));
    check(near(cli.command(hotApply).value("levelsDb").toArray().first(), 10, 0.01), "input-referred hot normalization");
    auto badCompute = batch(f, cold, cold);
    check(!ok(cli.command(badCompute)) && command("yfactor.read").value("validBins").toInt() == 4, "invalid Y-factor does not replace profile");
    auto tooShort = compute;
    auto seven = compute.value("coldFramesDbfs").toArray();
    seven.removeLast();
    tooShort.insert("coldFramesDbfs", seven);
    check(!ok(cli.command(tooShort)), "no silent minimum frame clamping");
    auto badUnit = compute;
    badUnit.insert("unit", "dBm");
    check(!ok(cli.command(badUnit)), "Y-factor input unit enforced");
    auto badSource = compute;
    badSource.insert("sourceMode", 1);
    check(!ok(cli.command(badSource)), "source mode has strict type");
    badSource = compute;
    badSource.insert("hotTemperatureKelvin", 290);
    check(!ok(cli.command(badSource)), "nonphysical source temperatures rejected");
    auto mismatched = apply;
    mismatched.insert("levelsDbfs", QJsonArray{-100, -100, -100});
    check(!ok(cli.command(mismatched)), "apply array length mismatch rejected");
    auto outside = apply;
    outside.insert("frequenciesHz", QJsonArray{97e6, 98e6, 99e6, 100e6});
    check(!ok(cli.command(outside)), "no silent out-of-span application");
    auto mixedHot = hot;
    mixedHot[1] = cold[1];
    check(ok(cli.command(batch(f, cold, mixedHot))), "partially valid Y-factor table retained");
    report = command("yfactor.read");
    const auto invalid = report.value("rows").toArray().at(1).toObject();
    check(report.value("validBins").toInt() == 3 && !invalid.value("valid").toBool() &&
          invalid.value("noiseFigureDb").isNull() && invalid.value("equivalentNoiseTemperatureKelvin").isNull(), "invalid scientific bins are null, not zero");
    check(!ok(cli.command(apply)), "invalid-bin application cannot return mixed reference units");
    auto enr = batch(f, cold, std::vector<float>(4, static_cast<float>(-100 + 10 * std::log10(1.5))));
    enr.insert("sourceMode", "enr"); enr.insert("enrDb", 0);
    check(ok(cli.command(enr)) && near(command("yfactor.read").value("rows").toArray().first().toObject().value("noiseFigureDb"), 3.0103), "ENR source mode");
    auto oversized = compute;
    QJsonArray wide, manyFrames;
    for (int i = 0; i < 32768; ++i) wide.append(-100);
    for (int i = 0; i < 33; ++i) manyFrames.append(wide);
    oversized.insert("frequenciesHz", wide);
    oversized.insert("coldFramesDbfs", manyFrames);
    oversized.insert("hotFramesDbfs", manyFrames);
    check(!ok(cli.command(oversized)), "batch work bound");

    check(ok(command("noise.capture")) && cli.capturing(), "arm noise capture");
    check(!ok(select(otherKey)) && !ok(command("table.clear")) && !ok(command("yfactor.compute")), "busy capture protected");
    for (int i = 0; i < 8; ++i) {
        const std::vector<float> levels(4, i % 2 ? -90.0f : -100.0f);
        const auto event = cli.submitSpectrum(f, levels, key);
        check(ok(event) && event.value("result") == (i == 7 ? "completed" : "accepted"), "noise capture progress");
    }
    check(!cli.capturing() && cli.submitSpectrum({}, {}, {}).isEmpty(), "bounded capture stops, returns to idle");
    report = command("noise.read");
    check(ok(report) && report.value("capturedFrames").toInt() == 8 &&
          near(report.value("rows").toArray().first().toObject().value("referenceLevelDbfs"), -100 + 10 * std::log10(5.5)), "linear-power averaging, not mean dB");
    QJsonObject noiseApply{{"operation", "noise.apply"}, {"frequenciesHz", array(f)},
                           {"levelsDbfs", array(std::vector<float>(4, static_cast<float>(-100 + 10 * std::log10(11))))}};
    applied = cli.command(noiseApply);
    check(ok(applied) && near(applied.value("levelsDb").toArray().first(), 3.0103), "noise relative reference");
    noiseApply.insert("mode", "excess");
    check(near(cli.command(noiseApply).value("levelsDb").toArray().first(), 0), "noise excess-power reference");
    check(ok(cli.command({{"operation", "noise.export"}, {"path", directory.filePath("noise.json")}, {"limit", 2}})), "bounded profile report export");
    check(cli.command({{"operation", "yfactor.read"}, {"offset", 1}, {"limit", 2}}).value("rows").toArray().size() == 2,
          "profile report pagination");
    check(!ok(cli.command({{"operation", "noise.capture"}, {"frames", 7}})), "capture frame bounds");
    command("noise.capture");
    ReceiverNoiseBaselineKey changed = key;
    changed.fftWindowType = 2;
    check(!ok(cli.submitSpectrum(f, cold, changed)) && !cli.capturing(), "changed key cancels capture");
    command("noise.capture");
    auto nan = cold; nan[2] = std::numeric_limits<float>::quiet_NaN();
    check(!ok(cli.submitSpectrum(f, nan, key)) && !cli.capturing(), "nonfinite frame cancels capture");
    command("noise.capture");
    auto irregular = f; irregular[1] += 20000;
    check(!ok(cli.submitSpectrum(irregular, cold, key)), "nonuniform grid rejected");
    command("yfactor.cold");
    for (int i = 0; i < 8; ++i) cli.submitSpectrum(f, cold, key);
    check(command("status").value("coldReady").toBool(), "live cold capture ready");
    check(ok(cli.command({{"operation", "yfactor.hot"}, {"hotTemperatureKelvin", 2900}})), "live hot stage starts");
    auto shifted = f;
    for (float &hz : shifted) hz += 1000;
    check(!ok(cli.submitSpectrum(shifted, hot, key)) && !command("status").value("coldReady").toBool(), "hot grid must match cold grid, not just size");
    check(!ok(cli.command({{"operation", "yfactor.hot"}, {"hotTemperatureKelvin", 2900}})), "stale cold is not reused");
    command("yfactor.cold");
    for (int i = 0; i < 8; ++i) cli.submitSpectrum(f, cold, key);
    cli.command({{"operation", "yfactor.hot"}, {"hotTemperatureKelvin", 2900}});
    for (int i = 0; i < 8; ++i) cli.submitSpectrum(f, hot, key);
    check(!cli.capturing() && ok(command("yfactor.read")), "live Y-factor completes");
    command("yfactor.cold");
    check(ok(command("capture.cancel")) && !cli.capturing() && !command("status").value("coldReady").toBool(), "explicit cancellation resets pending state");
    select(otherKey);
    check(!ok(command("noise.read")) && !ok(command("yfactor.read")), "receiver mismatch cannot use scientific profiles");
    select(keyObject);
    check(ok(command("noise.read")) && ok(command("yfactor.read")), "matching profiles survive profile switches");
    command("table.clear");
    check(!ok(command("noise.read")) && !ok(command("yfactor.read")), "table replacement invalidates scientific profiles");
    check(!ok(command("unknown")), "unknown command rejected");

    check(ok(cli.command({{"operation", "noise.capture"}, {"frames", 512}})), "maximum frame target supported");
    for (int i = 0; i < 512; ++i) cli.submitSpectrum(f, cold, key);
    check(!cli.capturing() && command("noise.read").value("capturedFrames").toInt() == 512, "maximum capture is still bounded");
    CliCalibration bounded;
    for (int i = 0; i < 17; ++i) {
        auto receiver = key.receiver;
        receiver.backendId = QStringLiteral("receiver-%1").arg(i);
        auto captureKey = key;
        captureKey.receiver = receiver;
        auto jsonKey = keyObject;
        jsonKey.insert("receiver", receiver.toJson());
        bounded.command({{"operation", "profile.select"}, {"key", jsonKey}});
        bounded.command({{"operation", "noise.capture"}});
        for (int frame = 0; frame < 8; ++frame) bounded.submitSpectrum(f, cold, captureKey);
    }
    check(bounded.command({{"operation", "status"}}).value("noiseProfiles").toInt() == 16, "profile storage bound");
    auto evictedKey = keyObject;
    auto receiverZero = key.receiver.toJson(); receiverZero.insert("backend", "receiver-0");
    evictedKey.insert("receiver", receiverZero);
    bounded.command({{"operation", "profile.select"}, {"key", evictedKey}});
    check(!ok(bounded.command({{"operation", "noise.read"}})), "oldest profile evicted");
    std::cout << "CLI calibration: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
