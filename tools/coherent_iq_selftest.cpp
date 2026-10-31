#include "coherentiqaligner.h"
#include "coherentiqsession.h"

#include <QDir>
#include <QTemporaryDir>

#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        constexpr std::size_t samples = 16384;
        constexpr int delay = 137;
        constexpr double phase = 0.61;
        constexpr double targetGain = 0.42;
        std::vector<float> reference(samples * 2U), target(samples * 2U);
        for (std::size_t index = 0; index < samples; ++index) {
            const double carrier = 0.071 * index + 0.37 * std::sin(0.0031 * index);
            const std::complex<double> value = std::polar(0.55 + 0.2 * std::sin(0.017 * index), carrier);
            reference[index * 2U] = static_cast<float>(value.real());
            reference[index * 2U + 1U] = static_cast<float>(value.imag());
            if (index >= std::size_t(delay)) {
                const std::complex<double> shifted(reference[(index - delay) * 2U],
                                                   reference[(index - delay) * 2U + 1U]);
                const auto observed = shifted * std::polar(targetGain, -phase);
                target[index * 2U] = static_cast<float>(observed.real());
                target[index * 2U + 1U] = static_cast<float>(observed.imag());
            }
        }
        const auto calibration = CoherentIqAligner::calibrate(
            DspSourceIdentity::normalized(QStringLiteral("main"), QStringLiteral("rx0")), reference,
            DspSourceIdentity::normalized(QStringLiteral("multi-rx-2"), QStringLiteral("rx0")), target,
            10.0e6, 100.0e6, 512);
        require(calibration.valid, "Calibration rejected a correlated signal");
        require(std::abs(calibration.integerDelaySamples - delay) <= 1, "Integer delay is wrong");
        require(std::abs(calibration.phaseCorrectionRadians - phase) < 0.02, "Phase correction is wrong");
        require(std::abs(calibration.gainCorrection - 1.0 / targetGain) < 0.03, "Gain correction is wrong");
        require(calibration.coherence > 0.995, "Coherence is unexpectedly low");
        const auto aligned = CoherentIqAligner::alignTarget(target, calibration, 4096);
        require(aligned.size() == 8192U, "Aligned span has an unexpected size");

        DspIqSourceSnapshot referenceSnapshot;
        referenceSnapshot.source = calibration.reference;
        referenceSnapshot.iq = reference;
        referenceSnapshot.epoch = 11;
        referenceSnapshot.startComplexSample = 5000;
        referenceSnapshot.endComplexSample = 5000 + samples;
        referenceSnapshot.sampleRateHz = 10.0e6;
        referenceSnapshot.centerHz = 100.0e6;
        referenceSnapshot.clockId = QStringLiteral("simulated-10mhz");
        referenceSnapshot.sampleClockValid = true;
        referenceSnapshot.clockLocked = true;
        referenceSnapshot.nativeAdcBits = 14;
        referenceSnapshot.nativeSampleFormat = QStringLiteral("simulated ADC14");
        DspIqSourceSnapshot targetSnapshot = referenceSnapshot;
        targetSnapshot.source = calibration.target;
        targetSnapshot.iq = target;
        targetSnapshot.nativeAdcBits = 8;
        targetSnapshot.nativeSampleFormat = QStringLiteral("simulated SC8");

        const std::vector<DspIqSourceSnapshot> snapshots{referenceSnapshot, targetSnapshot};
        const std::vector<CoherentIqCalibration> calibrations{calibration};
        for (const auto mode : {CoherentIqCombineMode::ReferenceOnly,
                                CoherentIqCombineMode::BestSnr,
                                CoherentIqCombineMode::EqualAverage,
                                CoherentIqCombineMode::MaximumRatio}) {
            const auto combined = CoherentIqSession::combine(
                snapshots, calibrations, referenceSnapshot.source, mode, 4096);
            require(combined.valid && combined.iq.size() == 8192U,
                    "Coherent combiner rejected valid mixed-bit IQ");
            double weightSum = 0.0;
            for (const double weight : combined.weights) weightSum += weight;
            require(std::abs(weightSum - 1.0) < 1.0e-9,
                    "Coherent combiner weights are not normalized");
        }

        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create temporary coherent-session directory");
        QString manifestPath;
        QString error;
        require(CoherentIqSession::write(temporary.path(), snapshots, calibrations,
                                         manifestPath, error),
                error.toLocal8Bit().constData());
        std::vector<DspIqSourceSnapshot> restoredSnapshots;
        std::vector<CoherentIqCalibration> restoredCalibrations;
        require(CoherentIqSession::read(manifestPath, restoredSnapshots,
                                        restoredCalibrations, error),
                error.toLocal8Bit().constData());
        require(restoredSnapshots.size() == 2U && restoredCalibrations.size() == 1U &&
                    restoredSnapshots[0].iq == reference && restoredSnapshots[1].iq == target &&
                    restoredSnapshots[0].nativeAdcBits == 14 &&
                    restoredSnapshots[1].nativeAdcBits == 8,
                "Coherent-session roundtrip lost IQ or native precision metadata");
        std::cout << "Coherent IQ delay, phase, gain and alignment self-test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
