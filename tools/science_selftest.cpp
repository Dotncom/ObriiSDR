#include "tonalspectrum.h"
#include "receivercalibrationtable.h"
#include "receivernoisebaseline.h"
#include "receiveryfactorcalibration.h"
#include "spectrumscienceanalyzer.h"
#include "spectrumframerecorder.h"
#include "hdf5spectrumexport.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDataStream>
#include <QJsonDocument>
#include <QFileInfo>
#include <cmath>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    int failures = 0, checks = 0;
    const auto check = [&](bool ok, const char *label) { ++checks; if (!ok) { ++failures; std::cerr << label << '\n'; } };
    ReceiverCalibrationTable table;
    ReceiverCalibrationPoint generic{100, 0, 1, 0, "generic"};
    ReceiverCalibrationPoint first{100, 2, 10, 1, "bound"}, second{200, 4, 20, 3, "bound"};
    first.conditions.backendId = second.conditions.backendId = "fobos-agile";
    first.conditions.lnaGain = second.conditions.lnaGain = 3;
    first.conditions.sampleRateHz = second.conditions.sampleRateHz = 5e7;
    table.setPoints({generic, first, second});
    check(table.points().size() == 3, "same frequency across different profiles must survive");
    check(table.correctionAt(150).amplitudeOffsetDb == 1, "legacy wildcard selection");
    auto context = first.conditions;
    table.setContext(context);
    const auto correction = table.correctionAt(150);
    check(correction.valid && correction.amplitudeOffsetDb == 15 && correction.frequencyOffsetHz == 3 && correction.uncertaintyDb == 2, "profile interpolation");
    context.lnaGain = 2;
    check(table.correctionAt(150, context).amplitudeOffsetDb == 1, "different gain cannot use bound calibration");
    context = first.conditions; context.sampleRateHz = 2e7;
    check(table.correctionAt(150, context).amplitudeOffsetDb == 1, "different rate cannot use bound calibration");
    check(ReceiverCalibrationContext::fromJson(first.conditions.toJson()).toJson() == first.conditions.toJson(), "condition serialization");
    ReceiverNoiseBaselineKey noiseKey;
    noiseKey.receiver = first.conditions;
    noiseKey.centerFrequencyHz = 100e6;
    noiseKey.fftLength = 4;
    noiseKey.fftWindowType = 1;
    ReceiverNoiseBaseline noiseBaseline;
    const std::vector<float> noiseFrequencies{98, 99, 100, 101};
    const std::vector<float> noiseLevels{-100, -90, -80, -70};
    noiseBaseline.beginCapture(noiseKey, 8);
    for (int frameIndex = 0; frameIndex < 8; ++frameIndex) {
        const auto result = noiseBaseline.addFrame(noiseFrequencies, noiseLevels, noiseKey);
        check(result == (frameIndex == 7 ? ReceiverNoiseBaseline::CaptureResult::Completed
                                        : ReceiverNoiseBaseline::CaptureResult::Accepted),
              "noise baseline capture progress");
    }
    check(noiseBaseline.hasMatchingProfile(noiseKey), "noise baseline profile match");
    std::vector<float> signalLevels{-90, -80, -70, -60};
    check(noiseBaseline.apply(noiseFrequencies, signalLevels, noiseKey,
                              ReceiverNoiseBaselineMode::RelativeFloor, -120.0),
          "noise baseline relative apply");
    check(std::abs(signalLevels.front() - (-110.0f)) < 0.01f &&
          std::abs(signalLevels.back() - (-110.0f)) < 0.01f,
          "noise baseline preserves relative level");
    ReceiverNoiseBaselineKey mismatchedNoiseKey = noiseKey;
    mismatchedNoiseKey.centerFrequencyHz += 20000000.0;
    check(!noiseBaseline.hasMatchingProfile(mismatchedNoiseKey), "noise baseline rejects changed tuning");
    ReceiverNoiseBaseline restoredNoiseBaseline;
    QString noiseRestoreError;
    check(restoredNoiseBaseline.deserialize(noiseBaseline.serialize(), &noiseRestoreError) &&
          noiseRestoreError.isEmpty() && restoredNoiseBaseline.hasMatchingProfile(noiseKey),
          "noise baseline serialization");
    ReceiverYFactorCalibration yFactor;
    const std::vector<float> coldLevels(4, 0.0f);
    const std::vector<float> hotLevels(4, static_cast<float>(10.0 * std::log10(5.5)));
    yFactor.beginColdCapture(noiseKey, 8);
    for (int frameIndex = 0; frameIndex < 8; ++frameIndex)
        yFactor.addFrame(noiseFrequencies, coldLevels, noiseKey);
    check(yFactor.coldReady(noiseKey), "Y-factor cold capture");
    QString yFactorError;
    check(yFactor.beginHotCapture(noiseKey, 8, YFactorSourceMode::Temperatures,
                                  290.0, 2900.0, 0.0, &yFactorError),
          "Y-factor hot capture start");
    for (int frameIndex = 0; frameIndex < 8; ++frameIndex)
        yFactor.addFrame(noiseFrequencies, hotLevels, noiseKey);
    const ReceiverYFactorProfile yProfile = yFactor.matchingProfile(noiseKey);
    check(yProfile.isValid() && std::abs(yProfile.noiseFigureDb.front() - 3.0103f) < 0.01f,
          "Y-factor noise figure");
    std::vector<float> inputReferred = coldLevels;
    check(yFactor.applyInputReferred(noiseFrequencies, inputReferred, noiseKey, -120.0) &&
          std::abs(inputReferred.front() + 120.0f) < 0.01f,
          "Y-factor input-referred application");
    ReceiverYFactorCalibration restoredYFactor;
    check(restoredYFactor.deserialize(yFactor.serialize(), &yFactorError) &&
          restoredYFactor.hasMatchingProfile(noiseKey), "Y-factor serialization");
    std::vector<float> frequencies(8192), levels(8192, -100);
    for (size_t i=0; i<frequencies.size(); ++i) frequencies[i] = float(i);
    levels[100] = -10; levels[200] = -30; levels[300] = -40;
    const auto tone = analyzeTonalSpectrum(frequencies, levels, 100, 2, 5);
    check(tone.valid && tone.completeHarmonicSpan && tone.harmonicsMeasured == 4, "tone coverage");
    check(std::abs(tone.thdDb-10*std::log10(0.0011/0.1)) < 0.0001, "THD power-domain ratio");
    check(std::abs(tone.sinadDb+tone.thdnDb) < 1e-10, "SINAD / THD+N reciprocal definition");
    check(tone.snrDb > tone.sinadDb && tone.thdnPercent >= tone.thdPercent, "noise/distortion ordering");
    check(!analyzeTonalSpectrum(frequencies, levels, 2000, 2, 5).completeHarmonicSpan, "out-of-band harmonics marked partial");
    std::vector<float> flat(8192, -100);
    check(!analyzeTonalSpectrum(frequencies, flat, 100, 2).valid, "noise is not a valid tone");
    auto invalidAxis = frequencies; invalidAxis[300] = 299.5f; invalidAxis[301] = 500;
    check(!analyzeTonalSpectrum(invalidAxis, levels, 100, 2).valid, "discontinuous axes rejected");
    SpectrumScienceAnalyzer analyzer;
    analyzer.update(frequencies, levels);
    for (int i=2; i<64; ++i) check(analyzer.addMarker() == i, "dynamic marker allocation");
    check(analyzer.addMarker() == -1, "marker capacity bound");
    analyzer.setMarker(40, 100);
    check(analyzer.hasMarkers() && analyzer.marker(40).levelDb == -10, "extra marker analysis");
    check(!analyzer.removeMarker(0) && analyzer.removeMarker(40), "A/B retained / extra marker removed");
    analyzer.setMarker(0, 100);
    const auto savedMarkers = analyzer.markerConfiguration();
    SpectrumScienceAnalyzer restored;
    restored.update(frequencies, levels);
    restored.restoreMarkerConfiguration(savedMarkers);
    check(restored.markerCount() == 63 && restored.markerConfiguration() == savedMarkers, "marker settings round trip");
    restored.restoreMarkerConfiguration({});
    check(restored.markerCount() == 2 && !restored.hasMarkers(), "empty marker settings retain disabled A/B");
    restored.update({100, 100, 102, 102}, {-10, -20, -30, -40});
    restored.setMarker(0, 101);
    check(restored.marker(0).levelDb == -10, "nearest-bin ties select first duplicate");
    restored.setMarker(0, 103);
    check(restored.marker(0).levelDb == -30, "out-of-range marker selects first last-frequency duplicate");
    restored.update({102, 100, 101}, {-30, -10, -20});
    restored.setMarker(0, 100);
    check(restored.marker(0).levelDb == -10, "unsorted frequency axis uses linear nearest-bin fallback");
    analyzer.clearMarkers(); check(!analyzer.hasMarkers(), "clear all markers");
    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory");
    const QString input = directory.filePath("recording.fbspec");
    {
        QFile file(input); check(file.open(QIODevice::WriteOnly), "fixture open");
        file.write("FBSPEC1\n", 8); QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian); stream.setVersion(QDataStream::Qt_5_15);
        const auto metadata = QJsonDocument(QJsonObject{{"test", true}}).toJson(QJsonDocument::Compact);
        stream << quint32(metadata.size()); file.write(metadata);
        for (int frame=0; frame<3; ++frame) {
            const std::vector<float> values = frame == 2 ? std::vector<float>{8,9} : std::vector<float>{0,1,2,3};
            stream << quint32(0x314d5246) << qint64(frame*20) << qint64(1700000000000LL+frame*20)
                   << double(100) << double(90) << double(110) << qint32(2048) << qint32(values.size());
            file.write(reinterpret_cast<const char *>(values.data()), qint64(values.size()*sizeof(float)));
        }
    }
    QString error;
    SpectrumFrameReader reader; check(reader.open(input, &error), "streaming reader header");
    int frameCount=0; SpectrumFrameRecord frame;
    while (!reader.atEnd()) { if (!reader.readNext(frame, &error)) break; ++frameCount; }
    check(frameCount == 3 && error.isEmpty(), "streaming reader all frames");
    SpectrumFrameRecording recording; check(SpectrumFrameRecorder::loadFile(input, recording, &error) && recording.frames.size()==3, "replay parser shares streaming reader");
    if (hdf5SpectrumExportBuilt()) {
        const QString output = directory.filePath("spectrum.h5");
        const bool ok = exportSpectrumHdf5(input, output, &error);
        if (!ok) std::cerr << error.toStdString() << '\n';
        check(ok && QFileInfo(output).size() > 0, "native HDF5 export");
        check(!exportSpectrumHdf5(input, output, &error), "export does not overwrite existing data");
        std::atomic_bool cancel{true}; const QString cancelled = directory.filePath("cancelled.h5");
        check(!exportSpectrumHdf5(input, cancelled, &error, &cancel) && !QFileInfo::exists(cancelled), "cancelled export is not published");
        if (qEnvironmentVariableIsSet("OBRII_SCIENCE_SAVE_FIXTURE") && ok)
            QFile::copy(output, qEnvironmentVariable("OBRII_SCIENCE_SAVE_FIXTURE"));
    }
    std::cout << "Science: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
