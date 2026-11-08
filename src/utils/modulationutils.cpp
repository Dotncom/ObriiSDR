#include "modulationutils.h"

#include "radiosettings.h"

#include <QColor>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

double defaultBandwidthForModulation(int modulationType) {
    switch (modulationType) {
    case MOD_NFM:
        return 12500.0;
    case MOD_SAM:
        return 6000.0;
    case MOD_USB:
    case MOD_LSB:
        return 2700.0;
    case MOD_DSB:
        return 6000.0;
    case MOD_CW:
        return 500.0;
    case MOD_WFM:
        return 200000.0;
    case MOD_FT8:
    case MOD_FT4:
        return 3000.0;
    case MOD_RTTY:
        return 2700.0;
    case MOD_PSK:
        return 2500.0;
    case MOD_FSK:
        return 12000.0;
    case MOD_DMR:
    case MOD_M17:
        return 12500.0;
    case MOD_ATV:
        return 5000000.0;
    case MOD_SSTV:
        return 3000.0;
    case MOD_APT:
        return 40000.0;
    case MOD_WEFAX:
        return 3000.0;
    case MOD_LRPT:
        return 140000.0;
    case MOD_AM:
    default:
        return 10000.0;
    }
}

QString formatBandwidthHz(double bandwidth) {
    return QString::number(bandwidth, 'f', 0);
}

double recommendedFpvDemodBandwidthHz(double detectedWidthHz) {
    if (!std::isfinite(detectedWidthHz) || detectedWidthHz <= 0.0) {
        return 5000000.0;
    }
    const double paddedWidthHz = detectedWidthHz * 1.18;
    if (paddedWidthHz <= 3200000.0) {
        return 3000000.0;
    }
    if (paddedWidthHz <= 5600000.0) {
        return 5000000.0;
    }
    if (paddedWidthHz <= 8500000.0) {
        return 8000000.0;
    }
    if (paddedWidthHz <= 11500000.0) {
        return 10000000.0;
    }
    return (std::clamp)(paddedWidthHz, 10000000.0, 20000000.0);
}

double recommendedDigitalVideoBandwidthHz(double detectedWidthHz) {
    if (!std::isfinite(detectedWidthHz) || detectedWidthHz <= 0.0) {
        return 5000000.0;
    }
    const double paddedWidthHz = detectedWidthHz * 1.15;
    if (paddedWidthHz <= 2500000.0) {
        return 2000000.0;
    }
    if (paddedWidthHz <= 5600000.0) {
        return 5000000.0;
    }
    if (paddedWidthHz <= 8500000.0) {
        return 8000000.0;
    }
    if (paddedWidthHz <= 11500000.0) {
        return 10000000.0;
    }
    return (std::clamp)(paddedWidthHz, 10000000.0, 20000000.0);
}

QImage createSstvTestPattern() {
    constexpr int width = 320;
    constexpr int height = 256;
    QImage image(width, height, QImage::Format_RGB32);
    static const QRgb bars[] = {
        qRgb(255, 255, 255),
        qRgb(255, 255, 0),
        qRgb(0, 255, 255),
        qRgb(0, 255, 0),
        qRgb(255, 0, 255),
        qRgb(255, 0, 0),
        qRgb(0, 0, 255),
        qRgb(16, 16, 16),
    };

    for (int y = 0; y < height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            if (y < 86) {
                line[x] = bars[(x * 8) / width];
            } else if (y < 172) {
                const int value = (x * 255) / (width - 1);
                line[x] = qRgb(value, value, value);
            } else {
                const bool checker = (((x / 16) + (y / 16)) % 2) == 0;
                const int ramp = ((x + y) * 255) / (width + height - 2);
                line[x] = checker ? qRgb(ramp, 80, 255 - ramp)
                                  : qRgb(255 - ramp, ramp, 80);
            }
        }
    }
    return image;
}
