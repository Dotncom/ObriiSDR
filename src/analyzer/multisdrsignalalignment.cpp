#include "multisdrsignalalignment.h"

#include "fftplannermutex.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int TemplateSamples = 4096;
constexpr int SearchSamples = 12288;
constexpr int TargetSamples = TemplateSamples + 2 * SearchSamples;
constexpr int HistorySamples = 65536;
constexpr int CorrelationFft = 32768;
static_assert(CorrelationFft >= TargetSamples + TemplateSamples - 1);

double power(const float *iq, int index) {
    return double(iq[2 * index]) * iq[2 * index] + double(iq[2 * index + 1]) * iq[2 * index + 1];
}
}

void MultiSdrSignalAlignment::Source::discard() {
    channelizer.reset();
    iq.clear();
}

void MultiSdrSignalAlignment::discardHistory() {
    for (auto &state : states) state.discard();
}

bool MultiSdrSignalAlignment::update(const std::vector<DspSourceIdentity> &sources,
                                    bool &received, bool &discontinuity, QString &error) {
    received = false;
    discontinuity = false;
    if (sources.size() != states.size()) {
        error = QStringLiteral("Signal alignment requires exactly two Fobos receivers");
        return false;
    }
    auto &registry = DspIqSourceRegistry::instance();
    for (std::size_t i = 0; i < states.size(); ++i) {
        auto &state = states[i];
        identities[i] = sources[i];
        DspIqSourceSnapshot raw;
        if (!registry.snapshotSince(sources[i], state.epoch, state.endFloat, 2 * 1024 * 1024, raw, false))
            continue;
        received = true;
        if (raw.nativeAdcBits != 14 || !raw.sampleClockValid || raw.sampleRateHz != InputRate) {
            error = QStringLiteral("Signal alignment needs two Fobos RF streams at 8 MS/s");
            return false;
        }
        if (state.continuity && (raw.continuityId != state.continuity ||
                                 raw.startComplexSample != state.endRaw || raw.centerHz != state.center)) {
            state.discard();
            discontinuity = true;
        }
        state.epoch = raw.epoch;
        state.endFloat = raw.endFloatCount;
        state.endRaw = raw.endComplexSample;
        state.continuity = raw.continuityId;
        state.center = raw.centerHz;
        RadioSettings settings;
        settings.inputMode = INPUT_RF;
        settings.sampleRate = InputRate;
        settings.centerFrequency = settings.listeningFrequency = state.center;
        IqChannelizer::Options options;
        options.outputRateHz = OutputRate;
        options.cutoffHz = 47000;
        options.decimationFactor = 64;
        std::vector<float> channel;
        const auto result = state.channelizer.processFloatIq(raw.iq.data(), raw.iq.size(), settings, channel, false, &options);
        if (!result.valid || result.outputRate != OutputRate) {
            error = QStringLiteral("Could not channelize the Fobos IQ");
            return false;
        }
        state.end += channel.size() / 2;
        state.iq.insert(state.iq.end(), channel.begin(), channel.end());
        if (state.iq.size() > 2 * HistorySamples)
            state.iq.erase(state.iq.begin(), state.iq.end() - 2 * HistorySamples);
    }
    return true;
}

bool MultiSdrSignalAlignment::findMatch(const float *reference, const float *target, int &position) {
    if (!correlation.ready()) {
        if (!correlation.allocateBuffers(CorrelationFft, FftTransformCore::Storage::OutOfPlace))
            return false;
        std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
        if (!correlation.createPlanLocked()) return false;
        referenceBins.resize(2 * CorrelationFft);
    }
    correlation.stageInputRange(reference, 0, TemplateSamples);
    correlation.zeroInputTail(TemplateSamples);
    correlation.execute();
    const auto spectrum = correlation.output();
    for (int i = 0; i < CorrelationFft; ++i) {
        referenceBins[2 * i] = spectrum[i][0];
        referenceBins[2 * i + 1] = spectrum[i][1];
    }
    correlation.stageInputRange(target, 0, TargetSamples);
    correlation.zeroInputTail(TargetSamples);
    correlation.execute();
    auto *input = correlation.input();
    for (int i = 0; i < CorrelationFft; ++i) {
        const float re = referenceBins[2 * i], im = referenceBins[2 * i + 1];
        // Conjugate target * conj(reference). A forward FFT then gives the
        // conjugate of the inverse transform, up to scale.
        input[i][0] = spectrum[i][0] * re + spectrum[i][1] * im;
        input[i][1] = spectrum[i][0] * im - spectrum[i][1] * re;
    }
    correlation.execute();
    double referencePower = 0, targetPower = 0;
    for (int i = 0; i < TemplateSamples; ++i) {
        referencePower += power(reference, i);
        targetPower += power(target, i);
    }
    if (!(referencePower > 0)) return false;
    std::vector<float> scores(2 * SearchSamples + 1);
    for (int i = 0; i <= 2 * SearchSamples; ++i) {
        const double norm = std::sqrt(referencePower * std::max(0.0, targetPower)) * CorrelationFft;
        if (norm > 0) scores[i] = float(std::hypot(double(spectrum[i][0]), double(spectrum[i][1])) / norm);
        if (i < 2 * SearchSamples)
            targetPower += power(target, i + TemplateSamples) - power(target, i);
    }
    const auto peak = std::max_element(scores.begin(), scores.end());
    position = int(peak - scores.begin());
    if (!std::isfinite(*peak) || *peak < 0.55f || position < 4 || position > 2 * SearchSamples - 4)
        return false;
    // An unmodulated carrier gives many equally good positions.
    for (int i = 0; i <= 2 * SearchSamples; ++i)
        if (std::abs(i - position) > 8 && scores[i] >= *peak - 0.06f) return false;
    return true;
}

bool MultiSdrSignalAlignment::align(bool tracking, QString &status) {
    if (states[0].iq.size() < 2 * TargetSamples || states[1].iq.size() < 2 * TargetSamples) {
        status = QStringLiteral("Waiting for two Fobos IQ streams");
        return false;
    }
    if (std::abs(states[0].center - states[1].center) > 0.5) {
        status = QStringLiteral("Set both receivers to the same center frequency");
        return false;
    }
    const auto reference = states[0].end - (TemplateSamples + SearchSamples);
    const auto target = states[1].end - TargetSamples;
    int match = 0;
    if (!findMatch(states[0].iq.data() + 2 * (reference - states[0].start()),
                   states[1].iq.data() + 2 * (target - states[1].start()), match)) {
        status = QStringLiteral("Waiting for a common signal with a unique correlation peak");
        return false;
    }
    if (!tracking) {
        anchors = {reference, target + match};
        return true;
    }
    const auto a = mapIndex(0, reference);
    const auto b = mapIndex(1, target + match);
    if (std::max(a, b) - std::min(a, b) <= 1) return true;
    status = QStringLiteral("Sample delay changed; reacquiring signal alignment");
    return false;
}

std::uint64_t MultiSdrSignalAlignment::mapIndex(std::size_t source, std::uint64_t index) const {
    // The bias covers all history preceding the chosen anchor.
    return index >= anchors[source] ? HistorySamples + (index - anchors[source])
                                    : HistorySamples - (anchors[source] - index);
}

void MultiSdrSignalAlignment::windows(std::vector<DspIqSourceSnapshot> &inputs) const {
    for (std::size_t i = 0; i < states.size(); ++i) {
        const auto &state = states[i];
        auto &input = inputs[i];
        input.source = identities[i];
        input.sampleRateHz = OutputRate;
        input.centerHz = states[0].center;
        input.sampleClockValid = true;
        input.continuityId = state.continuity;
        input.startComplexSample = mapIndex(i, state.start());
        input.endComplexSample = mapIndex(i, state.end);
        input.iq = state.iq;
    }
}
