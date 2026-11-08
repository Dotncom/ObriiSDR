#pragma once

#include "dspiqsourceregistry.h"
#include "ffttransformcore.h"
#include "iqchannelizer.h"

#include <array>

// Software alignment of two independently numbered Fobos streams.
class MultiSdrSignalAlignment final {
public:
    static constexpr double InputRate = 8000000;
    static constexpr double OutputRate = 125000;

    bool update(const std::vector<DspSourceIdentity> &sources, bool &received, bool &discontinuity, QString &error);
    void discardHistory();
    bool align(bool tracking, QString &status);
    void windows(std::vector<DspIqSourceSnapshot> &inputs) const;

private:
    struct Source {
        IqChannelizer channelizer;
        std::uint64_t epoch = 0;
        std::uint64_t endFloat = 0;
        std::uint64_t endRaw = 0;
        std::uint64_t continuity = 0;
        std::uint64_t end = 0;
        double center = 0;
        std::vector<float> iq;

        void discard();
        std::uint64_t start() const { return end - iq.size() / 2; }
    };

    bool findMatch(const float *reference, const float *target, int &position);
    std::uint64_t mapIndex(std::size_t source, std::uint64_t index) const;

    std::array<Source, 2> states;
    std::array<DspSourceIdentity, 2> identities;
    std::array<std::uint64_t, 2> anchors{};
    FftTransformCore correlation;
    std::vector<float> referenceBins;
};
