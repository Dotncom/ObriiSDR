#include "clispectrummeter.h"
#include "clispectrumaccumulator.h"
#include "../core/fftplannermutex.h"

#include <QJsonArray>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <new>
#include <stdexcept>

CliSpectrumMeter::CliSpectrumMeter(int fftSize) : size(fftSize) {
    if (!CliSpectrumLimits::supportsSize(size)) return;
    try {
        if (!transform.allocateBuffers(size, FftTransformCore::Storage::InPlace)) return;
        transform.ensureWindow(FFT_WINDOW_HANN);
        {
            std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
            transform.createPlanLocked();
        }
        if (!transform.ready()) return;
        windowSum = transform.windowAmplitudeSum(size);
    } catch (const std::bad_alloc &) {
        // Leave an invalid, fully released meter for the caller to reject.
        transform.releasePlan();
    } catch (const std::length_error &) {
        transform.releasePlan();
    }
}

CliSpectrumMeter::~CliSpectrumMeter() = default;

bool CliSpectrumMeter::valid() const { return transform.ready() && windowSum > 0.0; }

QJsonObject CliSpectrumMeter::analyze(const float *interleavedIq, int complexSamples,
                                      double sampleRateHz, double centerHz,
                                      std::vector<float> *orderedTrace) const {
    if (!valid() || !interleavedIq || complexSamples < size ||
        !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0 || !std::isfinite(centerHz)) {
        if (orderedTrace) orderedTrace->clear();
        return {};
    }

    try {
        const float *source = interleavedIq + static_cast<std::size_t>(complexSamples - size) * 2;
        transform.stageInputRange(source, 0, size);
        transform.execute();
        float *input = reinterpret_cast<float *>(transform.output());

        // Compact levels forwards into the first half of the in-place buffer;
        // each complex bin is read before its storage can be overwritten.
        const double normalizationDb = 20.0 * std::log10(windowSum);
        transform.writeMagnitudes(input, 0, size, normalizationDb);
        const int firstNegative = (size + 1) / 2;
        if (orderedTrace) {
            orderedTrace->resize(static_cast<std::size_t>(size));
            transform.writeShiftedMagnitudes(input, orderedTrace->data());
        }

        // The unused second half holds an exact median workspace. Neither
        // noise estimation nor peak selection reduces the scientific trace.
        float *noise = input + size;
        int noiseCount = 0;
        for (int i = 0; i < size; ++i) {
            const int offset = i < firstNegative ? i : i - size;
            if (std::abs(offset) > 2) noise[noiseCount++] = input[i];
        }
        float *midpoint = noise + noiseCount / 2;
        std::nth_element(noise, midpoint, noise + noiseCount);
        const float noiseFloor = *midpoint;

        QJsonArray peaks;
        std::array<int, 3> selected{};
        int selectedCount = 0;
        // Three full-resolution scans bound peak-selection storage, including
        // spectra with millions of local maxima and nearby stronger lobes.
        while (selectedCount < static_cast<int>(selected.size())) {
            int best = -1;
            for (int i = 0; i < size; ++i) {
                const float level = input[i];
                if (level < noiseFloor + 6.0f || (best >= 0 && level <= input[best])) continue;
                const int previous = i ? i - 1 : size - 1;
                const int next = i + 1 == size ? 0 : i + 1;
                if (level < input[previous] || level <= input[next]) continue;
                bool tooClose = false;
                for (int j = 0; j < selectedCount; ++j) {
                    const int distance = std::abs(i - selected[static_cast<std::size_t>(j)]);
                    if ((std::min)(distance, size - distance) <= 2) {
                        tooClose = true;
                        break;
                    }
                }
                if (!tooClose) best = i;
            }
            if (best < 0) break;
            selected[static_cast<std::size_t>(selectedCount++)] = best;
            const double offset = (best < firstNegative ? best : best - size) * (sampleRateHz / size);
            peaks.append(QJsonObject{{QStringLiteral("frequencyHz"), centerHz + offset},
                                     {QStringLiteral("offsetHz"), offset},
                                     {QStringLiteral("levelDbfs"), input[best]}});
        }
        return {{QStringLiteral("fftSize"), size},
                {QStringLiteral("binHz"), sampleRateHz / size},
                {QStringLiteral("noiseFloorDbfs"), noiseFloor},
                {QStringLiteral("peaks"), peaks}};
    } catch (const std::bad_alloc &) {
    } catch (const std::length_error &) {
    }
    if (orderedTrace) orderedTrace->clear();
    return {};
}
