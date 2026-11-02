#include "cliiqquality.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>

QJsonObject CliIqQuality::request(int blocks) {
    if (blocks < 1 || blocks > 100)
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("error"), QStringLiteral("Quality capture requires 1..100 IQ blocks")}};
    remaining = requested = blocks;
    samples = clipped = 0;
    sumI = sumQ = sumI2 = sumQ2 = sumIq = 0.0;
    sourceRate = 0.0;
    timer.invalidate();
    return {{QStringLiteral("ok"), true}, {QStringLiteral("pendingBlocks"), remaining}};
}

void CliIqQuality::cancel() {
    remaining = requested = 0;
    timer.invalidate();
}

QJsonObject CliIqQuality::status() const {
    return {{QStringLiteral("pendingBlocks"), remaining},
            {QStringLiteral("latest"), latest}};
}

QJsonObject CliIqQuality::submit(const float *iq, int count, double rate) {
    if (!remaining || !iq || count <= 0) return {};
    if (!timer.isValid()) timer.start();
    sourceRate = rate;
    for (int i = 0; i < count; ++i) {
        const float real = iq[2 * i], imaginary = iq[2 * i + 1];
        if (!std::isfinite(real) || !std::isfinite(imaginary)) continue;
        sumI += real;
        sumQ += imaginary;
        sumI2 += static_cast<double>(real) * real;
        sumQ2 += static_cast<double>(imaginary) * imaginary;
        sumIq += static_cast<double>(real) * imaginary;
        if (std::abs(real) >= 0.98f || std::abs(imaginary) >= 0.98f) ++clipped;
        ++samples;
    }
    if (--remaining) return {};
    const double n = (std::max)(1.0, static_cast<double>(samples));
    const double meanI = sumI / n, meanQ = sumQ / n;
    const double varianceI = (std::max)(0.0, sumI2 / n - meanI * meanI);
    const double varianceQ = (std::max)(0.0, sumQ2 / n - meanQ * meanQ);
    const double elapsed = timer.elapsed() / 1000.0;
    latest = {{QStringLiteral("event"), QStringLiteral("iqQuality")},
              {QStringLiteral("utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
              {QStringLiteral("blocks"), requested},
              {QStringLiteral("samples"), static_cast<double>(samples)},
              {QStringLiteral("sourceRateHz"), sourceRate},
              {QStringLiteral("wallRateHz"), elapsed > 0.0 ? samples / elapsed : 0.0},
              {QStringLiteral("dcI"), meanI}, {QStringLiteral("dcQ"), meanQ},
              {QStringLiteral("rmsI"), std::sqrt(varianceI)},
              {QStringLiteral("rmsQ"), std::sqrt(varianceQ)},
              {QStringLiteral("iqGainImbalanceDb"), 10.0 * std::log10((std::max)(varianceI, 1e-20) /
                                                                    (std::max)(varianceQ, 1e-20))},
              {QStringLiteral("normalizedIqCorrelation"), (sumIq / n - meanI * meanQ) /
                  std::sqrt((std::max)(varianceI * varianceQ, 1e-20))},
              {QStringLiteral("nearClippingPercent"), 100.0 * clipped / n}};
    return latest;
}
