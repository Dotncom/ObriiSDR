#pragma once

#include "signallabpreview.h"

// I and Q must already be DC-centered; this is a diagnostic symbol sampler, not a decoder.
SignalLabIqPreviewData buildResearchIqConstellation(const std::vector<float> &first,
                                                   const std::vector<float> &second,
                                                   double sampleRate,
                                                   double symbolRate,
                                                   double phaseFraction);
