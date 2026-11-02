#include "clizerospan.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace {
QJsonObject errorReply(const QString &message) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}
}

QJsonObject CliZeroSpan::status() const {
    QJsonObject result{{QStringLiteral("running"), active},
                       {QStringLiteral("startHz"), startHz},
                       {QStringLiteral("endHz"), endHz},
                       {QStringLiteral("points"), static_cast<int>(points.size())},
                       {QStringLiteral("maxPoints"), maxPoints},
                       {QStringLiteral("timeBase"), QStringLiteral("host FFT observation clock")}};
    if (!points.empty()) {
        const Point &latest = points.back();
        result.insert(QStringLiteral("latestElapsedMs"), latest.elapsedMs);
        result.insert(QStringLiteral("latestMeanPowerDbfs"), latest.meanPowerDbfs);
        result.insert(QStringLiteral("latestPeakDbfs"), latest.peakDbfs);
    }
    return result;
}

QJsonObject CliZeroSpan::read(int offset, int limit) const {
    if (offset < 0 || limit < 1 || limit > 1000)
        return errorReply(QStringLiteral("Zero-span read requires offset >= 0 and limit 1..1000"));
    QJsonArray values;
    const int total = static_cast<int>(points.size());
    const int last = offset >= total ? total : offset + (std::min)(limit, total - offset);
    for (int i = offset; i < last; ++i) {
        const Point &point = points[static_cast<std::size_t>(i)];
        values.append(QJsonObject{{QStringLiteral("utcMs"), static_cast<double>(point.utcMs)},
                                  {QStringLiteral("elapsedMs"), point.elapsedMs},
                                  {QStringLiteral("meanPowerDbfs"), point.meanPowerDbfs},
                                  {QStringLiteral("peakDbfs"), point.peakDbfs}});
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("offset"), offset},
            {QStringLiteral("total"), static_cast<int>(points.size())},
            {QStringLiteral("points"), values}};
}

QJsonObject CliZeroSpan::exportCsv(const QString &path) const {
    if (points.empty()) return errorReply(QStringLiteral("No zero-span points to export"));
    if (path.isEmpty()) return errorReply(QStringLiteral("CSV path is required"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return errorReply(QStringLiteral("Cannot create new CSV file: %1").arg(file.errorString()));
    if (file.write("utc_ms,elapsed_ms,band_start_hz,band_end_hz,mean_power_dbfs,peak_dbfs\n") < 0)
        return errorReply(file.errorString());
    for (const Point &point : points) {
        const QByteArray line = QStringLiteral("%1,%2,%3,%4,%5,%6\n")
            .arg(point.utcMs)
            .arg(point.elapsedMs, 0, 'f', 3)
            .arg(startHz, 0, 'f', 3)
            .arg(endHz, 0, 'f', 3)
            .arg(point.meanPowerDbfs, 0, 'f', 4)
            .arg(point.peakDbfs, 0, 'f', 4).toUtf8();
        if (file.write(line) != line.size()) return errorReply(file.errorString());
    }
    if (!file.flush()) return errorReply(file.errorString());
    return {{QStringLiteral("ok"), true}, {QStringLiteral("path"), path},
            {QStringLiteral("points"), static_cast<int>(points.size())}};
}

QJsonObject CliZeroSpan::command(const QJsonObject &request, double centerHz, double sampleRateHz) {
    const QString operation = request.value(QStringLiteral("operation")).toString();
    if (operation == QStringLiteral("status"))
        return {{QStringLiteral("ok"), true}, {QStringLiteral("zeroSpan"), status()}};
    if (operation == QStringLiteral("stop")) {
        stop();
        return {{QStringLiteral("ok"), true}, {QStringLiteral("zeroSpan"), status()}};
    }
    if (operation == QStringLiteral("read"))
        return read(request.value(QStringLiteral("offset")).toInt(),
                    request.value(QStringLiteral("limit")).toInt(100));
    if (operation == QStringLiteral("export"))
        return exportCsv(request.value(QStringLiteral("path")).toString());
    if (operation != QStringLiteral("start")) return errorReply(QStringLiteral("Unknown zero-span operation"));
    const double start = request.value(QStringLiteral("startHz")).toDouble(-1.0);
    const double end = request.value(QStringLiteral("endHz")).toDouble(-1.0);
    const int limit = request.value(QStringLiteral("maxPoints")).toInt(10000);
    if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(centerHz) ||
        !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0 || start <= 0.0 || end <= start ||
        start < centerHz - sampleRateHz * 0.5 || end > centerHz + sampleRateHz * 0.5 ||
        limit < 1 || limit > 100000)
        return errorReply(QStringLiteral("Zero-span band must fit the current IQ span; max points 1..100000"));
    active = true;
    startHz = start;
    endHz = end;
    sourceCenterHz = centerHz;
    sourceRateHz = sampleRateHz;
    maxPoints = limit;
    points.clear();
    clock.start();
    return {{QStringLiteral("ok"), true}, {QStringLiteral("zeroSpan"), status()}};
}

QJsonObject CliZeroSpan::observe(const std::vector<float> &levels, double centerHz, double sampleRateHz) {
    if (!active || levels.empty()) return {};
    if (std::abs(centerHz - sourceCenterHz) > 0.5 || std::abs(sampleRateHz - sourceRateHz) > 0.5) {
        stop();
        return {{QStringLiteral("event"), QStringLiteral("zeroSpanStopped")},
                {QStringLiteral("reason"), QStringLiteral("IQ source span changed")}};
    }
    const double firstHz = centerHz - sampleRateHz * 0.5;
    const double binHz = sampleRateHz / levels.size();
    const int first = (std::max)(0, static_cast<int>(std::ceil((startHz - firstHz) / binHz)));
    const int last = (std::min)(static_cast<int>(levels.size()) - 1,
        static_cast<int>(std::floor((endHz - firstHz) / binHz)));
    if (last < first) {
        stop();
        return {{QStringLiteral("event"), QStringLiteral("zeroSpanStopped")},
                {QStringLiteral("reason"), QStringLiteral("Selected band contains no FFT bin")}};
    }
    double power = 0.0;
    double peak = -200.0;
    for (int i = first; i <= last; ++i) {
        const double db = levels[static_cast<std::size_t>(i)];
        if (!std::isfinite(db)) continue;
        power += std::pow(10.0, db / 10.0);
        peak = (std::max)(peak, db);
    }
    const double mean = 10.0 * std::log10((std::max)(power / (last - first + 1), 1e-20));
    if (points.empty()) clock.restart();
    points.push_back({QDateTime::currentMSecsSinceEpoch(), clock.nsecsElapsed() / 1e6, mean, peak});
    if (static_cast<int>(points.size()) < maxPoints) return {};
    stop();
    return {{QStringLiteral("event"), QStringLiteral("zeroSpanComplete")},
            {QStringLiteral("points"), static_cast<int>(points.size())}};
}
