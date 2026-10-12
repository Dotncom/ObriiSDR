#include "presethelpers.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
QString agileScanPresetSpec(const QString &rangesMhz, double stepMhz) {
    return QStringLiteral("%1\t%2").arg(rangesMhz.trimmed(),
                                      QString::number(stepMhz, 'f', 6));
}

QString standardScanPresetSpec(const QString &centersMhz, int dwellMs, int settleMs) {
    return QStringLiteral("%1\t%2\t%3").arg(centersMhz.trimmed(),
                                            QString::number(dwellMs),
                                            QString::number(settleMs));
}

QString listeningScanPresetSpec(const QString &targetsMhz, int dwellMs, int settleMs) {
    return QStringLiteral("%1\t%2\t%3").arg(targetsMhz.trimmed(),
                                            QString::number(dwellMs),
                                            QString::number(settleMs));
}

QString agileScanPresetRanges(const QString &spec, const QString &fallback) {
    const QStringList parts = spec.split(QChar('\t'));
    return parts.isEmpty() ? fallback : parts.first().trimmed();
}

QString standardScanPresetCenters(const QString &spec, const QString &fallback) {
    const QStringList parts = spec.split(QChar('\t'));
    return parts.isEmpty() ? fallback : parts.first().trimmed();
}

QString listeningScanPresetTargets(const QString &spec, const QString &fallback) {
    const QStringList parts = spec.split(QChar('\t'));
    return parts.isEmpty() ? fallback : parts.first().trimmed();
}

double agileScanPresetStepMhz(const QString &spec, double fallback) {
    const QStringList parts = spec.split(QChar('\t'));
    if (parts.size() < 2) {
        return fallback;
    }
    bool ok = false;
    const double value = parts.at(1).toDouble(&ok);
    return ok ? (std::clamp)(value, PRESET_AGILE_SCAN_MIN_STEP_MHZ, PRESET_AGILE_SCAN_MAX_STEP_MHZ) : fallback;
}

int standardScanPresetDwellMs(const QString &spec, int fallback) {
    const QStringList parts = spec.split(QChar('\t'));
    if (parts.size() < 2) {
        return fallback;
    }
    bool ok = false;
    const int value = parts.at(1).toInt(&ok);
    return ok ? (std::clamp)(value, PRESET_STANDARD_SCAN_MIN_DWELL_MS, PRESET_STANDARD_SCAN_MAX_DWELL_MS) : fallback;
}

int standardScanPresetSettleMs(const QString &spec, int fallback) {
    const QStringList parts = spec.split(QChar('\t'));
    if (parts.size() < 3) {
        return fallback;
    }
    bool ok = false;
    const int value = parts.at(2).toInt(&ok);
    return ok ? (std::clamp)(value, PRESET_STANDARD_SCAN_MIN_SETTLE_MS, PRESET_STANDARD_SCAN_MAX_SETTLE_MS) : fallback;
}

int listeningScanPresetDwellMs(const QString &spec, int fallback) {
    const QStringList parts = spec.split(QChar('\t'));
    if (parts.size() < 2) {
        return fallback;
    }
    bool ok = false;
    const int value = parts.at(1).toInt(&ok);
    return ok ? (std::clamp)(value, PRESET_LISTENING_SCAN_MIN_DWELL_MS, PRESET_LISTENING_SCAN_MAX_DWELL_MS) : fallback;
}

int listeningScanPresetSettleMs(const QString &spec, int fallback) {
    const QStringList parts = spec.split(QChar('\t'));
    if (parts.size() < 3) {
        return fallback;
    }
    bool ok = false;
    const int value = parts.at(2).toInt(&ok);
    return ok ? (std::clamp)(value, PRESET_LISTENING_SCAN_MIN_SETTLE_MS, PRESET_LISTENING_SCAN_MAX_SETTLE_MS) : fallback;
}

QStringList defaultCenterFrequencyPresetOrder() {
    QStringList order;
    order << QStringLiteral("FM broadcast 100 MHz")
          << QStringLiteral("Airband 125 MHz")
          << QStringLiteral("VHF 145 MHz")
          << QStringLiteral("UHF Satcom 255 MHz")
          << QStringLiteral("UHF 433 MHz")
          << QStringLiteral("LTE 700 downlink 780.5 MHz")
          << QStringLiteral("LTE 800 downlink 806 MHz")
          << QStringLiteral("GSM/LTE 900 MHz")
          << QStringLiteral("UMTS/LTE 1800 downlink 1842.5 MHz")
          << QStringLiteral("UMTS/LTE 2100 downlink 2140 MHz")
          << QStringLiteral("LTE 2600 downlink 2655 MHz")
          << QStringLiteral("GSM/LTE B8 downlink 942.5 MHz")
          << QStringLiteral("NR n78 band center 3550 MHz")
          << QStringLiteral("BLE advertising 37 2402 MHz")
          << QStringLiteral("BLE advertising 38 2426 MHz")
          << QStringLiteral("BLE advertising 39 2480 MHz")
          << QStringLiteral("Bluetooth 2.4 band center 2441 MHz")
          << QStringLiteral("Wi-Fi 2.4 ch1 2412 MHz")
          << QStringLiteral("Wi-Fi 2.4 ch6 2437 MHz")
          << QStringLiteral("Wi-Fi 2.4 ch11 2462 MHz")
          << QStringLiteral("Wi-Fi 2.4 ch13 2472 MHz")
          << QStringLiteral("Wi-Fi 5 ch36 5180 MHz")
          << QStringLiteral("Wi-Fi 5 ch100 5500 MHz")
          << QStringLiteral("Wi-Fi 5 ch149 5745 MHz")
          << QStringLiteral("Wi-Fi 6 ch1 5955 MHz")
          << QStringLiteral("GNSS L1 compact center 1583 MHz")
          << QStringLiteral("GNSS L1 band center 1584.5 MHz")
          << QStringLiteral("GPS/Galileo L1 1575.42 MHz")
          << QStringLiteral("BeiDou B1I 1561.098 MHz")
          << QStringLiteral("GLONASS L1 center 1602 MHz")
          << QStringLiteral("FPV 1.1 GHz")
          << QStringLiteral("FPV 1.2 GHz")
          << QStringLiteral("FPV 1.3 GHz")
          << QStringLiteral("FPV 2.4 GHz")
          << QStringLiteral("FPV 3.3 GHz")
          << QStringLiteral("Experimental 7.0 GHz")
          << QStringLiteral("Experimental 7.5 GHz");
    return order;
}

QStringList defaultListeningFrequencyPresetOrder() {
    QStringList order;
    order << QStringLiteral("HF center 0 Hz")
          << QStringLiteral("HF 500 kHz")
          << QStringLiteral("HF 1.25 MHz")
          << QStringLiteral("80 m 3.65 MHz")
          << QStringLiteral("40 m 7.05 MHz")
          << QStringLiteral("20 m FT8 14.074 MHz")
          << QStringLiteral("VHF 145 MHz")
          << QStringLiteral("UHF Satcom 255 MHz")
          << QStringLiteral("UHF 433 MHz")
          << QStringLiteral("LTE 700 downlink 780.5 MHz")
          << QStringLiteral("LTE 800 downlink 806 MHz")
          << QStringLiteral("GSM/LTE 900 MHz")
          << QStringLiteral("UMTS/LTE 1800 downlink 1842.5 MHz")
          << QStringLiteral("UMTS/LTE 2100 downlink 2140 MHz")
          << QStringLiteral("LTE 2600 downlink 2655 MHz")
          << QStringLiteral("GSM/LTE B8 downlink 942.5 MHz")
          << QStringLiteral("NR n78 band center 3550 MHz")
          << QStringLiteral("BLE advertising 37 2402 MHz")
          << QStringLiteral("BLE advertising 38 2426 MHz")
          << QStringLiteral("BLE advertising 39 2480 MHz")
          << QStringLiteral("Bluetooth 2.4 band center 2441 MHz")
          << QStringLiteral("Wi-Fi 2.4 ch1 2412 MHz")
          << QStringLiteral("Wi-Fi 2.4 ch6 2437 MHz")
          << QStringLiteral("Wi-Fi 2.4 ch11 2462 MHz")
          << QStringLiteral("Wi-Fi 2.4 ch13 2472 MHz")
          << QStringLiteral("Wi-Fi 5 ch36 5180 MHz")
          << QStringLiteral("Wi-Fi 5 ch100 5500 MHz")
          << QStringLiteral("Wi-Fi 5 ch149 5745 MHz")
          << QStringLiteral("Wi-Fi 6 ch1 5955 MHz")
          << QStringLiteral("GNSS L1 compact center 1583 MHz")
          << QStringLiteral("GNSS L1 band center 1584.5 MHz")
          << QStringLiteral("GPS/Galileo L1 1575.42 MHz")
          << QStringLiteral("BeiDou B1I 1561.098 MHz")
          << QStringLiteral("GLONASS L1 center 1602 MHz")
          << QStringLiteral("FPV 1.1 GHz")
          << QStringLiteral("FPV 1.2 GHz")
          << QStringLiteral("FPV 1.3 GHz")
          << QStringLiteral("FPV 2.4 GHz")
          << QStringLiteral("FPV 3.3 GHz");
    return order;
}

QStringList defaultBandwidthPresetOrder() {
    QStringList order;
    order << QStringLiteral("CW 500 Hz")
          << QStringLiteral("SSB 2.7 kHz")
          << QStringLiteral("FT8 3 kHz")
          << QStringLiteral("AM 6 kHz")
          << QStringLiteral("AM 10 kHz")
          << QStringLiteral("NFM 12.5 kHz")
          << QStringLiteral("UHF Satcom NFM 25 kHz")
          << QStringLiteral("DMR 12.5 kHz")
          << QStringLiteral("WFM 200 kHz")
          << QStringLiteral("SSTV 3 kHz")
          << QStringLiteral("NOAA APT 40 kHz")
          << QStringLiteral("WEFAX 3 kHz")
          << QStringLiteral("LRPT 140 kHz")
          << QStringLiteral("ATV 3 MHz")
          << QStringLiteral("ATV 5 MHz")
          << QStringLiteral("FPV 8 MHz")
          << QStringLiteral("FPV 10 MHz")
          << QStringLiteral("GNSS C/A 2.046 MHz")
          << QStringLiteral("GNSS raw 4.092 MHz")
          << QStringLiteral("GLONASS L1OF 9 MHz")
          << QStringLiteral("GNSS L1 survey 50 MHz");
    return order;
}

QStringList defaultAgileScanPresetOrder() {
    QStringList order;
    order << QStringLiteral("Narrow DMR example")
          << QStringLiteral("VHF DMR 160-174 coarse")
          << QStringLiteral("UHF DMR 400-470 coarse")
          << QStringLiteral("UHF Satcom 240-270 250kHz")
          << QStringLiteral("REB broad check 300/600/5800")
          << QStringLiteral("REB 300-400 1MHz")
          << QStringLiteral("REB 600-1200 5MHz")
          << QStringLiteral("Cellular LTE/3G downlinks sparse")
          << QStringLiteral("Digital video sparse")
          << QStringLiteral("REB 5.8GHz 5MHz")
          << QStringLiteral("FPV 1.1-1.3 common")
          << QStringLiteral("FPV 1.2/2.4 sparse")
          << QStringLiteral("FPV 3.3GHz sparse")
          << QStringLiteral("GNSS L1 1559-1610 50MHz");
    return order;
}

QStringList defaultStandardScanPresetOrder() {
    QStringList order;
    order << QStringLiteral("RF 100-300 by 50MHz")
          << QStringLiteral("UHF broad 400-700 by 50MHz")
          << QStringLiteral("Cellular LTE/3G downlinks")
          << QStringLiteral("GNSS L1 two-center 50MHz useful");
    return order;
}

QStringList defaultListeningScanPresetOrder() {
    QStringList order;
    order << QStringLiteral("FT8 HF common")
          << QStringLiteral("UHF Satcom survey")
          << QStringLiteral("Cellular LTE/3G anchors")
          << QStringLiteral("GNSS L1 main signals")
          << QStringLiteral("GLONASS L1OF channels")
          << QStringLiteral("GNSS L1 dense");
    return order;
}

QVector<double> parseAgileScanFrequenciesMhz(const QString &rangesMhz,
                                             double stepMhz,
                                             QString *error) {
    QVector<double> frequencies;
    QString text = rangesMhz.trimmed();
    text.replace(QChar(0x2013), QLatin1Char('-'));
    text.replace(QChar(0x2014), QLatin1Char('-'));
    text.replace(QLatin1Char('\\'), QLatin1Char(','));
    text.replace(QLatin1Char('/'), QLatin1Char(','));
    text.replace(QLatin1Char(';'), QLatin1Char(','));
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QString());
    text.replace(QRegularExpression(QStringLiteral("mhz|мгц"), QRegularExpression::CaseInsensitiveOption),
                 QString());

    if (text.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Scan ranges are empty");
        }
        return frequencies;
    }

    stepMhz = (std::clamp)(stepMhz, PRESET_AGILE_SCAN_MIN_STEP_MHZ, PRESET_AGILE_SCAN_MAX_STEP_MHZ);
    const QStringList tokens = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        const int dashIndex = token.indexOf(QLatin1Char('-'));
        bool startOk = false;
        bool endOk = false;
        double startMhz = 0.0;
        double endMhz = 0.0;

        if (dashIndex > 0) {
            startMhz = token.left(dashIndex).toDouble(&startOk);
            endMhz = token.mid(dashIndex + 1).toDouble(&endOk);
        } else {
            startMhz = token.toDouble(&startOk);
            endMhz = startMhz;
            endOk = startOk;
        }

        if (!startOk || !endOk || startMhz <= 0.0 || endMhz <= 0.0) {
            if (error) {
                *error = QStringLiteral("Bad scan range: %1").arg(token);
            }
            frequencies.clear();
            return frequencies;
        }
        if (endMhz < startMhz) {
            std::swap(startMhz, endMhz);
        }
        const double startHz = startMhz * 1000000.0;
        const double endHz = endMhz * 1000000.0;
        if (startHz < PRESET_RF_MIN_CENTER_FREQUENCY ||
            endHz > PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY) {
            if (error) {
                *error = QStringLiteral("Scan uses RF input only: %1 MHz to %2 MHz")
                             .arg(PRESET_RF_MIN_CENTER_FREQUENCY / 1000000.0, 0, 'f', 0)
                             .arg(PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY / 1000000.0, 0, 'f', 0);
            }
            frequencies.clear();
            return frequencies;
        }

        const int countBefore = frequencies.size();
        for (double mhz = startMhz; mhz <= endMhz + stepMhz * 0.25; mhz += stepMhz) {
            frequencies.push_back(mhz * 1000000.0);
            if (frequencies.size() > PRESET_AGILE_SCAN_MAX_POINTS) {
                if (error) {
                    *error = QStringLiteral("Too many scan points (%1 max). Increase step or split presets.")
                                 .arg(PRESET_AGILE_SCAN_MAX_POINTS);
                }
                frequencies.clear();
                return frequencies;
            }
        }
        if (frequencies.size() == countBefore) {
            frequencies.push_back(startMhz * 1000000.0);
        }
    }

    std::sort(frequencies.begin(), frequencies.end());
    auto last = std::unique(frequencies.begin(), frequencies.end(), [](double a, double b) {
        return std::abs(a - b) < 0.5;
    });
    frequencies.erase(last, frequencies.end());

    if (frequencies.size() < PRESET_AGILE_SCAN_MIN_POINTS) {
        if (error) {
            *error = QStringLiteral("Agile scan needs at least two frequencies");
        }
        frequencies.clear();
    }
    return frequencies;
}

QString formatMhzList(const QVector<double> &frequenciesHz) {
    QStringList parts;
    parts.reserve(frequenciesHz.size());
    for (const double frequencyHz : frequenciesHz) {
        QString text = QString::number(frequencyHz / 1000000.0, 'f', 6);
        while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0'))) {
            text.chop(1);
        }
        if (text.endsWith(QLatin1Char('.'))) {
            text.chop(1);
        }
        parts << text;
    }
    return parts.join(QStringLiteral(", "));
}

QVector<double> parseStandardScanCentersMhz(const QString &centersMhz,
                                            double sampleRateHz,
                                            int minimumPoints,
                                            QString *error,
                                            bool *adjusted) {
    if (adjusted) {
        *adjusted = false;
    }
    QVector<double> frequencies;
    QString text = centersMhz.trimmed();
    text.replace(QChar(0x2013), QLatin1Char('-'));
    text.replace(QChar(0x2014), QLatin1Char('-'));
    text.replace(QLatin1Char('\\'), QLatin1Char(','));
    text.replace(QLatin1Char('/'), QLatin1Char(','));
    text.replace(QLatin1Char(';'), QLatin1Char(','));
    text.replace(QRegularExpression(QStringLiteral("mhz|мгц|РјРіС†"),
                                    QRegularExpression::CaseInsensitiveOption),
                 QString());
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(","));

    if (text.isEmpty()) {
        if (minimumPoints > 0 && error) {
            *error = QStringLiteral("Standard scan centers are empty");
        }
        return frequencies;
    }

    const QStringList tokens = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        bool ok = false;
        const double mhz = token.toDouble(&ok);
        const double frequencyHz = mhz * 1000000.0;
        if (!ok ||
            !std::isfinite(frequencyHz) ||
            frequencyHz < PRESET_RF_MIN_CENTER_FREQUENCY ||
            frequencyHz > PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY) {
            if (error) {
                *error = QStringLiteral("Standard scan uses RF centers only: %1 MHz to %2 MHz")
                             .arg(PRESET_RF_MIN_CENTER_FREQUENCY / 1000000.0, 0, 'f', 0)
                             .arg(PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY / 1000000.0, 0, 'f', 0);
            }
            frequencies.clear();
            return frequencies;
        }
        frequencies.push_back(frequencyHz);
    }

    std::sort(frequencies.begin(), frequencies.end());
    if (std::isfinite(sampleRateHz) && sampleRateHz > 0.0 && frequencies.size() > 1) {
        const double minimumStepHz = (std::max)(1.0, sampleRateHz);
        for (int i = 1; i < frequencies.size(); ++i) {
            const double minimumFrequency = frequencies.at(i - 1) + minimumStepHz;
            if (frequencies.at(i) < minimumFrequency - 0.5) {
                frequencies[i] = minimumFrequency;
                if (adjusted) {
                    *adjusted = true;
                }
            }
            if (frequencies.at(i) > PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY) {
                if (error) {
                    *error = QStringLiteral("Standard scan list exceeds receiver range after sample-rate spacing");
                }
                frequencies.clear();
                return frequencies;
            }
        }
    }

    if (frequencies.size() < minimumPoints) {
        if (error) {
            *error = QStringLiteral("Standard scan needs at least two centers");
        }
        frequencies.clear();
    }
    return frequencies;
}

QVector<double> parseListeningScanTargetsMhz(const QString &targetsMhz,
                                             double minimumHz,
                                             double maximumHz,
                                             int minimumPoints,
                                             QString *error) {
    QVector<double> frequencies;
    QString text = targetsMhz.trimmed();
    text.replace(QChar(0x2013), QLatin1Char('-'));
    text.replace(QChar(0x2014), QLatin1Char('-'));
    text.replace(QLatin1Char('\\'), QLatin1Char(','));
    text.replace(QLatin1Char('/'), QLatin1Char(','));
    text.replace(QLatin1Char(';'), QLatin1Char(','));
    text.replace(QRegularExpression(QStringLiteral("mhz|РјРіС†|Р СР С–РЎвЂ "),
                                    QRegularExpression::CaseInsensitiveOption),
                 QString());
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(","));

    if (text.isEmpty()) {
        if (minimumPoints > 0 && error) {
            *error = QStringLiteral("Listening scan targets are empty");
        }
        return frequencies;
    }

    if (!std::isfinite(minimumHz) || minimumHz < 0.0) {
        minimumHz = 0.0;
    }
    if (!std::isfinite(maximumHz) || maximumHz <= minimumHz) {
        maximumHz = PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY;
    }

    const QStringList tokens = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        bool ok = false;
        const double mhz = token.toDouble(&ok);
        const double frequencyHz = mhz * 1000000.0;
        if (!ok ||
            !std::isfinite(frequencyHz) ||
            frequencyHz < minimumHz ||
            frequencyHz > maximumHz) {
            if (error) {
                *error = QStringLiteral("Listening scan target out of range: %1 MHz to %2 MHz")
                             .arg(minimumHz / 1000000.0, 0, 'f', 3)
                             .arg(maximumHz / 1000000.0, 0, 'f', 3);
            }
            frequencies.clear();
            return frequencies;
        }
        bool duplicate = false;
        for (const double existingHz : std::as_const(frequencies)) {
            if (std::abs(existingHz - frequencyHz) < 0.5) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            frequencies.push_back(frequencyHz);
        }
    }

    if (frequencies.size() < minimumPoints) {
        if (error) {
            *error = QStringLiteral("Listening scan needs at least %1 target%2")
                         .arg(minimumPoints)
                         .arg(minimumPoints == 1 ? QString() : QStringLiteral("s"));
        }
        frequencies.clear();
    }
    return frequencies;
}
