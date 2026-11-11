#include "receivernoisebaseline.h"
#include "receiveryfactorcalibration.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QIODevice>

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    int failures = 0;
    int checks = 0;
    const auto check = [&](bool ok, const char *label) {
        ++checks;
        if (!ok) {
            ++failures;
            std::cerr << label << '\n';
        }
    };

    ReceiverNoiseBaselineKey key;
    key.receiver.backendId = QStringLiteral("test");
    key.receiver.inputMode = 0;
    key.receiver.sampleRateHz = 4.0e6;
    key.centerFrequencyHz = 100.0e6;
    key.fftLength = 4;
    key.fftWindowType = 1;
    const std::vector<float> frequencies{98.0e6f, 99.0e6f, 100.0e6f, 101.0e6f};
    const std::vector<float> baselineLevels{-100.0f, -90.0f, -80.0f, -70.0f};

    ReceiverNoiseBaseline baseline;
    baseline.beginCapture(key, 8);
    for (int frame = 0; frame < 8; ++frame)
        baseline.addFrame(frequencies, baselineLevels, key);
    std::vector<float> raisedLevels{-90.0f, -80.0f, -70.0f, -60.0f};
    check(baseline.apply(frequencies, raisedLevels, key,
                         ReceiverNoiseBaselineMode::RelativeFloor, -120.0),
          "baseline apply");
    check(std::abs(raisedLevels.front() + 110.0f) < 0.01f,
          "baseline relative level");
    ReceiverNoiseBaseline restoredBaseline;
    check(restoredBaseline.deserialize(baseline.serialize()) &&
          restoredBaseline.hasMatchingProfile(key), "baseline serialization");

    ReceiverYFactorCalibration yFactor;
    const std::vector<float> coldLevels(4, 0.0f);
    const std::vector<float> hotLevels(4, static_cast<float>(10.0 * std::log10(5.5)));
    yFactor.beginColdCapture(key, 8);
    for (int frame = 0; frame < 8; ++frame)
        yFactor.addFrame(frequencies, coldLevels, key);
    check(yFactor.coldReady(key), "cold capture");
    check(yFactor.beginHotCapture(key, 8, YFactorSourceMode::Temperatures,
                                  290.0, 2900.0, 0.0), "hot capture start");
    for (int frame = 0; frame < 8; ++frame)
        yFactor.addFrame(frequencies, hotLevels, key);
    const ReceiverYFactorProfile profile = yFactor.matchingProfile(key);
    check(profile.isValid(), "Y-factor profile");
    check(std::abs(profile.noiseFigureDb.front() - 3.0103f) < 0.01f,
          "Y-factor NF");
    check(profile.ratioRepeatabilityDb.size() == 4 && profile.ratioRepeatabilityDb.front() == 0.0f,
          "constant Y-factor captures have zero statistical spread");
    std::vector<float> inputReferred = coldLevels;
    check(yFactor.applyInputReferred(frequencies, inputReferred, key, -120.0) &&
          std::abs(inputReferred.front() + 120.0f) < 0.01f,
          "Y-factor apply");
    ReceiverYFactorCalibration restoredYFactor;
    check(restoredYFactor.deserialize(yFactor.serialize()) &&
          restoredYFactor.hasMatchingProfile(key), "Y-factor serialization");

    yFactor.beginColdCapture(key, 8);
    for (int frame = 0; frame < 8; ++frame) {
        auto levels = coldLevels;
        levels[0] = static_cast<float>(10.0 * std::log10(frame % 2 ? 1.2 : 0.8));
        if (frame == 0) levels[1] = std::numeric_limits<float>::quiet_NaN();
        yFactor.addFrame(frequencies, levels, key);
    }
    check(yFactor.beginHotCapture(key, 8, YFactorSourceMode::Temperatures,
                                  290.0, 2900.0, 0.0), "mismatched hot capture start");
    auto shiftedFrequencies = frequencies;
    for (auto &frequency : shiftedFrequencies) frequency += 10000.0f;
    ReceiverYFactorCalibration::CaptureResult mismatch = ReceiverYFactorCalibration::CaptureResult::Ignored;
    for (int frame = 0; frame < 8; ++frame) mismatch = yFactor.addFrame(shiftedFrequencies, hotLevels, key);
    check(mismatch == ReceiverYFactorCalibration::CaptureResult::Rejected && yFactor.coldReady(key),
          "mismatched Hot/Cold frequency grids are rejected without losing Cold");
    check(yFactor.beginHotCapture(key, 8, YFactorSourceMode::Temperatures,
                                  290.0, 2900.0, 0.0), "varying hot capture start");
    for (int frame = 0; frame < 8; ++frame) {
        auto levels = hotLevels;
        levels[0] = static_cast<float>(10.0 * std::log10(5.5 * (frame % 2 ? 1.1 : 0.9)));
        yFactor.addFrame(frequencies, levels, key);
    }
    const auto varying = yFactor.matchingProfile(key);
    const double expectedRepeatability = 10.0 / std::log(10.0) * std::hypot(0.2, 0.1) / std::sqrt(7.0);
    check(varying.isValid() && varying.ratioRepeatabilityDb.size() == 4 &&
          std::abs(varying.ratioRepeatabilityDb[0] - expectedRepeatability) < 1e-6 &&
          std::isnan(varying.ratioRepeatabilityDb[1]), "independent Hot/Cold standard uncertainty and unavailable bin");
    check(restoredYFactor.deserialize(yFactor.serialize()), "repeatability profile round trip");
    const auto restoredVarying = restoredYFactor.matchingProfile(key);
    check(restoredVarying.ratioRepeatabilityDb.size() == 4 &&
          restoredVarying.ratioRepeatabilityDb[0] == varying.ratioRepeatabilityDb[0] &&
          std::isnan(restoredVarying.ratioRepeatabilityDb[1]), "persisted repeatability values");

    QByteArray legacyYFactorPayload;
    QDataStream legacyYFactor(&legacyYFactorPayload, QIODevice::WriteOnly);
    legacyYFactor.setVersion(QDataStream::Qt_5_12);
    legacyYFactor.setByteOrder(QDataStream::LittleEndian);
    legacyYFactor << quint32(0x4f594643) << quint32(1) << quint32(1)
        << key.receiver.backendId << qint32(key.receiver.inputMode) << key.receiver.sampleRateHz
        << qint32(key.receiver.lnaGain) << qint32(key.receiver.vgaGain)
        << qint32(key.receiver.rtlGainTenthsDb) << qint32(key.receiver.gainState)
        << key.centerFrequencyHz << qint32(key.fftLength) << qint32(key.fftWindowType)
        << qint32(key.rfBandwidthHz) << key.amplitudeOffsetDb << key.calibrationTableEnabled
        << qint32(key.calibrationPointCount) << qint32(YFactorSourceMode::Temperatures)
        << profile.coldTemperatureKelvin << profile.hotTemperatureKelvin << profile.enrDb
        << profile.minimumFrequencyHz << profile.maximumFrequencyHz << qint32(profile.coldFrames)
        << qint32(profile.hotFrames) << qint32(profile.validBins) << profile.capturedUtcMs << quint32(4);
    for (int i = 0; i < 4; ++i) {
        legacyYFactor << profile.coldPower[i] << profile.hotPower[i]
                      << profile.equivalentNoiseTemperatureKelvin[i] << profile.noiseFigureDb[i];
    }
    check(restoredYFactor.deserialize(qCompress(legacyYFactorPayload)) &&
          restoredYFactor.hasMatchingProfile(key) &&
          restoredYFactor.matchingProfile(key).ratioRepeatabilityDb.isEmpty(),
          "legacy Y-factor profile has no inferred uncertainty");

    ReceiverNoiseBaseline broad;
    ReceiverNoiseBaselineKey shapeKey;
    shapeKey.receiver.backendId = "fobos-agile"; shapeKey.receiver.inputMode = 0;
    shapeKey.receiver.sampleRateHz = 50e6; shapeKey.receiver.lnaGain = 1; shapeKey.receiver.vgaGain = 15;
    shapeKey.centerFrequencyHz = 100e6; shapeKey.fftLength = 2048; shapeKey.fftWindowType = 0;
    shapeKey.hardwareIdentity = "test-receiver/IF1";
    std::vector<float> shapeFrequencies(2048), shapeLevels(2048);
    for (int i = 0; i < 2048; ++i) {
        const double offset = (i - 1024) * 50e6 / 2048;
        shapeFrequencies[i] = static_cast<float>(100e6 + offset);
        shapeLevels[i] = static_cast<float>(-115 + 12 * std::exp(-0.5 * std::pow((offset + 8e6) / 150000, 2)) +
            4 * std::exp(-0.5 * std::pow(offset / 8e6, 2)));
    }
    const int hump = static_cast<int>(std::llround(-8e6 / (50e6 / 2048))) + 1024;
    const int signal = static_cast<int>(std::llround(2e6 / (50e6 / 2048))) + 1024;
    const int narrow = static_cast<int>(std::llround(14e6 / (50e6 / 2048))) + 1024;
    auto captured = shapeLevels; captured[narrow] += 30;
    broad.beginCapture(shapeKey, 8, 1024);
    for (int i = 0; i < 8; ++i) broad.addFrame(shapeFrequencies, captured, shapeKey);
    check(broad.hasMatchingProfile(shapeKey, true), "compact broad profile lookup");
    check(!broad.hasMatchingProfile(shapeKey), "broad-only profile does not replace scientific calibration");
    auto live = captured; live[signal] += 15;
    auto corrected = live;
    check(broad.apply(shapeFrequencies, corrected, shapeKey, ReceiverNoiseBaselineMode::WideShape, -120, 150000, 1), "broad apply");
    check(live[hump] - corrected[hump] > 7, "broad hump reduced");
    auto correctedNoise = shapeLevels;
    broad.apply(shapeFrequencies, correctedNoise, shapeKey, ReceiverNoiseBaselineMode::WideShape, -120, 150000, 1);
    check(std::abs((corrected[signal] - correctedNoise[signal]) - 15) < 0.02, "new signal contrast preserved");
    check(corrected[narrow] - correctedNoise[narrow] > 29.9f, "broad filter does not learn narrow spur");
    auto zeroStrength = live;
    broad.apply(shapeFrequencies, zeroStrength, shapeKey, ReceiverNoiseBaselineMode::WideShape, -120, 150000, 0);
    check(zeroStrength == live, "zero strength leaves data unchanged");
    auto halfStrength = live;
    broad.apply(shapeFrequencies, halfStrength, shapeKey, ReceiverNoiseBaselineMode::WideShape, -120, 150000, .5);
    check(std::abs((live[hump] - halfStrength[hump]) * 2 - (live[hump] - corrected[hump])) < .02, "strength control");
    auto alternate = shapeKey; alternate.fftLength = 1048576; alternate.fftWindowType = 4;
    check(broad.hasMatchingProfile(alternate, true), "shape accepts FFT/window changes");
    alternate.receiver.vgaGain = 8;
    check(!broad.hasMatchingProfile(alternate, true), "shape rejects wrong VGA");
    alternate = shapeKey; alternate.hardwareIdentity = "test-receiver/IF0";
    check(!broad.hasMatchingProfile(alternate, true), "shape rejects wrong receiver/IF identity");
    alternate = shapeKey; alternate.centerFrequencyHz += 1e6;
    auto movedFrequencies = shapeFrequencies; for (auto &f : movedFrequencies) f += 1e6;
    auto moved = live;
    broad.apply(movedFrequencies, moved, alternate, ReceiverNoiseBaselineMode::WideShape, -120, 150000, 1);
    check(std::abs(moved[hump] - corrected[hump]) < .03, "nearby retune follows baseband shape");
    alternate.centerFrequencyHz += 50e6;
    check(!broad.hasMatchingProfile(alternate, true), "unmeasured RF region rejected");
    ReceiverNoiseBaseline restoredBroad;
    check(restoredBroad.deserialize(broad.serialize()) && restoredBroad.hasMatchingProfile(shapeKey, true), "broad v2 serialization");
    const auto oldData = broad.serialize();
    auto staged = broad;
    alternate = shapeKey; alternate.centerFrequencyHz += 25e6;
    staged.beginCapture(alternate, 8, 1024);
    auto stagedFrequencies = shapeFrequencies; for (auto &f : stagedFrequencies) f += 25e6;
    for (int i = 0; i < 8; ++i) staged.addFrame(stagedFrequencies, shapeLevels, alternate);
    check(broad.serialize() == oldData && staged.profileCount() == 2, "staged sweep preserves original profiles");
    check(restoredBroad.clearMatching(shapeKey, true) && !restoredBroad.hasMatchingProfile(shapeKey, true), "clear wide profile/cache");

    // Restore an actual version1 layout (before receiver identity was stored).
    QByteArray legacyPayload;
    QDataStream legacy(&legacyPayload, QIODevice::WriteOnly);
    legacy.setVersion(QDataStream::Qt_5_12); legacy.setByteOrder(QDataStream::LittleEndian);
    legacy << quint32(0x4f4e424c) << quint32(1) << quint32(1)
        << key.receiver.backendId << qint32(key.receiver.inputMode) << key.receiver.sampleRateHz
        << qint32(key.receiver.lnaGain) << qint32(key.receiver.vgaGain)
        << qint32(key.receiver.rtlGainTenthsDb) << qint32(key.receiver.gainState)
        << key.centerFrequencyHz << qint32(key.fftLength) << qint32(key.fftWindowType)
        << qint32(key.rfBandwidthHz) << key.amplitudeOffsetDb << key.calibrationTableEnabled << qint32(key.calibrationPointCount)
        << double(98e6) << double(101e6) << qint32(8) << double(0) << qint64(1) << quint32(4);
    for (float level : baselineLevels) legacy << static_cast<float>(std::pow(10.0, level / 10.0));
    ReceiverNoiseBaseline legacyRestored;
    check(legacyRestored.deserialize(qCompress(legacyPayload)) && legacyRestored.hasMatchingProfile(key), "v1 profile backward compatibility");

    ReceiverNoiseBaseline survey;
    for (int center = 0; center < 275; ++center) {
        auto sweepKey = shapeKey; sweepKey.centerFrequencyHz = 50e6 + center * 25e6;
        auto sweepFrequencies = shapeFrequencies;
        for (auto &f : sweepFrequencies) f += static_cast<float>(sweepKey.centerFrequencyHz - shapeKey.centerFrequencyHz);
        survey.beginCapture(sweepKey, 8, 1024);
        for (int frame = 0; frame < 8; ++frame) survey.addFrame(sweepFrequencies, shapeLevels, sweepKey);
    }
    check(survey.profileCount() == 275, "full RF sweep retains more than16 profiles");
    auto lowBand = shapeKey; lowBand.centerFrequencyHz = 50e6;
    check(survey.hasMatchingProfile(lowBand, true), "low RF profiles survive full sweep");
    check(restoredBroad.deserialize(survey.serialize()) && restoredBroad.profileCount() == 275, "full RF survey serialization");

    std::cout << "Receiver calibration: " << checks << " checks, "
              << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
