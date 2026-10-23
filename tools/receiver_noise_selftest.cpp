#include "receivernoisebaseline.h"
#include "receiveryfactorcalibration.h"

#include <QCoreApplication>

#include <cmath>
#include <iostream>
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
    std::vector<float> inputReferred = coldLevels;
    check(yFactor.applyInputReferred(frequencies, inputReferred, key, -120.0) &&
          std::abs(inputReferred.front() + 120.0f) < 0.01f,
          "Y-factor apply");
    ReceiverYFactorCalibration restoredYFactor;
    check(restoredYFactor.deserialize(yFactor.serialize()) &&
          restoredYFactor.hasMatchingProfile(key), "Y-factor serialization");

    std::cout << "Receiver calibration: " << checks << " checks, "
              << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
