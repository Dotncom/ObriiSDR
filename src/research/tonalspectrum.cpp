#include "tonalspectrum.h"
#include <algorithm>
#include <cmath>

TonalSpectrumResult analyzeTonalSpectrum(const std::vector<float> &f,
                                        const std::vector<float> &db,
                                        double fundamental, double halfWidth, int lastHarmonic) {
    TonalSpectrumResult r;
    const size_t n = std::min(f.size(), db.size());
    if (n < 8 || !std::isfinite(fundamental) || !std::isfinite(halfWidth) ||
        halfWidth <= 0 || !std::isfinite(f.front()) || !std::isfinite(f[n-1]) || f[n-1] <= f.front()) return r;
    r.binWidthHz = (double(f[n-1])-f.front()) / double(n-1);
    const double floatResolution = std::abs(double(std::nextafter(f[n-1], std::numeric_limits<float>::infinity()))-f[n-1]);
    for (size_t i = 1; i < n; ++i)
        if (!std::isfinite(f[i]) || f[i] < f[i-1] || double(f[i])-f[i-1] > std::max(4*r.binWidthHz, 2*floatResolution)) return {};
    halfWidth = std::max(halfWidth, 1.5*r.binWidthHz);
    if (fundamental <= 3*halfWidth || fundamental-halfWidth < f.front() || fundamental+halfWidth > f[n-1]) return r;
    lastHarmonic = std::clamp(lastHarmonic, 2, 20);
    r.fundamentalHz = fundamental;
    r.harmonicsRequested = lastHarmonic-1;
    r.completeHarmonicSpan = lastHarmonic*fundamental+halfWidth <= f[n-1];
    for (int h = 2; h <= lastHarmonic; ++h)
        if (h*fundamental-halfWidth >= f.front() && h*fundamental+halfWidth <= f[n-1]) ++r.harmonicsMeasured;
    double signalBand = 0, harmonicBands = 0, outside = 0;
    size_t signalBins = 0, harmonicBins = 0;
    std::vector<double> noiseBins;
    noiseBins.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(db[i]) || db[i] > 300 || db[i] < -1000) return {};
        // Reconstruct the uniform axis to avoid high-RF float frequency quantization.
        const double frequency = f.front()+i*r.binWidthHz;
        if (std::abs(frequency) <= halfWidth) continue;
        const double power = std::pow(10.0, db[i]/10.0);
        if (std::abs(frequency-fundamental) <= halfWidth) { signalBand += power; ++signalBins; continue; }
        const int harmonic = int(std::llround(frequency/fundamental));
        if (harmonic >= 2 && harmonic <= lastHarmonic && harmonic*fundamental+halfWidth <= f[n-1] &&
            harmonic*fundamental-halfWidth >= f.front() && std::abs(frequency-harmonic*fundamental) <= halfWidth) {
            harmonicBands += power; ++harmonicBins;
        } else { outside += power; noiseBins.push_back(power); }
    }
    if (signalBins == 0 || noiseBins.size() < 4) return r;
    const auto middle = noiseBins.begin()+noiseBins.size()/2;
    std::nth_element(noiseBins.begin(), middle, noiseBins.end());
    const double noisePerBin = *middle;
    r.signalPower = std::max(0.0, signalBand-noisePerBin*signalBins);
    r.harmonicPower = std::max(0.0, harmonicBands-noisePerBin*harmonicBins);
    r.noisePower = outside+noisePerBin*(signalBins+harmonicBins);
    if (r.signalPower <= 0 || r.signalPower < 4*noisePerBin*signalBins) return r;
    const auto ratioDb = [](double numerator, double denominator) {
        return denominator > 0 ? 10*std::log10(numerator/denominator) : std::numeric_limits<double>::infinity();
    };
    r.snrDb = ratioDb(r.signalPower, r.noisePower);
    r.sinadDb = ratioDb(r.signalPower, r.harmonicPower+r.noisePower);
    r.thdDb = r.harmonicPower > 0 ? 10*std::log10(r.harmonicPower/r.signalPower) : -std::numeric_limits<double>::infinity();
    r.thdnDb = 10*std::log10((r.harmonicPower+r.noisePower)/r.signalPower);
    r.thdPercent = 100*std::sqrt(r.harmonicPower/r.signalPower);
    r.thdnPercent = 100*std::sqrt((r.harmonicPower+r.noisePower)/r.signalPower);
    r.valid = true;
    return r;
}
