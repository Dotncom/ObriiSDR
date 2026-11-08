#include "cliiqquality.h"

#include <QCoreApplication>

#include <cmath>
#include <vector>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    CliIqQuality quality;
    if (!quality.request(2).value(QStringLiteral("ok")).toBool()) return 1;
    std::vector<float> iq(1024 * 2);
    for (int i = 0; i < 1024; ++i) {
        iq[2 * i] = i % 2 ? 0.5f : -0.5f;
        iq[2 * i + 1] = i % 2 ? 0.25f : -0.25f;
    }
    if (!quality.submit(iq.data(), 1024, 2.048e6).isEmpty()) return 2;
    const QJsonObject result = quality.submit(iq.data(), 1024, 2.048e6);
    if (result.value(QStringLiteral("event")) != QStringLiteral("iqQuality") ||
        result.value(QStringLiteral("samples")).toDouble() != 2048.0 ||
        std::abs(result.value(QStringLiteral("iqGainImbalanceDb")).toDouble() - 6.0206) > 0.01 ||
        result.value(QStringLiteral("nearClippingPercent")).toDouble() != 0.0)
        return 3;
    return 0;
}
