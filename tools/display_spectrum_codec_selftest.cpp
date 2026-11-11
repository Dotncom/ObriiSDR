#include "displayspectrumcodec.h"
#include "../src/utils/displaylevelstep.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const std::vector<float> levels{-160.04f, -84.26f, 0.0f, 73.94f,
                                    std::numeric_limits<float>::quiet_NaN()};
    const std::vector<float> reference{-140.0f, -80.0f, -20.0f, 0.0f, 10.0f};
    const QByteArray payload = DisplaySpectrumCodec::encodeDb10(levels, reference);
    std::vector<float> output, restoredReference;
    if (payload.size() != 20 ||
        !DisplaySpectrumCodec::decodeDb10(payload, 5, true, &output, &restoredReference) ||
        output.size() != levels.size() || restoredReference.size() != levels.size() ||
        std::abs(output[0] + 160.0f) > 0.001f ||
        std::abs(output[1] + 84.3f) > 0.001f ||
        std::abs(output[3] - 73.9f) > 0.001f ||
        output[4] != -160.0f || restoredReference[4] != 10.0f ||
        DisplaySpectrumCodec::decodeDb10(payload.left(18), 5, true, &output, &restoredReference)) {
        std::fprintf(stderr, "Display spectrum codec self-test failed\n");
        return 1;
    }
    const std::vector<float> fullScale{-160.0f, -103.247f, -83.6f, 0.0f, 20.0f};
    for (const int step : {10, 20}) {
        const auto packed = DisplaySpectrumCodec::encodeU8(fullScale, reference, step);
        const QJsonObject header{{"binFormat", "u8-level-step"}, {"fftLength", 5},
                                 {"hasReferenceMagnitudes", true}, {"levelStepTenths", step},
                                 {"levelOffsetSteps", packed.offsetSteps}};
        if (packed.payload.size() != payload.size() / 2 ||
            !DisplaySpectrumCodec::decodeCompact(header, packed.payload, &output, &restoredReference) ||
            output.size() != fullScale.size() || restoredReference.size() != reference.size() ||
            output[1] != (step == 10 ? -103.0f : -104.0f) || output[2] != -84.0f ||
            output[0] != -160.0f || output[4] != 20.0f ||
            restoredReference[4] != 10.0f ||
            DisplaySpectrumCodec::decodeCompact(header, packed.payload.chopped(1), &output, &restoredReference)) {
            std::fprintf(stderr, "Packed spectrum self-test failed for step %d\n", step);
            return 1;
        }
    }
    if (DisplayLevelStep::rounded(-103.247f, 0) != -103.247f ||
        DisplayLevelStep::rounded(-103.247f, 10) != -103.0f ||
        !DisplaySpectrumCodec::encodeU8({-160.0f, 96.0f}, {}, 10).payload.isEmpty() ||
        DisplaySpectrumCodec::encodeU8({-160.0f, 95.0f}, {}, 10).payload.size() != 2) {
        std::fprintf(stderr, "Spectrum quantization boundary self-test failed\n");
        return 1;
    }
    std::fprintf(stdout, "Display spectrum codec self-test passed\n");
    return 0;
}
