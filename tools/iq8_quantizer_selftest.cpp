#include "iq8quantizer.h"

#include <chrono>
#include <cstdio>
#include <limits>
#include <vector>

int main() {
    constexpr std::size_t count = 524291;
    std::vector<float> input(count);
    std::uint32_t state = 0x12345678U;
    for (float &sample : input) {
        state = state * 1664525U + 1013904223U;
        sample = static_cast<float>(static_cast<int>((state >> 8) % 400001U) - 200000) / 100000.0f;
    }
    input[0] = std::numeric_limits<float>::quiet_NaN();
    input[1] = std::numeric_limits<float>::infinity();
    input[2] = -std::numeric_limits<float>::infinity();
    input[3] = -0.0f;
    input[4] = 1.0f / (127.0f * 16.0f) * 0.5f;
    input[5] = -input[4];

    std::vector<char> output(count);
    for (std::size_t length : {std::size_t(1), std::size_t(15), std::size_t(16),
                               std::size_t(17), std::size_t(31), std::size_t(32),
                               std::size_t(33), count}) {
        ObriiIq8Quantizer::quantize(input.data(), output.data(), length);
        for (std::size_t index = 0; index < length; ++index) {
            if (output[index] != ObriiIq8Quantizer::quantizeOne(input[index])) {
                std::fprintf(stderr, "Mismatch at %zu of %zu\n", index, length);
                return 1;
            }
        }
    }
    for (float gain : {0.1f, 0.5f, 1.0f, 16.0f, 128.0f}) {
        ObriiIq8Quantizer::quantizeScaled(input.data(), output.data(), count, gain);
        for (std::size_t index = 0; index < count; ++index) {
            if (output[index] != ObriiIq8Quantizer::quantizeOneScaled(input[index], gain)) {
                std::fprintf(stderr, "Scaled mismatch at %zu (gain %.2f)\n", index, gain);
                return 1;
            }
        }
    }

    constexpr int repetitions = 100;
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < repetitions; ++iteration)
        ObriiIq8Quantizer::quantize(input.data(), output.data(), count);
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("IQ8 quantizer OK: %zu values, %.1f Mvalues/s, %.3f ms per 262144 IQ block\n",
                count, count * repetitions / seconds / 1.0e6,
                seconds / repetitions * (524288.0 / count) * 1000.0);
    return 0;
}
