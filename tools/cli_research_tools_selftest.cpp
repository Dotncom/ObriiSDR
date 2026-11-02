#include "cliresearchtools.h"
#include "headlessresearchmetrics.h"

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
int failures = 0;

void check(bool condition, const char *name) {
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

bool ok(const QJsonObject &object) { return object.value("ok").toBool(); }

QJsonObject metadata(bool iq, double rate = 8, double center = 0) {
    return {{"source", "synthetic"}, {"sampleRateHz", rate}, {"centerHz", center},
            {"amplitudeUnit", iq ? "normalized" : "dBFS"}};
}

QJsonObject iqRequest(const QJsonArray &iq) {
    return {{"operation", "iq.analyze"}, {"metadata", metadata(true)}, {"iq", iq}};
}

QJsonObject spectrumRequest(int count = 9, double step = 100, double peak = -10) {
    QJsonArray f, db;
    for (int i = 0; i < count; ++i) {
        f.append(i * step);
        db.append(i == count / 2 ? peak : -40);
    }
    return {{"operation", "spectrum.analyze"}, {"frequenciesHz", f}, {"levelsDb", db},
            {"metadata", metadata(false, count * step, (count - 1) * step * 0.5)}};
}

QJsonObject getMetric(const QJsonObject &reply, const char *tool, const char *name) {
    return reply.value(QLatin1String(tool)).toObject().value("metrics").toObject().value(QLatin1String(name)).toObject();
}

void near(const QJsonObject &m, double expected, double tolerance, const char *name) {
    check(m.value("valid").toBool() && m.value("value").isDouble() &&
          std::abs(m.value("value").toDouble() - expected) <= tolerance, name);
}

void iqTests(CliResearchTools &tools) {
    const QJsonArray samples{0.5, 0.25, -0.5, -0.25, 0.5, 0.25, -0.5, -0.25};
    const QJsonObject result = tools.command(iqRequest(samples));
    check(ok(result), "IQ analysis accepted");
    near(getMetric(result, "iq", "dcI"), 0, 0, "zero I DC");
    near(getMetric(result, "iq", "dcQ"), 0, 0, "zero Q DC");
    near(getMetric(result, "iq", "rmsI"), 0.5, 1e-12, "AC RMS I");
    near(getMetric(result, "iq", "rmsQ"), 0.25, 1e-12, "AC RMS Q");
    near(getMetric(result, "iq", "iqGainImbalanceDb"), 20 * std::log10(2.0), 1e-10, "analytic gain imbalance");
    near(getMetric(result, "iq", "normalizedIqCorrelation"), 1, 1e-12, "analytic correlation");
    near(getMetric(result, "iq", "nearClippingPercent"), 0, 0, "unclipped IQ");
    check(getMetric(result, "iq", "rmsI").value("unit") == QJsonValue("normalized amplitude"), "IQ units explicit");
    check(!result.value("iq").toObject().value("metrics").toObject().contains("wallRateHz"), "no timing-dependent measurement");

    const QString revision = result.value("iq").toObject().value("revision").toString();
    QJsonObject request{{"operation", "iq.read"}, {"offset", 1}, {"limit", 2}, {"revision", revision}};
    const QJsonObject page = tools.command(request);
    const QJsonArray rows = page.value("rows").toArray();
    check(ok(page) && rows.size() == 2 && page.value("nextOffset").toInt() == 3 &&
          page.value("total").toInt() == 4, "bounded scope paging");
    check(rows[0].toArray() == QJsonArray{1, 0.125, -0.5, -0.25}, "scope raw sample and source-rate time");
    check(page.value("metadata").toObject() == metadata(true), "metadata retained without inference");
    request.insert("offset", 4);
    check(tools.command(request).value("nextOffset").isNull(), "end of scope");
    request.insert("offset", (std::numeric_limits<int>::max)());
    check(tools.command(request).value("rows").toArray().isEmpty(), "large offset does not overflow");

    const auto constant = tools.command(iqRequest(QJsonArray{0.25, -0.5, 0.25, -0.5}));
    near(getMetric(constant, "iq", "dcI"), 0.25, 0, "constant IQ DC");
    near(getMetric(constant, "iq", "rmsI"), 0, 0, "constant AC RMS");
    const auto undefined = getMetric(constant, "iq", "normalizedIqCorrelation");
    check(!undefined.value("valid").toBool() && undefined.value("value").isNull() &&
          undefined.value("reason") == QJsonValue("zero_ac_variance"), "degenerate correlation not faked as zero");
    check(!getMetric(constant, "iq", "iqGainImbalanceDb").value("valid").toBool(), "degenerate gain imbalance invalid");
    request.insert("offset", 0);
    check(tools.command(request).value("code") == QJsonValue("stale_revision"), "paging detects snapshot replacement");
    request.insert("revision", 1);
    check(!ok(tools.command(request)), "revision must be a string");

    const auto quadrature = tools.command(iqRequest(QJsonArray{1, 0, 0, 1, -1, 0, 0, -1}));
    near(getMetric(quadrature, "iq", "rmsI"), std::sqrt(0.5), 1e-12, "quadrature RMS");
    near(getMetric(quadrature, "iq", "normalizedIqCorrelation"), 0, 0, "orthogonal IQ correlation");
    near(getMetric(quadrature, "iq", "nearClippingPercent"), 100, 0, "near-clipping percentage");

    const auto offsetIq = tools.command(iqRequest(QJsonArray{0.75, 0.125, -0.25, -0.375}));
    near(getMetric(offsetIq, "iq", "dcI"), 0.25, 0, "nonzero DC I");
    near(getMetric(offsetIq, "iq", "dcQ"), -0.125, 0, "nonzero DC Q");
    near(getMetric(offsetIq, "iq", "rmsI"), 0.5, 0, "RMS is DC-removed");
}

void spectrumTests(CliResearchTools &tools) {
    const auto result = tools.command(spectrumRequest());
    check(ok(result), "spectrum analysis accepted");
    near(getMetric(result, "spectrum", "peakFrequencyHz"), 400, 0, "peak frequency");
    near(getMetric(result, "spectrum", "centroidFrequencyHz"), 400, 1e-9, "symmetric power centroid");
    near(getMetric(result, "spectrum", "peakDb"), -10, 0, "peak level");
    near(getMetric(result, "spectrum", "noiseFloorDb"), -40, 0, "shared 40th percentile noise floor");
    near(getMetric(result, "spectrum", "snrDb"), 30, 0, "peak-to-floor SNR");
    near(getMetric(result, "spectrum", "channelPowerDb"), 10 * std::log10(0.1 + 8e-4), 1e-5, "linear-bin power integration");
    near(getMetric(result, "spectrum", "occupiedBandwidth99Hz"), 0, 0, "noise-subtracted single-bin occupied width");
    near(getMetric(result, "spectrum", "width3DbHz"), 0, 0, "single-bin width without invented resolution");
    check(getMetric(result, "spectrum", "peakDb").value("unit") == QJsonValue("dBFS"), "spectrum unit preserved");
    auto request = spectrumRequest();
    auto meta = request.value("metadata").toObject();
    meta.insert("amplitudeUnit", "dBm");
    request.insert("metadata", meta);
    const auto calibrated = tools.command(request);
    check(getMetric(calibrated, "spectrum", "peakDb").value("unit") == QJsonValue("dBm"), "dBm is caller-declared, no conversion");
    check(getMetric(calibrated, "spectrum", "snrDb").value("unit") == QJsonValue("dB"), "ratio units are dB");
    auto page = tools.command({{"operation", "spectrum.read"}, {"offset", 3}, {"limit", 1}});
    check(page.value("rows").toArray().first().toArray() == QJsonArray{3, 300, -40}, "spectrum paged data");

    QJsonArray frequencies, levels;
    for (int i = 0; i <= 100; ++i) {
        frequencies.append(i);
        levels.append(i == 20 ? -10 : i == 40 ? -30 : i == 60 ? -40 : i == 80 ? -50 : -60);
    }
    request = {{"operation", "spectrum.analyze"}, {"metadata", metadata(false, 102, 50)},
               {"frequenciesHz", frequencies}, {"levelsDb", levels},
               {"tonal", QJsonObject{{"fundamentalHz", 20}, {"integrationHalfWidthHz", 1}, {"lastHarmonic", 4}}}};
    const auto tone = tools.command(request);
    // Core excludes DC bins 0,1; subtracts a 1e-6/bin noise floor from
    // three-bin signal/harmonic bands. There are 99 included noise bins.
    const double signal = 0.1 - 1e-6, distortion = 0.00111 - 3e-6, noise = 99e-6;
    near(getMetric(tone, "spectrum", "tonal.snrDb"), 10 * std::log10(signal / noise), 1e-7, "analytic tonal SNR");
    near(getMetric(tone, "spectrum", "tonal.sinadDb"), 10 * std::log10(signal / (noise + distortion)), 1e-7, "analytic tonal SINAD");
    near(getMetric(tone, "spectrum", "tonal.thdPercent"), 100 * std::sqrt(distortion / signal), 1e-7, "analytic THD");
    near(getMetric(tone, "spectrum", "tonal.harmonicsMeasured"), 3, 0, "harmonics measured");
    near(getMetric(tone, "spectrum", "tonal.completeHarmonicSpan"), 1, 0, "complete harmonic span");
    request.insert("tonal", QJsonObject{{"fundamentalHz", 20}, {"integrationHalfWidthHz", 1}, {"lastHarmonic", 6}});
    const auto partial = tools.command(request);
    near(getMetric(partial, "spectrum", "tonal.completeHarmonicSpan"), 0, 0, "partial harmonic span explicit");
    levels = {};
    for (int i = 0; i <= 100; ++i) levels.append(-60);
    request.insert("levelsDb", levels);
    const auto flat = tools.command(request);
    check(ok(flat) && !getMetric(flat, "spectrum", "tonal.snrDb").value("valid").toBool() &&
          getMetric(flat, "spectrum", "tonal.snrDb").value("value").isNull(), "unresolved tone invalid, not synthetic SNR");
}

void validationTests(CliResearchTools &tools) {
    const auto before = tools.command({{"operation", "status"}});
    for (const QJsonArray bad : {QJsonArray{}, QJsonArray{1}, QJsonArray{1, 2, 3},
                               QJsonArray{1, QJsonValue(QJsonValue::Null)}, QJsonArray{1, "0"},
                               QJsonArray{1, 1e100}})
        check(!ok(tools.command(iqRequest(bad))), "bad IQ rejected");
    auto request = iqRequest(QJsonArray{0, 0});
    request.remove("metadata");
    check(!ok(tools.command(request)), "source metadata mandatory");
    for (const char *key : {"source", "sampleRateHz", "centerHz", "amplitudeUnit"}) {
        request = iqRequest(QJsonArray{0, 0});
        auto m = request.value("metadata").toObject();
        m.remove(QLatin1String(key));
        request.insert("metadata", m);
        check(!ok(tools.command(request)), "each metadata field required");
    }
    for (const QJsonValue &rate : {QJsonValue(0), QJsonValue(-1), QJsonValue(1e13), QJsonValue(1e-300), QJsonValue("8")}) {
        request = iqRequest(QJsonArray{0, 0});
        auto m = metadata(true);
        m.insert("sampleRateHz", rate);
        request.insert("metadata", m);
        check(!ok(tools.command(request)), "invalid source rate rejected");
    }
    auto invalidSpectrum = spectrumRequest();
    invalidSpectrum.insert("levelsDb", QJsonArray{0, 0});
    check(!ok(tools.command(invalidSpectrum)), "mismatched spectrum rejected");
    invalidSpectrum = spectrumRequest();
    auto f = invalidSpectrum.value("frequenciesHz").toArray();
    f[2] = 201;
    invalidSpectrum.insert("frequenciesHz", f);
    check(!ok(tools.command(invalidSpectrum)), "nonuniform spectrum rejected");
    invalidSpectrum = spectrumRequest();
    f = invalidSpectrum.value("frequenciesHz").toArray();
    f[2] = QJsonValue(QJsonValue::Null);
    invalidSpectrum.insert("frequenciesHz", f);
    check(!ok(tools.command(invalidSpectrum)), "spectrum gaps not interpolated");
    invalidSpectrum = spectrumRequest();
    invalidSpectrum.insert("metadata", metadata(false, 2, 400));
    check(!ok(tools.command(invalidSpectrum)), "spectrum metadata span checked");
    invalidSpectrum = spectrumRequest();
    invalidSpectrum.insert("frequenciesHz", QJsonArray{1e9, 1e9 + 1});
    invalidSpectrum.insert("levelsDb", QJsonArray{-30, -40});
    invalidSpectrum.insert("metadata", metadata(false, 1000, 1e9));
    check(!ok(tools.command(invalidSpectrum)), "float32-collapsed high-RF bins rejected");
    for (const QJsonValue &limit : {QJsonValue(0), QJsonValue(1025), QJsonValue(1.5), QJsonValue("1")})
        check(!ok(tools.command({{"operation", "iq.read"}, {"limit", limit}})), "invalid paging limit rejected");
    check(!ok(tools.command({{"operation", "iq.read"}, {"offset", -1}})), "negative paging offset rejected");
    check(!ok(tools.command({{"operation", "iq.read"}, {"view", "constellation"}})), "no silent view fallback");
    check(tools.command({{"operation", "status"}}) == before, "failed requests preserve both snapshots");
}

void exportTests(CliResearchTools &tools) {
    auto request = iqRequest(QJsonArray{0.5, 0.25, -0.5, -0.25});
    auto m = metadata(true);
    m.insert("source", "synthetic,\"quoted\"");
    request.insert("metadata", m);
    tools.command(request);
    const auto json = tools.command({{"operation", "iq.export"}, {"format", "json"}, {"limit", 1}});
    const auto parsed = QJsonDocument::fromJson(json.value("content").toString().toUtf8()).object();
    check(ok(json) && parsed.value("rows").toArray().size() == 1 && parsed.value("nextOffset").toInt() == 1,
          "JSON export is a bounded page");
    const auto csv = tools.command({{"operation", "iq.export"}, {"format", "csv"}, {"limit", 1}});
    const QString content = csv.value("content").toString();
    check(ok(csv) && content.count('\n') == 2 && content.contains("\"timeSeconds[s]\"") &&
          content.contains("\"synthetic,\"\"quoted\"\"\""), "CSV units, quoting and page bound");
    const auto metrics = tools.command({{"operation", "iq.export"}, {"format", "csv"}, {"view", "metrics"}});
    check(ok(metrics) && metrics.value("content").toString().contains("\"valid[boolean]\"") &&
          metrics.value("content").toString().contains("\"iqGainImbalanceDb\""), "CSV metric validity and units");
    QTemporaryDir directory;
    check(directory.isValid(), "temporary export directory");
    if (!directory.isValid()) return;
    const QString path = directory.filePath("research.csv");
    const QJsonObject exportRequest{{"operation", "iq.export"}, {"format", "csv"}, {"limit", 1}, {"path", path}};
    const auto written = tools.command(exportRequest);
    QFile file(path);
    check(ok(written) && !written.contains("content") && file.open(QIODevice::ReadOnly), "explicit file export");
    check(file.readAll() == content.toUtf8(), "file equals inline CSV page");
    file.close();
    check(!ok(tools.command(exportRequest)), "existing file never overwritten");
    check(!ok(tools.command({{"operation", "iq.export"}, {"format", "csv"}, {"path", directory.filePath("missing/file.csv")}})),
          "missing directory is not created implicitly");
    check(!ok(tools.command({{"operation", "iq.export"}, {"format", "png"}})), "unsupported export format rejected");
}

void densityTests(CliResearchTools &tools) {
    namespace R = HeadlessResearchMetrics;
    R::DensityHistogram histogram;
    R::DensityConfig config;
    config.columns = 3;
    config.historyFrames = 2;
    config.retainPercent = 50;
    config.minimumDb = -100;
    config.maximumDb = 0;
    check(histogram.configure(config), "pure density configure");
    check(histogram.append({-100, 0}, 0, 100), "pure density append");
    check(histogram.histogram()[255 * 3] == 1 && histogram.histogram()[128 * 3 + 1] == 1 &&
          histogram.histogram()[2] == 1, "density dB interpolation and lround row mapping");
    histogram.append({0, 0}, 0, 100);
    check(histogram.histogram()[128 * 3 + 1] == 0.5f && histogram.histogram()[1] == 1, "per-frame retention");
    histogram.append({-100, -100}, 0, 100);
    check(histogram.histogram()[128 * 3 + 1] == 0 && histogram.histogram()[1] == 0.5f &&
          histogram.histogram()[255 * 3 + 1] == 1 && histogram.retainedFrames() == 2, "aged contribution eviction");
    const auto before = histogram.histogram();
    check(!histogram.append({-100, std::numeric_limits<float>::quiet_NaN()}, 0, 100) && histogram.histogram() == before,
          "core density invalid frame is transactional");
    bool reset = false;
    histogram.append({-100, 0}, 10, 90, &reset);
    check(reset && histogram.retainedFrames() == 1, "GUI range tolerance reset");
    config.columns = 1;
    config.linearPower = true;
    config.retainPercent = 100;
    histogram.configure(config);
    histogram.append({-10, -10}, 0, 100);
    histogram.append({-10, -10}, 0, 100);
    check(std::abs(histogram.histogram()[26] - 0.2f) < 1e-7, "max-referenced linear power weights");
    config.columns = R::MaximumDensityColumns;
    config.historyFrames = R::MaximumHistoryFrames;
    check(!histogram.configure(config), "core density history memory budget");

    const auto m = metadata(false, 100, 50);
    QJsonObject start{{"operation", "density.start"}, {"metadata", m}, {"columns", 2}, {"historyFrames", 2},
        {"retainPercent", 50}, {"minimumDb", -100}, {"maximumDb", 0}};
    check(ok(tools.command(start)), "CLI density start");
    check(tools.command({{"operation", "density.read"}}).value("total").toInt() == 0, "no implied frequency axis before first density frame");
    QJsonObject frame{{"operation", "density.append"}, {"metadata", m}, {"frequenciesHz", QJsonArray{0, 100}},
        {"levelsDb", QJsonArray{-100, 0}}};
    const auto first = tools.command(frame);
    check(ok(first) && !first.value("rangeReset").toBool(), "CLI density append");
    const QString revision = first.value("density").toObject().value("revision").toString();
    const auto page = tools.command({{"operation", "density.read"}, {"offset", 255}, {"limit", 2}, {"revision", revision}});
    check(page.value("total").toInt() == 512 && page.value("rows").toArray() ==
        QJsonArray{QJsonArray{0, 255, 0, -100, 1}, QJsonArray{1, 0, 100, 0, 1}}, "density x-major paging with exact axes and raw weights");
    const auto unchanged = tools.command({{"operation", "density.status"}});
    tools.command({{"operation", "density.read"}, {"offset", 0}, {"limit", 1024}});
    check(tools.command({{"operation", "density.status"}}) == unchanged, "reads do not decay or accumulate");
    auto bad = frame;
    auto wrong = m;
    wrong.insert("source", "other");
    bad.insert("metadata", wrong);
    check(!ok(tools.command(bad)) && tools.command({{"operation", "density.status"}}) == unchanged, "density source changes rejected without mutation");
    frame.insert("levelsDb", QJsonArray{0, -100});
    tools.command(frame);
    check(tools.command({{"operation", "density.read"}, {"revision", revision}}).value("code") == QJsonValue("stale_revision"), "density append invalidates paging revision");
    frame.insert("levelsDb", QJsonArray{-100, 0});
    const auto third = tools.command(frame);
    near(getMetric(third, "density", "retainedFrames"), 2, 0, "density retained frame bound");
    const auto csv = tools.command({{"operation", "density.export"}, {"format", "csv"}, {"offset", 255}, {"limit", 2}});
    check(ok(csv) && csv.value("content").toString().count('\n') == 3 &&
        csv.value("content").toString().contains("density[decayed frame count]"), "density CSV paged exact units");
    frame.insert("frequenciesHz", QJsonArray{10, 90});
    check(tools.command(frame).value("rangeReset").toBool(), "density CLI reports range reset");
    start.insert("columns", 1024);
    start.insert("historyFrames", 4096);
    const auto previous = tools.command({{"operation", "density.status"}});
    check(!ok(tools.command(start)) && tools.command({{"operation", "density.status"}}) == previous, "bad density configuration preserves accumulation");
}

void pulseTests(CliResearchTools &tools) {
    QJsonArray times, levels;
    for (int i = 0; i <= 40; ++i) {
        times.append(i * 10);
        const int phase = i % 20;
        levels.append(phase == 5 || phase == 9 ? -80 : phase == 6 || phase == 8 ? -60 : phase == 7 ? -20 : -100);
    }
    QJsonObject request{{"operation", "pulse.analyze"}, {"metadata", metadata(false)}, {"timeBase", "observation"},
        {"timeMs", times}, {"levelsDb", levels}, {"thresholdDb", -70}, {"hysteresisDb", 10}};
    const auto result = tools.command(request);
    check(ok(result), "offline pulse analysis");
    near(getMetric(result, "pulse", "pulseCount"), 2, 0, "two completed pulses");
    near(getMetric(result, "pulse", "averageWidthMs"), 30, 0, "sampled threshold-to-release width");
    near(getMetric(result, "pulse", "averagePriMs"), 200, 0, "analytic PRI");
    near(getMetric(result, "pulse", "repetitionHz"), 5, 0, "analytic pulse frequency");
    near(getMetric(result, "pulse", "dutyPercent"), 15, 0, "completed widths divided by observed interval");
    near(getMetric(result, "pulse", "riseMs"), 10, 0, "10/90 percent dB excursion rise");
    near(getMetric(result, "pulse", "fallMs"), 10, 0, "10/90 percent dB excursion fall");
    const auto rows = tools.command({{"operation", "pulse.read"}, {"limit", 1}}).value("rows").toArray();
    check(rows.size() == 1 && rows.first().toArray() == QJsonArray{60, 90, 30, 10, 10, -20, 0}, "pulse event timestamps and censor flag");
    request.insert("thresholdMode", "relative");
    request.insert("thresholdDb", 30);
    const auto relative = tools.command(request);
    near(getMetric(relative, "pulse", "thresholdDb"), -70, 0, "GUI ordered[n/5] relative threshold");
    near(getMetric(relative, "pulse", "averageWidthMs"), 30, 0, "relative threshold pulse semantics");
    levels[40] = -60;
    request.insert("levelsDb", levels);
    const auto trailing = tools.command(request);
    near(getMetric(trailing, "pulse", "pulseCount"), 2, 0, "unfinished final pulse excluded");
    near(getMetric(trailing, "pulse", "trailingActive"), 1, 0, "unfinished pulse explicit");
    request.insert("thresholdMode", "absolute");
    request.insert("thresholdDb", -70);
    request.insert("timeMs", QJsonArray{0, 10, 20});
    request.insert("levelsDb", QJsonArray{-20, -60, -100});
    const auto censored = tools.command(request);
    check(ok(censored) && tools.command({{"operation", "pulse.read"}}).value("rows").toArray().first().toArray()[6].toInt() == 1,
        "left-censored GUI pulse retained and marked");
    check(!getMetric(censored, "pulse", "averagePriMs").value("valid").toBool(), "single pulse does not fabricate PRI");
    request.insert("levelsDb", QJsonArray{-100, -100, -100});
    const auto quiet = tools.command(request);
    near(getMetric(quiet, "pulse", "pulseCount"), 0, 0, "quiet pulse count valid zero");
    near(getMetric(quiet, "pulse", "dutyPercent"), 0, 0, "quiet duty valid zero");
    check(getMetric(quiet, "pulse", "averageWidthMs").value("value").isNull(), "no-event width invalid/null");
    const auto previous = tools.command({{"operation", "pulse.status"}});
    request.insert("timeMs", QJsonArray{0, 10, 10});
    check(!ok(tools.command(request)) && tools.command({{"operation", "pulse.status"}}) == previous, "nonmonotonic pulse observations rejected");
    request.insert("timeMs", QJsonArray{0, 10, 20.5});
    check(!ok(tools.command(request)), "fractional millisecond timestamps rejected");
    request.insert("timeMs", QJsonArray{0, 10, 20});
    request.remove("timeBase");
    check(!ok(tools.command(request)), "pulse timing provenance is mandatory");
}

void iqViewTests(CliResearchTools &tools) {
    QJsonArray iq;
    for (int i = 0; i < 20; ++i) { const double a = 0.25 + i * 0.125; iq.append(a); iq.append(2 * a - 0.5); }
    QJsonObject request{{"operation", "constellation.analyze"}, {"metadata", metadata(true)}, {"iq", iq},
        {"symbolRateHz", 2}, {"phaseFraction", 0.375}};
    const auto constellation = tools.command(request);
    near(getMetric(constellation, "constellation", "dcI"), 1.4375, 0, "constellation removed DC I");
    near(getMetric(constellation, "constellation", "dcQ"), 2.375, 0, "constellation removed DC Q");
    near(getMetric(constellation, "constellation", "pointCount"), 5, 0, "constellation symbol count");
    const auto points = tools.command({{"operation", "constellation.read"}, {"limit", 1}}).value("rows").toArray();
    check(points.size() == 1 && points.first().toArray() == QJsonArray{0, 0.1875, 1.5, -1, -2}, "fractional symbol-phase linear interpolation");
    request.insert("operation", "eye.analyze");
    request.insert("traces", 2);
    const auto eye = tools.command(request);
    near(getMetric(eye, "eye", "traceCount"), 2, 0, "requested complete eye traces");
    near(getMetric(eye, "eye", "limited"), 1, 0, "additional eye traces available but bounded");
    const auto page = tools.command({{"operation", "eye.read"}, {"offset", 191}, {"limit", 2}});
    const auto rows = page.value("rows").toArray();
    check(rows == QJsonArray{QJsonArray{0, 191, 2, 1.1875, 0, 0}, QJsonArray{1, 0, 0, 0.6875, -0.5, -1}},
        "two-symbol eye endpoints and cross-trace paging");
    request.insert("traces", 128);
    near(getMetric(tools.command(request), "eye", "traceCount"), 3, 0, "incomplete eye traces omitted");
    const auto previous = tools.command({{"operation", "eye.status"}});
    request.insert("symbolRateHz", 5);
    check(!ok(tools.command(request)) && tools.command({{"operation", "eye.status"}}) == previous, "too few samples/symbol rejected transactionally");
    request.insert("symbolRateHz", 2);
    request.insert("phaseFraction", -0.01);
    check(!ok(tools.command(request)), "out-of-range phase not silently clamped");
    request.insert("phaseFraction", 0.5);
    request.insert("traces", 129);
    check(!ok(tools.command(request)), "eye trace bound enforced");
    request.insert("traces", 1);
    request.insert("iq", QJsonArray{0, 0});
    const auto shortEye = tools.command(request);
    near(getMetric(shortEye, "eye", "traceCount"), 0, 0, "short input yields no fabricated eye");

    QJsonArray rotating;
    for (int i = 0; i < 16; ++i) {
        rotating.append(i % 4 == 0 ? 0.5 : i % 4 == 2 ? -0.5 : 0);
        rotating.append(i % 4 == 1 ? 0.5 : i % 4 == 3 ? -0.5 : 0);
    }
    request = {{"operation", "iq.phase.analyze"}, {"metadata", metadata(true, 16)}, {"iq", rotating}};
    const auto phase = tools.command(request);
    near(getMetric(phase, "phase", "frequencyHz"), 4, 1e-12, "analytic phase-step frequency");
    near(getMetric(phase, "phase", "frequencyStdHz"), 0, 1e-7, "constant phase-step deviation");
    near(getMetric(phase, "phase", "imageEstimateDb"), 10 * std::log10(4.0 / 3.5), 1e-10, "GUI pseudo-power estimate excludes first sample");
    const auto autocorrelation = tools.command({{"operation", "iq.phase.read"}, {"limit", 3}}).value("rows").toArray();
    check(autocorrelation == QJsonArray{QJsonArray{0, 2, 1}, QJsonArray{1, 0, 1}, QJsonArray{2, -2, 1}},
        "GUI stride-four autocorrelation, not a redefined unbiased estimator");
    const auto missingLag = tools.command({{"operation", "iq.phase.read"}, {"offset", 16}, {"limit", 1}}).value("rows").toArray().first().toArray();
    check(missingLag[1].isNull() && missingLag[2].toInt() == 0, "unobserved correlation lag invalid/null");
    request.insert("iq", QJsonArray{1, 0, -1, 0, 1, 0, -1, 0});
    const auto realOnly = tools.command(request);
    check(getMetric(realOnly, "phase", "imageEstimateDb").value("value").isNull(), "GUI 99 dB singular image sentinel not a valid measurement");
    request.insert("iq", QJsonArray{0, 0, 0, 0});
    check(!getMetric(tools.command(request), "phase", "frequencyHz").value("valid").toBool(), "zero signal frequency invalid");
}

void combTests(CliResearchTools &tools) {
    auto request = spectrumRequest(129);
    request.insert("operation", "comb.analyze");
    request.insert("minimumSpacingHz", 1000);
    request.insert("maximumSpacingHz", 4000);
    QJsonArray levels, reference;
    for (int i = 0; i < 129; ++i) {
        levels.append(i == 10 || i == 30 || i == 50 || i == 70 ? -20 : -100);
        reference.append(-100);
    }
    request.insert("levelsDb", levels);
    request.insert("referenceLevelsDb", reference);
    tools.command({{"operation", "comb.clear"}});
    const auto result = tools.command(request);
    near(getMetric(result, "comb", "peakCount"), 4, 0, "four interference peaks");
    near(getMetric(result, "comb", "familyCount"), 1, 0, "one spacing-vote family");
    near(getMetric(result, "comb", "family1.stepHz"), 2000, 0, "analytic comb spacing");
    near(getMetric(result, "comb", "family1.members"), 4, 0, "comb members");
    check(!getMetric(result, "comb", "stepDriftHz").value("valid").toBool(), "first comb has no invented drift");
    const auto rows = tools.command({{"operation", "comb.read"}, {"limit", 1}}).value("rows").toArray();
    check(rows.size() == 1 && rows.first().toArray() == QJsonArray{10, 1000, -20, 80, 80, 1, 0, 1}, "comb reference delta and harmonic assignment");
    QJsonArray quiet, drift;
    for (int i = 0; i < 129; ++i) {
        quiet.append(-100);
        drift.append(i == 10 || i == 32 || i == 54 || i == 76 ? -17 : -100);
    }
    request.insert("levelsDb", quiet);
    near(getMetric(tools.command(request), "comb", "familyCount"), 0, 0, "no-family observation valid zero");
    request.insert("levelsDb", drift);
    const auto changed = tools.command(request);
    near(getMetric(changed, "comb", "stepDriftHz"), 200, 0, "drift from last detected comparable comb");
    near(getMetric(changed, "comb", "amplitudeDriftDb"), 3, 0, "analytic amplitude drift");
    auto m = request.value("metadata").toObject();
    m.insert("source", "other");
    request.insert("metadata", m);
    check(!getMetric(tools.command(request), "comb", "stepDriftHz").value("valid").toBool(), "no cross-source comb drift");
    const auto previous = tools.command({{"operation", "comb.status"}});
    request.insert("minimumSpacingHz", 5000);
    check(!ok(tools.command(request)) && tools.command({{"operation", "comb.status"}}) == previous, "invalid comb options preserve state");
}

void newToolExportAndBoundTests(CliResearchTools &tools) {
    for (const char *tool : {"density", "pulse", "constellation", "eye", "comb", "iq.phase"}) {
        const QString prefix = QString::fromLatin1(tool);
        const auto json = tools.command({{"operation", prefix + ".export"}, {"format", "json"}, {"limit", 1}});
        const auto page = QJsonDocument::fromJson(json.value("content").toString().toUtf8()).object();
        check(ok(json) && page.value("rows").toArray().size() <= 1 && page.value("columns").isArray(), "new-tool JSON export uses bounded common schema");
        const auto csv = tools.command({{"operation", prefix + ".export"}, {"format", "csv"}, {"view", "metrics"}, {"limit", 1}});
        check(ok(csv) && csv.value("content").toString().count('\n') == 2, "new-tool CSV metric paging");
    }
    QJsonArray iq;
    for (int i = 0; i < CliResearchTools::MaximumIqSamples; ++i) { iq.append(i % 2 ? 0.5 : -0.5); iq.append(0.25); }
    auto request = iqRequest(iq);
    request.insert("operation", "constellation.analyze");
    request.insert("symbolRateHz", 4);
    const auto constellation = tools.command(request);
    near(getMetric(constellation, "constellation", "pointCount"), 3000, 0, "bounded constellation points");
    near(getMetric(constellation, "constellation", "limited"), 1, 0, "constellation truncation explicit");
    request.insert("operation", "eye.analyze");
    request.insert("traces", 128);
    const auto eye = tools.command(request);
    near(getMetric(eye, "eye", "pointCount"), 128 * 192, 0, "bounded eye points");
    check(tools.command({{"operation", "eye.read"}, {"limit", 1024}}).value("rows").toArray().size() == 1024, "large eye output paginated");
    namespace R = HeadlessResearchMetrics;
    check(!R::buildIqViews({0, 0}, 8, 5, 0.5, 1).valid && !R::analyzePulses({}, {}).valid,
        "pure helpers validate independently of CLI");
    std::vector<float> f(257), db(257, -100);
    for (std::size_t i = 0; i < f.size(); ++i) { f[i] = float(i * 100); if (i % 2 == 0 && i >= 2 && i + 2 < f.size()) db[i] = -20; }
    const auto manyPeaks = R::analyzeComb(f, db, 6, 200, 200);
    check(manyPeaks.valid && manyPeaks.peaks.size() == 96 && manyPeaks.families.size() <= 3, "comb storage and family caps");
    for (const char *tool : {"density", "pulse", "constellation", "eye", "comb", "iq.phase"}) {
        const QString prefix = QString::fromLatin1(tool);
        check(ok(tools.command({{"operation", prefix + ".clear"}})) &&
            tools.command({{"operation", prefix + ".read"}}).value("code") == QJsonValue("no_data"), "per-tool clear releases new research state");
    }
}

void limitsAndUnsupportedTests(CliResearchTools &tools) {
    QJsonArray full;
    for (int i = 0; i < CliResearchTools::MaximumIqSamples * 2; ++i) full.append(i % 2 ? 0.25 : 0.5);
    check(ok(tools.command(iqRequest(full))), "maximum IQ count accepted");
    auto page = tools.command({{"operation", "iq.read"}, {"limit", CliResearchTools::MaximumPageRows}});
    check(page.value("rows").toArray().size() == CliResearchTools::MaximumPageRows, "maximum page count enforced");
    full.append(0);
    full.append(0);
    check(!ok(tools.command(iqRequest(full))), "oversized IQ rejected");
    check(ok(tools.command(spectrumRequest(CliResearchTools::MaximumSpectrumBins, 1))), "maximum spectrum count accepted");
    check(!ok(tools.command(spectrumRequest(CliResearchTools::MaximumSpectrumBins + 1, 1))), "oversized spectrum rejected");

    const auto before = tools.command({{"operation", "status"}});
    const auto capabilities = tools.command({{"operation", "capabilities"}});
    check(ok(capabilities) && !capabilities.value("backgroundWork").toBool() &&
          !capabilities.value("submitHooks").toBool(), "idle has no subscriptions or hidden work");
    for (const QJsonValue &entry : capabilities.value("unsupported").toArray()) {
        const QString tool = entry.toObject().value("tool").toString();
        for (const char *action : {"analyze", "start", "read", "export"}) {
            const auto reply = tools.command({{"operation", tool + QLatin1Char('.') + QLatin1String(action)}});
            check(!ok(reply) && reply.value("code") == QJsonValue("unsupported") &&
                  !reply.value("valid").toBool() && !reply.value("error").toString().isEmpty(), "unsupported tool returns an explicit reason");
        }
    }
    check(before == tools.command({{"operation", "status"}}), "unsupported tools do not mutate snapshots");
    check(!ok(tools.command({{"operation", "unknown"}})) && !ok(tools.command({})), "unknown/missing operation rejected");
    check(ok(tools.command({{"operation", "iq.clear"}})), "per-tool clear");
    check(!ok(tools.command({{"operation", "iq.read"}})) && ok(tools.command({{"operation", "spectrum.read"}})), "clear preserves other tool");
    tools.command({{"operation", "clear"}});
    check(tools.command({{"operation", "spectrum.read"}}).value("code") == QJsonValue("no_data"), "global clear releases snapshots");
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    CliResearchTools tools;
    check(tools.command({{"operation", "iq.read"}}).value("code") == QJsonValue("no_data"), "initial state empty");
    iqTests(tools);
    spectrumTests(tools);
    validationTests(tools);
    exportTests(tools);
    densityTests(tools);
    pulseTests(tools);
    iqViewTests(tools);
    combTests(tools);
    newToolExportAndBoundTests(tools);
    limitsAndUnsupportedTests(tools);
    std::cout << "CLI research tools: " << failures << " failures\n";
    return failures ? 1 : 0;
}
