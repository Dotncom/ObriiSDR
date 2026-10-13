#include "spectrumframereplaydialog.h"
#include "appsettingsutils.h"
#include "iqbuffer.h"
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QSettings>
#include <QTemporaryDir>
#include <QThreadPool>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
QString testSettingsPath;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
QByteArray settingsBytes() { QFile file(testSettingsPath); file.open(QIODevice::ReadOnly); return file.readAll(); }
SpectrumFrameRecording fixture(bool embedded) {
    SpectrumFrameRecording recording;
    const QJsonObject analysis{{"amplitudeCalibrationOffsetDb", 13.0}, {"effectiveAmplitudeCalibrationOffsetDb", 15.0},
        {"frequencyCalibrationOffsetHz", 125.0}, {"displayLevelMinDb", -130}, {"displayLevelMaxDb", -20}};
    QJsonObject scientific{{"start", QJsonObject{{"analysis", analysis}, {"radio", QJsonObject{{"bandwidthHz", 12500}}}}}};
    if (embedded) scientific["experimentContext"] = QJsonObject{
        {"display", QJsonObject{{"levelMin", -110}, {"levelMax", -10}, {"contrast", 7}, {"sensitivity", 11},
            {"waterfallDisplayMode", 0}, {"waterfallRowsPerFrame", 64}, {"waterfall3DResolutionDivisor", 64}, {"coloredSpectrum", false}}},
        {"settings", QJsonObject{{"bandwidth", 90000}, {"modulationType", MOD_WFM}, {"audioLowPassHz", 3200}}}};
    recording.metadata = {{"sourceSampleRate", 192000}, {"centerFrequency", 100000000}, {"listeningFrequency", 100001000},
        {"bandwidth", 12500}, {"modulationType", MOD_NFM}, {"scientificMetadata", scientific}};
    SpectrumFrameRecord frame;
    frame.centerFrequency = 100000000; frame.minFrequency = 99904000; frame.maxFrequency = 100096000;
    frame.fftLength = 2048; frame.utcMs = 1700000000000; frame.magnitudes.assign(32, -37.25f);
    recording.frames.push_back(std::move(frame));
    return recording;
}
}

// Isolated target deliberately substitutes settings storage, never the user's INI.
QString persistentSettingsFilePath() { return testSettingsPath; }

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    std::cout << std::unitbuf;
    QTemporaryDir directory;
    testSettingsPath = directory.filePath("replay.ini");
    try {
        require(directory.isValid(), "Temporary settings directory failed");
        { QSettings settings(testSettingsPath, QSettings::IniFormat); settings.setValue("ui/language", "uk");
          settings.setValue("spectrumReplay/levelMin", -177); settings.sync(); }
        const auto beforeIni = settingsBytes();
        const float iq[] = {0.1f, -0.2f, 0.3f, -0.4f};
        IqBuffer::publish(iq, 4);
        const auto before = IqBuffer::stats();
        const auto unexpectedIq = directory.filePath("recording_channel_iq.wav");
        { QFile file(unexpectedIq); require(file.open(QIODevice::WriteOnly), "Cannot create undeclared IQ fixture"); file.write("not a WAV"); }
        QJsonObject fallback{{"display", QJsonObject{{"levelMin", -150}, {"levelMax", -50}, {"contrast", 6},
            {"waterfall3DResolutionDivisor", 32}}}, {"settings", QJsonObject{{"modulationType", MOD_AM}, {"bandwidth", 500000}}}};
        {
            SpectrumFrameReplayDialog viewer(nullptr, true);
            QString error;
            require(viewer.loadExperimentRecording(directory.filePath("recording_spectrum.fbspec"), fixture(true), fallback, &error), qPrintable(error));
            const auto state = viewer.experimentReplayState(), display = state.value("display").toObject(), settings = state.value("settings").toObject();
            require(state.value("paused").toBool() && !state.value("persistentSettingsEnabled").toBool(), "Replay activated or enabled persistence");
            require(display.value("levelMin").toDouble() == -110 && !display.value("coloredSpectrum").toBool(), "Embedded display lost precedence");
            require(display.value("waterfall3DResolutionDivisor").toInt() == 64, "Native 1/64 resolution rejected");
            require(display.value("waterfallRowsPerFrame").toInt() == 16 && state.value("displayAdjustedToReplayLimits").toBool(), "Large row loupe not bounded explicitly");
            require(settings.value("modulationType").toInt() == MOD_NFM && settings.value("bandwidth").toDouble() == 12500, "Header lost demodulation precedence");
            require(state.value("firstStoredMagnitude").toDouble() == -37.25, "Calibration applied twice");
            require(!state.value("channelIqAvailable").toBool(), "Undeclared neighboring IQ became available");
            require(state.value("sharedRecordingBacking").toBool(), "Replay copied its immutable frame backing");
            require(state.value("calibrationProvenance").toObject().value("amplitudeCalibrationOffsetDb").toDouble() == 13, "Calibration provenance missing");
            require(IqBuffer::stats().epoch == before.epoch && IqBuffer::stats().queuedFloatCount == before.queuedFloatCount, "Opening replay changed shared RX IQ");
        }
        {
            SpectrumFrameReplayDialog viewer(nullptr, true); QString error;
            require(viewer.loadExperimentRecording(directory.filePath("recording_spectrum.fbspec"), fixture(false), fallback, &error, {unexpectedIq}), qPrintable(error));
            require(viewer.experimentReplayState().value("channelIqAvailable").toBool(), "Explicitly verified IQ allowlist ignored");
            require(viewer.experimentReplayState().value("paused").toBool(), "Allowlisted IQ auto-started playback");
        }
        {
            SpectrumFrameReplayDialog viewer(nullptr, true); QString error;
            auto tall = fixture(false);
            tall.frames.resize(65536, tall.frames.front());
            require(viewer.loadExperimentRecording(directory.filePath("tall_spectrum.fbspec"), std::move(tall), fallback, &error), qPrintable(error));
            for (auto *slider : viewer.findChildren<QSlider *>()) if (slider->maximum() == 400) slider->setValue(400);
            QThreadPool::globalInstance()->waitForDone();
            QCoreApplication::processEvents();
            const auto state = viewer.experimentReplayState();
            require(state.value("frameCount").toInt() == 65536, "Tall recording truncated");
            require(state.value("viewportRasterPixels").toInt() > 0 && state.value("viewportRasterPixels").toInt() <= 4096*2048,
                "Tall zoomed replay rasterized its whole scroll history");
        }
        {
            SpectrumFrameReplayDialog viewer(nullptr, true); QString error;
            require(viewer.loadExperimentRecording(directory.filePath("old_spectrum.fbspec"), fixture(false), fallback, &error), qPrintable(error));
            const auto state = viewer.experimentReplayState();
            require(state.value("display").toObject().value("levelMin").toDouble() == -130, "Old header analysis ignored");
            require(state.value("settings").toObject().value("modulationType").toInt() == MOD_NFM, "Export-time demodulation overrode recording");
            auto invalid = fallback; auto display = invalid.value("display").toObject(); display["contrast"] = "NaN"; invalid["display"] = display;
            require(!viewer.applyExperimentContext(invalid, &error), "Malformed numeric control accepted");
            require(viewer.experimentReplayState() == state, "Rejected context partially changed replay");
        }
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        require(settingsBytes() == beforeIni, "Bundle replay persisted into settings");
        const auto after = IqBuffer::stats();
        require(after.epoch == before.epoch && after.queuedFloatCount == before.queuedFloatCount && after.totalFloatCount == before.totalFloatCount,
                "Closing replay changed shared RX IQ");
        std::cout << "Experiment replay UI: context precedence, bounded controls, unchanged calibrated amplitudes, paused state, settings/RX isolation: passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
