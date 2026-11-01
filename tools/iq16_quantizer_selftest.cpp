#include "iq16quantizer.h"

#include <chrono>
#include <cstdio>
#include <vector>

int main() {
    constexpr std::size_t count = 524291;
    std::vector<float> input(count);
    std::uint32_t state = 0x9a7316c5U;
    for (float &sample : input) {
        state = state * 1664525U + 1013904223U;
        sample = float(int((state >> 8) % 400001U) - 200000) / 100000.0f;
    }
    input[0] = 0.0f;
    input[1] = -0.0f;
    input[2] = 32767.0f / 32767.0f;
    input[3] = -32767.0f / 32767.0f;

    std::vector<char> actual(count * 2);
    for (double scale : {1.0 / 32767.0, 2.0 / 32767.0, 0.37 / 32767.0}) {
        for (int value = 0; value < 20; ++value) {
            input[std::size_t(4 + value)] = float((double(value) + 0.5) * scale);
            input[std::size_t(24 + value)] = -input[std::size_t(4 + value)];
        }
        for (std::size_t length : {std::size_t(1), std::size_t(3), std::size_t(4),
                                   std::size_t(5), std::size_t(15), std::size_t(16),
                                   std::size_t(45), count}) {
            int actualClipped = 0;
            ObriiIq16Quantizer::quantize(input.data(), actual.data(), length,
                                         scale, actualClipped);
            int expectedClipped = 0;
            for (std::size_t index = 0; index < length; ++index) {
                const std::uint16_t value = std::uint16_t(
                    ObriiIq16Quantizer::quantizeOne(input[index], scale, expectedClipped));
                if (actual[index * 2] != char(value & 0xffU) ||
                    actual[index * 2 + 1] != char(value >> 8)) {
                    std::fprintf(stderr, "IQ16 byte mismatch at %zu (scale %.10f)\n", index, scale);
                    return 1;
                }
            }
            if (actualClipped != expectedClipped) {
                std::fprintf(stderr, "IQ16 clipped count mismatch\n");
                return 1;
            }
        }
    }
    int clipped = 0;
    constexpr int repetitions = 100;
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < repetitions; ++iteration) {
        clipped = 0;
        ObriiIq16Quantizer::quantize(input.data(), actual.data(), count,
                                     2.0 / 32767.0, clipped);
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("IQ16 quantizer OK: %zu values, %.1f Mvalues/s, %.3f ms per 262144 IQ block\n",
                count, count * repetitions / seconds / 1.0e6,
                seconds / repetitions * (524288.0 / count) * 1000.0);
    return 0;
}
