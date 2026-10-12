#ifndef SIGNALUNITS_H
#define SIGNALUNITS_H

#include <QComboBox>
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace signalunits {
constexpr int MaximumSampleRate = 1000000000;
inline QString sampleRateText(int rate) {
    const double divisor = rate >= 1000000000 ? 1e9 : rate >= 1000000 ? 1e6 : 1e3;
    const char *unit = rate >= 1000000000 ? " Gsps" : rate >= 1000000 ? " Msps" : " ksps";
    return QString::number(rate / divisor, 'g', 10) + QLatin1String(unit);
}
// Bare numbers retain the legacy sps/Hz meaning; unit-bearing input is strict.
inline double parse(const QString &input, bool sampleRate) {
    QString value = input.trimmed().toLower();
    value.replace(QLatin1Char(','), QLatin1Char('.'));
    static const QRegularExpression expression(QStringLiteral(
        "^([0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)\\s*([a-z/]*|[\\x{0433}\\x{043A}\\x{043C}]?\\x{0433}\\x{0446})$"));
    const auto match = expression.match(value);
    if (!match.hasMatch()) return std::numeric_limits<double>::quiet_NaN();
    QString unit = match.captured(2);
    double multiplier = 1.0;
    if (!unit.isEmpty()) {
        if (unit.startsWith('k') || unit.startsWith(QChar(0x043a))) { multiplier = 1e3; unit.remove(0, 1); }
        else if (unit.startsWith('m') || unit.startsWith(QChar(0x043c))) { multiplier = 1e6; unit.remove(0, 1); }
        else if (unit.startsWith('g') || (unit.size() == 3 && unit.startsWith(QChar(0x0433)))) { multiplier = 1e9; unit.remove(0, 1); }
        const bool valid = sampleRate ? (unit == "sps" || unit == "s/s" || unit == "sa/s")
                                      : (unit == "hz" || unit == QString(QChar(0x0433)) + QChar(0x0446));
        if (!valid) return std::numeric_limits<double>::quiet_NaN();
    }
    return match.captured(1).toDouble() * multiplier;
}
inline double comboValue(const QComboBox *combo, bool sampleRate) {
    const int index = combo->currentIndex();
    if (index >= 0 && combo->currentText() == combo->itemText(index))
        return combo->itemData(index).toDouble();
    return parse(combo->currentText(), sampleRate);
}
inline int sampleRateValue(const QComboBox *combo) {
    const double rate = comboValue(combo, true);
    if (!std::isfinite(rate) || rate < 8000 || rate > MaximumSampleRate || std::abs(rate-std::round(rate)) > 1e-6) return 0;
    return static_cast<int>(std::round(rate));
}
}
#endif
