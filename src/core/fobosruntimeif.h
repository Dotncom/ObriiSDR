#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <cmath>
#include <cstdint>
#ifdef OBRII_LOCAL_FOBOS_IF_HEADER
#include OBRII_LOCAL_FOBOS_IF_HEADER
#else
inline constexpr unsigned OBRII_TABLE_WORDS = 128;
inline unsigned obriiIfBaseMhz(double, bool) { return 0; }
#endif

// Private local application helpers. This header describes only the negotiated
// wire frame and frequency rules; it contains no native firmware addresses.
namespace FobosRuntimeIf {
inline constexpr unsigned PolicyVersion = 2;
using Frame = QVector<quint32>;
struct Rule { quint64 fromHz = 0, toHz = 0; quint32 ifMhz = 2350; };

#ifdef OBRII_LOCAL_FOBOS_IF_HEADER
inline constexpr bool HasFactoryPolicy = true;
inline Frame builtinFrame() {
    Frame frame(static_cast<int>(OBRII_TABLE_WORDS), 0);
    obrii_table_builtin(frame.data());
    return frame;
}

inline bool validFrame(const Frame &frame, QString *error = nullptr) {
    const auto fail = [&](const char *message) {
        if (error) *error = QString::fromLatin1(message);
        return false;
    };
    if (frame.size() < 8 || frame.size() > static_cast<int>(OBRII_TABLE_WORDS))
        return fail("Invalid IF table frame length.");
    if (frame[2] > OBRII_MAX_IF_RULES || 8u + frame[2] * 5u > static_cast<quint32>(frame.size()))
        return fail("The IF table exceeds24 rules or is truncated.");
    if (!obrii_table_valid(frame.constData(), 32u + frame[2] * 20u))
        return fail("Invalid IF table header, checksum, interval order, IF value or RF-bypass overlap.");
    if (error) error->clear();
    return true;
}

inline quint32 rulesCrc(const Frame &frame) { return validFrame(frame) ? frame[3] : 0; }
inline quint32 frameBytes(const Frame &frame) { return validFrame(frame) ? 32u + frame[2] * 20u : 0; }

inline QVector<Rule> rules(const Frame &frame) {
    QVector<Rule> result;
    if (!validFrame(frame)) return result;
    result.reserve(static_cast<int>(frame[2]));
    for (quint32 index = 0; index < frame[2]; ++index) {
        const quint32 *row = frame.constData() + 8 + index * 5;
        result.append({obrii_u64(row), obrii_u64(row + 2), row[4]});
    }
    return result;
}

inline bool frameForRules(const QVector<Rule> &input, Frame *output, QString *error = nullptr) {
    if (!output || input.size() > static_cast<int>(OBRII_MAX_IF_RULES)) {
        if (error) *error = QStringLiteral("A table may contain at most24 rules.");
        return false;
    }
    Frame frame(static_cast<int>(OBRII_TABLE_WORDS), 0);
    frame[0] = OBRII_TABLE_MAGIC; frame[1] = 1; frame[2] = static_cast<quint32>(input.size());
    frame[4] = 2; frame[5] = 49000000; frame[6] = 51000000;
    for (int index = 0; index < input.size(); ++index) {
        quint32 *row = frame.data() + 8 + index * 5;
        obrii_put64(row, input[index].fromHz); obrii_put64(row + 2, input[index].toHz);
        row[4] = input[index].ifMhz;
    }
    frame[3] = obrii_crc32(frame.constData() + 8, frame[2] * 20u);
    if (!validFrame(frame, error)) return false;
    *output = frame;
    return true;
}

inline bool sameRules(const Frame &left, const Frame &right) {
    if (!validFrame(left) || !validFrame(right) || left[2] != right[2]) return false;
    const int words = 8 + static_cast<int>(left[2]) * 5;
    for (int index = 0; index < words; ++index)
        if (left[index] != right[index]) return false;
    return true;
}

inline QJsonObject frameToJson(const Frame &frame) {
    if (!validFrame(frame)) return {};
    QJsonArray array;
    for (const auto &rule : rules(frame))
        array.append(QJsonObject{{QStringLiteral("fromHz"), static_cast<double>(rule.fromHz)},
            {QStringLiteral("toHz"), static_cast<double>(rule.toHz)},
            {QStringLiteral("ifMHz"), static_cast<int>(rule.ifMhz)}});
    return {{QStringLiteral("schema"), 1}, {QStringLiteral("policy"), 2},
        {QStringLiteral("minRateHz"), 49000000}, {QStringLiteral("maxRateHz"), 51000000},
        {QStringLiteral("rules"), array}};
}

inline bool frameFromJson(const QJsonObject &object, Frame *output, QString *error = nullptr) {
    const auto fail = [&](const char *message) {
        if (error) *error = QString::fromLatin1(message);
        return false;
    };
    if (object.value(QStringLiteral("schema")).toInt(-1) != 1 ||
        object.value(QStringLiteral("policy")).toInt(-1) != 2 ||
        object.value(QStringLiteral("minRateHz")).toDouble(-1) != 49000000 ||
        object.value(QStringLiteral("maxRateHz")).toDouble(-1) != 51000000 ||
        !object.value(QStringLiteral("rules")).isArray()) return fail("Unknown IF rule JSON schema or sample-rate range.");
    const auto array = object.value(QStringLiteral("rules")).toArray();
    if (array.size() > static_cast<int>(OBRII_MAX_IF_RULES)) return fail("Too many IF rules.");
    QVector<Rule> result;
    for (const auto &value : array) {
        if (!value.isObject()) return fail("Each IF rule must be an object.");
        const auto row = value.toObject();
        const auto start = row.value(QStringLiteral("fromHz"));
        const auto end = row.value(QStringLiteral("toHz"));
        const auto frequency = row.value(QStringLiteral("ifMHz"));
        const double from = start.toDouble(-1), to = end.toDouble(-1), mhz = frequency.toDouble(-1);
        if (!start.isDouble() || !end.isDouble() || !frequency.isDouble() ||
            !std::isfinite(from) || !std::isfinite(to) || !std::isfinite(mhz) ||
            from < 50000000.0 || to > 6900000001.0 || from >= to ||
            std::floor(from) != from || std::floor(to) != to || (mhz != 2350 && mhz != 2450))
            return fail("IF frequencies and ordered interval boundaries must be valid integer Hz.");
        result.append({static_cast<quint64>(from), static_cast<quint64>(to), static_cast<quint32>(mhz)});
    }
    return frameForRules(result, output, error);
}

inline unsigned int chooseEffectiveIf(const Frame &frame, double centerHz, double rateHz,
    bool corrected, int mode) {
    if (!std::isfinite(centerHz) || centerHz < 50e6 || centerHz > 7750e6) return 0;
    const unsigned int base = obriiIfBaseMhz(centerHz, corrected);
    if (!base) return 0; // RF bypass remains independent of either table.
    if (mode == 1 || mode == 2) return mode == 1 ? 2350u : 2450u;
    if (mode != 3 || !obriiIfAutoRateEligible(rateHz)) return base;
    if (!validFrame(frame)) return obriiIfAutoMhz(centerHz, rateHz, corrected);
    return obrii_table_choose(frame.constData(), static_cast<quint64>(centerHz), base);
}
#else
inline constexpr bool HasFactoryPolicy = false;
inline Frame builtinFrame() { return {}; }
inline bool validFrame(const Frame &, QString *error = nullptr) {
    if (error) *error = QStringLiteral("The optional receiver IF policy is unavailable.");
    return false;
}
inline quint32 rulesCrc(const Frame &) { return 0; }
inline quint32 frameBytes(const Frame &) { return 0; }
inline QVector<Rule> rules(const Frame &) { return {}; }
inline bool frameForRules(const QVector<Rule> &, Frame *, QString *error = nullptr) { return validFrame({}, error); }
inline bool sameRules(const Frame &, const Frame &) { return false; }
inline QJsonObject frameToJson(const Frame &) { return {}; }
inline bool frameFromJson(const QJsonObject &, Frame *, QString *error = nullptr) { return validFrame({}, error); }
inline unsigned int chooseEffectiveIf(const Frame &, double, double, bool, int) { return 0; }
#endif
} // namespace FobosRuntimeIf
