#include "cliresearchtools.h"

#include "cliiqquality.h"
#include "../research/spectrumscienceanalyzer.h"
#include "../research/tonalspectrum.h"
#include "../research/headlessresearchmetrics.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <utility>

namespace Research = HeadlessResearchMetrics;

namespace {
QJsonObject failure(const QString &message, const QString &code = QStringLiteral("invalid_request")) {
    return {{"ok", false}, {"code", code}, {"error", message}};
}

bool number(const QJsonValue &value, double *result) {
    if (!value.isDouble() || !std::isfinite(value.toDouble())) return false;
    *result = value.toDouble();
    return true;
}

bool integer(const QJsonObject &request, const char *key, int fallback, int low, int high, int *out) {
    if (!request.contains(QLatin1String(key))) {
        *out = fallback;
        return true;
    }
    double n = 0;
    if (!number(request.value(QLatin1String(key)), &n) || n < low || n > high || std::floor(n) != n)
        return false;
    *out = static_cast<int>(n);
    return true;
}

bool option(const QJsonObject &request, const char *key, double fallback, double low, double high, double *out) {
    if (!request.contains(QLatin1String(key))) { *out = fallback; return true; }
    return number(request.value(QLatin1String(key)), out) && *out >= low && *out <= high;
}

QJsonObject metric(double value, const QString &unit, bool valid = true,
                   const QString &reason = QString()) {
    const bool usable = valid && std::isfinite(value);
    QJsonObject result{{"value", usable ? QJsonValue(value) : QJsonValue(QJsonValue::Null)},
                       {"valid", usable}, {"unit", unit}};
    if (!usable)
        result.insert("reason", reason.isEmpty() ? QStringLiteral("nonfinite_core_result") : reason);
    return result;
}

QJsonObject column(const QString &name, const QString &unit) {
    return {{"name", name}, {"unit", unit}};
}

QString csvField(QString value) {
    value.replace('"', QStringLiteral("\"\""));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

QString csvValue(const QJsonValue &value) {
    if (value.isNull() || value.isUndefined()) return {};
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 17);
    return csvField(value.toString());
}

bool parseMetadata(const QJsonObject &request, bool iq, QJsonObject *out) {
    if (!request.value("metadata").isObject()) return false;
    const QJsonObject input = request.value("metadata").toObject();
    const QString source = input.value("source").toString();
    const QString unit = input.value("amplitudeUnit").toString();
    double rate = 0, center = 0;
    if (source.trimmed().isEmpty() || source.size() > 128 ||
        source.contains('\n') || source.contains('\r') || source.contains(QChar(0)) ||
        !number(input.value("sampleRateHz"), &rate) || rate < 1.0e-9 || rate > 1.0e12 ||
        !number(input.value("centerHz"), &center) || center < 0 || center > 1.0e15 ||
        (iq ? unit != QStringLiteral("normalized")
            : unit != QStringLiteral("dBFS") && unit != QStringLiteral("dBm"))) return false;
    *out = {{"source", source}, {"sampleRateHz", rate}, {"centerHz", center},
            {"amplitudeUnit", unit}};
    return true;
}

bool floatArray(const QJsonValue &input, int minimum, int maximum,
                std::vector<float> *out, double low, double high) {
    if (!input.isArray()) return false;
    const QJsonArray values = input.toArray();
    if (values.size() < minimum || values.size() > maximum) return false;
    out->reserve(static_cast<std::size_t>(values.size()));
    for (const QJsonValue &value : values) {
        double n = 0;
        if (!number(value, &n) || n < low || n > high) return false;
        out->push_back(static_cast<float>(n));
    }
    return true;
}

QJsonArray unsupportedTools() {
    return QJsonArray{QJsonObject{{"tool", "coherent"},
        {"reason", "Coherent hardware measurements require synchronized calibrated multi-receiver IQ, not an offline single-stream array"}}};
}

QJsonObject unsupported(const QString &tool) {
    for (const QJsonValue &entry : unsupportedTools()) {
        const QJsonObject item = entry.toObject();
        if (item.value("tool").toString() == tool) {
            QJsonObject result = failure(item.value("reason").toString(), QStringLiteral("unsupported"));
            result.insert("tool", tool);
            result.insert("valid", false);
            return result;
        }
    }
    return {};
}

bool sameSource(const QJsonObject &a, const QJsonObject &b) {
    for (const char *key : {"source", "sampleRateHz", "centerHz", "amplitudeUnit"})
        if (a.value(QLatin1String(key)) != b.value(QLatin1String(key))) return false;
    return true;
}

QString spectrumInput(const QJsonObject &request, QJsonObject *metadata,
                      std::vector<float> *frequencies, std::vector<float> *levels, int minimum = 2) {
    if (!parseMetadata(request, false, metadata)) return QStringLiteral("Explicit spectrum source/rate/center and dBFS or dBm metadata required");
    if (!floatArray(request.value("frequenciesHz"), minimum, Research::MaximumSamples, frequencies, -1e15, 1e15) ||
        !floatArray(request.value("levelsDb"), minimum, Research::MaximumSamples, levels, -200, 100) || frequencies->size() != levels->size())
        return QStringLiteral("Matching bounded finite frequenciesHz/levelsDb arrays required; levels -200..100 dB");
    const auto original = request.value("frequenciesHz").toArray();
    const double start = original.first().toDouble(), end = original.last().toDouble();
    const double center = metadata->value("centerHz").toDouble(), rate = metadata->value("sampleRateHz").toDouble();
    const double step = (end - start) / (original.size() - 1);
    if (!(step > 0) || start < center - rate * 0.5 || end > center + rate * 0.5)
        return QStringLiteral("Ascending uniform spectrum must fit the declared source span");
    for (int i = 0; i < original.size(); ++i) {
        if (std::abs(original[i].toDouble() - (start + i * step)) > (std::max)(1e-6, step * 1e-6) ||
            (i && (*frequencies)[i] <= (*frequencies)[i - 1]))
            return QStringLiteral("Spectrum must be uniform and remain strictly increasing at float32 precision");
    }
    return {};
}
}

struct CliResearchTools::Impl {
    struct Snapshot {
        bool present = false;
        QString revision;
        QJsonObject metadata;
        QJsonObject metrics;
        QString core;
        // IQ: first is interleaved raw IQ, second is empty. Spectrum: frequency/level.
        std::vector<float> first;
        std::vector<float> second;
        QJsonArray columns;
        std::vector<std::array<double, 8>> rows;
        std::unique_ptr<Research::DensityHistogram> density;
        int count(bool iq) const {
            if (density) return density->hasRange() ? density->configuration().columns * Research::DensityRows : 0;
            if (!columns.isEmpty()) return static_cast<int>(rows.size());
            return static_cast<int>(first.size()) / (iq ? 2 : 1);
        }
    };

    Snapshot iq;
    Snapshot spectrum;
    Snapshot density, pulse, constellation, eye, phase, comb;
    quint64 revision = 0;
    double lastCombStep = 0, lastCombMean = std::numeric_limits<double>::quiet_NaN();

    QJsonObject info(const Snapshot &snapshot, bool isIq) const {
        return {{"available", snapshot.present}, {"revision", snapshot.revision},
                {"count", snapshot.count(isIq)}, {"metadata", snapshot.metadata},
                {"metrics", snapshot.metrics}, {"core", snapshot.core}};
    }

    QJsonObject commit(Snapshot &target, Snapshot next, const QString &name) {
        next.present = true;
        next.revision = QString::number(++revision);
        target = std::move(next);
        return {{"ok", true}, {name, info(target, false)}};
    }

    QJsonObject startDensity(const QJsonObject &request) {
        Snapshot next;
        Research::DensityConfig config;
        if (!parseMetadata(request, false, &next.metadata))
            return failure(QStringLiteral("Density requires explicit spectrum metadata"));
        if (!integer(request, "columns", 512, 1, Research::MaximumDensityColumns, &config.columns) ||
            !integer(request, "historyFrames", 256, 1, Research::MaximumHistoryFrames, &config.historyFrames) ||
            !option(request, "retainPercent", 98, 0, 100, &config.retainPercent) ||
            !option(request, "minimumDb", -140, -200, 100, &config.minimumDb) ||
            !option(request, "maximumDb", -20, -200, 100, &config.maximumDb))
            return failure(QStringLiteral("Invalid density options"));
        const QString mode = request.contains("mode") ? request.value("mode").toString() : QStringLiteral("count");
        if (mode != QStringLiteral("count") && mode != QStringLiteral("linearPower"))
            return failure(QStringLiteral("Density mode must be count or linearPower"));
        config.linearPower = mode == QStringLiteral("linearPower");
        next.density = std::make_unique<Research::DensityHistogram>();
        QString error;
        if (!next.density->configure(config, &error)) return failure(error);
        next.metadata.insert("density", QJsonObject{{"columns", config.columns}, {"rows", Research::DensityRows},
            {"historyFrames", config.historyFrames}, {"retainPercent", config.retainPercent},
            {"minimumDb", config.minimumDb}, {"maximumDb", config.maximumDb}, {"mode", mode}});
        next.metrics = {{"retainedFrames", metric(0, "frame")},
            {"peakDensity", metric(0, config.linearPower ? "decayed relative-power sum" : "decayed frame count")}};
        next.core = QStringLiteral("HeadlessResearchMetrics::DensityHistogram; SignalDensityWidget histogram semantics");
        return commit(density, std::move(next), QStringLiteral("density"));
    }

    QJsonObject appendDensity(const QJsonObject &request) {
        if (!density.present) return failure(QStringLiteral("Call density.start first"), QStringLiteral("no_data"));
        if (request.contains("revision") && request.value("revision") != QJsonValue(density.revision))
            return failure(QStringLiteral("Density revision changed"), QStringLiteral("stale_revision"));
        QJsonObject metadata;
        std::vector<float> frequencies, levels;
        const QString error = spectrumInput(request, &metadata, &frequencies, &levels);
        if (!error.isEmpty()) return failure(error);
        if (!sameSource(metadata, density.metadata))
            return failure(QStringLiteral("Density source/rate/center/unit changed; start a new accumulation"));
        bool reset = false;
        QString coreError;
        if (!density.density->append(levels, frequencies.front(), frequencies.back(), &reset, &coreError)) return failure(coreError);
        density.metadata.insert("rangeStartHz", density.density->firstHz());
        density.metadata.insert("rangeEndHz", density.density->lastHz());
        density.metrics.insert("retainedFrames", metric(density.density->retainedFrames(), "frame"));
        const auto &histogram = density.density->histogram();
        density.metrics.insert("peakDensity", metric(*std::max_element(histogram.begin(), histogram.end()),
            density.density->configuration().linearPower ? "decayed relative-power sum" : "decayed frame count"));
        density.revision = QString::number(++revision);
        return {{"ok", true}, {"rangeReset", reset}, {"density", info(density, false)}};
    }

    QJsonObject analyzePulse(const QJsonObject &request) {
        Snapshot next;
        if (!parseMetadata(request, false, &next.metadata)) return failure(QStringLiteral("Pulse requires explicit spectrum metadata"));
        const QString timeBase = request.value("timeBase").toString();
        if (timeBase != QStringLiteral("observation") && timeBase != QStringLiteral("sample"))
            return failure(QStringLiteral("timeBase must explicitly be observation or sample; timestamps are integer milliseconds"));
        const auto times = request.value("timeMs").toArray();
        std::vector<float> levels;
        if (!request.value("timeMs").isArray() || !floatArray(request.value("levelsDb"), 3, Research::MaximumSamples, &levels, -200, 100) ||
            times.size() != static_cast<int>(levels.size())) return failure(QStringLiteral("Pulse requires 3..65536 matching timeMs/levelsDb entries"));
        std::vector<Research::PulseSample> samples;
        samples.reserve(levels.size());
        for (int i = 0; i < times.size(); ++i) {
            double time = 0;
            if (!number(times[i], &time) || time < 0 || time > 9007199254740991.0 || std::floor(time) != time)
                return failure(QStringLiteral("timeMs must contain exact nonnegative integer milliseconds <= 2^53-1"));
            samples.push_back({static_cast<qint64>(time), levels[i]});
        }
        Research::PulseConfig options;
        const QString mode = request.contains("thresholdMode") ? request.value("thresholdMode").toString() : QStringLiteral("absolute");
        if ((mode != QStringLiteral("absolute") && mode != QStringLiteral("relative")) ||
            !option(request, "thresholdDb", -80, -300, 300, &options.thresholdDb) ||
            !option(request, "hysteresisDb", 3, 0, 300, &options.hysteresisDb)) return failure(QStringLiteral("Invalid pulse thresholdMode, thresholdDb or hysteresisDb"));
        options.relativeToNoise = mode == QStringLiteral("relative");
        const auto result = Research::analyzePulses(samples, options);
        if (!result.valid) return failure(result.error);
        const bool events = !result.events.empty(), periodic = result.events.size() >= 2;
        const QString unit = next.metadata.value("amplitudeUnit").toString();
        next.metrics = {{"thresholdDb", metric(result.thresholdDb, unit)}, {"releaseDb", metric(result.releaseDb, unit)},
            {"pulseCount", metric(double(result.events.size()), "count")}, {"observedMs", metric(result.observedMs, "ms")},
            {"averageWidthMs", metric(result.averageWidthMs, "ms", events, "no_completed_pulses")},
            {"averagePriMs", metric(result.averagePriMs, "ms", periodic, "fewer_than_two_completed_pulses")},
            {"repetitionHz", metric(result.repetitionHz, "Hz", periodic, "fewer_than_two_completed_pulses")},
            {"dutyPercent", metric(result.dutyPercent, "%")}, {"riseMs", metric(result.riseMs, "ms", events, "no_completed_pulses")},
            {"fallMs", metric(result.fallMs, "ms", events, "no_completed_pulses")}, {"trailingActive", metric(result.trailingActive ? 1 : 0, "boolean")}};
        next.metadata.insert("pulse", QJsonObject{{"timeBase", timeBase}, {"thresholdMode", mode},
            {"thresholdDb", options.thresholdDb}, {"hysteresisDb", options.hysteresisDb}, {"inputObservations", times.size()}});
        next.columns = {column("startMs", "ms"), column("endMs", "ms"), column("widthMs", "ms"),
            column("riseMs", "ms"), column("fallMs", "ms"), column("peakDb", unit), column("leftCensored", "boolean")};
        for (const auto &event : result.events)
            next.rows.push_back({double(event.startMs), double(event.endMs), double(event.endMs - event.startMs),
                event.riseMs, event.fallMs, event.peakDb, event.leftCensored ? 1.0 : 0.0, 0});
        next.core = QStringLiteral("HeadlessResearchMetrics::analyzePulses; PulseAnalysisWidget sampled dB threshold/rise/fall semantics");
        return commit(pulse, std::move(next), QStringLiteral("pulse"));
    }

    QJsonObject analyzeViews(const QJsonObject &request, bool isEye) {
        Snapshot next;
        if (!parseMetadata(request, true, &next.metadata) ||
            !floatArray(request.value("iq"), 2, MaximumIqSamples * 2, &next.first, -1e12, 1e12) || next.first.size() % 2)
            return failure(QStringLiteral("IQ views require explicit normalized metadata and 1..65536 finite I/Q pairs in [-1e12,1e12]"));
        double symbolRate = 0, phaseFraction = 0;
        int traces = 32;
        if (!number(request.value("symbolRateHz"), &symbolRate) || symbolRate <= 0 ||
            !option(request, "phaseFraction", 0.5, 0, 1, &phaseFraction) ||
            !integer(request, "traces", 32, 1, Research::MaximumEyeTraces, &traces)) return failure(QStringLiteral("Explicit positive symbolRateHz, phaseFraction 0..1, traces 1..128 required"));
        const double rate = next.metadata.value("sampleRateHz").toDouble();
        const auto result = Research::buildIqViews(next.first, rate, symbolRate, phaseFraction, isEye ? traces : 1);
        if (!result.valid) return failure(result.error);
        next.first.clear();
        next.metrics = {{"dcI", metric(result.dcI, "normalized amplitude")}, {"dcQ", metric(result.dcQ, "normalized amplitude")},
            {"samplesPerSymbol", metric(result.samplesPerSymbol, "sample/symbol")},
            {"pointCount", metric(double(isEye ? result.eye.size() : result.constellation.size()), "count")},
            {"limited", metric((isEye ? result.eyeLimited : result.constellationLimited) ? 1 : 0, "boolean")}};
        next.metadata.insert("symbolRateHz", symbolRate);
        next.metadata.insert("phaseFraction", phaseFraction);
        next.metadata.insert("dcCentered", true);
        if (isEye) {
            next.metadata.insert("requestedTraces", traces);
            next.metrics.insert("traceCount", metric(result.eyeTraces, "trace"));
            next.metrics.insert("pointsPerTrace", metric(Research::EyePoints, "point/trace"));
            next.columns = {column("trace", "trace"), column("point", "point"), column("phaseSymbols", "symbol"),
                column("timeSeconds", "s"), column("i", "normalized amplitude"), column("q", "normalized amplitude")};
            for (std::size_t i = 0; i < result.eye.size(); ++i) {
                const auto &p = result.eye[i];
                next.rows.push_back({double(i / Research::EyePoints), double(i % Research::EyePoints),
                    2.0 * (i % Research::EyePoints) / (Research::EyePoints - 1), p.samplePosition / rate, p.i, p.q, 0, 0});
            }
        } else {
            next.columns = {column("index", "symbol"), column("timeSeconds", "s"), column("samplePosition", "sample"),
                column("i", "normalized amplitude"), column("q", "normalized amplitude")};
            for (std::size_t i = 0; i < result.constellation.size(); ++i) {
                const auto &p = result.constellation[i];
                next.rows.push_back({double(i), p.samplePosition / rate, p.samplePosition, p.i, p.q, 0, 0, 0});
            }
        }
        next.core = QStringLiteral("HeadlessResearchMetrics::buildIqViews; ResearchAnalysisDialog 2D symbol interpolation and 192-point two-symbol eyes");
        return commit(isEye ? eye : constellation, std::move(next), isEye ? QStringLiteral("eye") : QStringLiteral("constellation"));
    }

    QJsonObject analyzePhase(const QJsonObject &request) {
        Snapshot next;
        if (!parseMetadata(request, true, &next.metadata) ||
            !floatArray(request.value("iq"), 4, MaximumIqSamples * 2, &next.first, -1e12, 1e12) || next.first.size() % 2)
            return failure(QStringLiteral("IQ phase requires explicit normalized metadata and 2..65536 finite I/Q pairs"));
        const auto count = next.first.size() / 2;
        const auto result = Research::analyzeIqPhase(next.first, next.metadata.value("sampleRateHz").toDouble());
        if (!result.valid) return failure(result.error);
        next.first.clear();
        next.metrics = {{"frequencyHz", metric(result.frequencyHz, "Hz", result.frequencyValid, "zero_ac_power")},
            {"frequencyStdHz", metric(result.frequencyStdHz, "Hz", result.frequencyValid, "zero_ac_power")},
            {"imageEstimateDb", metric(result.imageEstimateDb, "dB", result.imageValid,
                result.imageSaturated ? QStringLiteral("singular_image_estimate_gui_99db_sentinel") : QStringLiteral("zero_ac_power"))}};
        next.columns = {column("lag", "sample"), column("autocorrelation", "1"), column("valid", "boolean")};
        for (std::size_t i = 0; i < result.autocorrelation.size(); ++i) {
            const bool valid = result.autocorrelationValid && i < count;
            next.rows.push_back({double(i), valid ? result.autocorrelation[i] : std::numeric_limits<double>::quiet_NaN(), valid ? 1.0 : 0.0, 0, 0, 0, 0, 0});
        }
        next.core = QStringLiteral("HeadlessResearchMetrics::analyzeIqPhase; ResearchAnalysisDialog phase-step mean/std, pseudo-power and stride-4 I autocorrelation");
        return commit(phase, std::move(next), QStringLiteral("phase"));
    }

    QJsonObject analyzeComb(const QJsonObject &request) {
        Snapshot next;
        const QString error = spectrumInput(request, &next.metadata, &next.first, &next.second, 5);
        if (!error.isEmpty()) return failure(error);
        double prominence = 0, minimum = 0, maximum = 0;
        if (!option(request, "prominenceDb", 6, 0, 300, &prominence) ||
            !option(request, "minimumSpacingHz", 100, 1e-9, 2e15, &minimum) ||
            !option(request, "maximumSpacingHz", 1e6, minimum, 2e15, &maximum)) return failure(QStringLiteral("Invalid comb prominence/spacing options"));
        std::vector<float> reference;
        if (request.contains("referenceLevelsDb") && (!floatArray(request.value("referenceLevelsDb"), 5, MaximumSpectrumBins, &reference, -200, 100) ||
            reference.size() != next.second.size())) return failure(QStringLiteral("referenceLevelsDb must match the current explicit frequency grid"));
        const auto result = Research::analyzeComb(next.first, next.second, prominence, minimum, maximum, reference);
        if (!result.valid) return failure(result.error);
        next.metadata.insert("comb", QJsonObject{{"prominenceDb", prominence}, {"minimumSpacingHz", minimum}, {"maximumSpacingHz", maximum}});
        const bool comparable = comb.present && comb.metadata == next.metadata && comb.first == next.first;
        const double previousStep = comparable ? lastCombStep : 0;
        const double previousMean = comparable ? lastCombMean : std::numeric_limits<double>::quiet_NaN();
        const QString unit = next.metadata.value("amplitudeUnit").toString();
        next.metrics = {{"noiseDb", metric(result.noiseDb, unit)}, {"peakCount", metric(double(result.peaks.size()), "count")},
            {"familyCount", metric(double(result.families.size()), "count")}, {"referenceValid", metric(result.referenceValid ? 1 : 0, "boolean")}};
        for (std::size_t i = 0; i < result.families.size(); ++i) {
            const QString prefix = QStringLiteral("family%1.").arg(i + 1);
            next.metrics.insert(prefix + "stepHz", metric(result.families[i].stepHz, "Hz"));
            next.metrics.insert(prefix + "anchorHz", metric(result.families[i].anchorHz, "Hz"));
            next.metrics.insert(prefix + "members", metric(result.families[i].members, "count"));
        }
        double mean = 0;
        int members = 0;
        for (const auto &p : result.peaks) if (p.family == 1) { mean += p.levelDb; ++members; }
        const bool found = !result.families.empty();
        if (members) mean /= members;
        const double step = found ? result.families.front().stepHz : 0;
        next.metrics.insert("stepDriftHz", metric(step - previousStep, "Hz", found && previousStep > 0, "no_comparable_previous_comb"));
        next.metrics.insert("amplitudeDriftDb", metric(mean - previousMean, "dB", found && std::isfinite(previousMean), "no_comparable_previous_comb"));
        next.columns = {column("bin", "bin"), column("frequencyHz", "Hz"), column("levelDb", unit), column("prominenceDb", "dB"),
            column("referenceDeltaDb", "dB"), column("family", "family"), column("harmonic", "index"), column("referenceValid", "boolean")};
        for (const auto &p : result.peaks)
            next.rows.push_back({double(p.bin), p.frequencyHz, p.levelDb, p.prominenceDb,
                result.referenceValid ? p.referenceDeltaDb : std::numeric_limits<double>::quiet_NaN(), double(p.family),
                p.family ? double(p.harmonic) : std::numeric_limits<double>::quiet_NaN(), result.referenceValid ? 1.0 : 0.0});
        next.second.clear();
        next.core = QStringLiteral("HeadlessResearchMetrics::analyzeComb; ResearchAnalysisDialog prominence peaks and spacing-vote families (not emitter identification)");
        lastCombStep = found ? step : previousStep;
        lastCombMean = found ? mean : previousMean;
        return commit(comb, std::move(next), QStringLiteral("comb"));
    }

    QJsonObject analyzeIq(const QJsonObject &request) {
        Snapshot next;
        if (!parseMetadata(request, true, &next.metadata))
            return failure(QStringLiteral("IQ requires metadata: source (1..128 characters), sampleRateHz [1e-9..1e12], centerHz [0..1e15], amplitudeUnit normalized"));
        const double maximum = std::numeric_limits<float>::max();
        if (!floatArray(request.value("iq"), 2, MaximumIqSamples * 2, &next.first, -maximum, maximum) ||
            next.first.size() % 2 != 0)
            return failure(QStringLiteral("iq must contain 1..65536 finite float32-representable interleaved I/Q pairs"));

        CliIqQuality quality;
        quality.request(1);
        const QJsonObject raw = quality.submit(next.first.data(), next.count(true),
                                              next.metadata.value("sampleRateHz").toDouble());
        const QString amplitude = QStringLiteral("normalized amplitude");
        for (const char *name : {"dcI", "dcQ", "rmsI", "rmsQ"})
            next.metrics.insert(QLatin1String(name), metric(raw.value(QLatin1String(name)).toDouble(), amplitude));
        const bool bothVary = raw.value("rmsI").toDouble() > 0 && raw.value("rmsQ").toDouble() > 0;
        next.metrics.insert("iqGainImbalanceDb", metric(raw.value("iqGainImbalanceDb").toDouble(),
            QStringLiteral("dB"), bothVary, QStringLiteral("zero_ac_variance")));
        next.metrics.insert("normalizedIqCorrelation", metric(raw.value("normalizedIqCorrelation").toDouble(),
            QStringLiteral("1"), bothVary, QStringLiteral("zero_ac_variance")));
        next.metrics.insert("nearClippingPercent", metric(raw.value("nearClippingPercent").toDouble(), QStringLiteral("%")));
        next.core = QStringLiteral("CliIqQuality (AC RMS, near-clipping at abs(I) or abs(Q) >= 0.98f)");
        next.present = true;
        next.revision = QString::number(++revision);
        iq = std::move(next);
        return {{"ok", true}, {"iq", info(iq, true)}};
    }

    QJsonObject analyzeSpectrum(const QJsonObject &request) {
        Snapshot next;
        if (!parseMetadata(request, false, &next.metadata))
            return failure(QStringLiteral("Spectrum requires metadata: source, sampleRateHz [1e-9..1e12], centerHz [0..1e15], amplitudeUnit dBFS or dBm"));
        if (!floatArray(request.value("frequenciesHz"), 2, MaximumSpectrumBins, &next.first, -1.0e15, 1.0e15) ||
            !floatArray(request.value("levelsDb"), 2, MaximumSpectrumBins, &next.second, -200, 100) ||
            next.first.size() != next.second.size())
            return failure(QStringLiteral("frequenciesHz and levelsDb require 2..65536 matching finite entries; levelsDb range -200..100"));

        const auto original = request.value("frequenciesHz").toArray();
        const double center = next.metadata.value("centerHz").toDouble();
        const double rate = next.metadata.value("sampleRateHz").toDouble();
        const double start = original.first().toDouble(), end = original.last().toDouble();
        const double step = (end - start) / (original.size() - 1);
        if (!(step > 0) || start < center - rate * 0.5 || end > center + rate * 0.5)
            return failure(QStringLiteral("Spectrum must be ascending and inside the explicitly supplied source span"));
        const double tolerance = (std::max)(1.0e-6, step * 1.0e-6);
        for (int index = 0; index < original.size(); ++index) {
            if (std::abs(original[index].toDouble() - (start + step * index)) > tolerance ||
                (index && next.first[static_cast<std::size_t>(index)] <= next.first[static_cast<std::size_t>(index - 1)]))
                return failure(QStringLiteral("Spectrum must be uniform and strictly ascending after float32 conversion; mosaics and collapsed RF bins are unsupported"));
        }

        double fundamental = 0, halfWidth = 0;
        int lastHarmonic = 5;
        const bool tonal = request.contains("tonal");
        if (tonal) {
            const QJsonObject options = request.value("tonal").toObject();
            if (!request.value("tonal").isObject() || next.first.size() < 8 ||
                !number(options.value("fundamentalHz"), &fundamental) || fundamental <= 0 ||
                !number(options.value("integrationHalfWidthHz"), &halfWidth) || halfWidth <= 0 ||
                !integer(options, "lastHarmonic", 5, 2, 20, &lastHarmonic))
                return failure(QStringLiteral("tonal requires at least 8 bins, positive fundamentalHz/integrationHalfWidthHz and lastHarmonic 2..20"));
        }

        SpectrumScienceAnalyzer analyzer;
        analyzer.update(next.first, next.second);
        const SpectrumScienceMetrics &m = analyzer.metrics();
        const QString hz = QStringLiteral("Hz"), db = QStringLiteral("dB");
        const QString unit = next.metadata.value("amplitudeUnit").toString();
        next.metrics = {
            {"rangeStartHz", metric(m.rangeStartHz, hz, m.valid)},
            {"rangeEndHz", metric(m.rangeEndHz, hz, m.valid)},
            {"peakFrequencyHz", metric(m.peakFrequencyHz, hz, m.valid)},
            {"centroidFrequencyHz", metric(m.centroidFrequencyHz, hz, m.valid)},
            {"occupiedBandwidth90Hz", metric(m.occupiedBandwidth90Hz, hz, m.valid)},
            {"occupiedBandwidth95Hz", metric(m.occupiedBandwidth95Hz, hz, m.valid)},
            {"occupiedBandwidth99Hz", metric(m.occupiedBandwidthHz, hz, m.valid)},
            {"width3DbHz", metric(m.width3DbHz, hz, m.valid)},
            {"width6DbHz", metric(m.width6DbHz, hz, m.valid)},
            {"width20DbHz", metric(m.width20DbHz, hz, m.valid)},
            {"peakDb", metric(m.peakDb, unit, m.valid)},
            {"noiseFloorDb", metric(m.noiseFloorDb, unit, m.valid)},
            {"snrDb", metric(m.snrDb, db, m.valid)},
            {"channelPowerDb", metric(m.channelPowerDb, unit, m.valid)}
        };
        next.core = QStringLiteral("SpectrumScienceAnalyzer (single-frame, full-span sample detector)");
        if (tonal) {
            const TonalSpectrumResult t = analyzeTonalSpectrum(next.first, next.second, fundamental, halfWidth, lastHarmonic);
            const QString invalid = QStringLiteral("tonal_core_rejected_or_no_resolved_signal");
            next.metrics.insert("tonal.snrDb", metric(t.snrDb, db, t.valid, invalid));
            next.metrics.insert("tonal.sinadDb", metric(t.sinadDb, db, t.valid, invalid));
            next.metrics.insert("tonal.thdDb", metric(t.thdDb, db, t.valid,
                t.valid ? QStringLiteral("no_resolved_harmonic_power") : invalid));
            next.metrics.insert("tonal.thdnDb", metric(t.thdnDb, db, t.valid, invalid));
            next.metrics.insert("tonal.thdPercent", metric(t.thdPercent, QStringLiteral("%"), t.valid, invalid));
            next.metrics.insert("tonal.thdnPercent", metric(t.thdnPercent, QStringLiteral("%"), t.valid, invalid));
            next.metrics.insert("tonal.harmonicsMeasured", metric(t.harmonicsMeasured, QStringLiteral("count"), t.valid, invalid));
            next.metrics.insert("tonal.harmonicsRequested", metric(t.harmonicsRequested, QStringLiteral("count"), t.valid, invalid));
            next.metrics.insert("tonal.completeHarmonicSpan", metric(t.completeHarmonicSpan ? 1 : 0, QStringLiteral("boolean"), t.valid, invalid));
            next.metadata.insert("tonal", QJsonObject{{"fundamentalHz", fundamental},
                {"integrationHalfWidthHz", halfWidth}, {"lastHarmonic", lastHarmonic}});
            next.core += QStringLiteral("; analyzeTonalSpectrum (absolute-axis harmonics, not center-relative)");
        }
        next.metadata.insert("axisPrecision", QStringLiteral("float32 Hz (shared core API)"));
        next.present = true;
        next.revision = QString::number(++revision);
        spectrum = std::move(next);
        return {{"ok", true}, {"spectrum", info(spectrum, false)}};
    }

    QJsonObject read(const Snapshot &snapshot, bool isIq, const QJsonObject &request) const {
        if (!snapshot.present) return failure(QStringLiteral("No snapshot for this tool"), QStringLiteral("no_data"));
        if (request.contains("revision") && (!request.value("revision").isString() ||
            request.value("revision").toString() != snapshot.revision))
            return failure(QStringLiteral("Snapshot changed; restart paging"), QStringLiteral("stale_revision"));
        int offset = 0, limit = 0;
        if (!integer(request, "offset", 0, 0, (std::numeric_limits<int>::max)(), &offset) ||
            !integer(request, "limit", 256, 1, MaximumPageRows, &limit))
            return failure(QStringLiteral("Paging requires integer offset >= 0, limit 1..1024"));
        const QString view = request.contains("view") ? request.value("view").toString() : QStringLiteral("samples");
        if (view != QStringLiteral("samples") && view != QStringLiteral("metrics"))
            return failure(QStringLiteral("view must be samples or metrics"));
        const bool metrics = view == QStringLiteral("metrics");
        const int total = metrics ? snapshot.metrics.size() : snapshot.count(isIq);
        const int last = offset >= total ? total : offset + (std::min)(limit, total - offset);
        QJsonArray columns, rows;
        if (metrics) {
            columns = {column("name", "text"), column("value", "per-row unit"),
                       column("valid", "boolean"), column("unit", "text"), column("reason", "text")};
            const QStringList names = snapshot.metrics.keys();
            for (int index = offset; index < last; ++index) {
                const QString name = names[index];
                const QJsonObject m = snapshot.metrics.value(name).toObject();
                rows.append(QJsonArray{name, m.value("value"), m.value("valid"), m.value("unit"), m.value("reason").toString()});
            }
        } else if (snapshot.density) {
            const auto &histogram = *snapshot.density;
            const auto &config = histogram.configuration();
            columns = {column("column", "bin"), column("row", "bin"), column("frequencyHz", "Hz"),
                column("levelDb", snapshot.metadata.value("amplitudeUnit").toString()),
                column("density", config.linearPower ? "decayed relative-power sum" : "decayed frame count")};
            for (int index = offset; index < last; ++index) {
                const int x = index / Research::DensityRows, y = index % Research::DensityRows;
                const double frequency = config.columns == 1 ? histogram.firstHz() :
                    histogram.firstHz() + (histogram.lastHz() - histogram.firstHz()) * x / (config.columns - 1);
                const double level = config.maximumDb - (config.maximumDb - config.minimumDb) * y / (Research::DensityRows - 1);
                rows.append(QJsonArray{x, y, frequency, level, histogram.histogram()[static_cast<std::size_t>(y * config.columns + x)]});
            }
        } else if (!snapshot.columns.isEmpty()) {
            columns = snapshot.columns;
            for (int index = offset; index < last; ++index) {
                QJsonArray row;
                for (int c = 0; c < columns.size(); ++c) {
                    const double value = snapshot.rows[static_cast<std::size_t>(index)][static_cast<std::size_t>(c)];
                    row.append(std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null));
                }
                rows.append(row);
            }
        } else if (isIq) {
            columns = {column("index", "sample"), column("timeSeconds", "s"),
                       column("i", "normalized amplitude"), column("q", "normalized amplitude")};
            const double rate = snapshot.metadata.value("sampleRateHz").toDouble();
            for (int index = offset; index < last; ++index)
                rows.append(QJsonArray{index, index / rate, snapshot.first[static_cast<std::size_t>(2 * index)],
                                       snapshot.first[static_cast<std::size_t>(2 * index + 1)]});
        } else {
            columns = {column("index", "bin"), column("frequencyHz", "Hz"),
                       column("levelDb", snapshot.metadata.value("amplitudeUnit").toString())};
            for (int index = offset; index < last; ++index)
                rows.append(QJsonArray{index, snapshot.first[static_cast<std::size_t>(index)],
                                       snapshot.second[static_cast<std::size_t>(index)]});
        }
        return {{"ok", true}, {"valid", true}, {"revision", snapshot.revision},
                {"metadata", snapshot.metadata}, {"core", snapshot.core}, {"view", view},
                {"offset", offset}, {"total", total}, {"returned", rows.size()},
                {"nextOffset", last < total ? QJsonValue(last) : QJsonValue(QJsonValue::Null)},
                {"columns", columns}, {"rows", rows}};
    }

    QJsonObject exportPage(const Snapshot &snapshot, bool isIq, const QJsonObject &request) const {
        QJsonObject page = read(snapshot, isIq, request);
        if (!page.value("ok").toBool()) return page;
        const QString format = request.value("format").toString();
        QByteArray content;
        if (format == QStringLiteral("json")) {
            content = QJsonDocument(page).toJson(QJsonDocument::Compact);
            content.append('\n');
        } else if (format == QStringLiteral("csv")) {
            const QJsonArray columns = page.value("columns").toArray();
            QStringList header{"source", "sampleRateHz", "centerHz", "amplitudeUnit", "revision"};
            for (const QJsonValue &entry : columns) {
                const QJsonObject c = entry.toObject();
                header.append(csvField(c.value("name").toString() + QLatin1Char('[') +
                                       c.value("unit").toString() + QLatin1Char(']')));
            }
            content = header.join(',').toUtf8() + '\n';
            QStringList prefix{csvField(snapshot.metadata.value("source").toString()),
                csvValue(snapshot.metadata.value("sampleRateHz")), csvValue(snapshot.metadata.value("centerHz")),
                csvField(snapshot.metadata.value("amplitudeUnit").toString()), csvField(snapshot.revision)};
            for (const QJsonValue &entry : page.value("rows").toArray()) {
                QStringList fields = prefix;
                for (const QJsonValue &value : entry.toArray()) fields.append(csvValue(value));
                content += fields.join(',').toUtf8() + '\n';
            }
        } else {
            return failure(QStringLiteral("Export format must be json or csv"));
        }
        // Each export is one bounded page, never an implicit full-data dump.
        if (request.contains("path")) {
            const QString path = request.value("path").toString();
            if (path.isEmpty() || path.size() > 4096 || path.contains(QChar(0)))
                return failure(QStringLiteral("Export path must be nonempty and at most 4096 characters"));
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                return failure(QStringLiteral("Cannot create export: %1").arg(file.errorString()), QStringLiteral("io_error"));
            if (file.write(content) != content.size() || !file.flush()) {
                const QString error = file.errorString();
                file.remove();
                return failure(QStringLiteral("Cannot complete export: %1").arg(error), QStringLiteral("io_error"));
            }
            file.close();
            page.insert("path", path);
        } else {
            page.insert("content", QString::fromUtf8(content));
        }
        page.remove("rows");
        page.insert("format", format);
        page.insert("bytes", content.size());
        return page;
    }
};

CliResearchTools::CliResearchTools() : impl(std::make_unique<Impl>()) {}
CliResearchTools::~CliResearchTools() = default;

QJsonObject CliResearchTools::command(const QJsonObject &request) {
    const QString operation = request.value("operation").toString();
    if (operation.isEmpty() || operation.size() > 64)
        return failure(QStringLiteral("operation is required (at most 64 characters)"));
    if (operation == QStringLiteral("capabilities"))
        return {{"ok", true}, {"supported", QJsonArray{"iq.analyze", "iq.read", "iq.export", "iq.clear",
            "spectrum.analyze", "spectrum.read", "spectrum.export", "spectrum.clear",
            "density.start", "density.append", "density.status", "density.read", "density.export", "density.clear",
            "pulse.analyze", "pulse.status", "pulse.read", "pulse.export", "pulse.clear",
            "constellation.analyze", "constellation.status", "constellation.read", "constellation.export", "constellation.clear",
            "eye.analyze", "eye.status", "eye.read", "eye.export", "eye.clear",
            "comb.analyze", "comb.status", "comb.read", "comb.export", "comb.clear",
            "iq.phase.analyze", "iq.phase.status", "iq.phase.read", "iq.phase.export", "iq.phase.clear", "status", "clear"}},
            {"unsupported", unsupportedTools()}, {"maximumIqSamples", MaximumIqSamples},
            {"maximumSpectrumBins", MaximumSpectrumBins}, {"maximumPageRows", MaximumPageRows},
            {"maximumDensityColumns", Research::MaximumDensityColumns}, {"densityRows", Research::DensityRows},
            {"maximumDensityHistoryFrames", Research::MaximumHistoryFrames}, {"maximumDensityHistoryCells", Research::MaximumHistoryCells},
            {"maximumConstellationPoints", Research::MaximumConstellationPoints},
            {"maximumEyeTraces", Research::MaximumEyeTraces}, {"eyePointsPerTrace", Research::EyePoints},
            {"backgroundWork", false}, {"submitHooks", false}};
    if (operation == QStringLiteral("status"))
        return {{"ok", true}, {"iq", impl->info(impl->iq, true)}, {"spectrum", impl->info(impl->spectrum, false)},
            {"density", impl->info(impl->density, false)}, {"pulse", impl->info(impl->pulse, false)},
            {"constellation", impl->info(impl->constellation, false)}, {"eye", impl->info(impl->eye, false)},
            {"comb", impl->info(impl->comb, false)}, {"phase", impl->info(impl->phase, false)}};
    if (operation == QStringLiteral("clear")) {
        impl->iq = {};
        impl->spectrum = {};
        impl->density = {};
        impl->pulse = {};
        impl->constellation = {};
        impl->eye = {};
        impl->phase = {};
        impl->comb = {};
        impl->lastCombStep = 0;
        impl->lastCombMean = std::numeric_limits<double>::quiet_NaN();
        return {{"ok", true}};
    }
    if (operation == QStringLiteral("iq.analyze")) return impl->analyzeIq(request);
    if (operation == QStringLiteral("spectrum.analyze")) return impl->analyzeSpectrum(request);
    if (operation == QStringLiteral("density.start")) return impl->startDensity(request);
    if (operation == QStringLiteral("density.append")) return impl->appendDensity(request);
    if (operation == QStringLiteral("pulse.analyze")) return impl->analyzePulse(request);
    if (operation == QStringLiteral("constellation.analyze")) return impl->analyzeViews(request, false);
    if (operation == QStringLiteral("eye.analyze")) return impl->analyzeViews(request, true);
    if (operation == QStringLiteral("iq.phase.analyze")) return impl->analyzePhase(request);
    if (operation == QStringLiteral("comb.analyze")) return impl->analyzeComb(request);
    const QString group = operation.startsWith(QStringLiteral("iq.phase.")) ? QStringLiteral("iq.phase") : operation.section('.', 0, 0);
    Impl::Snapshot *extra = nullptr;
    if (group == QStringLiteral("density")) extra = &impl->density;
    else if (group == QStringLiteral("pulse")) extra = &impl->pulse;
    else if (group == QStringLiteral("constellation")) extra = &impl->constellation;
    else if (group == QStringLiteral("eye")) extra = &impl->eye;
    else if (group == QStringLiteral("iq.phase")) extra = &impl->phase;
    else if (group == QStringLiteral("comb")) extra = &impl->comb;
    if (extra) {
        const QString action = operation.mid(group.size() + 1);
        if (action == QStringLiteral("status")) return {{"ok", true}, {"snapshot", impl->info(*extra, false)}};
        if (action == QStringLiteral("read")) return impl->read(*extra, false, request);
        if (action == QStringLiteral("export")) return impl->exportPage(*extra, false, request);
        if (action == QStringLiteral("clear")) {
            *extra = {};
            if (extra == &impl->comb) { impl->lastCombStep = 0; impl->lastCombMean = std::numeric_limits<double>::quiet_NaN(); }
            return {{"ok", true}};
        }
    }
    const bool isIq = operation.startsWith(QStringLiteral("iq."));
    const bool isSpectrum = operation.startsWith(QStringLiteral("spectrum."));
    if (isIq || isSpectrum) {
        Impl::Snapshot &snapshot = isIq ? impl->iq : impl->spectrum;
        const QString action = operation.mid(operation.indexOf('.') + 1);
        if (action == QStringLiteral("read")) return impl->read(snapshot, isIq, request);
        if (action == QStringLiteral("export")) return impl->exportPage(snapshot, isIq, request);
        if (action == QStringLiteral("clear")) {
            snapshot = {};
            return {{"ok", true}};
        }
    }
    const QString tool = operation.startsWith(QStringLiteral("iq.phase"))
        ? QStringLiteral("iq.phase") : operation.section('.', 0, 0);
    const QJsonObject unavailable = unsupported(tool);
    if (!unavailable.isEmpty()) return unavailable;
    return failure(QStringLiteral("Unknown research operation"), QStringLiteral("unknown_operation"));
}
