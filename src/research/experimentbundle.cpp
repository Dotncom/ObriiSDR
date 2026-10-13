#include "experimentbundle.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace ExperimentBundle {
namespace {
constexpr qint64 JsonLimit = 16 * 1024 * 1024;
constexpr qint64 ChunkSize = 256 * 1024;
const QString ManifestName = QStringLiteral("manifest.json");
const QString PartialName = QStringLiteral(".experiment-bundle.partial");
const QString SpectrumName = QStringLiteral("recording_spectrum.fbspec");
const QStringList Names{SpectrumName, QStringLiteral("recording_channel_iq.wav"),
    QStringLiteral("recording_channel_iq.json"), QStringLiteral("recording_full_iq_s8.iq8"),
    QStringLiteral("recording_full_iq_s8.json"), QStringLiteral("recording_full_iq_s8.sigmf-meta")};

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
bool listed(const QString &value, const char *words) {
    return QString::fromLatin1(words).split(' ', Qt::SkipEmptyParts).contains(value);
}
QJsonObject numbers(const QJsonObject &input, const char *keys) {
    QJsonObject out;
    for (const auto &key : QString::fromLatin1(keys).split(' ', Qt::SkipEmptyParts)) {
        const auto value = input.value(key);
        if (value.isDouble() && std::isfinite(value.toDouble())) out.insert(key, value);
    }
    return out;
}
void booleans(QJsonObject &out, const QJsonObject &input, const char *keys) {
    for (const auto &key : QString::fromLatin1(keys).split(' ', Qt::SkipEmptyParts))
        if (input.value(key).isBool()) out.insert(key, input.value(key));
}
void utc(QJsonObject &out, const QJsonObject &input, const QString &key) {
    const auto text = input.value(key).toString();
    if (text.size() > 64) return;
    auto date = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (date.isValid()) out.insert(key, date.toUTC().toString(Qt::ISODateWithMs));
}
QString version(const QJsonValue &v) {
    const auto s = v.toString();
    static const QRegularExpression pattern(QStringLiteral("^(?:[0-9][A-Za-z0-9.+_-]{0,63}|unknown)$"));
    return pattern.match(s).hasMatch() ? s : QStringLiteral("unknown");
}
QJsonArray numericArray(const QJsonArray &input, int maximum, int depth = 0) {
    QJsonArray out;
    for (int i = 0; i < input.size() && i < maximum; ++i) {
        const auto v = input[i];
        if (v.isDouble() && std::isfinite(v.toDouble())) out.append(v);
        else if (v.isArray() && depth == 0) out.append(numericArray(v.toArray(), maximum, 1));
    }
    return out;
}
QJsonArray calibration(const QJsonArray &input) {
    QJsonArray out;
    for (int i = 0; i < input.size() && i < 4096; ++i) {
        const auto point = input[i].toObject();
        auto clean = numbers(point, "frequencyHz frequencyOffsetHz amplitudeOffsetDb uncertaintyDb");
        if (!clean.contains("frequencyHz")) continue;
        const auto source = point.value("conditions").toObject();
        auto conditions = numbers(source, "input rate lna vga rtlGain gainState");
        const auto backend = source.value("backend").toString();
        if (!backend.isEmpty() && !listed(backend, "fobos fobos-agile fobos-standard rtl-native rtl-tcp soapy agile standard rtl_tcp rtlsdr-native soapysdr bladerf-native hackrf-native network-client"))
            continue; // Unknown conditions must not become a wildcard calibration.
        if (!backend.isEmpty())
            conditions["backend"] = backend;
        clean["conditions"] = conditions;
        out.append(clean);
    }
    return out;
}
const char *FilterTypes = "low_pass high_pass band_pass notch dc_blocker gain limiter noise_gate de_emphasis parametric_eq low_shelf high_shelf compressor adaptive_notch noise_blanker cw_filter ctcss_suppressor spectral_denoise custom_fir";
const char *FilterNumbers = "frequencyHz frequency2Hz q gainDb thresholdDb ratio attackMs releaseMs timeConstantUs bandwidthHz amount";
QJsonObject filterParameters(const QJsonObject &input) {
    auto out = numbers(input, FilterNumbers);
    QJsonArray curve;
    const auto points = input.value("curve").toArray();
    for (int i = 0; i < points.size() && i < 256; ++i) {
        const auto point = numbers(points[i].toObject(), "frequencyHz gainDb");
        if (point.size() == 2) curve.append(point);
    }
    if (!curve.isEmpty()) out["curve"] = curve;
    return out;
}
QString filterChain(const QJsonValue &input) {
    if (!input.isString() || input.toString().size() > 1024 * 1024) return QStringLiteral("[]");
    const auto values = QJsonDocument::fromJson(input.toString().toUtf8()).array();
    QJsonArray out;
    for (int i = 0; i < values.size() && i < 32; ++i) {
        const auto value = values[i].toObject(); const auto type = value.value("type").toString();
        if (!listed(type, FilterTypes)) continue;
        auto clean = filterParameters(value);
        clean["id"] = QStringLiteral("filter-%1").arg(out.size());
        clean["type"] = type; clean["enabled"] = value.value("enabled").toBool(true);
        out.append(clean);
    }
    return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
}
QJsonObject settings(const QJsonObject &input) {
    auto out = numbers(input,
        "clockSource inputMode centerFrequency actualFrequency listeningFrequency sampleRate bandwidth modulationType "
        "cwDecoderSelectivity pskDecoderToneHz pskDecoderSquelchPercent pskDecoderEncoding sstvDemodulationMode "
        "fftLength fftTargetBinWidthHz fftWindowType lnaGain vgaGain hackRfLnaGainDb hackRfVgaGainDb hackRfBandwidthHz "
        "rtlTunerGainTenthsDb audioLowPassHz audioHighPassHz hfNoiseCancelDepth hfNoiseCancelRefGainDb "
        "hfNoiseCancelRefDelayNs hfNoiseCancelRefTiltDb hfAudioBlankerThreshold dmrBasebandSampleRate dmrAmbeLayout "
        "dmrManualTimingOffset dmrSlicerRatio scalePercent additionalScaleDivisor spectrumScienceAverageSeconds "
        "spectrumDetectorMode spectrumDetectorFrames spectrumVbwHz spectrumFftOverlapPercent spectrumAverageFrameCount "
        "spectrumAmplitudeUnit spectrumUpdateIntervalMs fftBackendPreference frequencyCalibrationOffsetHz "
        "amplitudeCalibrationOffsetDb waterfallRowsPerFrame waterfallDisplayMode waterfall3DResolutionDivisor "
        "waterfall3DSurfaceStyle waterfall3DSmoothing waterfall3DLighting waterfall3DHistoryRows waterfall3DSliceScrollStep waterfall3DSliceWidth");
    booleans(out, input, "pskDecoderAfc fftBinWidthModeEnabled rtlAgc simplifiedAudioChannelizer hfNoiseCancelFreeze "
        "hfAudioBlankerEnabled dmrManualTimingEnabled dmrAdaptiveSlicer spectrumScienceMaxHold spectrumScienceMinHold "
        "spectrumScienceAverage spectrumPercentile50 spectrumPercentile90 spectrumPercentile99 calibrationTableEnabled "
        "waterfall3DMonochrome");
    if (input.contains("calibrationTable")) out["calibrationTable"] = calibration(input.value("calibrationTable").toArray());
    if (input.contains("audioFilterChainJson")) out["audioFilterChainJson"] = filterChain(input.value("audioFilterChainJson"));
    out["audioEnabled"] = false; out["syncEnabled"] = false;
    return out;
}
QJsonObject workspace(const QJsonObject &input) {
    if (input.isEmpty()) return {};
    auto out = numbers(input, "sectorColumns sectorRows");
    out["version"] = 8;
    for (const auto &key : {"sectorColumnCuts", "sectorRowCuts", "sectorColumnRows", "sectorRowCutsByColumn"})
        if (input.value(key).isArray()) out[key] = numericArray(input.value(key).toArray(), 64);
    QJsonArray blocks, connections;
    QHash<QString, QString> ids, bindings, groups;
    const auto source = input.value("blocks").toArray();
    for (int i = 0; i < source.size() && i < 512; ++i) {
        const auto b = source[i].toObject(); const auto id = b.value("id").toString();
        const auto type = b.value("type").toString();
        const bool filter = type.startsWith("filter_") && listed(type.mid(7), FilterTypes);
        if (id.isEmpty() || id.size() > 256 || ids.contains(id) || (!filter && !listed(type,
            "receiver_source iq_source playback frequency_control fine_tune_control display_scale channel_filter "
            "multi_vfo_channelizer vfo_channel vfo_spectrum vfo_waterfall workspace_spectrum workspace_ruler workspace_waterfall "
            "resampler fft hf_interference demodulator audio_filter decoder digital_audio_settings digital_video_settings "
            "digital_text_output digital_image_output spectrum_display waterfall_2d waterfall_3d second_spectrum "
            "spectrum_measurement zoom_spectrum zoom_density zero_span research_analysis research_interference "
            "research_statistics research_iq research_dual_input research_analyzer research_density research_masks "
            "research_pulse research_session oscilloscope constellation eye_diagram digital_sync_lab oscilloscope_view "
            "constellation_view eye_diagram_view digital_sync_view spur_suppression audio_output ddc_channelizer "
            "pfb_channelizer branch_demodulator branch_audio_filter branch_audio_output branch_fft branch_spectrum branch_waterfall"))) continue;
        const auto newId = QStringLiteral("block-%1").arg(blocks.size()); ids.insert(id, newId);
        auto clean = numbers(b, "x y width height sector localX localY vfoIndex");
        clean["id"] = newId; clean["type"] = type;
        const auto parameters = b.value("settings").toObject();
        auto s = numbers(parameters, "frequencyHz bandwidthHz outputRateHz decimation firTaps transitionHz channels tapsPerPhase "
            "fftLength window refreshMs samples modulation lowPassHz squelchDb volume workspaceSector "
            "levelMin levelMax contrast sensitivity waterfallDisplayMode waterfallRowsPerFrame waterfall3DResolutionDivisor "
            "displayMode minimumDbfs maximumDbfs rowsPerFrame resolutionDivisor historyRows fftWindowType spectrumGradientOpacity "
            "waterfall3DSurfaceStyle waterfall3DSmoothing waterfall3DLighting waterfall3DSliceScrollStep waterfall3DSliceWidth "
            "waterfall3DSpectrumSliceScrollStep waterfall3DSpectrumSliceRows workspaceCanvasScale workspaceGroupOrder "
            "dockedSettingsSector dockedSettingsY");
        booleans(s, parameters, "mainFftRouteExplicit levelOverride secondSpectrum colorSpectrum gpuPrepared "
            "spectrumGradientFill showSpectrumFps showWaterfallFps showExtendedSpectrumInfo waterfallAreaMeasurementEnabled "
            "waterfall3DFixedPlane waterfall3DMonochrome waterfall3DSpectrumSliceCapture waterfall3DSpectrumSliceCaptureFixed "
            "waterfall3DVncSliceInput workspaceAutoFit workspaceAutoWidth workspaceGroupLayout workspaceGroupLocked dockedSettingsExpanded paused");
        for (const auto &key : {"bindingId", "workspaceGroupId"}) {
            const auto value = parameters.value(key).toString();
            if (value.isEmpty() || value.size() > 256) continue;
            auto &map = QString::fromLatin1(key) == "bindingId" ? bindings : groups;
            if (!map.contains(value)) map.insert(value, QStringLiteral("binding-%1").arg(map.size()));
            s[key] = map.value(value);
        }
        if (parameters.value("workspaceGroupCuts").isArray())
            s["workspaceGroupCuts"] = numericArray(parameters.value("workspaceGroupCuts").toArray(), 64);
        if (filter) {
            const auto f = filterParameters(parameters);
            for (auto it = f.begin(); it != f.end(); ++it) s.insert(it.key(), it.value());
        }
        if (parameters.contains("filters")) s["filters"] = filterChain(parameters.value("filters"));
        s["enabled"] = filter ? parameters.value("enabled").toBool(true) : false;
        s["externalProcessing"] = false; s["audioEnabled"] = false;
        clean["settings"] = s; blocks.append(clean);
    }
    QSet<QString> seen;
    const auto edges = input.value("connections").toArray();
    for (int i = 0; i < edges.size() && i < 1024; ++i) {
        const auto edge = edges[i].toObject(); const auto a = ids.value(edge.value("source").toString());
        const auto b = ids.value(edge.value("target").toString()); const auto key = a + ':' + b;
        if (a.isEmpty() || b.isEmpty() || a == b || seen.contains(key)) continue;
        seen.insert(key); connections.append(QJsonObject{{"source", a}, {"target", b}});
    }
    out["blocks"] = blocks; out["connections"] = connections;
    out["spectrumCompute"] = QJsonObject{{"mode", 0}, {"fallbackToLocal", true}};
    return out;
}
QJsonObject session(const QJsonObject &input) {
    auto out = numbers(input, "totalSnapshotCount omittedSnapshotCount");
    if (input.value("name").isString()) out["name"] = input.value("name").toString().left(1024);
    if (input.value("notes").isString()) out["notes"] = input.value("notes").toString().left(65536);
    QJsonArray snapshots; const auto values = input.value("snapshots").toArray();
    for (int i = 0; i < values.size() && i < 4096; ++i) {
        const auto value = values[i].toObject(); auto clean = numbers(value, "utcMs amplitudeUnit");
        clean["context"] = numbers(value.value("context").toObject(),
            "sampleRateHz centerFrequencyHz listeningFrequencyHz bandwidthHz inputMode modulationType fftLength fftWindowType");
        const auto metrics = value.value("metrics").toObject();
        auto m = numbers(metrics, "peakFrequencyHz peakDb noiseFloorDb snrDb channelPowerDb occupiedBandwidth95Hz");
        booleans(m, metrics, "valid"); clean["metrics"] = m; snapshots.append(clean);
    }
    out["snapshots"] = snapshots;
    out["exportedSnapshotCount"] = snapshots.size();
    return out;
}
QJsonObject scientific(const QJsonObject &input) {
    QJsonObject out;
    if (input.isEmpty()) return out;
    out["schema"] = "fobosapp-scientific-recording";
    out["schemaVersion"] = 1;
    const auto kind = input.value("captureKind").toString();
    if (listed(kind, "spectrum_event spectrum audio channel_iq full_iq")) out["captureKind"] = kind;
    for (const auto &phase : {"start", "end"}) {
        if (!input.value(phase).isObject()) continue;
        const auto value = input.value(phase).toObject(); QJsonObject snapshot;
        snapshot["phase"] = phase; utc(snapshot, value, "capturedAtUtc");
        if (value.value("software").isObject()) snapshot["software"] = QJsonObject{
            {"name", "Obrii SDR"}, {"version", version(value.value("software").toObject().value("version"))}};
        const auto radio = value.value("radio").toObject();
        auto r = numbers(radio, "centerFrequencyHz actualFrequencyHz listeningFrequencyHz sampleRateHz bandwidthHz modulationType clockSource inputMode lnaGain vgaGain rtlTunerGainTenthsDb");
        booleans(r, radio, "rtlAgc"); snapshot["radio"] = r;
        const auto analysis = value.value("analysis").toObject();
        auto a = numbers(analysis, "fftLength fftWindowType fftBinWidthHz rbwHz displayScalePercent displayLevelMinDb displayLevelMaxDb "
            "spectrumUpdateIntervalMs frequencyCalibrationOffsetHz amplitudeCalibrationOffsetDb effectiveFrequencyCalibrationOffsetHz "
            "effectiveAmplitudeCalibrationOffsetDb averageTimeSeconds detectorMode detectorFrames vbwHz fftOverlapPercent averageFrameCount amplitudeUnit");
        booleans(a, analysis, "calibrationTableEnabled maxHoldEnabled minHoldEnabled averageEnabled percentile50Enabled percentile90Enabled percentile99Enabled");
        if (analysis.contains("calibrationTable")) a["calibrationTable"] = calibration(analysis.value("calibrationTable").toArray());
        snapshot["analysis"] = a;
        const auto measurement = value.value("measurement").toObject();
        auto m = numbers(measurement, "rangeStartHz rangeEndHz peakFrequencyHz centroidFrequencyHz occupiedBandwidth99Hz occupiedBandwidth90Hz "
            "occupiedBandwidth95Hz width3DbHz width6DbHz width20DbHz peakDb noiseFloorDb snrDb channelPowerDb adjacentPowerLowerDb "
            "adjacentPowerUpperDb acprLowerDb acprUpperDb binCount");
        booleans(m, measurement, "valid"); snapshot["measurement"] = m;
        const auto markers = value.value("markers").toObject(); QJsonObject cleanMarkers;
        for (const auto &key : {"a", "b"}) if (markers.value(key).isObject()) {
            const auto marker = markers.value(key).toObject();
            auto clean = numbers(marker, "frequencyHz levelDb"); booleans(clean, marker, "enabled"); cleanMarkers[key] = clean;
        }
        if (markers.value("all").isArray()) {
            QJsonArray clean; const auto all = markers.value("all").toArray();
            for (int i = 0; i < all.size() && i < 256; ++i) {
                auto marker = numbers(all[i].toObject(), "frequencyHz levelDb");
                booleans(marker, all[i].toObject(), "enabled"); clean.append(marker);
            }
            cleanMarkers["all"] = clean;
        }
        if (!cleanMarkers.isEmpty()) snapshot["markers"] = cleanMarkers;
        out[phase] = snapshot;
    }
    if (input.value("experimentContext").isObject()) out["experimentContext"] = sanitizeContext(input.value("experimentContext").toObject());
    return out;
}
QJsonObject metadata(const QJsonObject &input, int kind) {
    auto out = numbers(input, "version targetBins scalePercent clockSource inputMode centerFrequency actualFrequency listeningFrequency "
        "sourceSampleRate bandwidth modulationType fftLength fftWindowType lnaGain vgaGain sampleRate bytesWritten firstFrameUtcMs "
        "lastFrameUtcMs spectrumFirstFrameUtcMs spectrumOffsetMs durationUtcSeconds effectiveSampleRate gaps droppedSamples droppedBlocks");
    out["app"] = "ObriiSDR";
    out["format"] = kind == 0 ? "fobos_spectrum_frames" : kind == 1 ? "channel_iq_wav_s16le" : "full_iq_raw_s8_interleaved";
    utc(out, input, "recordedAtUtc"); utc(out, input, "recordedAtLocal");
    if (input.value("scientificMetadata").isObject()) out["scientificMetadata"] = scientific(input.value("scientificMetadata").toObject());
    if (kind) out["filePath"] = Names[kind == 1 ? 1 : 3];
    return out;
}
QJsonObject sigmf(const QJsonObject &input) {
    auto global = numbers(input.value("global").toObject(), "core:sample_rate core:offset");
    global["core:datatype"] = "ci8"; global["core:version"] = "1.0.0";
    global["core:recorder"] = "Obrii SDR"; global["core:dataset"] = Names[3];
    QJsonObject out{{"global", global}};
    for (const auto &key : {"captures", "annotations"}) {
        QJsonArray clean; const auto values = input.value(key).toArray();
        for (int i = 0; i < values.size() && i < 4096; ++i) {
            auto entry = numbers(values[i].toObject(), "core:sample_start core:sample_count core:frequency core:freq_lower_edge core:freq_upper_edge");
            utc(entry, values[i].toObject(), "core:datetime"); clean.append(entry);
        }
        out[key] = clean;
    }
    return out;
}
bool safeLeaf(const QString &name) {
    if (name.isEmpty() || name.size() > 240 || name == "." || name == ".." || name.endsWith('.') || name.endsWith(' ')) return false;
    for (QChar c : name) if (c.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(c)) return false;
    const auto stem = name.section('.', 0, 0).toUpper();
    if (listed(stem, "CON PRN AUX NUL CLOCK$ CONIN$ CONOUT$")) return false;
    if (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT"))) {
        const auto digit = stem.back().unicode();
        if ((digit >= '1' && digit <= '9') || digit == 0x00b9 || digit == 0x00b2 || digit == 0x00b3) return false;
    }
    return true;
}
void noLinks(const QString &path) {
    QFileInfo info(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    for (;;) {
        check(!info.isSymLink(), "Symbolic links are not permitted in bundle paths");
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(info.absoluteFilePath().utf16()));
        check(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Windows reparse points are not permitted in bundle paths");
#endif
        const auto parent = info.absolutePath();
        if (parent == info.absoluteFilePath()) break;
        info.setFile(parent);
    }
}
void openSource(QFile &file, const QString &path) {
    noLinks(path);
    const QFileInfo info(path);
    check(info.isFile() && safeLeaf(info.fileName()), "Missing or unsafe bundle source file");
    file.setFileName(info.absoluteFilePath());
#ifdef Q_OS_WIN
    // Deny writers while copying, and reject a recorder which still has the file open for writing.
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(info.absoluteFilePath().utf16()), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "Cannot read completed recording; stop recording before export");
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(handle); check(false, "Cannot open recording file descriptor"); }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { _close(fd); check(false, "Cannot open recording"); }
#else
    check(file.open(QIODevice::ReadOnly), "Cannot open recording");
#endif
}
QJsonObject json(QFile &file) {
    check(file.size() > 0 && file.size() <= JsonLimit && file.seek(0), "JSON metadata exceeds the size limit");
    const auto bytes = file.read(file.size());
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error);
    check(error.error == QJsonParseError::NoError && doc.isObject(), "Invalid JSON metadata");
    return doc.object();
}
struct Work {
    const std::atomic_bool *cancel;
    std::function<void(qint64, qint64)> progress;
    qint64 done = 0, total = 0;
    void poll() const { check(!cancel || !cancel->load(), "Experiment bundle operation cancelled"); }
    void advance(qint64 n) { poll(); done += n; if (progress) progress(done, total); poll(); }
};
void emitBytes(QIODevice *out, QCryptographicHash &hash, const QByteArray &bytes) {
    if (out) check(out->write(bytes) == bytes.size(), "Cannot write experiment bundle file");
    hash.addData(bytes);
}
QByteArray take(QFile &file, qint64 size, Work &work) {
    work.poll(); const auto bytes = file.read(size);
    check(bytes.size() == size, "Truncated recording or failed read"); work.advance(size); return bytes;
}
void copy(QFile &source, QIODevice *destination, qint64 count, QCryptographicHash &hash, Work &work) {
    while (count > 0) {
        const auto size = (std::min)(ChunkSize, count);
        emitBytes(destination, hash, take(source, size, work)); count -= size;
    }
}
QJsonObject spectrum(QFile &source, QIODevice *destination, bool exporting, QCryptographicHash &hash,
                     Work &work, QJsonObject &summary) {
    check(source.seek(0), "Cannot seek spectrum recording");
    const auto prefix = take(source, 12, work);
    check(prefix.left(8) == QByteArray("FBSPEC1\n", 8), "Not an FBSPEC1 spectrum recording");
    const auto length = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(prefix.constData() + 8));
    check(length > 0 && length <= JsonLimit, "Invalid spectrum JSON header length");
    QJsonParseError parse; const auto doc = QJsonDocument::fromJson(take(source, length, work), &parse);
    check(parse.error == QJsonParseError::NoError && doc.isObject(), "Invalid spectrum JSON header");
    const auto original = doc.object();
    check(original.value("format") == "fobos_spectrum_frames" && original.value("version").toInt() == 1,
        "Unsupported spectrum recording format/version");
    const auto clean = metadata(original, 0);
    if (!exporting) check(clean == original, "Bundle spectrum metadata is not sanitized");
    const auto bytes = QJsonDocument(exporting ? clean : original).toJson(QJsonDocument::Compact);
    if (exporting) {
        QByteArray header("FBSPEC1\n", 8); header.resize(12);
        qToLittleEndian<quint32>(quint32(bytes.size()), reinterpret_cast<uchar *>(header.data() + 8));
        emitBytes(destination, hash, header); emitBytes(destination, hash, bytes);
    } else {
        // Hash the original bytes, including JSON whitespace, not a reserialization.
        check(source.seek(0), "Cannot seek spectrum header");
        const auto raw = source.read(12 + length); check(raw.size() == 12 + length, "Truncated spectrum header");
        emitBytes(nullptr, hash, raw);
    }
    quint64 frames = 0; qint64 firstUtc = 0, lastUtc = 0, firstElapsed = 0, lastElapsed = 0;
    while (source.pos() < source.size()) {
        const auto header = take(source, 52, work);
        QDataStream stream(header); stream.setByteOrder(QDataStream::LittleEndian); stream.setVersion(QDataStream::Qt_5_15);
        quint32 magic; qint64 elapsed, time; double center, low, high; qint32 fft, bins;
        stream >> magic >> elapsed >> time >> center >> low >> high >> fft >> bins;
        check(stream.status() == QDataStream::Ok && magic == 0x314d5246U && fft > 0 && bins > 0 && bins <= 2097152 &&
            std::isfinite(center) && std::isfinite(low) && std::isfinite(high) && high >= low, "Invalid spectrum frame");
        emitBytes(destination, hash, header); copy(source, destination, qint64(bins) * 4, hash, work);
        if (!frames) { firstUtc = time; firstElapsed = elapsed; }
        ++frames; lastUtc = time; lastElapsed = elapsed;
    }
    check(frames > 0, "Spectrum recording has no complete frames");
    summary = {{"frameCount", QString::number(frames)}, {"firstFrameUtcMs", QString::number(firstUtc)},
        {"lastFrameUtcMs", QString::number(lastUtc)}, {"firstElapsedMs", QString::number(firstElapsed)},
        {"lastElapsedMs", QString::number(lastElapsed)}, {"gapMetadata", "FBSPEC1 has no explicit gap records; original frame timestamps preserved"}};
    return clean;
}
struct File {
    QString name, path;
    std::unique_ptr<QFile> source;
    qint64 size = 0;
    QDateTime modified;
    QJsonObject clean;
};
void add(std::vector<File> &files, const QString &name, const QString &path) {
    File entry; entry.name = name; entry.path = QFileInfo(path).absoluteFilePath();
    entry.source = std::make_unique<QFile>(); openSource(*entry.source, entry.path);
    entry.size = entry.source->size(); entry.modified = QFileInfo(path).lastModified(); files.push_back(std::move(entry));
}
File *find(std::vector<File> &files, const QString &name) {
    for (auto &file : files) if (file.name == name) return &file;
    return nullptr;
}
void validateCompanions(std::vector<File> &files, bool exporting) {
    for (int kind = 1; kind <= 2; ++kind) {
        auto *data = find(files, Names[kind == 1 ? 1 : 3]); auto *sidecar = find(files, Names[kind == 1 ? 2 : 4]);
        check(bool(data) == bool(sidecar), "IQ data and timing sidecar must both be present");
        if (!data) continue;
        const auto original = json(*sidecar->source); sidecar->clean = metadata(original, kind);
        check(original.value("format") == sidecar->clean.value("format"), "Unsupported IQ sidecar format");
        check(exporting || original == sidecar->clean, "Bundle IQ metadata is not sanitized");
        const auto rate = original.value("sampleRate").toDouble();
        const auto count = original.value("bytesWritten").toDouble(-1);
        const qint64 payload = data->size - (kind == 1 ? 44 : 0);
        check(std::isfinite(rate) && rate > 0 && payload > 0 && payload % (kind == 1 ? 4 : 2) == 0 &&
            count == double(payload), "Incomplete IQ recording or invalid byte count/sample rate");
        if (kind == 1) {
            auto &f = *data->source; check(f.seek(0), "Cannot seek IQ WAV"); const auto header = f.read(44);
            const auto *p = reinterpret_cast<const uchar *>(header.constData());
            check(header.size() == 44 && header.left(4) == "RIFF" && header.mid(8, 8) == "WAVEfmt " && header.mid(36, 4) == "data",
                "Unsupported IQ WAV header");
            check(qFromLittleEndian<quint32>(p+16) == 16 && qFromLittleEndian<quint16>(p+20) == 1 &&
                qFromLittleEndian<quint16>(p+22) == 2 && qFromLittleEndian<quint16>(p+32) == 4 && qFromLittleEndian<quint16>(p+34) == 16 &&
                double(qFromLittleEndian<quint32>(p+24)) == std::round(rate) &&
                double(qFromLittleEndian<quint32>(p+28)) == std::round(rate) * 4 &&
                qFromLittleEndian<quint32>(p+40) == quint32((std::min)(payload, qint64(0xffffffffULL))) &&
                qFromLittleEndian<quint32>(p+4) == quint32((std::min)(data->size - 8, qint64(0xffffffffULL))),
                "IQ WAV was not finalized or has incompatible sample geometry");
        }
    }
    if (auto *meta = find(files, Names[5])) {
        check(find(files, Names[3]) != nullptr, "SigMF metadata has no linked IQ data");
        const auto original = json(*meta->source); meta->clean = sigmf(original);
        check(original.value("global").toObject().value("core:datatype") == "ci8" &&
            meta->clean.value("global").toObject().value("core:sample_rate") == find(files, Names[4])->clean.value("sampleRate"),
            "SigMF data type or sample rate disagrees with IQ sidecar");
        check(exporting || original == meta->clean, "Bundle SigMF metadata is not sanitized");
    }
}
qint64 totalSize(const std::vector<File> &files) {
    qint64 total = 0;
    for (const auto &f : files) {
        check(f.size >= 0 && f.size <= (std::numeric_limits<qint64>::max)() - total, "Bundle byte count overflow"); total += f.size;
    }
    return total;
}
void unchanged(const File &f) {
    noLinks(f.path);
    check(f.source->size() == f.size && QFileInfo(f.path).lastModified() == f.modified,
        "Recording changed during bundle operation");
}
QJsonObject selectedContext(const QJsonObject &header, const QJsonObject &exportContext, QString &origin) {
    auto recorded = header.value("scientificMetadata").toObject().value("experimentContext").toObject();
    origin = recorded.isEmpty() ? QStringLiteral("export-time") : QStringLiteral("recording-start");
    auto chosen = recorded.isEmpty() ? sanitizeContext(exportContext) : sanitizeContext(recorded);
    const auto extra = sanitizeContext(exportContext);
    for (const auto &key : {"session", "note"}) if (extra.contains(key)) chosen[key] = extra.value(key);
    return chosen;
}
}

QJsonObject sanitizeContext(QJsonObject context) {
    QJsonObject out;
    if (context.contains("appVersion")) out["appVersion"] = version(context.value("appVersion"));
    utc(out, context, "capturedAtUtc");
    if (context.value("settings").isObject()) out["settings"] = settings(context.value("settings").toObject());
    if (context.value("display").isObject()) {
        const auto input = context.value("display").toObject();
        auto display = numbers(input, "levelMin levelMax contrast sensitivity waterfallDisplayMode waterfallRowsPerFrame waterfall3DResolutionDivisor");
        booleans(display, input, "coloredSpectrum"); out["display"] = display;
    }
    if (context.value("workspace").isObject()) out["workspace"] = workspace(context.value("workspace").toObject());
    if (context.value("note").isString()) out["note"] = context.value("note").toString().left(8192);
    if (context.value("session").isObject()) out["session"] = session(context.value("session").toObject());
    return out;
}

bool write(const QString &destinationDirectory, const QString &recordingPath, const QJsonObject &context,
           QString *error, const std::atomic_bool *cancel, std::function<void(qint64, qint64)> progress) {
    if (error) error->clear();
    try {
        Work work{cancel, std::move(progress)}; work.poll();
        const QFileInfo sourceInfo(recordingPath), destinationInfo(destinationDirectory);
        check(recordingPath.endsWith(".fbspec") && safeLeaf(sourceInfo.fileName()), "Select a completed .fbspec recording");
        check(!destinationDirectory.isEmpty() && safeLeaf(destinationInfo.fileName()), "Unsafe bundle destination name");
        noLinks(destinationDirectory);
        check(!destinationInfo.exists() && destinationInfo.dir().exists(), "Bundle destination must be a new folder in an existing directory");
        std::vector<File> files; add(files, SpectrumName, sourceInfo.absoluteFilePath());
        QString base = sourceInfo.absoluteFilePath();
        base.chop(base.endsWith("_spectrum.fbspec") ? 16 : 7);
        const QStringList suffixes{"_channel_iq.wav", "_channel_iq.json", "_full_iq_s8.iq8", "_full_iq_s8.json", "_full_iq_s8.sigmf-meta"};
        for (int i = 0; i < suffixes.size(); ++i) {
            const QFileInfo info(base + suffixes[i]);
            if (info.exists() || info.isSymLink()) add(files, Names[i+1], info.absoluteFilePath());
        }
        validateCompanions(files, true); work.total = totalSize(files); work.advance(0);
        check(destinationInfo.dir().mkdir(destinationInfo.fileName()), "Cannot create new bundle directory");
        QDir directory(destinationInfo.absoluteFilePath());
        QFile marker(directory.filePath(PartialName));
        check(marker.open(QIODevice::WriteOnly | QIODevice::NewOnly) && marker.write("Incomplete experiment bundle\n") > 0,
            "Cannot create partial bundle marker"); marker.close();
        QJsonArray entries; QJsonObject header, summary;
        for (auto &file : files) {
            work.poll(); noLinks(directory.absolutePath());
            QFile output(directory.filePath(file.name));
            check(output.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Cannot create bundle file without overwriting");
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (file.name == SpectrumName) header = spectrum(*file.source, &output, true, hash, work, summary);
            else if (!file.clean.isEmpty()) {
                emitBytes(&output, hash, QJsonDocument(file.clean).toJson(QJsonDocument::Compact)); work.advance(file.size);
            } else { check(file.source->seek(0), "Cannot seek IQ data"); copy(*file.source, &output, file.size, hash, work); }
            check(output.flush(), "Cannot flush bundle file");
            entries.append(QJsonObject{{"path", file.name}, {"size", QString::number(output.size())},
                {"sha256", QString::fromLatin1(hash.result().toHex())}});
            output.close(); unchanged(file);
        }
        QString origin;
        const auto selected = selectedContext(header, context, origin);
        QJsonObject manifest{{"schemaVersion", 1}, {"format", "obrii-experiment-bundle"}, {"recording", SpectrumName},
            {"appVersion", version(context.value("appVersion"))}, {"createdAtUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"context", selected}, {"contextOrigin", origin}, {"recordingMetadata", header}, {"recordingSummary", summary}, {"files", entries},
            {"privacyWarning", "Explicit notes/session names and unchanged signal payloads may contain private information."}};
        work.poll();
        for (const auto &file : files) unchanged(file);
        noLinks(directory.filePath(ManifestName));
        check(!QFileInfo::exists(directory.filePath(ManifestName)), "Bundle manifest already exists");
        QSaveFile final(directory.filePath(ManifestName)); final.setDirectWriteFallback(false);
        check(final.open(QIODevice::WriteOnly), "Cannot create atomic bundle manifest");
        const auto bytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
        check(bytes.size() <= JsonLimit && final.write(bytes) == bytes.size(), "Cannot write bundle manifest");
        work.poll(); noLinks(directory.filePath(ManifestName));
        check(!QFileInfo::exists(directory.filePath(ManifestName)), "Bundle manifest already exists");
        check(final.commit(), "Cannot commit bundle manifest");
        check(QFile::remove(directory.filePath(PartialName)), "Bundle finalization could not remove its partial marker");
        return true;
    } catch (const std::exception &e) { if (error) *error = QString::fromUtf8(e.what()); return false; }
}

bool read(const QString &manifestPath, Contents &contents, QString *error, const std::atomic_bool *cancel,
          std::function<void(qint64, qint64)> progress) {
    contents = {}; if (error) error->clear();
    try {
        Work work{cancel, std::move(progress)}; work.poll();
        const QFileInfo info(manifestPath); check(info.fileName() == ManifestName, "Select manifest.json");
        noLinks(info.absoluteFilePath()); const QDir directory = info.absoluteDir();
        const QFileInfo partial(directory.filePath(PartialName));
        check(!partial.exists() && !partial.isSymLink(), "Bundle is partial and cannot be opened");
        QFile manifestFile; openSource(manifestFile, info.absoluteFilePath()); const auto manifest = json(manifestFile);
        check(manifest.value("schemaVersion").toInt() == 1 && manifest.value("format") == "obrii-experiment-bundle" &&
            manifest.value("recording") == SpectrumName && manifest.value("files").isArray(), "Unsupported experiment bundle manifest");
        const auto entries = manifest.value("files").toArray();
        check(!entries.isEmpty() && entries.size() <= Names.size(), "Invalid bundle file list");
        std::vector<File> files; QHash<QString, QJsonObject> expected; QSet<QString> paths;
        static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
        for (const auto &value : entries) {
            const auto entry = value.toObject(); const auto name = entry.value("path").toString();
            check(safeLeaf(name) && Names.contains(name) && !paths.contains(name.toCaseFolded()), "Unsafe, duplicate or unsupported bundle path");
            paths.insert(name.toCaseFolded());
            bool ok = false; const auto sizeText = entry.value("size").toString(); const auto size = sizeText.toLongLong(&ok);
            check(ok && size >= 0 && QString::number(size) == sizeText && digest.match(entry.value("sha256").toString()).hasMatch(),
                "Invalid bundle size or SHA256");
            add(files, name, directory.filePath(name)); check(files.back().size == size, "Bundle file size mismatch"); expected.insert(name, entry);
        }
        check(find(files, SpectrumName) != nullptr, "Bundle does not contain its spectrum recording");
        validateCompanions(files, false); work.total = totalSize(files); work.advance(0);
        QJsonObject header, summary;
        for (auto &file : files) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (file.name == SpectrumName) header = spectrum(*file.source, nullptr, false, hash, work, summary);
            else { check(file.source->seek(0), "Cannot seek bundle file"); copy(*file.source, nullptr, file.size, hash, work); }
            check(QString::fromLatin1(hash.result().toHex()) == expected.value(file.name).value("sha256").toString(), "Bundle SHA256 mismatch");
            unchanged(file);
        }
        work.poll(); QString origin;
        auto selected = selectedContext(header, manifest.value("context").toObject(), origin);
        QJsonObject verified{{"schemaVersion", 1}, {"format", "obrii-experiment-bundle"}, {"recording", SpectrumName},
            {"appVersion", version(manifest.value("appVersion"))}, {"context", selected}, {"contextOrigin", origin},
            {"recordingMetadata", header}, {"recordingSummary", summary}, {"files", QJsonArray()}};
        utc(verified, manifest, "createdAtUtc");
        QJsonArray verifiedFiles;
        for (const auto &file : files) verifiedFiles.append(QJsonObject{{"path", file.name},
            {"size", QString::number(file.size)}, {"sha256", expected.value(file.name).value("sha256")}});
        verified["files"] = verifiedFiles;
        verified["privacyWarning"] = "Explicit notes/session names and unchanged signal payloads may contain private information.";
        contents.recordingPath = directory.filePath(SpectrumName); contents.context = selected; contents.manifest = verified;
        return true;
    } catch (const std::exception &e) { contents = {}; if (error) *error = QString::fromUtf8(e.what()); return false; }
}
} // namespace ExperimentBundle
