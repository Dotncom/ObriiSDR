#include "displayspectrumcodec.h"

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
    std::fprintf(stdout, "Display spectrum codec self-test passed\n");
    return 0;
}
