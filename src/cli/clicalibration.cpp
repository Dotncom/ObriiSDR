#include "clicalibration.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr int kMaxPoints = 4096;
constexpr int kMaxBins = 32768;
constexpr int kMaxBatchSamples = 1048576;
constexpr int kMaxFileBytes = 4 * 1024 * 1024;

QJsonObject fail(const QString &message) {
    return {{"ok", false}, {"error", message}};
}

bool real(const QJsonObject &o, const char *name, double fallback, double &value) {
    const QJsonValue v = o.value(QLatin1String(name));
    value = v.isUndefined() ? fallback : v.toDouble(std::numeric_limits<double>::quiet_NaN());
    return (v.isUndefined() || v.isDouble()) && std::isfinite(value);
}

bool integer(const QJsonObject &o, const char *name, int fallback, int minimum, int maximum, int &value) {
    double d = 0;
    if (!real(o, name, fallback, d) || d < minimum || d > maximum || std::floor(d) != d) return false;
    value = static_cast<int>(d);
    return true;
}

bool context(const QJsonValue &value, ReceiverCalibrationContext &c) {
    if (!value.isUndefined() && !value.isObject()) return false;
    const auto o = value.toObject();
    if (o.contains("backend") && (!o.value("backend").isString() || o.value("backend").toString().size() > 128))
        return false;
    c.backendId = o.value("backend").toString().trimmed();
    return real(o, "rate", 0, c.sampleRateHz) && c.sampleRateHz >= 0 && c.sampleRateHz <= 1e12 &&
        integer(o, "input", -1, -1, 1000000, c.inputMode) &&
        integer(o, "lna", -1, -1, 1000000, c.lnaGain) &&
        integer(o, "vga", -1, -1, 1000000, c.vgaGain) &&
        integer(o, "rtlGain", -1, -1, 1000000, c.rtlGainTenthsDb) &&
        integer(o, "gainState", -1, -1, 1000000, c.gainState);
}

bool keyFromJson(const QJsonValue &value, ReceiverNoiseBaselineKey &key) {
    if (!value.isObject()) return false;
    const auto o = value.toObject();
    if (o.contains("calibrationTableEnabled") && !o.value("calibrationTableEnabled").isBool()) return false;
    key.calibrationTableEnabled = o.value("calibrationTableEnabled").toBool();
    return context(o.value("receiver"), key.receiver) &&
        real(o, "centerFrequencyHz", 0, key.centerFrequencyHz) && key.centerFrequencyHz >= 0 && key.centerFrequencyHz <= 1e12 &&
        real(o, "amplitudeOffsetDb", 0, key.amplitudeOffsetDb) && std::abs(key.amplitudeOffsetDb) <= 300 &&
        integer(o, "fftLength", 0, 0, 1048576, key.fftLength) &&
        integer(o, "fftWindowType", 0, 0, 1000, key.fftWindowType) &&
        integer(o, "rfBandwidthHz", 0, 0, 2147483647, key.rfBandwidthHz) &&
        integer(o, "calibrationPointCount", 0, 0, kMaxPoints, key.calibrationPointCount);
}

QJsonObject keyJson(const ReceiverNoiseBaselineKey &key) {
    return {{"receiver", key.receiver.toJson()}, {"centerFrequencyHz", key.centerFrequencyHz},
            {"fftLength", key.fftLength}, {"fftWindowType", key.fftWindowType},
            {"rfBandwidthHz", key.rfBandwidthHz}, {"amplitudeOffsetDb", key.amplitudeOffsetDb},
            {"calibrationTableEnabled", key.calibrationTableEnabled}, {"calibrationPointCount", key.calibrationPointCount}};
}

bool noiseKeyValid(const ReceiverNoiseBaselineKey &key) {
    return !key.receiver.backendId.isEmpty() && key.receiver.sampleRateHz > 0 && key.fftLength >= 2;
}

bool dbfs(const QJsonObject &o) {
    return !o.contains("unit") || o.value("unit") == QJsonValue(QStringLiteral("dBFS"));
}

bool numbers(const QJsonValue &value, std::vector<float> &out) {
    if (!value.isArray()) return false;
    const auto array = value.toArray();
    if (array.size() < 2 || array.size() > kMaxBins) return false;
    out.reserve(array.size());
    for (const auto &v : array) {
        if (!v.isDouble() || !std::isfinite(v.toDouble())) return false;
        const float f = static_cast<float>(v.toDouble());
        if (!std::isfinite(f)) return false;
        out.push_back(f);
    }
    return true;
}

double gridTolerance(float frequency) {
    return (std::max)(1.0, std::abs(static_cast<double>(frequency)) * std::numeric_limits<float>::epsilon() * 2);
}

bool spectrumValid(const std::vector<float> &f, const std::vector<float> &db) {
    if (f.size() < 2 || f.size() > kMaxBins || f.size() != db.size()) return false;
    const double step = (static_cast<double>(f.back()) - f.front()) / (f.size() - 1);
    if (!std::isfinite(step) || step <= 0) return false;
    for (std::size_t i = 0; i < f.size(); ++i) {
        // Bounds avoid float overflow/underflow in the shared power accumulators.
        if (!std::isfinite(f[i]) || f[i] < 0 || f[i] > 1e12 || !std::isfinite(db[i]) || db[i] < -300 || db[i] > 300 ||
            (i && f[i] <= f[i - 1]) ||
            std::abs(f[i] - (f.front() + step * i)) > gridTolerance(f[i])) return false;
    }
    return true;
}

bool sameGrid(const std::vector<float> &a, const std::vector<float> &b) {
    // Identical float grids prevent silent remapping of measured bins.
    return a == b;
}

QJsonValue finiteJsonNumber(double v) {
    return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
}

QJsonArray jsonNumbers(const std::vector<float> &v) {
    QJsonArray result;
    for (float n : v) result.append(finiteJsonNumber(n));
    return result;
}

QJsonObject tableDocument(const ReceiverCalibrationTable &table) {
    QJsonArray rows;
    for (const auto &p : table.points()) {
        rows.append(QJsonObject{{"frequencyHz", p.frequencyHz}, {"frequencyOffsetHz", p.frequencyOffsetHz},
            {"amplitudeOffsetDb", p.amplitudeOffsetDb}, {"uncertaintyDb", p.uncertaintyDb},
            {"note", p.note}, {"conditions", p.conditions.toJson()}});
    }
    return {{"schema", "obrii.cli.calibration.table"}, {"version", 1}, {"calibrationTable", rows}};
}

QJsonObject exportDocument(const QJsonObject &request, const QJsonObject &document) {
    if (!request.contains("path")) return {{"ok", true}, {"document", document}};
    const QString path = request.value("path").toString();
    if (path.trimmed().isEmpty()) return fail(QStringLiteral("A nonempty export path is required"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail(file.errorString());
    const QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        const QString error = file.errorString();
        file.remove();
        return fail(error);
    }
    return {{"ok", true}, {"path", path}};
}

bool source(const QJsonObject &r, YFactorSourceMode &mode, double &cold, double &hot, double &enr) {
    if (r.contains("sourceMode") && !r.value("sourceMode").isString()) return false;
    const QString s = r.value("sourceMode").toString(QStringLiteral("temperatures"));
    if (s != "temperatures" && s != "enr") return false;
    mode = s == "enr" ? YFactorSourceMode::Enr : YFactorSourceMode::Temperatures;
    return real(r, "coldTemperatureKelvin", 290, cold) && cold > 0 && cold <= 1e9 &&
        real(r, "hotTemperatureKelvin", 0, hot) && hot >= 0 && hot <= 1e12 &&
        real(r, "enrDb", 15, enr) && enr > -20 && enr <= 100 &&
        (mode == YFactorSourceMode::Enr || hot > cold);
}

bool inRange(const std::vector<float> &f, double low, double high) {
    return f.front() >= low && f.back() <= high;
}
} // namespace

void CliCalibration::cancel() {
    noise.cancelCapture();
    yfactor.cancelCapture();
    yfactor.clearPendingCold();
    captureGrid.clear();
}

QJsonObject CliCalibration::status() const {
    const QString stage = noise.isCapturing() ? QStringLiteral("noise") :
        yfactor.stage() == YFactorCaptureStage::Cold ? QStringLiteral("cold") :
        yfactor.stage() == YFactorCaptureStage::Hot ? QStringLiteral("hot") : QStringLiteral("idle");
    return {{"ok", true}, {"selected", haveSelection}, {"key", keyJson(selected)},
            {"inputUnit", "dBFS"}, {"absolutePowerCalibrated", false},
            {"tablePoints", table.points().size()}, {"activeTablePoints", table.activePointCount()},
            {"noiseProfiles", noise.profileCount()}, {"yfactorProfiles", yfactor.profileCount()},
            {"capture", stage}, {"capturedFrames", noise.isCapturing() ? noise.capturedFrames() : yfactor.capturedFrames()},
            {"targetFrames", noise.isCapturing() ? noise.targetFrames() : yfactor.targetFrames()},
            {"coldReady", haveSelection && yfactor.coldReady(selected)}};
}

QJsonObject CliCalibration::command(const QJsonObject &r) {
    const QString op = r.value("operation").toString();
    if (op == "status") return status();
    if (op == "capture.cancel") { cancel(); return status(); }
    if (op == "table.export") return exportDocument(r, tableDocument(table));
    if (op == "profile.select") {
        ReceiverNoiseBaselineKey next;
        if (!keyFromJson(r.value("key"), next)) return fail(QStringLiteral("Invalid receiver profile key"));
        if (capturing()) return fail(QStringLiteral("Cancel or finish capture before selecting a profile"));
        if (!haveSelection || keyJson(next) != keyJson(selected)) cancel();
        selected = next;
        haveSelection = true;
        table.setContext(selected.receiver);
        return status();
    }
    if (op == "table.load" || op == "table.clear") {
        if (capturing()) return fail(QStringLiteral("Cancel or finish capture before replacing the table"));
        QJsonObject doc = r;
        if (op == "table.load" && r.contains("path")) {
            if (r.contains("calibrationTable")) return fail(QStringLiteral("Use path or calibrationTable, not both"));
            QFile file(r.value("path").toString());
            if (!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
            if (file.size() > kMaxFileBytes) return fail(QStringLiteral("Table file exceeds 4 MiB"));
            const QByteArray bytes = file.read(kMaxFileBytes + 1);
            QJsonParseError error;
            const auto parsed = QJsonDocument::fromJson(bytes, &error);
            if (bytes.size() > kMaxFileBytes || error.error != QJsonParseError::NoError || !parsed.isObject())
                return fail(QStringLiteral("Expected a JSON table object (maximum 4 MiB)"));
            doc = parsed.object();
        }
        QVector<ReceiverCalibrationPoint> points;
        if (op == "table.load") {
            if ((doc.contains("schema") && doc.value("schema") != QJsonValue(QStringLiteral("obrii.cli.calibration.table"))) ||
                (doc.contains("version") && doc.value("version") != QJsonValue(1)))
                return fail(QStringLiteral("Unsupported table schema/version"));
            if (!doc.value("calibrationTable").isArray()) return fail(QStringLiteral("calibrationTable array is required"));
            const auto rows = doc.value("calibrationTable").toArray();
            if (rows.size() > kMaxPoints) return fail(QStringLiteral("Maximum 4096 calibration points"));
            for (const auto &row : rows) {
                if (!row.isObject()) return fail(QStringLiteral("Calibration points must be objects"));
                const auto o = row.toObject();
                ReceiverCalibrationPoint p;
                const double missing = std::numeric_limits<double>::quiet_NaN();
                if (!real(o, "frequencyHz", missing, p.frequencyHz) || p.frequencyHz < 0 || p.frequencyHz > 1e12 ||
                    !real(o, "frequencyOffsetHz", missing, p.frequencyOffsetHz) || std::abs(p.frequencyOffsetHz) > 1e12 ||
                    !real(o, "amplitudeOffsetDb", missing, p.amplitudeOffsetDb) || std::abs(p.amplitudeOffsetDb) > 300 ||
                    !real(o, "uncertaintyDb", 0, p.uncertaintyDb) || p.uncertaintyDb < 0 || p.uncertaintyDb > 300 ||
                    !context(o.value("conditions"), p.conditions) ||
                    (o.contains("note") && (!o.value("note").isString() || o.value("note").toString().size() > 256)))
                    return fail(QStringLiteral("Invalid calibration row; table unchanged"));
                p.note = o.value("note").toString();
                points.append(p);
            }
        }
        // Profile keys contain point count, not a table content hash.
        cancel();
        noise.clearAll();
        yfactor.clearAll();
        table.setPoints(points);
        table.setContext(selected.receiver);
        return status();
    }
    if (!haveSelection) return fail(QStringLiteral("Select a receiver profile first with profile.select"));
    if (op == "table.correction") {
        double frequency = 0, level = 0;
        if (!real(r, "frequencyHz", -1, frequency) || frequency < 0 || frequency > 1e12 || !dbfs(r) ||
            (r.contains("levelDbfs") && (!real(r, "levelDbfs", 0, level) || std::abs(level) > 300)))
            return fail(QStringLiteral("Expected frequencyHz and optional levelDbfs [-300,300] in dBFS"));
        const auto c = table.correctionAt(frequency);
        QJsonObject out{{"ok", true}, {"valid", c.valid}, {"frequencyHz", frequency},
                        {"unit", "dBFS"}, {"absolutePowerCalibrated", false}};
        out.insert("frequencyOffsetHz", c.valid ? finiteJsonNumber(c.frequencyOffsetHz) : QJsonValue(QJsonValue::Null));
        out.insert("amplitudeOffsetDb", c.valid ? finiteJsonNumber(c.amplitudeOffsetDb) : QJsonValue(QJsonValue::Null));
        out.insert("uncertaintyDb", c.valid ? finiteJsonNumber(c.uncertaintyDb) : QJsonValue(QJsonValue::Null));
        if (r.contains("levelDbfs")) out.insert("correctedLevelDbfs", c.valid ? finiteJsonNumber(level + c.amplitudeOffsetDb) : QJsonValue(QJsonValue::Null));
        return out;
    }
    if (!noiseKeyValid(selected)) return fail(QStringLiteral("Noise/Y-factor requires backend, positive rate and fftLength >= 2"));
    if (op == "noise.clear" || op == "yfactor.clear") {
        if (capturing()) return fail(QStringLiteral("Cancel or finish capture before clearing profiles"));
        cancel();
        const bool removed = op == "noise.clear" ? noise.clearMatching(selected) : yfactor.clearMatching(selected);
        return {{"ok", true}, {"removed", removed}};
    }
    if (op == "noise.capture" || op == "yfactor.cold" || op == "yfactor.hot") {
        int frames = 0;
        if (!integer(r, "frames", 8, 8, 512, frames) || !dbfs(r)) return fail(QStringLiteral("Capture requires frames 8..512, unit dBFS"));
        if (capturing()) return fail(QStringLiteral("A capture is already active"));
        if (op == "noise.capture") { cancel(); noise.beginCapture(selected, frames); }
        else if (op == "yfactor.cold") { cancel(); yfactor.beginColdCapture(selected, frames); }
        else {
            YFactorSourceMode mode;
            double cold = 0, hot = 0, enr = 0;
            QString error;
            if (!source(r, mode, cold, hot, enr)) return fail(QStringLiteral("Invalid Y-factor source parameters"));
            if (!yfactor.beginHotCapture(selected, frames, mode, cold, hot, enr, &error)) return fail(error);
        }
        return status();
    }
    if (op == "yfactor.compute") {
        if (capturing()) return fail(QStringLiteral("Cancel or finish capture before computing a table"));
        std::vector<float> f;
        if (!numbers(r.value("frequenciesHz"), f) || !dbfs(r)) return fail(QStringLiteral("Expected frequenciesHz and unit dBFS"));
        const auto coldFrames = r.value("coldFramesDbfs").toArray();
        const auto hotFrames = r.value("hotFramesDbfs").toArray();
        if (coldFrames.size() < 8 || coldFrames.size() > 512 || hotFrames.size() < 8 || hotFrames.size() > 512 ||
            (coldFrames.size() + hotFrames.size()) * f.size() > kMaxBatchSamples)
            return fail(QStringLiteral("Expected 8..512 cold/hot frames; maximum 1048576 total level samples"));
        YFactorSourceMode mode;
        double cold = 0, hot = 0, enr = 0;
        if (!source(r, mode, cold, hot, enr)) return fail(QStringLiteral("Invalid Y-factor source parameters"));
        auto candidate = yfactor;
        candidate.clearPendingCold();
        candidate.beginColdCapture(selected, coldFrames.size());
        const auto feed = [&](const QJsonArray &frames) {
            for (const auto &frame : frames) {
                std::vector<float> db;
                if (!numbers(frame, db) || !spectrumValid(f, db) ||
                    candidate.addFrame(f, db, selected) == ReceiverYFactorCalibration::CaptureResult::Rejected) return false;
            }
            return true;
        };
        QString error;
        if (!feed(coldFrames) || !candidate.beginHotCapture(selected, hotFrames.size(), mode, cold, hot, enr, &error) || !feed(hotFrames))
            return fail(QStringLiteral("Invalid hot/cold spectra or no physically valid Y-factor bins; profiles unchanged"));
        yfactor = std::move(candidate);
        captureGrid.clear();
        return status();
    }
    if (op == "noise.read" || op == "yfactor.read" || op == "noise.export" || op == "yfactor.export") {
        int offset = 0, limit = 0;
        if (!integer(r, "offset", 0, 0, kMaxBins, offset) || !integer(r, "limit", 256, 1, 4096, limit))
            return fail(QStringLiteral("Expected offset 0..32768 and limit 1..4096"));
        QJsonObject doc{{"key", keyJson(selected)}, {"inputUnit", "dBFS"}};
        QJsonArray rows;
        const bool isNoise = op.startsWith("noise.");
        const auto n = isNoise ? noise.matchingProfile(selected) : ReceiverNoiseBaselineProfile{};
        const auto y = !isNoise ? yfactor.matchingProfile(selected) : ReceiverYFactorProfile{};
        if (isNoise ? !n.isValid() : !y.isValid()) return fail(QStringLiteral("No matching profile"));
        const int total = isNoise ? n.averagePower.size() : y.coldPower.size();
        const double low = isNoise ? n.minimumFrequencyHz : y.minimumFrequencyHz;
        const double high = isNoise ? n.maximumFrequencyHz : y.maximumFrequencyHz;
        doc.insert("totalBins", total);
        doc.insert("offset", offset);
        doc.insert("minimumFrequencyHz", low);
        doc.insert("maximumFrequencyHz", high);
        doc.insert("capturedUtcMs", static_cast<double>(isNoise ? n.capturedUtcMs : y.capturedUtcMs));
        if (isNoise) { doc.insert("capturedFrames", n.capturedFrames); doc.insert("spreadDb", n.spreadDb); }
        else {
            doc.insert("coldFrames", y.coldFrames); doc.insert("hotFrames", y.hotFrames); doc.insert("validBins", y.validBins);
            doc.insert("sourceMode", y.sourceMode == YFactorSourceMode::Enr ? "enr" : "temperatures");
            doc.insert("coldTemperatureKelvin", y.coldTemperatureKelvin); doc.insert("hotTemperatureKelvin", y.hotTemperatureKelvin);
            doc.insert("enrDb", y.enrDb);
        }
        for (int i = offset; i < (std::min)(total, offset + limit); ++i) {
            QJsonObject row{{"frequencyHz", low + (high - low) * i / (total - 1)}};
            if (isNoise) row.insert("referenceLevelDbfs", 10 * std::log10(n.averagePower[i]));
            else {
                const bool valid = std::isfinite(y.noiseFigureDb[i]) && std::isfinite(y.equivalentNoiseTemperatureKelvin[i]);
                row.insert("valid", valid);
                row.insert("coldLevelDbfs", 10 * std::log10(y.coldPower[i]));
                row.insert("hotLevelDbfs", 10 * std::log10(y.hotPower[i]));
                row.insert("yDb", 10 * std::log10(static_cast<double>(y.hotPower[i]) / y.coldPower[i]));
                row.insert("equivalentNoiseTemperatureKelvin", finiteJsonNumber(y.equivalentNoiseTemperatureKelvin[i]));
                row.insert("noiseFigureDb", finiteJsonNumber(y.noiseFigureDb[i]));
            }
            rows.append(row);
        }
        doc.insert("rows", rows);
        doc.insert("returnedBins", rows.size());
        if (op.endsWith(".export")) return exportDocument(r, doc);
        doc.insert("ok", true);
        return doc;
    }
    if (op == "noise.apply" || op == "yfactor.apply") {
        std::vector<float> f, db;
        double target = 0;
        if (!numbers(r.value("frequenciesHz"), f) || !numbers(r.value("levelsDbfs"), db) || !spectrumValid(f, db) ||
            !dbfs(r) || !real(r, "targetFloorDb", 0, target) || std::abs(target) > 300)
            return fail(QStringLiteral("Expected uniform frequenciesHz, levelsDbfs [-300,300], unit dBFS and targetFloorDb [-300,300]"));
        if (op == "noise.apply") {
            if (r.contains("mode") && !r.value("mode").isString()) return fail(QStringLiteral("Noise mode must be a string"));
            const QString mode = r.value("mode").toString(QStringLiteral("relative"));
            if (mode != "relative" && mode != "excess") return fail(QStringLiteral("Noise mode must be relative or excess"));
            const auto p = noise.matchingProfile(selected);
            if (!p.isValid() || !inRange(f, p.minimumFrequencyHz, p.maximumFrequencyHz)) return fail(QStringLiteral("No matching noise profile covering this span"));
            if (!noise.apply(f, db, selected, mode == "excess" ? ReceiverNoiseBaselineMode::ExcessPower : ReceiverNoiseBaselineMode::RelativeFloor, target))
                return fail(QStringLiteral("Noise application failed"));
        } else {
            const auto p = yfactor.matchingProfile(selected);
            if (!p.isValid() || !inRange(f, p.minimumFrequencyHz, p.maximumFrequencyHz)) return fail(QStringLiteral("No matching Y-factor profile covering this span"));
            for (float hz : f) {
                const double pos = (hz - p.minimumFrequencyHz) / (p.maximumFrequencyHz - p.minimumFrequencyHz) * (p.coldPower.size() - 1);
                const int lo = (std::clamp)(static_cast<int>(std::floor(pos)), 0, p.coldPower.size() - 1);
                const int hi = (std::min)(lo + 1, p.coldPower.size() - 1);
                if (!std::isfinite(p.equivalentNoiseTemperatureKelvin[lo]) || !std::isfinite(p.equivalentNoiseTemperatureKelvin[hi]))
                    return fail(QStringLiteral("Application intersects invalid Y-factor bins; no mixed-unit output returned"));
            }
            if (!yfactor.applyInputReferred(f, db, selected, target)) return fail(QStringLiteral("Y-factor application failed"));
        }
        return {{"ok", true}, {"frequenciesHz", jsonNumbers(f)}, {"levelsDb", jsonNumbers(db)},
                {"unit", op == "noise.apply" ? "dB re noise reference" : "dB re cold input"},
                {"targetFloorDb", target}, {"absolutePowerCalibrated", false}};
    }
    return fail(QStringLiteral("Unknown calibration operation"));
}

QJsonObject CliCalibration::submitSpectrum(const std::vector<float> &f, const std::vector<float> &db,
                                          const ReceiverNoiseBaselineKey &key) {
    if (!capturing()) return {};
    QJsonObject event{{"event", "calibrationCapture"}};
    if (!selected.matches(key) || !spectrumValid(f, db) || (!captureGrid.empty() && !sameGrid(captureGrid, f))) {
        cancel();
        event.insert("ok", false);
        event.insert("result", "rejected");
        event.insert("error", "Receiver key or spectrum geometry/levels changed; capture and pending cold cancelled");
        return event;
    }
    if (captureGrid.empty()) captureGrid = f;
    QString result;
    if (noise.isCapturing()) {
        const auto value = noise.addFrame(f, db, key);
        result = value == ReceiverNoiseBaseline::CaptureResult::Completed ? "completed" :
                 value == ReceiverNoiseBaseline::CaptureResult::Accepted ? "accepted" : "rejected";
        if (value == ReceiverNoiseBaseline::CaptureResult::Completed) captureGrid.clear();
    } else {
        const auto value = yfactor.addFrame(f, db, key);
        result = value == ReceiverYFactorCalibration::CaptureResult::Completed ? "completed" :
                 value == ReceiverYFactorCalibration::CaptureResult::ColdCompleted ? "coldCompleted" :
                 value == ReceiverYFactorCalibration::CaptureResult::Accepted ? "accepted" : "rejected";
        if (value == ReceiverYFactorCalibration::CaptureResult::Completed) captureGrid.clear();
    }
    if (result == "rejected") cancel();
    event.insert("ok", result != "rejected");
    event.insert("result", result);
    event.insert("status", status());
    return event;
}
