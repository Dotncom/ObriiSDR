#include "gnssacquisition.h"

#include <fftw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <utility>

namespace {

constexpr double kGpsCaChipRate = 1023000.0;
constexpr double kAcquisitionSampleRate = 4092000.0;
constexpr int kChannelizerTaps = 33;
constexpr int kSamplesPerMs = 4092;
constexpr int kSamplesPerChip = 4;
constexpr int kPeakExclusionSamples = kSamplesPerChip * 2;
constexpr int kGpsCaChips = 1023;
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr int kTopCandidateCount = 8;
constexpr int kDopplerMinHz = -50000;
constexpr int kDopplerMaxHz = 50000;
constexpr int kDopplerStepHz = 1000;
constexpr int kDopplerMinStepHz = 250;
constexpr int kMaxCoherentMs = 1200;
constexpr double kToneNotchThreshold = 0.0025;
constexpr double kGpsL1FrequencyHz = 1575420000.0;

struct FftwPlanScope {
    explicit FftwPlanScope(fftwf_plan plan = nullptr)
        : plan(plan) {
    }

    ~FftwPlanScope() {
        if (plan) {
            fftwf_destroy_plan(plan);
        }
    }

    FftwPlanScope(const FftwPlanScope &) = delete;
    FftwPlanScope &operator=(const FftwPlanScope &) = delete;

    fftwf_plan plan = nullptr;
};

using FftwBuffer = std::unique_ptr<fftwf_complex, decltype(&fftwf_free)>;

struct ToneNotchStats {
    int count = 0;
    double strongestDb = 0.0;
};

struct AcquisitionCodeRow {
    int id = 0;
    QString label;
    double targetFrequency = 0.0;
    std::vector<float> code;
};

FftwBuffer makeFftwBuffer(int count) {
    return FftwBuffer(static_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) *
                                                               static_cast<std::size_t>(count))),
                      &fftwf_free);
}

const std::array<std::array<int, 2>, 32> &gpsCaG2Taps() {
    static const std::array<std::array<int, 2>, 32> taps = {{
        {{2, 6}}, {{3, 7}}, {{4, 8}}, {{5, 9}},
        {{1, 9}}, {{2, 10}}, {{1, 8}}, {{2, 9}},
        {{3, 10}}, {{2, 3}}, {{3, 4}}, {{5, 6}},
        {{6, 7}}, {{7, 8}}, {{8, 9}}, {{9, 10}},
        {{1, 4}}, {{2, 5}}, {{3, 6}}, {{4, 7}},
        {{5, 8}}, {{6, 9}}, {{1, 3}}, {{4, 6}},
        {{5, 7}}, {{6, 8}}, {{7, 9}}, {{8, 10}},
        {{1, 6}}, {{2, 7}}, {{3, 8}}, {{4, 9}}
    }};
    return taps;
}

std::vector<float> gpsCaChips(int prn) {
    std::vector<int> g1(10, 1);
    std::vector<int> g2(10, 1);
    std::vector<float> chips(kGpsCaChips);

    const auto taps = gpsCaG2Taps()[static_cast<std::size_t>(prn - 1)];
    for (int chip = 0; chip < kGpsCaChips; ++chip) {
        const int g1Out = g1[9];
        const int g2Out = g2[taps[0] - 1] ^ g2[taps[1] - 1];
        const int caBit = g1Out ^ g2Out;
        chips[static_cast<std::size_t>(chip)] = caBit ? -1.0f : 1.0f;

        const int g1Feedback = g1[2] ^ g1[9];
        const int g2Feedback = g2[1] ^ g2[2] ^ g2[5] ^ g2[7] ^ g2[8] ^ g2[9];
        for (int i = 9; i > 0; --i) {
            g1[static_cast<std::size_t>(i)] = g1[static_cast<std::size_t>(i - 1)];
            g2[static_cast<std::size_t>(i)] = g2[static_cast<std::size_t>(i - 1)];
        }
        g1[0] = g1Feedback;
        g2[0] = g2Feedback;
    }

    return chips;
}

std::vector<float> codeSamplesFromChips(const std::vector<float> &chips) {
    if (chips.empty()) {
        return {};
    }
    std::vector<float> sampled(kSamplesPerMs);
    for (int i = 0; i < kSamplesPerMs; ++i) {
        const int chipIndex = (std::min)(static_cast<int>(chips.size()) - 1,
                                         static_cast<int>((static_cast<long long>(i) *
                                                          static_cast<long long>(chips.size())) /
                                                          kSamplesPerMs));
        sampled[static_cast<std::size_t>(i)] = chips[static_cast<std::size_t>(chipIndex)];
    }
    return sampled;
}

std::vector<float> gpsCaCodeSamples(int prn) {
    return codeSamplesFromChips(gpsCaChips(prn));
}

std::vector<float> glonassL1OfChips() {
    constexpr int kGlonassCodeChips = 511;
    std::array<int, 9> reg = {{1, 1, 1, 1, 1, 1, 1, 1, 1}};
    std::vector<float> chips(kGlonassCodeChips);
    for (int chip = 0; chip < kGlonassCodeChips; ++chip) {
        chips[static_cast<std::size_t>(chip)] = reg[8] ? 1.0f : -1.0f;
        const int feedback = reg[4] ^ reg[8];
        for (int i = 8; i > 0; --i) {
            reg[static_cast<std::size_t>(i)] = reg[static_cast<std::size_t>(i - 1)];
        }
        reg[0] = feedback;
    }
    return chips;
}

QVector<AcquisitionCodeRow> gpsL1CaRows(double targetFrequency) {
    QVector<AcquisitionCodeRow> rows;
    rows.reserve(32);
    for (int prn = 1; prn <= 32; ++prn) {
        AcquisitionCodeRow row;
        row.id = prn;
        row.label = QStringLiteral("G%1").arg(prn, 2, 10, QLatin1Char('0'));
        row.targetFrequency = targetFrequency;
        row.code = gpsCaCodeSamples(prn);
        rows.append(std::move(row));
    }
    return rows;
}

QVector<AcquisitionCodeRow> glonassL1OfRows() {
    QVector<AcquisitionCodeRow> rows;
    rows.reserve(14);
    const std::vector<float> code = codeSamplesFromChips(glonassL1OfChips());
    for (int channel = -7; channel <= 6; ++channel) {
        AcquisitionCodeRow row;
        row.id = channel + 8;
        row.label = QStringLiteral("R%1").arg(channel >= 0
                                                  ? QStringLiteral("+%1").arg(channel)
                                                  : QString::number(channel));
        row.targetFrequency = 1602000000.0 + static_cast<double>(channel) * 562500.0;
        row.code = code;
        rows.append(std::move(row));
    }
    return rows;
}

QVector<AcquisitionCodeRow> gpsGlonassL1Rows(double gpsTargetFrequency) {
    QVector<AcquisitionCodeRow> rows = gpsL1CaRows(gpsTargetFrequency);
    const QVector<AcquisitionCodeRow> glonassRows = glonassL1OfRows();
    rows.reserve(rows.size() + glonassRows.size());
    for (const AcquisitionCodeRow &row : glonassRows) {
        rows.append(row);
    }
    return rows;
}

double sinc(double x) {
    if (std::abs(x) < 1.0e-12) {
        return 1.0;
    }
    const double pix = kPi * x;
    return std::sin(pix) / pix;
}

bool resampleAndShift(const std::vector<float> &interleavedIq,
                      double inputSampleRate,
                      double frequencyOffset,
                      double channelFilterCutoffHz,
                      int coherentMs,
                      std::vector<std::complex<float>> *out,
                      int *usedInputSamples) {
    if (!out || coherentMs <= 0 || inputSampleRate <= 0.0 || !std::isfinite(inputSampleRate)) {
        return false;
    }

    const int inputSamples = static_cast<int>(interleavedIq.size() / 2);
    const int outputSamples = coherentMs * kSamplesPerMs;
    const double ratio = inputSampleRate / kAcquisitionSampleRate;
    if (inputSamples <= 0 || outputSamples <= 0 || ratio <= 0.0 || !std::isfinite(ratio)) {
        return false;
    }

    out->assign(static_cast<std::size_t>(outputSamples), std::complex<float>(0.0f, 0.0f));
    double meanRe = 0.0;
    double meanIm = 0.0;
    int lastInput = 0;
    const int halfTaps = kChannelizerTaps / 2;
    const double requestedCutoffHz = std::isfinite(channelFilterCutoffHz) && channelFilterCutoffHz > 0.0
                                         ? channelFilterCutoffHz
                                         : 1800000.0;
    const double cutoffHz = (std::min)(requestedCutoffHz,
                                       kAcquisitionSampleRate * 0.45);
    const double normalizedCutoff = (std::min)(0.49, cutoffHz / inputSampleRate);

    for (int outIndex = 0; outIndex < outputSamples; ++outIndex) {
        const double inputCenter = static_cast<double>(outIndex) * ratio;
        const int centerIndex = static_cast<int>(std::floor(inputCenter));
        double re = 0.0;
        double im = 0.0;
        double weightSum = 0.0;

        for (int tap = -halfTaps; tap <= halfTaps; ++tap) {
            const int inputIndex = centerIndex + tap;
            if (inputIndex < 0 || inputIndex >= inputSamples) {
                continue;
            }
            const double distance = static_cast<double>(inputIndex) - inputCenter;
            const double window = 0.54 + 0.46 *
                                  std::cos(kTwoPi * static_cast<double>(tap) /
                                           static_cast<double>(kChannelizerTaps - 1));
            const double weight = 2.0 * normalizedCutoff *
                                  sinc(2.0 * normalizedCutoff * distance) *
                                  window;
            const int offset = inputIndex * 2;
            const double iSample = static_cast<double>(interleavedIq[static_cast<std::size_t>(offset)]);
            const double qSample = static_cast<double>(interleavedIq[static_cast<std::size_t>(offset + 1)]);
            if (!std::isfinite(iSample) || !std::isfinite(qSample)) {
                continue;
            }

            const double phase = kTwoPi * frequencyOffset *
                                 (static_cast<double>(inputIndex) / inputSampleRate);
            const double c = std::cos(phase);
            const double s = std::sin(phase);
            re += (iSample * c + qSample * s) * weight;
            im += (qSample * c - iSample * s) * weight;
            weightSum += weight;
        }

        if (std::abs(weightSum) > std::numeric_limits<double>::epsilon()) {
            re /= weightSum;
            im /= weightSum;
        }
        (*out)[static_cast<std::size_t>(outIndex)] =
            std::complex<float>(static_cast<float>(re), static_cast<float>(im));
        meanRe += re;
        meanIm += im;
        lastInput = (std::max)(lastInput,
                               (std::min)(inputSamples,
                                          centerIndex + halfTaps + 1));
    }

    if (outputSamples > 0) {
        meanRe /= static_cast<double>(outputSamples);
        meanIm /= static_cast<double>(outputSamples);
        const std::complex<float> mean(static_cast<float>(meanRe), static_cast<float>(meanIm));
        for (std::complex<float> &sample : *out) {
            sample -= mean;
        }
    }

    for (int ms = 0; ms < coherentMs; ++ms) {
        const int chunkOffset = ms * kSamplesPerMs;
        std::complex<double> chunkMean(0.0, 0.0);
        for (int i = 0; i < kSamplesPerMs; ++i) {
            const std::complex<float> sample = (*out)[static_cast<std::size_t>(chunkOffset + i)];
            chunkMean += std::complex<double>(sample.real(), sample.imag());
        }
        chunkMean /= static_cast<double>(kSamplesPerMs);

        double power = 0.0;
        for (int i = 0; i < kSamplesPerMs; ++i) {
            std::complex<float> &sample = (*out)[static_cast<std::size_t>(chunkOffset + i)];
            sample -= std::complex<float>(static_cast<float>(chunkMean.real()),
                                          static_cast<float>(chunkMean.imag()));
            power += static_cast<double>(std::norm(sample));
        }
        const double rms = std::sqrt(power / static_cast<double>(kSamplesPerMs));
        if (std::isfinite(rms) && rms > std::numeric_limits<double>::epsilon()) {
            const float gain = static_cast<float>(1.0 / rms);
            for (int i = 0; i < kSamplesPerMs; ++i) {
                (*out)[static_cast<std::size_t>(chunkOffset + i)] *= gain;
            }
        }
    }

    if (usedInputSamples) {
        *usedInputSamples = lastInput;
    }
    return true;
}

ToneNotchStats suppressKnownGnssTones(std::vector<std::complex<float>> *samples) {
    ToneNotchStats stats;
    if (!samples || samples->empty()) {
        return stats;
    }

    double totalPower = 0.0;
    for (const std::complex<float> &sample : *samples) {
        totalPower += static_cast<double>(std::norm(sample));
    }
    const double averagePower = totalPower / static_cast<double>(samples->size());
    if (!std::isfinite(averagePower) || averagePower <= std::numeric_limits<double>::epsilon()) {
        return stats;
    }

    // These are the stable narrow lines seen on inexpensive RTL/GNSS setups in the live IQ monitor.
    // They are removed only when their estimated tone power is clearly above the local average.
    constexpr std::array<double, 7> kKnownToneOffsetsHz = {
        0.0,
        340000.0,
        370000.0,
        -430000.0,
        -1230000.0,
        -1250000.0,
        -1600000.0
    };

    for (const double toneHz : kKnownToneOffsetsHz) {
        if (std::abs(toneHz) >= kAcquisitionSampleRate * 0.49) {
            continue;
        }

        const double analysisStep = -kTwoPi * toneHz / kAcquisitionSampleRate;
        const std::complex<double> analysisRot(std::cos(analysisStep),
                                               std::sin(analysisStep));
        std::complex<double> analysisOsc(1.0, 0.0);
        std::complex<double> sum(0.0, 0.0);
        for (const std::complex<float> &sample : *samples) {
            sum += std::complex<double>(sample.real(), sample.imag()) * analysisOsc;
            analysisOsc *= analysisRot;
        }

        const std::complex<double> amplitude =
            sum / static_cast<double>(samples->size());
        const double relativePower = std::norm(amplitude) / averagePower;
        if (!std::isfinite(relativePower) || relativePower < kToneNotchThreshold) {
            continue;
        }

        const double synthesisStep = kTwoPi * toneHz / kAcquisitionSampleRate;
        const std::complex<double> synthesisRot(std::cos(synthesisStep),
                                                std::sin(synthesisStep));
        std::complex<double> synthesisOsc(1.0, 0.0);
        for (std::complex<float> &sample : *samples) {
            const std::complex<double> cleaned =
                std::complex<double>(sample.real(), sample.imag()) -
                amplitude * synthesisOsc;
            sample = std::complex<float>(static_cast<float>(cleaned.real()),
                                         static_cast<float>(cleaned.imag()));
            synthesisOsc *= synthesisRot;
        }

        const double relativeDb =
            10.0 * std::log10((std::max)(relativePower,
                                         std::numeric_limits<double>::min()));
        stats.strongestDb = stats.count == 0 ? relativeDb
                                             : (std::max)(stats.strongestDb, relativeDb);
        ++stats.count;
    }

    return stats;
}

void insertCandidate(QVector<GnssAcquisitionCandidate> *candidates,
                     const GnssAcquisitionCandidate &candidate) {
    if (!candidates || candidate.prn <= 0 || candidate.metric <= 0.0 ||
        !std::isfinite(candidate.metric)) {
        return;
    }

    candidates->append(candidate);
    std::sort(candidates->begin(), candidates->end(), [](const auto &a, const auto &b) {
        return a.metric > b.metric;
    });
    while (candidates->size() > kTopCandidateCount) {
        candidates->removeLast();
    }
}

void buildPromptProbe(const std::vector<std::complex<float>> &samples,
                      const std::vector<float> &code,
                      double dopplerHz,
                      int codePhase,
                      int coherentMs,
                      QVector<float> *promptI,
                      QVector<float> *promptQ,
                      QVector<float> *promptMagnitude,
                      QVector<int> *promptSigns1Ms,
                      QVector<int> *promptBitSigns20Ms,
                      double *promptResidualDopplerHz) {
    if (!promptI || !promptQ || !promptMagnitude || !promptSigns1Ms || !promptBitSigns20Ms ||
        code.size() != static_cast<std::size_t>(kSamplesPerMs) ||
        coherentMs <= 0 ||
        samples.size() < static_cast<std::size_t>(coherentMs * kSamplesPerMs)) {
        return;
    }

    promptI->clear();
    promptQ->clear();
    promptMagnitude->clear();
    promptSigns1Ms->clear();
    promptBitSigns20Ms->clear();
    promptI->reserve(coherentMs);
    promptQ->reserve(coherentMs);
    promptMagnitude->reserve(coherentMs);
    promptSigns1Ms->reserve(coherentMs);
    if (promptResidualDopplerHz) {
        *promptResidualDopplerHz = 0.0;
    }

    codePhase %= kSamplesPerMs;
    if (codePhase < 0) {
        codePhase += kSamplesPerMs;
    }

    std::vector<std::complex<double>> rawPrompt;
    rawPrompt.reserve(static_cast<std::size_t>(coherentMs));

    for (int ms = 0; ms < coherentMs; ++ms) {
        const int chunkOffset = ms * kSamplesPerMs;
        double sumRe = 0.0;
        double sumIm = 0.0;
        for (int i = 0; i < kSamplesPerMs; ++i) {
            const std::complex<float> sample =
                samples[static_cast<std::size_t>(chunkOffset + i)];
            const double phase =
                kTwoPi * dopplerHz *
                (static_cast<double>(chunkOffset + i) / kAcquisitionSampleRate);
            const double c = std::cos(phase);
            const double s = std::sin(phase);
            const double re = static_cast<double>(sample.real()) * c +
                              static_cast<double>(sample.imag()) * s;
            const double im = static_cast<double>(sample.imag()) * c -
                              static_cast<double>(sample.real()) * s;
            const int codeIndex = (i + codePhase) % kSamplesPerMs;
            const double codeValue = static_cast<double>(code[static_cast<std::size_t>(codeIndex)]);
            sumRe += re * codeValue;
            sumIm += im * codeValue;
        }

        rawPrompt.emplace_back(sumRe / static_cast<double>(kSamplesPerMs),
                               sumIm / static_cast<double>(kSamplesPerMs));
    }

    std::complex<double> phaseStepSum(0.0, 0.0);
    for (std::size_t i = 1; i < rawPrompt.size(); ++i) {
        phaseStepSum += rawPrompt[i] * std::conj(rawPrompt[i - 1]);
    }
    const double phaseStep = std::abs(phaseStepSum) > std::numeric_limits<double>::epsilon()
                                 ? std::atan2(phaseStepSum.imag(), phaseStepSum.real())
                                 : 0.0;
    if (promptResidualDopplerHz) {
        *promptResidualDopplerHz = phaseStep / kTwoPi * 1000.0;
    }

    for (int ms = 0; ms < coherentMs; ++ms) {
        const double correctionPhase = -phaseStep * static_cast<double>(ms);
        const std::complex<double> correction(std::cos(correctionPhase),
                                              std::sin(correctionPhase));
        const std::complex<double> corrected = rawPrompt[static_cast<std::size_t>(ms)] * correction;
        const float promptRe = static_cast<float>(corrected.real());
        const float promptIm = static_cast<float>(corrected.imag());
        promptI->push_back(promptRe);
        promptQ->push_back(promptIm);
        promptMagnitude->push_back(std::sqrt(promptRe * promptRe + promptIm * promptIm));
        promptSigns1Ms->push_back(promptRe >= 0.0f ? 1 : 0);
    }

    constexpr int kGpsLnavBitMs = 20;
    for (int offset = 0; offset + kGpsLnavBitMs <= promptI->size(); offset += kGpsLnavBitMs) {
        double bitSum = 0.0;
        for (int i = 0; i < kGpsLnavBitMs; ++i) {
            bitSum += static_cast<double>(promptI->at(offset + i));
        }
        promptBitSigns20Ms->push_back(bitSum >= 0.0 ? 1 : 0);
    }
}

void buildTrackedPromptProbe(const std::vector<std::complex<float>> &samples,
                             const std::vector<float> &code,
                             double dopplerHz,
                             int codePhase,
                             int coherentMs,
                             GnssAcquisitionResult *result) {
    if (!result ||
        code.size() != static_cast<std::size_t>(kSamplesPerMs) ||
        coherentMs <= 0 ||
        samples.size() < static_cast<std::size_t>(coherentMs * kSamplesPerMs)) {
        return;
    }

    result->promptI.clear();
    result->promptQ.clear();
    result->promptMagnitude.clear();
    result->promptSigns1Ms.clear();
    result->promptBitSigns20Ms.clear();
    result->promptI.reserve(coherentMs);
    result->promptQ.reserve(coherentMs);
    result->promptMagnitude.reserve(coherentMs);
    result->promptSigns1Ms.reserve(coherentMs);

    codePhase %= kSamplesPerMs;
    if (codePhase < 0) {
        codePhase += kSamplesPerMs;
    }

    constexpr int kCodeSearchRadiusSamples = 10;
    constexpr double kCodeLoopGain = 0.35;

    std::vector<std::complex<double>> rawPrompt;
    rawPrompt.reserve(static_cast<std::size_t>(coherentMs));
    std::vector<int> trackedCodePhases;
    trackedCodePhases.reserve(static_cast<std::size_t>(coherentMs));
    double trackedCodePhase = static_cast<double>(codePhase);
    double promptMagnitudeSum = 0.0;

    for (int ms = 0; ms < coherentMs; ++ms) {
        const int chunkOffset = ms * kSamplesPerMs;
        const int predictedPhase = static_cast<int>(std::lround(trackedCodePhase));
        double bestMagnitude = -1.0;
        std::complex<double> bestPrompt(0.0, 0.0);
        int bestPhase = predictedPhase;

        for (int phaseOffset = -kCodeSearchRadiusSamples;
             phaseOffset <= kCodeSearchRadiusSamples;
             ++phaseOffset) {
            int testPhase = (predictedPhase + phaseOffset) % kSamplesPerMs;
            if (testPhase < 0) {
                testPhase += kSamplesPerMs;
            }

            double sumRe = 0.0;
            double sumIm = 0.0;
            for (int i = 0; i < kSamplesPerMs; ++i) {
                const std::complex<float> sample =
                    samples[static_cast<std::size_t>(chunkOffset + i)];
                const double phase =
                    kTwoPi * dopplerHz *
                    (static_cast<double>(chunkOffset + i) / kAcquisitionSampleRate);
                const double c = std::cos(phase);
                const double s = std::sin(phase);
                const double re = static_cast<double>(sample.real()) * c +
                                  static_cast<double>(sample.imag()) * s;
                const double im = static_cast<double>(sample.imag()) * c -
                                  static_cast<double>(sample.real()) * s;
                const int codeIndex = (i + testPhase) % kSamplesPerMs;
                const double codeValue =
                    static_cast<double>(code[static_cast<std::size_t>(codeIndex)]);
                sumRe += re * codeValue;
                sumIm += im * codeValue;
            }

            const std::complex<double> prompt(sumRe / static_cast<double>(kSamplesPerMs),
                                              sumIm / static_cast<double>(kSamplesPerMs));
            const double magnitude = std::abs(prompt);
            if (magnitude > bestMagnitude) {
                bestMagnitude = magnitude;
                bestPrompt = prompt;
                bestPhase = testPhase;
            }
        }

        int phaseError = bestPhase - predictedPhase;
        if (phaseError > kSamplesPerMs / 2) {
            phaseError -= kSamplesPerMs;
        } else if (phaseError < -kSamplesPerMs / 2) {
            phaseError += kSamplesPerMs;
        }
        trackedCodePhase += kCodeLoopGain * static_cast<double>(phaseError);
        while (trackedCodePhase >= static_cast<double>(kSamplesPerMs)) {
            trackedCodePhase -= static_cast<double>(kSamplesPerMs);
        }
        while (trackedCodePhase < 0.0) {
            trackedCodePhase += static_cast<double>(kSamplesPerMs);
        }

        rawPrompt.push_back(bestPrompt);
        trackedCodePhases.push_back(bestPhase);
        promptMagnitudeSum += (std::max)(0.0, bestMagnitude);
    }

    std::complex<double> squaredPhaseStepSum(0.0, 0.0);
    for (std::size_t i = 1; i < rawPrompt.size(); ++i) {
        const std::complex<double> current = rawPrompt[i] * rawPrompt[i];
        const std::complex<double> previous = rawPrompt[i - 1] * rawPrompt[i - 1];
        squaredPhaseStepSum += current * std::conj(previous);
    }
    const double phaseStep =
        std::abs(squaredPhaseStepSum) > std::numeric_limits<double>::epsilon()
            ? 0.5 * std::atan2(squaredPhaseStepSum.imag(), squaredPhaseStepSum.real())
            : 0.0;

    std::vector<std::complex<double>> carrierCorrected;
    carrierCorrected.reserve(rawPrompt.size());
    std::complex<double> squaredAxisSum(0.0, 0.0);
    for (std::size_t ms = 0; ms < rawPrompt.size(); ++ms) {
        const double correctionPhase = -phaseStep * static_cast<double>(ms);
        const std::complex<double> correction(std::cos(correctionPhase),
                                              std::sin(correctionPhase));
        const std::complex<double> corrected = rawPrompt[ms] * correction;
        carrierCorrected.push_back(corrected);
        squaredAxisSum += corrected * corrected;
    }

    const double axisPhase =
        std::abs(squaredAxisSum) > std::numeric_limits<double>::epsilon()
            ? 0.5 * std::atan2(squaredAxisSum.imag(), squaredAxisSum.real())
            : 0.0;
    const std::complex<double> axisCorrection(std::cos(-axisPhase),
                                              std::sin(-axisPhase));

    for (const std::complex<double> &prompt : carrierCorrected) {
        const std::complex<double> corrected = prompt * axisCorrection;
        const float promptRe = static_cast<float>(corrected.real());
        const float promptIm = static_cast<float>(corrected.imag());
        result->promptI.push_back(promptRe);
        result->promptQ.push_back(promptIm);
        result->promptMagnitude.push_back(std::sqrt(promptRe * promptRe + promptIm * promptIm));
        result->promptSigns1Ms.push_back(promptRe >= 0.0f ? 1 : 0);
    }

    constexpr int kGpsLnavBitMs = 20;
    for (int offset = 0; offset + kGpsLnavBitMs <= result->promptI.size(); offset += kGpsLnavBitMs) {
        double bitSum = 0.0;
        for (int i = 0; i < kGpsLnavBitMs; ++i) {
            bitSum += static_cast<double>(result->promptI.at(offset + i));
        }
        result->promptBitSigns20Ms.push_back(bitSum >= 0.0 ? 1 : 0);
    }

    result->trackingApplied = true;
    result->trackingMs = coherentMs;
    result->trackingStartCodePhase = trackedCodePhases.empty() ? codePhase : trackedCodePhases.front();
    result->trackingEndCodePhase = trackedCodePhases.empty() ? codePhase : trackedCodePhases.back();
    result->trackingCarrierPhaseStepRad = phaseStep;
    result->trackingCarrierResidualHz = phaseStep / kTwoPi * 1000.0;
    result->trackingAveragePromptMagnitude =
        coherentMs > 0 ? promptMagnitudeSum / static_cast<double>(coherentMs) : 0.0;
    result->promptResidualDopplerHz = result->trackingCarrierResidualHz;
}

void detectGpsLnavPreamble(GnssAcquisitionResult *result) {
    if (!result || result->promptBitSigns20Ms.size() < 8) {
        return;
    }

    constexpr std::array<int, 8> kGpsLnavPreamble = {{1, 0, 0, 0, 1, 0, 1, 1}};
    for (int offset = 0; offset + static_cast<int>(kGpsLnavPreamble.size()) <= result->promptBitSigns20Ms.size(); ++offset) {
        bool normal = true;
        bool inverted = true;
        for (int i = 0; i < static_cast<int>(kGpsLnavPreamble.size()); ++i) {
            const int bit = result->promptBitSigns20Ms.at(offset + i);
            const int expected = kGpsLnavPreamble[static_cast<std::size_t>(i)];
            normal = normal && bit == expected;
            inverted = inverted && bit == (1 - expected);
        }
        if (normal || inverted) {
            result->gpsLnavPreambleFound = true;
            result->gpsLnavPreambleBitOffset = offset;
            result->gpsLnavPreamblePolarity = normal ? 1 : -1;
            return;
        }
    }
}

bool isCancelRequested(const std::atomic_bool *cancelRequested) {
    return cancelRequested && cancelRequested->load(std::memory_order_relaxed);
}

void markCancelled(GnssAcquisitionResult *result) {
    if (!result) {
        return;
    }
    result->valid = false;
    result->cancelled = true;
    result->status = QStringLiteral("GPS C/A acquisition cancelled");
}

GnssAcquisitionResult acquireBpsk1MsRows(const std::vector<float> &interleavedIq,
                                         double inputSampleRate,
                                         double centerFrequency,
                                         const QVector<AcquisitionCodeRow> &rows,
                                         const QString &systemName,
                                         const QString &rowLabel,
                                         int maxCoherentMs,
                                         double channelFilterCutoffHz,
                                         const std::atomic_bool *cancelRequested,
                                         int dopplerMinHz,
                                         int dopplerMaxHz,
                                         int dopplerStepHz) {
    GnssAcquisitionResult result;
    result.inputSampleRate = inputSampleRate;
    result.acquisitionSampleRate = kAcquisitionSampleRate;
    result.centerFrequency = centerFrequency;
    result.targetFrequency = rows.isEmpty() ? 0.0 : rows.first().targetFrequency;
    result.frequencyOffset = result.targetFrequency - centerFrequency;
    result.channelFilterCutoffHz =
        (std::clamp)(std::isfinite(channelFilterCutoffHz) ? channelFilterCutoffHz : 1800000.0,
                     300000.0,
                     kAcquisitionSampleRate * 0.45);
    result.channelizerTaps = kChannelizerTaps;
    result.millisecondAgc = true;
    result.inputSamples = static_cast<int>(interleavedIq.size() / 2);
    result.heatmapRows = rows.size();
    result.systemName = systemName;
    result.heatmapRowLabel = rowLabel;
    if (!rows.isEmpty()) {
        result.heatmapFirstRowLabel = rows.first().label;
        result.heatmapLastRowLabel = rows.last().label;
    }

    if (isCancelRequested(cancelRequested)) {
        markCancelled(&result);
        return result;
    }

    if (rows.isEmpty()) {
        result.status = QStringLiteral("%1 acquisition has no code rows").arg(systemName);
        return result;
    }
    if (result.inputSamples <= 0) {
        result.status = QStringLiteral("No IQ samples");
        return result;
    }
    if (inputSampleRate <= 0.0 || !std::isfinite(inputSampleRate)) {
        result.status = QStringLiteral("Invalid IQ sample rate");
        return result;
    }
    if (centerFrequency <= 0.0 || !std::isfinite(centerFrequency)) {
        result.status = QStringLiteral("Invalid GNSS frequency");
        return result;
    }
    if (dopplerMinHz > dopplerMaxHz) {
        std::swap(dopplerMinHz, dopplerMaxHz);
    }
    dopplerMinHz = (std::clamp)(dopplerMinHz, kDopplerMinHz, kDopplerMaxHz);
    dopplerMaxHz = (std::clamp)(dopplerMaxHz, kDopplerMinHz, kDopplerMaxHz);
    dopplerStepHz = (std::clamp)(std::abs(dopplerStepHz), kDopplerMinStepHz, 10000);
    if (dopplerMinHz == dopplerMaxHz) {
        dopplerMaxHz = (std::min)(kDopplerMaxHz, dopplerMinHz + dopplerStepHz);
    }

    const double availableMs =
        (static_cast<double>(result.inputSamples) / inputSampleRate) * 1000.0;
    result.requestedCoherentMs = (std::clamp)(maxCoherentMs, 1, kMaxCoherentMs);
    const int coherentMs = (std::clamp)(static_cast<int>(std::floor(availableMs)),
                                       1,
                                       result.requestedCoherentMs);
    if (coherentMs <= 0) {
        result.status = QStringLiteral("Not enough IQ for one %1 millisecond").arg(systemName);
        return result;
    }

    std::vector<double> dopplerBins;
    dopplerBins.reserve(static_cast<std::size_t>((dopplerMaxHz - dopplerMinHz) / dopplerStepHz + 1));
    for (int dopplerHz = dopplerMinHz; dopplerHz <= dopplerMaxHz; dopplerHz += dopplerStepHz) {
        dopplerBins.push_back(static_cast<double>(dopplerHz));
    }
    if (dopplerBins.empty() || dopplerBins.back() < static_cast<double>(dopplerMaxHz)) {
        dopplerBins.push_back(static_cast<double>(dopplerMaxHz));
    }
    result.dopplerBinsHz = QVector<double>(dopplerBins.begin(), dopplerBins.end());
    result.prnDopplerMetricDb.reserve(rows.size() * static_cast<int>(dopplerBins.size()));

    auto dataIn = makeFftwBuffer(kSamplesPerMs);
    auto dataFft = makeFftwBuffer(kSamplesPerMs);
    auto codeIn = makeFftwBuffer(kSamplesPerMs);
    auto codeFft = makeFftwBuffer(kSamplesPerMs);
    auto product = makeFftwBuffer(kSamplesPerMs);
    auto corr = makeFftwBuffer(kSamplesPerMs);
    if (!dataIn || !dataFft || !codeIn || !codeFft || !product || !corr) {
        result.status = QStringLiteral("GNSS FFT allocation failed");
        return result;
    }

    FftwPlanScope dataPlan(fftwf_plan_dft_1d(kSamplesPerMs,
                                             dataIn.get(),
                                             dataFft.get(),
                                             FFTW_FORWARD,
                                             FFTW_ESTIMATE));
    FftwPlanScope codePlan(fftwf_plan_dft_1d(kSamplesPerMs,
                                             codeIn.get(),
                                             codeFft.get(),
                                             FFTW_FORWARD,
                                             FFTW_ESTIMATE));
    FftwPlanScope inversePlan(fftwf_plan_dft_1d(kSamplesPerMs,
                                                product.get(),
                                                corr.get(),
                                                FFTW_BACKWARD,
                                                FFTW_ESTIMATE));
    if (!dataPlan.plan || !codePlan.plan || !inversePlan.plan) {
        result.status = QStringLiteral("GNSS FFT plan failed");
        return result;
    }

    std::vector<double> accumulatedPower(kSamplesPerMs, 0.0);
    double bestMetric = 0.0;
    std::vector<double> bestAccumulatedPower;
    bool usedAnyInput = false;
    bool anyRowInsideIqSpan = false;

    for (const AcquisitionCodeRow &row : rows) {
        if (isCancelRequested(cancelRequested)) {
            markCancelled(&result);
            return result;
        }
        if (row.code.size() != static_cast<std::size_t>(kSamplesPerMs) ||
            row.targetFrequency <= 0.0 ||
            !std::isfinite(row.targetFrequency)) {
            for (int i = 0; i < result.dopplerBinsHz.size(); ++i) {
                result.prnDopplerMetricDb.push_back(0.0);
            }
            continue;
        }
        if (std::abs(row.targetFrequency - centerFrequency) > inputSampleRate * 0.48) {
            for (int i = 0; i < result.dopplerBinsHz.size(); ++i) {
                result.prnDopplerMetricDb.push_back(0.0);
            }
            continue;
        }
        anyRowInsideIqSpan = true;

        std::vector<std::complex<float>> samples;
        int rowUsedInputSamples = 0;
        if (!resampleAndShift(interleavedIq,
                              inputSampleRate,
                              row.targetFrequency - centerFrequency,
                              result.channelFilterCutoffHz,
                              coherentMs,
                              &samples,
                              &rowUsedInputSamples)) {
            for (int i = 0; i < result.dopplerBinsHz.size(); ++i) {
                result.prnDopplerMetricDb.push_back(0.0);
            }
            continue;
        }
        usedAnyInput = true;
        result.usedInputSamples = (std::max)(result.usedInputSamples, rowUsedInputSamples);
        const ToneNotchStats notchStats = suppressKnownGnssTones(&samples);
        result.toneNotchesApplied += notchStats.count;
        result.strongestToneNotchDb = result.toneNotchesApplied == notchStats.count
                                          ? notchStats.strongestDb
                                          : (std::max)(result.strongestToneNotchDb,
                                                       notchStats.strongestDb);

        for (int i = 0; i < kSamplesPerMs; ++i) {
            codeIn.get()[i][0] = row.code[static_cast<std::size_t>(i)];
            codeIn.get()[i][1] = 0.0f;
        }
        fftwf_execute(codePlan.plan);

        for (double dopplerHz : dopplerBins) {
            if (isCancelRequested(cancelRequested)) {
                markCancelled(&result);
                return result;
            }
            std::fill(accumulatedPower.begin(), accumulatedPower.end(), 0.0);

            for (int ms = 0; ms < coherentMs; ++ms) {
                if (isCancelRequested(cancelRequested)) {
                    markCancelled(&result);
                    return result;
                }
                const int chunkOffset = ms * kSamplesPerMs;
                for (int i = 0; i < kSamplesPerMs; ++i) {
                    const std::complex<float> sample =
                        samples[static_cast<std::size_t>(chunkOffset + i)];
                    const double phase =
                        kTwoPi * dopplerHz *
                        (static_cast<double>(chunkOffset + i) / kAcquisitionSampleRate);
                    const double c = std::cos(phase);
                    const double s = std::sin(phase);
                    const double re = static_cast<double>(sample.real()) * c +
                                      static_cast<double>(sample.imag()) * s;
                    const double im = static_cast<double>(sample.imag()) * c -
                                      static_cast<double>(sample.real()) * s;
                    dataIn.get()[i][0] = static_cast<float>(re);
                    dataIn.get()[i][1] = static_cast<float>(im);
                }

                fftwf_execute(dataPlan.plan);
                for (int bin = 0; bin < kSamplesPerMs; ++bin) {
                    const float ar = dataFft.get()[bin][0];
                    const float ai = dataFft.get()[bin][1];
                    const float br = codeFft.get()[bin][0];
                    const float bi = codeFft.get()[bin][1];
                    product.get()[bin][0] = ar * br + ai * bi;
                    product.get()[bin][1] = ai * br - ar * bi;
                }
                fftwf_execute(inversePlan.plan);

                for (int phase = 0; phase < kSamplesPerMs; ++phase) {
                    const double re = corr.get()[phase][0];
                    const double im = corr.get()[phase][1];
                    accumulatedPower[static_cast<std::size_t>(phase)] += re * re + im * im;
                }
            }

            double sum = 0.0;
            double peak = 0.0;
            int peakPhase = 0;
            for (int phase = 0; phase < kSamplesPerMs; ++phase) {
                const double value = accumulatedPower[static_cast<std::size_t>(phase)];
                sum += value;
                if (value > peak) {
                    peak = value;
                    peakPhase = phase;
                }
            }
            const double average = (std::max)(std::numeric_limits<double>::min(),
                                              (sum - peak) /
                                                  static_cast<double>((std::max)(1, kSamplesPerMs - 1)));
            double secondPeak = 0.0;
            for (int phase = 0; phase < kSamplesPerMs; ++phase) {
                const int rawDistance = std::abs(phase - peakPhase);
                const int circularDistance = (std::min)(rawDistance, kSamplesPerMs - rawDistance);
                if (circularDistance <= kPeakExclusionSamples) {
                    continue;
                }
                secondPeak = (std::max)(secondPeak,
                                        accumulatedPower[static_cast<std::size_t>(phase)]);
            }
            secondPeak = (std::max)(secondPeak, std::numeric_limits<double>::min());
            GnssAcquisitionCandidate candidate;
            candidate.prn = row.id;
            candidate.label = row.label;
            candidate.dopplerHz = dopplerHz;
            candidate.targetFrequency = row.targetFrequency;
            candidate.codePhaseSamples = peakPhase;
            candidate.metric = peak / average;
            candidate.peak = peak;
            candidate.secondPeak = secondPeak;
            candidate.peakToSecond = peak / secondPeak;
            candidate.average = average;
            const double metricDb =
                10.0 * std::log10((std::max)(candidate.metric,
                                             std::numeric_limits<double>::min()));
            result.prnDopplerMetricDb.push_back(metricDb);
            if (candidate.metric > bestMetric) {
                bestMetric = candidate.metric;
                bestAccumulatedPower = accumulatedPower;
                result.targetFrequency = row.targetFrequency;
                result.frequencyOffset = row.targetFrequency - centerFrequency;
                buildPromptProbe(samples,
                                 row.code,
                                 dopplerHz,
                                 peakPhase,
                                 coherentMs,
                                 &result.promptI,
                                 &result.promptQ,
                                 &result.promptMagnitude,
                                 &result.promptSigns1Ms,
                                 &result.promptBitSigns20Ms,
                                 &result.promptResidualDopplerHz);
            }
            insertCandidate(&result.topCandidates, candidate);
        }
    }

    result.coherentMs = coherentMs;
    if (!anyRowInsideIqSpan) {
        result.status = QStringLiteral("%1 has no implemented channels inside the current IQ span")
                            .arg(systemName);
        return result;
    }
    if (!usedAnyInput) {
        result.status = QStringLiteral("%1 resample failed").arg(systemName);
        return result;
    }

    if (!bestAccumulatedPower.empty()) {
        result.bestCorrelationProfileDb.reserve(static_cast<int>(bestAccumulatedPower.size()));
        double maxPower = 0.0;
        for (const double value : bestAccumulatedPower) {
            maxPower = (std::max)(maxPower, value);
        }
        maxPower = (std::max)(maxPower, std::numeric_limits<double>::min());
        for (const double value : bestAccumulatedPower) {
            const double normalized = (std::max)(value, std::numeric_limits<double>::min()) / maxPower;
            result.bestCorrelationProfileDb.push_back(static_cast<float>(10.0 * std::log10(normalized)));
        }
    }

    result.valid = !result.topCandidates.isEmpty();
    detectGpsLnavPreamble(&result);
    result.status = result.valid
                        ? QStringLiteral("%1 acquisition finished").arg(systemName)
                        : QStringLiteral("No %1 candidates").arg(systemName);
    return result;
}

void applyGpsL1CaTracking(const std::vector<float> &interleavedIq,
                          double inputSampleRate,
                          double centerFrequency,
                          double targetFrequency,
                          int requestedMs,
                          double channelFilterCutoffHz,
                          GnssAcquisitionResult *result,
                          const std::atomic_bool *cancelRequested) {
    if (!result || result->topCandidates.isEmpty() || isCancelRequested(cancelRequested)) {
        return;
    }
    const GnssAcquisitionCandidate best = result->topCandidates.first();
    if (best.prn < 1 || best.prn > 32 || best.codePhaseSamples < 0) {
        return;
    }
    const double metricDb =
        10.0 * std::log10((std::max)(best.metric, std::numeric_limits<double>::min()));
    const double peakToSecondDb =
        10.0 * std::log10((std::max)(best.peakToSecond, std::numeric_limits<double>::min()));
    if (metricDb < 4.5 || peakToSecondDb < 3.0) {
        return;
    }

    const int availableMs = inputSampleRate > 0.0
                                ? static_cast<int>(std::floor(
                                      (static_cast<double>(interleavedIq.size() / 2) /
                                       inputSampleRate) * 1000.0))
                                : 0;
    const int trackingMs = (std::clamp)(requestedMs, 20, (std::max)(20, availableMs));
    if (trackingMs < 20) {
        return;
    }

    std::vector<std::complex<float>> samples;
    int usedInputSamples = 0;
    const double cutoffHz =
        (std::clamp)(std::isfinite(channelFilterCutoffHz) ? channelFilterCutoffHz : 1800000.0,
                     300000.0,
                     kAcquisitionSampleRate * 0.45);
    if (!resampleAndShift(interleavedIq,
                          inputSampleRate,
                          targetFrequency - centerFrequency,
                          cutoffHz,
                          trackingMs,
                          &samples,
                          &usedInputSamples)) {
        return;
    }
    if (isCancelRequested(cancelRequested)) {
        markCancelled(result);
        return;
    }

    const ToneNotchStats notchStats = suppressKnownGnssTones(&samples);
    result->toneNotchesApplied += notchStats.count;
    result->strongestToneNotchDb = result->toneNotchesApplied == notchStats.count
                                      ? notchStats.strongestDb
                                      : (std::max)(result->strongestToneNotchDb,
                                                   notchStats.strongestDb);
    result->usedInputSamples = (std::max)(result->usedInputSamples, usedInputSamples);
    buildTrackedPromptProbe(samples,
                            gpsCaCodeSamples(best.prn),
                            best.dopplerHz,
                            best.codePhaseSamples,
                            trackingMs,
                            result);
    detectGpsLnavPreamble(result);
}

} // namespace

GnssAcquisitionResult GnssAcquisition::acquireGpsL1Ca(const std::vector<float> &interleavedIq,
                                                      double inputSampleRate,
                                                      double centerFrequency,
                                                      double targetFrequency,
                                                      int maxCoherentMs,
                                                      double channelFilterCutoffHz,
                                                      const std::atomic_bool *cancelRequested,
                                                      int dopplerMinHz,
                                                      int dopplerMaxHz,
                                                      int dopplerStepHz) {
    return acquireBpsk1MsRows(interleavedIq,
                              inputSampleRate,
                              centerFrequency,
                              gpsL1CaRows(targetFrequency),
                              QStringLiteral("GPS L1 C/A"),
                              QStringLiteral("PRN"),
                              maxCoherentMs,
                              channelFilterCutoffHz,
                              cancelRequested,
                              dopplerMinHz,
                              dopplerMaxHz,
                              dopplerStepHz);
}

GnssAcquisitionResult GnssAcquisition::acquireGpsL1CaFocused(const std::vector<float> &interleavedIq,
                                                             double inputSampleRate,
                                                             double centerFrequency,
                                                             double targetFrequency,
                                                             int prn,
                                                             int maxCoherentMs,
                                                             double channelFilterCutoffHz,
                                                             const std::atomic_bool *cancelRequested,
                                                             int dopplerCenterHz,
                                                             int dopplerSpanHz,
                                                             int dopplerStepHz) {
    prn = (std::clamp)(prn, 1, 32);
    QVector<AcquisitionCodeRow> rows;
    rows.reserve(1);
    AcquisitionCodeRow row;
    row.id = prn;
    row.label = QStringLiteral("G%1").arg(prn, 2, 10, QLatin1Char('0'));
    row.targetFrequency = targetFrequency;
    row.code = gpsCaCodeSamples(prn);
    rows.append(std::move(row));

    const int spanHz = (std::clamp)(std::abs(dopplerSpanHz), 250, 10000);
    GnssAcquisitionResult result =
        acquireBpsk1MsRows(interleavedIq,
                           inputSampleRate,
                           centerFrequency,
                           rows,
                           QStringLiteral("GPS L1 C/A focused"),
                           QStringLiteral("PRN"),
                           maxCoherentMs,
                           channelFilterCutoffHz,
                           cancelRequested,
                           dopplerCenterHz - spanHz,
                           dopplerCenterHz + spanHz,
                           dopplerStepHz);
    if (result.valid && !result.topCandidates.isEmpty() && !isCancelRequested(cancelRequested)) {
        applyGpsL1CaTracking(interleavedIq,
                             inputSampleRate,
                             centerFrequency,
                             targetFrequency,
                             result.coherentMs,
                             channelFilterCutoffHz,
                             &result,
                             cancelRequested);
    }
    return result;
}

GnssAcquisitionResult GnssAcquisition::acquireGlonassL1Of(const std::vector<float> &interleavedIq,
                                                          double inputSampleRate,
                                                          double centerFrequency,
                                                          double targetFrequency,
                                                          int maxCoherentMs,
                                                          double channelFilterCutoffHz,
                                                          const std::atomic_bool *cancelRequested,
                                                          int dopplerMinHz,
                                                          int dopplerMaxHz,
                                                          int dopplerStepHz) {
    Q_UNUSED(targetFrequency);
    return acquireBpsk1MsRows(interleavedIq,
                              inputSampleRate,
                              centerFrequency,
                              glonassL1OfRows(),
                              QStringLiteral("GLONASS L1OF"),
                              QStringLiteral("Ch"),
                              maxCoherentMs,
                              channelFilterCutoffHz,
                              cancelRequested,
                              dopplerMinHz,
                              dopplerMaxHz,
                              dopplerStepHz);
}

GnssAcquisitionResult GnssAcquisition::acquireGpsGlonassL1(const std::vector<float> &interleavedIq,
                                                           double inputSampleRate,
                                                           double centerFrequency,
                                                           double targetFrequency,
                                                           int maxCoherentMs,
                                                           double channelFilterCutoffHz,
                                                           const std::atomic_bool *cancelRequested,
                                                           int dopplerMinHz,
                                                           int dopplerMaxHz,
                                                           int dopplerStepHz) {
    const double gpsTargetFrequency =
        std::isfinite(targetFrequency) && targetFrequency > 0.0
            ? targetFrequency
            : kGpsL1FrequencyHz;
    return acquireBpsk1MsRows(interleavedIq,
                              inputSampleRate,
                              centerFrequency,
                              gpsGlonassL1Rows(gpsTargetFrequency),
                              QStringLiteral("GPS+GLONASS L1"),
                              QStringLiteral("SV"),
                              maxCoherentMs,
                              channelFilterCutoffHz,
                              cancelRequested,
                              dopplerMinHz,
                              dopplerMaxHz,
                              dopplerStepHz);
}

#if 0
GnssAcquisitionResult GnssAcquisition::acquireGpsL1Ca_legacy(const std::vector<float> &interleavedIq,
                                                      double inputSampleRate,
                                                      double centerFrequency,
                                                      double targetFrequency,
                                                      int maxCoherentMs,
                                                      double channelFilterCutoffHz,
                                                      const std::atomic_bool *cancelRequested,
                                                      int dopplerMinHz,
                                                      int dopplerMaxHz,
                                                      int dopplerStepHz) {
    GnssAcquisitionResult result;
    result.inputSampleRate = inputSampleRate;
    result.acquisitionSampleRate = kAcquisitionSampleRate;
    result.centerFrequency = centerFrequency;
    result.targetFrequency = targetFrequency;
    result.frequencyOffset = targetFrequency - centerFrequency;
    result.channelFilterCutoffHz =
        (std::clamp)(std::isfinite(channelFilterCutoffHz) ? channelFilterCutoffHz : 1800000.0,
                     300000.0,
                     kAcquisitionSampleRate * 0.45);
    result.channelizerTaps = kChannelizerTaps;
    result.millisecondAgc = true;
    result.inputSamples = static_cast<int>(interleavedIq.size() / 2);

    if (isCancelRequested(cancelRequested)) {
        markCancelled(&result);
        return result;
    }

    if (result.inputSamples <= 0) {
        result.status = QStringLiteral("No IQ samples");
        return result;
    }
    if (inputSampleRate <= 0.0 || !std::isfinite(inputSampleRate)) {
        result.status = QStringLiteral("Invalid IQ sample rate");
        return result;
    }
    if (centerFrequency <= 0.0 || targetFrequency <= 0.0 ||
        !std::isfinite(centerFrequency) || !std::isfinite(targetFrequency)) {
        result.status = QStringLiteral("Invalid GNSS frequency");
        return result;
    }
    if (dopplerMinHz > dopplerMaxHz) {
        std::swap(dopplerMinHz, dopplerMaxHz);
    }
    dopplerMinHz = (std::clamp)(dopplerMinHz, kDopplerMinHz, kDopplerMaxHz);
    dopplerMaxHz = (std::clamp)(dopplerMaxHz, kDopplerMinHz, kDopplerMaxHz);
    dopplerStepHz = (std::clamp)(std::abs(dopplerStepHz), kDopplerMinStepHz, 10000);
    if (dopplerMinHz == dopplerMaxHz) {
        dopplerMaxHz = (std::min)(kDopplerMaxHz, dopplerMinHz + dopplerStepHz);
    }

    const double availableMs =
        (static_cast<double>(result.inputSamples) / inputSampleRate) * 1000.0;
    result.requestedCoherentMs = (std::clamp)(maxCoherentMs, 1, kMaxCoherentMs);
    const int coherentMs = (std::clamp)(static_cast<int>(std::floor(availableMs)),
                                       1,
                                       result.requestedCoherentMs);
    if (coherentMs <= 0) {
        result.status = QStringLiteral("Not enough IQ for one GPS C/A millisecond");
        return result;
    }

    std::vector<std::complex<float>> samples;
    if (!resampleAndShift(interleavedIq,
                          inputSampleRate,
                          result.frequencyOffset,
                          result.channelFilterCutoffHz,
                          coherentMs,
                          &samples,
                          &result.usedInputSamples)) {
        result.status = QStringLiteral("GNSS resample failed");
        return result;
    }
    const ToneNotchStats notchStats = suppressKnownGnssTones(&samples);
    result.toneNotchesApplied = notchStats.count;
    result.strongestToneNotchDb = notchStats.strongestDb;
    result.coherentMs = coherentMs;
    if (isCancelRequested(cancelRequested)) {
        markCancelled(&result);
        return result;
    }

    auto dataIn = makeFftwBuffer(kSamplesPerMs);
    auto dataFft = makeFftwBuffer(kSamplesPerMs);
    auto codeIn = makeFftwBuffer(kSamplesPerMs);
    auto codeFft = makeFftwBuffer(kSamplesPerMs);
    auto product = makeFftwBuffer(kSamplesPerMs);
    auto corr = makeFftwBuffer(kSamplesPerMs);
    if (!dataIn || !dataFft || !codeIn || !codeFft || !product || !corr) {
        result.status = QStringLiteral("GNSS FFT allocation failed");
        return result;
    }

    FftwPlanScope dataPlan(fftwf_plan_dft_1d(kSamplesPerMs,
                                             dataIn.get(),
                                             dataFft.get(),
                                             FFTW_FORWARD,
                                             FFTW_ESTIMATE));
    FftwPlanScope codePlan(fftwf_plan_dft_1d(kSamplesPerMs,
                                             codeIn.get(),
                                             codeFft.get(),
                                             FFTW_FORWARD,
                                             FFTW_ESTIMATE));
    FftwPlanScope inversePlan(fftwf_plan_dft_1d(kSamplesPerMs,
                                                product.get(),
                                                corr.get(),
                                                FFTW_BACKWARD,
                                                FFTW_ESTIMATE));
    if (!dataPlan.plan || !codePlan.plan || !inversePlan.plan) {
        result.status = QStringLiteral("GNSS FFT plan failed");
        return result;
    }

    std::vector<double> dopplerBins;
    dopplerBins.reserve(static_cast<std::size_t>((dopplerMaxHz - dopplerMinHz) / dopplerStepHz + 1));
    for (int dopplerHz = dopplerMinHz; dopplerHz <= dopplerMaxHz; dopplerHz += dopplerStepHz) {
        dopplerBins.push_back(static_cast<double>(dopplerHz));
    }
    if (dopplerBins.empty() || dopplerBins.back() < static_cast<double>(dopplerMaxHz)) {
        dopplerBins.push_back(static_cast<double>(dopplerMaxHz));
    }
    result.dopplerBinsHz = QVector<double>(dopplerBins.begin(), dopplerBins.end());
    result.prnDopplerMetricDb.reserve(32 * static_cast<int>(dopplerBins.size()));
    std::vector<double> accumulatedPower(kSamplesPerMs, 0.0);
    double bestMetric = 0.0;
    std::vector<double> bestAccumulatedPower;

    for (int prn = 1; prn <= 32; ++prn) {
        if (isCancelRequested(cancelRequested)) {
            markCancelled(&result);
            return result;
        }
        const std::vector<float> code = gpsCaCodeSamples(prn);
        for (int i = 0; i < kSamplesPerMs; ++i) {
            codeIn.get()[i][0] = code[static_cast<std::size_t>(i)];
            codeIn.get()[i][1] = 0.0f;
        }
        fftwf_execute(codePlan.plan);

        for (double dopplerHz : dopplerBins) {
            if (isCancelRequested(cancelRequested)) {
                markCancelled(&result);
                return result;
            }
            std::fill(accumulatedPower.begin(), accumulatedPower.end(), 0.0);

            for (int ms = 0; ms < coherentMs; ++ms) {
                if (isCancelRequested(cancelRequested)) {
                    markCancelled(&result);
                    return result;
                }
                const int chunkOffset = ms * kSamplesPerMs;
                for (int i = 0; i < kSamplesPerMs; ++i) {
                    const std::complex<float> sample =
                        samples[static_cast<std::size_t>(chunkOffset + i)];
                    const double phase =
                        kTwoPi * dopplerHz *
                        (static_cast<double>(chunkOffset + i) / kAcquisitionSampleRate);
                    const double c = std::cos(phase);
                    const double s = std::sin(phase);
                    const double re = static_cast<double>(sample.real()) * c +
                                      static_cast<double>(sample.imag()) * s;
                    const double im = static_cast<double>(sample.imag()) * c -
                                      static_cast<double>(sample.real()) * s;
                    dataIn.get()[i][0] = static_cast<float>(re);
                    dataIn.get()[i][1] = static_cast<float>(im);
                }

                fftwf_execute(dataPlan.plan);
                for (int bin = 0; bin < kSamplesPerMs; ++bin) {
                    const float ar = dataFft.get()[bin][0];
                    const float ai = dataFft.get()[bin][1];
                    const float br = codeFft.get()[bin][0];
                    const float bi = codeFft.get()[bin][1];
                    product.get()[bin][0] = ar * br + ai * bi;
                    product.get()[bin][1] = ai * br - ar * bi;
                }
                fftwf_execute(inversePlan.plan);

                for (int phase = 0; phase < kSamplesPerMs; ++phase) {
                    const double re = corr.get()[phase][0];
                    const double im = corr.get()[phase][1];
                    accumulatedPower[static_cast<std::size_t>(phase)] += re * re + im * im;
                }
            }

            double sum = 0.0;
            double peak = 0.0;
            int peakPhase = 0;
            for (int phase = 0; phase < kSamplesPerMs; ++phase) {
                const double value = accumulatedPower[static_cast<std::size_t>(phase)];
                sum += value;
                if (value > peak) {
                    peak = value;
                    peakPhase = phase;
                }
            }
            const double average = (std::max)(std::numeric_limits<double>::min(),
                                              (sum - peak) /
                                                  static_cast<double>((std::max)(1, kSamplesPerMs - 1)));
            double secondPeak = 0.0;
            for (int phase = 0; phase < kSamplesPerMs; ++phase) {
                const int rawDistance = std::abs(phase - peakPhase);
                const int circularDistance = (std::min)(rawDistance, kSamplesPerMs - rawDistance);
                if (circularDistance <= kPeakExclusionSamples) {
                    continue;
                }
                secondPeak = (std::max)(secondPeak,
                                        accumulatedPower[static_cast<std::size_t>(phase)]);
            }
            secondPeak = (std::max)(secondPeak, std::numeric_limits<double>::min());
            GnssAcquisitionCandidate candidate;
            candidate.prn = prn;
            candidate.dopplerHz = dopplerHz;
            candidate.codePhaseSamples = peakPhase;
            candidate.metric = peak / average;
            candidate.peak = peak;
            candidate.secondPeak = secondPeak;
            candidate.peakToSecond = peak / secondPeak;
            candidate.average = average;
            const double metricDb =
                10.0 * std::log10((std::max)(candidate.metric,
                                             std::numeric_limits<double>::min()));
            result.prnDopplerMetricDb.push_back(metricDb);
            if (candidate.metric > bestMetric) {
                bestMetric = candidate.metric;
                bestAccumulatedPower = accumulatedPower;
            }
            insertCandidate(&result.topCandidates, candidate);
        }
    }

    if (!bestAccumulatedPower.empty()) {
        result.bestCorrelationProfileDb.reserve(static_cast<int>(bestAccumulatedPower.size()));
        double maxPower = 0.0;
        for (const double value : bestAccumulatedPower) {
            maxPower = (std::max)(maxPower, value);
        }
        maxPower = (std::max)(maxPower, std::numeric_limits<double>::min());
        for (const double value : bestAccumulatedPower) {
            const double normalized = (std::max)(value, std::numeric_limits<double>::min()) / maxPower;
            result.bestCorrelationProfileDb.push_back(static_cast<float>(10.0 * std::log10(normalized)));
        }
    }

    result.valid = !result.topCandidates.isEmpty();
    result.status = result.valid
                        ? QStringLiteral("GPS C/A acquisition finished")
                        : QStringLiteral("No GPS C/A candidates");
    return result;
}
#endif

std::vector<float> GnssAcquisition::makeSyntheticGpsL1CaIq(double inputSampleRate,
                                                           double centerFrequency,
                                                           double targetFrequency,
                                                           int prn,
                                                           double dopplerHz,
                                                           int durationMs,
                                                           double amplitude) {
    if (!std::isfinite(inputSampleRate) || inputSampleRate <= 0.0 ||
        !std::isfinite(centerFrequency) || centerFrequency <= 0.0 ||
        !std::isfinite(targetFrequency) || targetFrequency <= 0.0 ||
        !std::isfinite(dopplerHz) ||
        prn < 1 || prn > 32 ||
        durationMs <= 0) {
        return {};
    }

    amplitude = (std::clamp)(std::isfinite(amplitude) ? amplitude : 0.25, 0.001, 0.95);
    const int inputSamples =
        static_cast<int>(std::llround(inputSampleRate *
                                      (static_cast<double>(durationMs) / 1000.0)));
    if (inputSamples <= 0) {
        return {};
    }

    const std::vector<float> chips = gpsCaChips(prn);
    const double carrierHz = (targetFrequency - centerFrequency) + dopplerHz;
    constexpr double kSyntheticCodePhaseChips = 123.25;
    constexpr double kSyntheticInitialPhaseRad = 0.37;
    constexpr std::array<int, 10> kSyntheticLnavBits = {{1, 0, 0, 0, 1, 0, 1, 1, 0, 1}};
    std::vector<float> iq;
    iq.reserve(static_cast<std::size_t>(inputSamples) * 2U);
    for (int n = 0; n < inputSamples; ++n) {
        const double t = static_cast<double>(n) / inputSampleRate;
        double chipPosition = std::fmod(t * kGpsCaChipRate + kSyntheticCodePhaseChips,
                                        static_cast<double>(kGpsCaChips));
        if (chipPosition < 0.0) {
            chipPosition += static_cast<double>(kGpsCaChips);
        }
        const int chipIndex =
            (std::clamp)(static_cast<int>(std::floor(chipPosition)), 0, kGpsCaChips - 1);
        const double code = static_cast<double>(chips[static_cast<std::size_t>(chipIndex)]);
        const int bitIndex = static_cast<int>(std::floor(t / 0.020)) %
                             static_cast<int>(kSyntheticLnavBits.size());
        const double navBit = kSyntheticLnavBits[static_cast<std::size_t>(bitIndex)] ? 1.0 : -1.0;
        const double phase = kTwoPi * carrierHz * t + kSyntheticInitialPhaseRad;
        iq.push_back(static_cast<float>(amplitude * navBit * code * std::cos(phase)));
        iq.push_back(static_cast<float>(amplitude * navBit * code * std::sin(phase)));
    }
    return iq;
}

std::vector<float> GnssAcquisition::makeSyntheticGlonassL1OfIq(double inputSampleRate,
                                                               double centerFrequency,
                                                               int channel,
                                                               double dopplerHz,
                                                               int durationMs,
                                                               double amplitude) {
    if (!std::isfinite(inputSampleRate) || inputSampleRate <= 0.0 ||
        !std::isfinite(centerFrequency) || centerFrequency <= 0.0 ||
        !std::isfinite(dopplerHz) ||
        channel < -7 || channel > 6 ||
        durationMs <= 0) {
        return {};
    }

    amplitude = (std::clamp)(std::isfinite(amplitude) ? amplitude : 0.25, 0.001, 0.95);
    const int inputSamples =
        static_cast<int>(std::llround(inputSampleRate *
                                      (static_cast<double>(durationMs) / 1000.0)));
    if (inputSamples <= 0) {
        return {};
    }

    const std::vector<float> chips = glonassL1OfChips();
    constexpr double kGlonassCodeRate = 511000.0;
    const double targetFrequency = 1602000000.0 + static_cast<double>(channel) * 562500.0;
    const double carrierHz = (targetFrequency - centerFrequency) + dopplerHz;
    constexpr double kSyntheticCodePhaseChips = 61.75;
    constexpr double kSyntheticInitialPhaseRad = 0.61;
    std::vector<float> iq;
    iq.reserve(static_cast<std::size_t>(inputSamples) * 2U);
    for (int n = 0; n < inputSamples; ++n) {
        const double t = static_cast<double>(n) / inputSampleRate;
        double chipPosition = std::fmod(t * kGlonassCodeRate + kSyntheticCodePhaseChips,
                                        static_cast<double>(chips.size()));
        if (chipPosition < 0.0) {
            chipPosition += static_cast<double>(chips.size());
        }
        const int chipIndex =
            (std::clamp)(static_cast<int>(std::floor(chipPosition)),
                         0,
                         static_cast<int>(chips.size()) - 1);
        const double code = static_cast<double>(chips[static_cast<std::size_t>(chipIndex)]);
        const double phase = kTwoPi * carrierHz * t + kSyntheticInitialPhaseRad;
        iq.push_back(static_cast<float>(amplitude * code * std::cos(phase)));
        iq.push_back(static_cast<float>(amplitude * code * std::sin(phase)));
    }
    return iq;
}
