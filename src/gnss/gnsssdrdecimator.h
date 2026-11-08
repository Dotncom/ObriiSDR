#ifndef GNSSSDRDECIMATOR_H
#define GNSSSDRDECIMATOR_H

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Three cascaded moving averages provide a cheap anti-alias filter before
// integer decimation. State is retained across IQ blocks.
class GnssSdrDecimator {
public:
    explicit GnssSdrDecimator(int factor) : factor_(factor) {
        for (auto &stage : delay_) stage.resize(static_cast<std::size_t>(factor));
    }

    void process(const float *input, std::size_t floatCount, std::vector<float> &output) {
        output.clear();
        if (factor_ == 1) {
            output.assign(input, input + floatCount);
            return;
        }
        output.reserve((floatCount / 2 / static_cast<std::size_t>(factor_) + 1) * 2);
        for (std::size_t index = 0; index + 1 < floatCount; index += 2) {
            std::array<double, 2> value{
                std::isfinite(input[index]) ? input[index] : 0.0,
                std::isfinite(input[index + 1]) ? input[index + 1] : 0.0};
            for (int stage = 0; stage < 3; ++stage) {
                auto &old = delay_[stage][position_];
                for (int component = 0; component < 2; ++component) {
                    sums_[stage][component] += value[component] - old[component];
                    old[component] = value[component];
                }
                value = sums_[stage];
            }
            if (++position_ == static_cast<std::size_t>(factor_)) {
                position_ = 0;
                const double scale = 1.0 / (static_cast<double>(factor_) * factor_ * factor_);
                output.push_back(static_cast<float>(value[0] * scale));
                output.push_back(static_cast<float>(value[1] * scale));
            }
        }
    }

private:
    int factor_;
    std::size_t position_ = 0;
    std::array<std::vector<std::array<double, 2>>, 3> delay_;
    std::array<std::array<double, 2>, 3> sums_{};
};

#endif
