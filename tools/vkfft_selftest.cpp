#include "gpufftbackend.h"

#include <fftw3.h>

#include <QCoreApplication>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double elapsedMilliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void fillInput(std::vector<float> &values, int length) {
    constexpr double TwoPi = 6.283185307179586476925286766559;
    uint32_t randomState = 0x5a17c9e3U;
    for (int index = 0; index < length; ++index) {
        randomState = randomState * 1664525U + 1013904223U;
        const double noiseI = (static_cast<double>((randomState >> 8) & 0xffffU) / 32767.5 - 1.0) * 0.002;
        randomState = randomState * 1664525U + 1013904223U;
        const double noiseQ = (static_cast<double>((randomState >> 8) & 0xffffU) / 32767.5 - 1.0) * 0.002;
        const double phaseA = TwoPi * 37.0 * static_cast<double>(index) / static_cast<double>(length);
        const double phaseB = TwoPi * 911.0 * static_cast<double>(index) / static_cast<double>(length);
        values[2 * index] = static_cast<float>(0.73 * std::cos(phaseA) +
                                               0.19 * std::cos(phaseB) + noiseI);
        values[2 * index + 1] = static_cast<float>(0.73 * std::sin(phaseA) -
                                                   0.19 * std::sin(phaseB) + noiseQ);
    }
}

bool runCase(GpuFftBackend &backend, int length, QTextStream &output) {
    std::vector<float> input(static_cast<std::size_t>(length) * 2U);
    std::vector<float> cpuOutput(input.size());
    std::vector<float> gpuOutput(input.size());
    fillInput(input, length);

    fftwf_plan cpuPlan = fftwf_plan_dft_1d(
        length,
        reinterpret_cast<fftwf_complex *>(input.data()),
        reinterpret_cast<fftwf_complex *>(cpuOutput.data()),
        FFTW_FORWARD,
        FFTW_ESTIMATE);
    if (!cpuPlan) {
        output << "FAIL length=" << length << " FFTW plan creation failed\n";
        return false;
    }

    const auto cpuStart = Clock::now();
    fftwf_execute(cpuPlan);
    const auto cpuEnd = Clock::now();

    QString error;
    const auto gpuStart = Clock::now();
    const bool gpuSuccess = backend.execute(input.data(), gpuOutput.data(), length, &error);
    const auto gpuEnd = Clock::now();
    if (!gpuSuccess) {
        output << "FAIL length=" << length << " VkFFT: " << error << '\n';
        fftwf_destroy_plan(cpuPlan);
        return false;
    }

    std::vector<float> repeatOutput(input.size());
    const auto repeatStart = Clock::now();
    const bool repeatSuccess = backend.execute(input.data(), repeatOutput.data(), length, &error);
    const auto repeatEnd = Clock::now();
    fftwf_destroy_plan(cpuPlan);
    if (!repeatSuccess) {
        output << "FAIL length=" << length << " repeated VkFFT: " << error << '\n';
        return false;
    }

    long double referenceEnergy = 0.0L;
    long double errorEnergy = 0.0L;
    double referenceMaximum = 0.0;
    double maximumError = 0.0;
    double repeatMaximumError = 0.0;
    for (std::size_t index = 0; index < cpuOutput.size(); ++index) {
        const double reference = cpuOutput[index];
        const double difference = static_cast<double>(gpuOutput[index]) - reference;
        referenceEnergy += reference * reference;
        errorEnergy += difference * difference;
        referenceMaximum = (std::max)(referenceMaximum, std::abs(reference));
        maximumError = (std::max)(maximumError, std::abs(difference));
        repeatMaximumError = (std::max)(
            repeatMaximumError,
            std::abs(static_cast<double>(repeatOutput[index]) - gpuOutput[index]));
    }

    const double relativeRms = std::sqrt(static_cast<double>(errorEnergy / referenceEnergy));
    const double normalizedMaximum = maximumError / (std::max)(referenceMaximum, 1.0);
    const double normalizedRepeatMaximum = repeatMaximumError / (std::max)(referenceMaximum, 1.0);
    constexpr double RelativeRmsLimit = 2.0e-4;
    constexpr double NormalizedMaximumLimit = 2.0e-4;
    const bool valid = std::isfinite(relativeRms) &&
                       std::isfinite(normalizedMaximum) &&
                       relativeRms <= RelativeRmsLimit &&
                       normalizedMaximum <= NormalizedMaximumLimit &&
                       normalizedRepeatMaximum <= 1.0e-7;

    output << (valid ? "PASS" : "FAIL")
           << " length=" << length
           << " device=\"" << backend.deviceName() << '"'
           << " cpu_ms=" << elapsedMilliseconds(cpuStart, cpuEnd)
           << " gpu_first_ms=" << elapsedMilliseconds(gpuStart, gpuEnd)
           << " gpu_repeat_ms=" << elapsedMilliseconds(repeatStart, repeatEnd)
           << " rel_rms=" << relativeRms
           << " max_norm=" << normalizedMaximum
           << " repeat_max_norm=" << normalizedRepeatMaximum
           << '\n';
    return valid;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QTextStream output(stdout);
    output << "CHECK Vulkan loader\n";
    output.flush();
    if (!GpuFftBackend::isCompiled()) {
        output << "FAIL VkFFT support is not compiled\n";
        return 2;
    }

    GpuFftBackend backend;
    bool success = true;
    std::vector<int> lengths = {4096, 65536, 1048576};
    if (application.arguments().size() > 1) {
        lengths.clear();
        for (const QString &argument : application.arguments().mid(1)) {
            bool ok = false;
            const int length = argument.toInt(&ok);
            if (!ok || length < 2) {
                output << "FAIL invalid FFT length: " << argument << '\n';
                return 2;
            }
            lengths.push_back(length);
        }
    }
    for (const int length : lengths) {
        output << "RUN length=" << length << '\n';
        output.flush();
        success = runCase(backend, length, output) && success;
        output.flush();
    }
    output.flush();
    return success ? 0 : 1;
}
