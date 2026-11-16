#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <algorithm>
#include <cmath>
#include "fobosifselection.h"
#include "fobosruntimeif.h"

struct SpurMaskEntry {
    double offsetHz = 0.0;
    double widthHz = 0.0;
    float prominenceDb = 0.0f;
    int hits = 0;
};

// A mask belongs to one receiver configuration and one tuning neighbourhood.
// Never union masks from unrelated RF bands into a global baseband mask.
struct SpurCalibrationProfile {
    struct Point { double centerHz = 0.0; QVector<SpurMaskEntry> entries; };
    QJsonObject context;
    QVector<Point> points;
    double stepHz = 0.0;

    static bool ifFrameForContext(const QJsonObject &source, FobosRuntimeIf::Frame *frame) {
        if (!FobosRuntimeIf::HasFactoryPolicy) return false;
        const auto custom = source.value("globalIfCustomRulesActive");
        if (!custom.isUndefined() && !custom.isBool()) return false;
        if (!custom.toBool()) {
            *frame = FobosRuntimeIf::builtinFrame();
            return true;
        }
        // An empty custom table is valid and has CRC0. The explicit activation
        // marker distinguishes it from an older profile with no custom fields.
        const auto crc = source.value("globalIfCustomRulesCrc");
        if (source.value("globalIfPolicy").toInt() != static_cast<int>(FobosRuntimeIf::PolicyVersion) ||
            !crc.isDouble() || !source.value("globalIfCustomRules").isObject() ||
            !FobosRuntimeIf::frameFromJson(source.value("globalIfCustomRules").toObject(), frame)) return false;
        return crc.toDouble(-1) == static_cast<double>(FobosRuntimeIf::rulesCrc(*frame));
    }

    static QVector<double> sweepCenters(double minimum, double maximum, double sampleRate) {
        QVector<double> centers;
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(sampleRate) ||
            minimum < 50e6 || maximum < minimum || sampleRate < 1e6) return centers;
        const double step = sampleRate * 0.5;
        if ((maximum - minimum) / step > 20000) return centers;
        for (double hz = minimum; hz < maximum; hz += step) centers.append(hz);
        centers.append(maximum); // Include the upper limit even for a short last step.
        return centers;
    }

    bool compatibleWith(const QJsonObject &conditions) const {
        // Gain and FFT/window affect level/resolution, not the hardware spur grid.
        // Fs, clock, physical receiver, input and IF path still select different grids.
        static const char *keys[] = {"backend", "input", "serial", "hardware", "clock", "gpo", "correctedIf"};
        for (const char *key : keys) if (context.value(key) != conditions.value(key)) return false;
        if (context.value("globalIfMode").toInt() != conditions.value("globalIfMode").toInt()) return false;
        if (context.value("globalIfPolicy").toInt() != conditions.value("globalIfPolicy").toInt()) return false;
        if (context.value("mixerBiasProfile").toInt() != conditions.value("mixerBiasProfile").toInt()) return false;
        const auto custom = context.value("globalIfCustomRulesActive");
        const auto otherCustom = conditions.value("globalIfCustomRulesActive");
        if ((!custom.isUndefined() && !custom.isBool()) ||
            (!otherCustom.isUndefined() && !otherCustom.isBool()) || custom.toBool() != otherCustom.toBool()) return false;
        if (custom.toBool()) {
            FobosRuntimeIf::Frame ownFrame, otherFrame;
            if (!ifFrameForContext(context, &ownFrame) || !ifFrameForContext(conditions, &otherFrame) ||
                !FobosRuntimeIf::sameRules(ownFrame, otherFrame)) return false;
        }
        if (context.value("row3If2450").toBool() != conditions.value("row3If2450").toBool()) return false;
        if (context.value("row3AutoPolicy").toInt() != conditions.value("row3AutoPolicy").toInt()) return false;
        const double rate = context.value("rate").toDouble(context.value("sampleRate").toDouble());
        const double other = conditions.value("rate").toDouble(conditions.value("sampleRate").toDouble());
        return rate > 0 && std::isfinite(other) && std::abs(rate - other) <= rate * 1e-6;
    }

    const QVector<SpurMaskEntry> *maskAt(double center, const QJsonObject &conditions) const {
        if (!compatibleWith(conditions) || points.isEmpty() || !std::isfinite(center) ||
            center < points.front().centerHz || center > points.back().centerHz) return nullptr;
        auto upper = std::lower_bound(points.cbegin(), points.cend(), center,
            [](const Point &p, double hz) { return p.centerHz < hz; });
        if ((context.value("globalIfPolicy").toInt() > 0 || context.value("globalIfMode").toInt() > 0)) {
            const double rate = context.value("rate").toDouble(context.value("sampleRate").toDouble());
            FobosRuntimeIf::Frame actualFrame;
            if (!ifFrameForContext(context, &actualFrame)) return nullptr;
            const auto effectiveIf = [&](double frequency) {
                return FobosRuntimeIf::chooseEffectiveIf(actualFrame, frequency, rate,
                    context.value("correctedIf").toBool(), context.value("globalIfPolicy").toInt()>0 ? 3 : context.value("globalIfMode").toInt());
            };
            const unsigned int targetIf = effectiveIf(center);
            const Point *nearest = nullptr;
            double distance = stepHz * 0.5 + 1.0;
            const auto consider = [&](const Point &point) {
                const double candidateDistance = std::abs(point.centerHz - center);
                if (candidateDistance <= distance && effectiveIf(point.centerHz) == targetIf) {
                    nearest = &point;
                    distance = candidateDistance;
                }
            };
            const double maximum = stepHz * 0.5 + 1.0;
            for (auto p = upper; p != points.cend() && p->centerHz - center <= maximum; ++p) consider(*p);
            for (auto p = upper; p != points.cbegin();) {
                --p;
                if (center - p->centerHz > maximum) break;
                consider(*p);
            }
            return nearest ? &nearest->entries : nullptr;
        }
        if (upper == points.cend()) --upper;
        if (upper != points.cbegin() && center - (upper - 1)->centerHz < upper->centerHz - center) --upper;
        if (std::abs(upper->centerHz - center) > stepHz * 0.5 + 1.0) return nullptr;
        return &upper->entries;
    }

    QVector<SpurMaskEntry> maskFor(double center, const QJsonObject &conditions) const {
        const auto *nearest = maskAt(center, conditions);
        if (!nearest) return {};
        QVector<SpurMaskEntry> result = *nearest;
        auto right = std::lower_bound(points.cbegin(), points.cend(), center,
            [](const Point &p, double hz) { return p.centerHz < hz; });
        if (right == points.cbegin() || right == points.cend() || std::abs(right->centerHz - center) < 1) return result;
        const auto &left = *(right - 1);
        if ((context.value("globalIfPolicy").toInt() > 0 || context.value("globalIfMode").toInt() > 0)) {
            const double rate = context.value("rate").toDouble(context.value("sampleRate").toDouble());
            FobosRuntimeIf::Frame actualFrame;
            if (!ifFrameForContext(context, &actualFrame)) return {};
            const auto effectiveIf = [&](double frequency) {
                return FobosRuntimeIf::chooseEffectiveIf(actualFrame, frequency, rate,
                    context.value("correctedIf").toBool(), context.value("globalIfPolicy").toInt()>0 ? 3 : context.value("globalIfMode").toInt());
            };
            const unsigned int targetIf = effectiveIf(center);
            if (effectiveIf(left.centerHz) != targetIf || effectiveIf(right->centerHz) != targetIf) return result;
        }
        auto sameOffset = [](const SpurMaskEntry &e, const QVector<SpurMaskEntry> &entries) {
            return std::any_of(entries.cbegin(), entries.cend(), [&](const SpurMaskEntry &other) {
                return std::abs(e.offsetHz - other.offsetHz) <= (std::max)({100.0, e.widthHz, other.widthHz});
            });
        };
        // An RF-fixed spur changes baseband position when retuning. Add it only
        // when both overlapping windows agree, excluding baseband-fixed lines.
        QVector<bool> stableLeft, stableRight;
        for (const auto &a : left.entries) stableLeft.append(sameOffset(a, right->entries));
        for (const auto &b : right->entries) stableRight.append(sameOffset(b, left.entries));
        for (int i = 0; i < left.entries.size(); ++i) {
            const auto &a = left.entries.at(i);
            if (stableLeft.at(i)) continue;
            for (int j = 0; j < right->entries.size(); ++j) {
                const auto &b = right->entries.at(j);
                if (stableRight.at(j)) continue;
                const double tolerance = (std::max)({100.0, a.widthHz, b.widthHz});
                if (std::abs((left.centerHz + a.offsetHz) - (right->centerHz + b.offsetHz)) > tolerance) continue;
                SpurMaskEntry translated = a;
                translated.offsetHz = ((left.centerHz + a.offsetHz) + (right->centerHz + b.offsetHz)) * 0.5 - center;
                translated.widthHz = (std::max)(a.widthHz, b.widthHz);
                if (!sameOffset(translated, result)) result.append(translated);
                break;
            }
        }
        return result;
    }

    QJsonObject toJson() const {
        QJsonArray samples;
        for (const Point &p : points) {
            QJsonArray mask;
            for (const auto &e : p.entries) mask.append(QJsonObject{
                {"offsetHz", e.offsetHz}, {"widthHz", e.widthHz},
                {"prominenceDb", e.prominenceDb}, {"hits", e.hits}});
            samples.append(QJsonObject{{"centerHz", p.centerHz}, {"entries", mask}});
        }
        return {{"version", 2}, {"context", context}, {"stepHz", stepHz}, {"points", samples}};
    }

    static SpurCalibrationProfile fromJson(const QJsonObject &json) {
        SpurCalibrationProfile result;
        if (json.value("version").toInt() != 1 && json.value("version").toInt() != 2) return result;
        result.context = json.value("context").toObject();
        if (result.context.value("globalIfCustomRulesActive").toBool()) {
            FobosRuntimeIf::Frame frame;
            if (!ifFrameForContext(result.context, &frame)) return {};
        }
        result.stepHz = json.value("stepHz").toDouble();
        const auto samples = json.value("points").toArray();
        if (result.context.isEmpty() || !std::isfinite(result.stepHz) || result.stepHz <= 0 ||
            samples.isEmpty() || samples.size() > 20001) return {};
        for (const auto &value : samples) {
            const auto obj = value.toObject();
            Point p;
            p.centerHz = obj.value("centerHz").toDouble(-1);
            if (!std::isfinite(p.centerHz) || p.centerHz < 50e6 || p.centerHz > 10e9 ||
                (!result.points.isEmpty() && p.centerHz <= result.points.back().centerHz)) return {};
            const auto entries = obj.value("entries").toArray();
            if (entries.size() > 256) return {};
            for (const auto &entry : entries) {
                const auto e = entry.toObject();
                SpurMaskEntry m{e.value("offsetHz").toDouble(NAN), e.value("widthHz").toDouble(NAN),
                    static_cast<float>(e.value("prominenceDb").toDouble()), e.value("hits").toInt()};
                if (!std::isfinite(m.offsetHz) || !std::isfinite(m.widthHz) ||
                    !std::isfinite(m.prominenceDb) || m.widthHz < 1 || m.widthHz > 20000 ||
                    std::abs(m.offsetHz) > 100e6 || m.hits < 0) return {};
                p.entries.append(m);
            }
            result.points.append(p);
        }
        return result;
    }
};

struct SpurCalibrationFrameGate {
    quint64 epoch = 0;
    quint64 lastEnd = 0;
    double centerHz = 0.0;
    bool accepts(quint64 frameEpoch, quint64 end, double center, bool settled) {
        if (!settled || epoch == 0 || frameEpoch != epoch || end == 0 || end <= lastEnd ||
            !std::isfinite(center) || std::abs(center - centerHz) > 1.0) return false;
        lastEnd = end;
        return true;
    }
};
