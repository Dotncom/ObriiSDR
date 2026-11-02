#include "headlessresearchmetrics.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <numeric>
#include <utility>

namespace HeadlessResearchMetrics {
namespace {
bool reject(QString *error, const char *message) {
    if (error) *error = QString::fromLatin1(message);
    return false;
}
bool boundedLevels(const std::vector<float> &levels, std::size_t minimum) {
    if (levels.size() < minimum || levels.size() > MaximumSamples) return false;
    return std::all_of(levels.begin(), levels.end(), [](float v) {
        return std::isfinite(v) && v >= -200 && v <= 100;
    });
}
bool centeredIq(const std::vector<float> &iq, std::vector<float> *first, std::vector<float> *second,
                double *meanI, double *meanQ) {
    if (iq.size() < 2 || iq.size() % 2 || iq.size() > MaximumSamples * 2) return false;
    *meanI = *meanQ = 0;
    for (std::size_t i = 0; i < iq.size(); i += 2) {
        if (!std::isfinite(iq[i]) || !std::isfinite(iq[i + 1]) ||
            std::abs(double(iq[i])) > 1e12 || std::abs(double(iq[i + 1])) > 1e12) return false;
        *meanI += iq[i];
        *meanQ += iq[i + 1];
    }
    const std::size_t n = iq.size() / 2;
    *meanI /= n;
    *meanQ /= n;
    first->resize(n);
    second->resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        (*first)[i] = static_cast<float>(iq[2 * i] - *meanI);
        (*second)[i] = static_cast<float>(iq[2 * i + 1] - *meanQ);
    }
    return true;
}
float interpolated(const std::vector<float> &values, double position) {
    if (position <= 0) return values.front();
    if (position >= double(values.size() - 1)) return values.back();
    const auto left = static_cast<std::size_t>(position);
    const float fraction = static_cast<float>(position - left);
    return values[left] + (values[left + 1] - values[left]) * fraction;
}
}

bool DensityHistogram::configure(const DensityConfig &c, QString *error) {
    if (c.columns < 1 || c.columns > MaximumDensityColumns || c.historyFrames < 1 ||
        c.historyFrames > MaximumHistoryFrames || c.columns * c.historyFrames > MaximumHistoryCells ||
        !std::isfinite(c.retainPercent) || c.retainPercent < 0 || c.retainPercent > 100 ||
        !std::isfinite(c.minimumDb) || !std::isfinite(c.maximumDb) || c.minimumDb < -200 ||
        c.maximumDb > 100 || c.maximumDb <= c.minimumDb)
        return reject(error, "Invalid density configuration or history storage budget");
    config_ = c;
    histogram_.assign(static_cast<std::size_t>(c.columns * DensityRows), 0);
    contributions_.clear();
    firstHz_ = lastHz_ = 0;
    haveRange_ = false;
    configured_ = true;
    if (error) error->clear();
    return true;
}

bool DensityHistogram::append(const std::vector<float> &levels, double firstHz, double lastHz,
                              bool *rangeReset, QString *error) {
    if (!configured_ || !boundedLevels(levels, 2) || !std::isfinite(firstHz) ||
        !std::isfinite(lastHz) || std::abs(firstHz) > 1e15 || std::abs(lastHz) > 1e15 || lastHz <= firstHz)
        return reject(error, "Density requires configuration, 2..65536 finite levels and an ascending finite span");
    // SignalDensityWidget::appendSpectrumFrame: decay, evict the aged oldest
    // contribution, then interpolate levels in dB and add this frame's weights.
    const double span = lastHz - firstHz;
    const double tolerance = (std::max)(1.0, span * 1e-4);
    const bool reset = haveRange_ && (std::abs(firstHz - firstHz_) > tolerance || std::abs(lastHz - lastHz_) > tolerance);
    if (reset) {
        std::fill(histogram_.begin(), histogram_.end(), 0);
        contributions_.clear();
    }
    firstHz_ = firstHz;
    lastHz_ = lastHz;
    haveRange_ = true;
    if (rangeReset) *rangeReset = reset;
    const float retain = static_cast<float>(config_.retainPercent / 100);
    for (float &value : histogram_) value *= retain;
    if (static_cast<int>(contributions_.size()) >= config_.historyFrames) {
        const Contribution &old = contributions_.front();
        const float ageScale = std::pow(retain, static_cast<float>(config_.historyFrames));
        for (int x = 0; x < config_.columns; ++x) {
            float &bin = histogram_[static_cast<std::size_t>(old.rows[x] * config_.columns + x)];
            bin = (std::max)(0.0f, bin - old.weights[x] * ageScale);
        }
        contributions_.pop_front();
    }
    Contribution contribution;
    contribution.rows.resize(static_cast<std::size_t>(config_.columns));
    contribution.weights.resize(static_cast<std::size_t>(config_.columns));
    const double range = (std::max)(1.0, config_.maximumDb - config_.minimumDb);
    for (int x = 0; x < config_.columns; ++x) {
        const double source = config_.columns == 1 ? 0 : double(x) * (levels.size() - 1) / (config_.columns - 1);
        const auto left = static_cast<std::size_t>(source);
        const auto right = (std::min)(left + 1, levels.size() - 1);
        const float sampled = static_cast<float>(levels[left] + (levels[right] - levels[left]) * (source - left));
        const int row = (std::clamp)(static_cast<int>(std::lround((config_.maximumDb - sampled) * (DensityRows - 1) / range)), 0, DensityRows - 1);
        const float weight = config_.linearPower ? static_cast<float>(std::pow(10.0, (double(sampled) - config_.maximumDb) / 10)) : 1.0f;
        contribution.rows[x] = static_cast<std::uint16_t>(row);
        contribution.weights[x] = weight;
        histogram_[static_cast<std::size_t>(row * config_.columns + x)] += weight;
    }
    contributions_.push_back(std::move(contribution));
    if (error) error->clear();
    return true;
}

PulseResult analyzePulses(const std::vector<PulseSample> &samples, const PulseConfig &config) {
    PulseResult r;
    if (samples.size() < 3 || samples.size() > MaximumSamples || !std::isfinite(config.thresholdDb) ||
        std::abs(config.thresholdDb) > 300 || !std::isfinite(config.hysteresisDb) || config.hysteresisDb < 0 || config.hysteresisDb > 300) {
        r.error = QStringLiteral("Invalid pulse sample count or threshold/hysteresis");
        return r;
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].timeMs < 0 || samples[i].timeMs > 9007199254740991LL ||
            (i && samples[i].timeMs <= samples[i - 1].timeMs) || !std::isfinite(samples[i].levelDb) ||
            samples[i].levelDb < -200 || samples[i].levelDb > 100) {
            r.error = QStringLiteral("Pulse timestamps must be strictly increasing integer ms in [0,2^53-1]; levels -200..100 dB");
            return r;
        }
    }
    r.thresholdDb = config.thresholdDb;
    if (config.relativeToNoise) {
        std::vector<float> ordered;
        ordered.reserve(samples.size());
        for (const auto &s : samples) ordered.push_back(s.levelDb);
        const auto index = ordered.size() / 5;
        std::nth_element(ordered.begin(), ordered.begin() + index, ordered.end());
        r.thresholdDb += ordered[index];
    }
    r.releaseDb = r.thresholdDb - config.hysteresisDb;
    r.observedMs = double(samples.back().timeMs - samples.front().timeMs);
    bool active = false;
    std::size_t start = 0;
    // PulseAnalysisWidget::Impl::analyze: inclusive sampled crossings and 10/90%
    // of the dB excursion from release threshold, with a 12-observation lookback.
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (!active && samples[i].levelDb >= r.thresholdDb) {
            active = true;
            start = i;
        } else if (active && samples[i].levelDb <= r.releaseDb) {
            PulseEvent e;
            e.startMs = samples[start].timeMs;
            e.endMs = samples[i].timeMs;
            e.leftCensored = start == 0;
            std::size_t peak = start;
            for (std::size_t j = start; j <= i; ++j)
                if (samples[j].levelDb > samples[peak].levelDb) peak = j;
            e.peakDb = samples[peak].levelDb;
            const double low = r.releaseDb + (e.peakDb - r.releaseDb) * 0.1;
            const double high = r.releaseDb + (e.peakDb - r.releaseDb) * 0.9;
            std::size_t riseLow = start > 12 ? start - 12 : 0, riseHigh = peak;
            bool found = false;
            for (std::size_t j = riseLow; j <= peak; ++j) {
                if (!found && samples[j].levelDb >= low) { riseLow = j; found = true; }
                if (found && samples[j].levelDb >= high) { riseHigh = j; break; }
            }
            e.riseMs = double(samples[riseHigh].timeMs - samples[riseLow].timeMs);
            std::size_t fallHigh = peak, fallLow = i;
            found = false;
            for (std::size_t j = peak; j <= i; ++j) {
                if (!found && samples[j].levelDb <= high) { fallHigh = j; found = true; }
                if (found && samples[j].levelDb <= low) { fallLow = j; break; }
            }
            e.fallMs = double(samples[fallLow].timeMs - samples[fallHigh].timeMs);
            r.events.push_back(e);
            active = false;
        }
    }
    r.trailingActive = active; // GUI omits an unfinished final pulse; keep it explicit.
    double width = 0, rise = 0, fall = 0, pri = 0;
    for (std::size_t i = 0; i < r.events.size(); ++i) {
        width += double(r.events[i].endMs - r.events[i].startMs);
        rise += r.events[i].riseMs;
        fall += r.events[i].fallMs;
        if (i) pri += double(r.events[i].startMs - r.events[i - 1].startMs);
    }
    if (!r.events.empty()) {
        r.averageWidthMs = width / r.events.size();
        r.riseMs = rise / r.events.size();
        r.fallMs = fall / r.events.size();
    }
    r.dutyPercent = width * 100 / r.observedMs;
    if (r.events.size() >= 2) {
        r.averagePriMs = pri / (r.events.size() - 1);
        r.repetitionHz = 1000 / r.averagePriMs;
    }
    r.valid = true;
    return r;
}

IqViews buildIqViews(const std::vector<float> &iq, double rate, double symbolRate, double phase, int traces) {
    IqViews r;
    if (!std::isfinite(rate) || rate < 1e-9 || rate > 1e12 || !std::isfinite(symbolRate) || symbolRate <= 0 ||
        !std::isfinite(rate / symbolRate) || rate / symbolRate < 2 ||
        !std::isfinite(phase) || phase < 0 || phase > 1 || traces < 1 || traces > MaximumEyeTraces) {
        r.error = QStringLiteral("IQ views require finite rates, at least 2 samples/symbol, phase 0..1 and traces 1..128");
        return r;
    }
    std::vector<float> first, second;
    if (!centeredIq(iq, &first, &second, &r.dcI, &r.dcQ)) {
        r.error = QStringLiteral("IQ views require 1..65536 finite I/Q pairs with components in [-1e12,1e12]");
        return r;
    }
    r.samplesPerSymbol = rate / symbolRate;
    const double initial = phase * r.samplesPerSymbol;
    const auto point = [&](double position) { return IqPoint{position, interpolated(first, position), interpolated(second, position)}; };
    // ResearchAnalysisDialog 2D constellation: first 3000 symbol-phase samples.
    double position = initial;
    for (; position + 1 < first.size() && r.constellation.size() < MaximumConstellationPoints; position += r.samplesPerSymbol)
        r.constellation.push_back(point(position));
    r.constellationLimited = position + 1 < first.size();
    const double length = 2 * r.samplesPerSymbol;
    for (int trace = 0; trace < traces; ++trace) {
        const double start = initial + trace * r.samplesPerSymbol;
        if (start + length + 1 >= first.size()) break;
        for (int index = 0; index < EyePoints; ++index)
            r.eye.push_back(point(start + length * index / (EyePoints - 1)));
        ++r.eyeTraces;
    }
    r.eyeLimited = r.eyeTraces == traces && initial + traces * r.samplesPerSymbol + length + 1 < first.size();
    r.valid = true;
    return r;
}

IqPhaseResult analyzeIqPhase(const std::vector<float> &iq, double rate) {
    IqPhaseResult r;
    std::vector<float> first, second;
    double meanI = 0, meanQ = 0;
    if (!std::isfinite(rate) || rate < 1e-9 || rate > 1e12 || iq.size() < 4 ||
        !centeredIq(iq, &first, &second, &meanI, &meanQ)) {
        r.error = QStringLiteral("IQ phase requires 2..65536 finite I/Q pairs and a finite positive rate");
        return r;
    }
    double powerI = 0;
    for (std::size_t i = 0; i < first.size(); ++i) powerI += std::pow(double(iq[2 * i]) - meanI, 2);
    powerI /= first.size();
    r.autocorrelationValid = powerI > 1e-20;
    for (std::size_t lag = 0; lag < r.autocorrelation.size(); ++lag) {
        double sum = 0;
        int count = 0;
        for (std::size_t i = 0; i + lag < first.size(); i += 4) {
            sum += (iq[2 * i] - meanI) * (iq[2 * (i + lag)] - meanI);
            ++count;
        }
        r.autocorrelation[lag] = count && r.autocorrelationValid ? static_cast<float>(sum / count / powerI) : 0;
    }
    double stepSum = 0, stepSquared = 0, power = 0;
    std::complex<double> pseudo(0, 0);
    for (std::size_t i = 1; i < first.size(); ++i) {
        const std::complex<double> previous(iq[2 * (i - 1)] - meanI, iq[2 * (i - 1) + 1] - meanQ);
        const std::complex<double> current(iq[2 * i] - meanI, iq[2 * i + 1] - meanQ);
        const double step = std::arg(current * std::conj(previous));
        stepSum += step;
        stepSquared += step * step;
        pseudo += current * current;
        power += std::norm(current);
    }
    constexpr double twoPi = 6.28318530717958647692;
    const double meanStep = stepSum / (first.size() - 1);
    r.frequencyHz = meanStep * rate / twoPi;
    r.frequencyStdHz = std::sqrt((std::max)(0.0, stepSquared / (first.size() - 1) - meanStep * meanStep)) * rate / twoPi;
    r.frequencyValid = power > 1e-20;
    const double p = std::abs(pseudo);
    r.imageValid = power > p + 1e-20;
    r.imageSaturated = !r.imageValid && power > 1e-20;
    if (r.imageValid) r.imageEstimateDb = 10 * std::log10((power + p) / (power - p));
    r.valid = true;
    return r;
}

CombResult analyzeComb(const std::vector<float> &frequencies, const std::vector<float> &levels,
                       double prominence, double minimumSpacing, double maximumSpacing,
                       const std::vector<float> &reference) {
    CombResult r;
    if (!boundedLevels(levels, 5) || frequencies.size() != levels.size() || !std::isfinite(prominence) ||
        prominence < 0 || prominence > 300 || !std::isfinite(minimumSpacing) || minimumSpacing <= 0 ||
        !std::isfinite(maximumSpacing) || maximumSpacing < minimumSpacing || maximumSpacing > 2e15 ||
        (!reference.empty() && (reference.size() != levels.size() || !boundedLevels(reference, 5)))) {
        r.error = QStringLiteral("Invalid comb arrays, reference or prominence/spacing options");
        return r;
    }
    const double binHz = (double(frequencies.back()) - frequencies.front()) / (frequencies.size() - 1);
    if (!(binHz > 0)) { r.error = QStringLiteral("Comb axis must ascend"); return r; }
    const double axisTolerance = (std::max)(binHz * 1e-5, std::abs(double(frequencies.back())) * std::numeric_limits<float>::epsilon() * 2);
    for (std::size_t i = 0; i < frequencies.size(); ++i) {
        if (!std::isfinite(frequencies[i]) || std::abs(double(frequencies[i])) > 1e15 ||
            (i && frequencies[i] <= frequencies[i - 1]) ||
            std::abs(frequencies[i] - (frequencies.front() + i * binHz)) > axisTolerance) {
            r.error = QStringLiteral("Comb requires a finite ascending uniform axis");
            return r;
        }
    }
    auto ordered = levels;
    const auto noiseIndex = static_cast<std::size_t>(0.4 * (levels.size() - 1));
    std::nth_element(ordered.begin(), ordered.begin() + noiseIndex, ordered.end());
    r.noiseDb = ordered[noiseIndex];
    r.referenceValid = !reference.empty();
    // ResearchAnalysisDialog::updateInterference, including its asymmetric peak
    // tie rule, local four-neighbor baseline, 96-peak cap and three-family votes.
    for (std::size_t i = 2; i + 2 < levels.size(); ++i) {
        if (levels[i] < r.noiseDb + prominence || levels[i] < levels[i - 1] || levels[i] <= levels[i + 1]) continue;
        const float base = (levels[i - 2] + levels[i - 1] + levels[i + 1] + levels[i + 2]) * 0.25f;
        CombPeak p;
        p.bin = static_cast<int>(i);
        p.frequencyHz = frequencies[i];
        p.levelDb = levels[i];
        p.prominenceDb = levels[i] - (std::max)(r.noiseDb, base);
        if (r.referenceValid) p.referenceDeltaDb = levels[i] - reference[i];
        r.peaks.push_back(p);
    }
    std::sort(r.peaks.begin(), r.peaks.end(), [](const CombPeak &a, const CombPeak &b) { return a.prominenceDb > b.prominenceDb; });
    if (r.peaks.size() > 96) r.peaks.resize(96);
    std::sort(r.peaks.begin(), r.peaks.end(), [](const CombPeak &a, const CombPeak &b) { return a.frequencyHz < b.frequencyHz; });
    struct Vote { double sum = 0; int count = 0; };
    struct Family { double step = 0, anchor = 0; std::vector<int> members; };
    const double resolution = (std::max)(10.0, binHz * 2);
    const auto detect = [&](const std::vector<int> &available) {
        std::map<qint64, Vote> votes;
        for (std::size_t i = 0; i < available.size(); ++i) {
            for (std::size_t j = i + 1; j < available.size(); ++j) {
                const double difference = r.peaks[available[j]].frequencyHz - r.peaks[available[i]].frequencyHz;
                if (difference < minimumSpacing) continue;
                if (difference > maximumSpacing) break;
                auto &vote = votes[static_cast<qint64>(std::llround(difference / resolution))];
                vote.sum += difference;
                ++vote.count;
            }
        }
        Family best;
        for (const auto &entry : votes) {
            const double candidate = entry.second.sum / entry.second.count;
            const double tolerance = (std::max)(binHz * 3, candidate * 0.015);
            for (int anchor : available) {
                Family trial;
                trial.step = candidate;
                trial.anchor = r.peaks[anchor].frequencyHz;
                for (int index : available) {
                    const double f = r.peaks[index].frequencyHz;
                    const double harmonic = std::round((f - trial.anchor) / candidate);
                    if (std::abs(f - (trial.anchor + harmonic * candidate)) <= tolerance) trial.members.push_back(index);
                }
                if (trial.members.size() > best.members.size() || (trial.members.size() == best.members.size() &&
                    trial.members.size() >= 4 && (best.step <= 0 || trial.step < best.step))) best = std::move(trial);
            }
        }
        if (best.members.size() < 4) return Family{};
        return best;
    };
    std::vector<int> available(r.peaks.size());
    std::iota(available.begin(), available.end(), 0);
    for (int family = 1; family <= 3 && available.size() >= 4; ++family) {
        const auto found = detect(available);
        if (found.members.empty()) break;
        for (int index : found.members) {
            r.peaks[index].family = family;
            r.peaks[index].harmonic = static_cast<int>(std::llround((r.peaks[index].frequencyHz - found.anchor) / found.step));
        }
        available.erase(std::remove_if(available.begin(), available.end(), [&](int i) {
            return std::find(found.members.begin(), found.members.end(), i) != found.members.end();
        }), available.end());
        r.families.push_back({found.step, found.anchor, static_cast<int>(found.members.size())});
    }
    r.valid = true;
    return r;
}
}
