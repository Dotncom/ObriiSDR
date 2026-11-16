#pragma once
#include "spurcalibrationprofile.h"
#include <vector>
#include <limits>

class SpurLineCalibrator {
public:
    void reset() { sumDb.clear(); sumSquares.clear(); frames = 0; rate = 0; }
    bool addFrame(const std::vector<float> &unshiftedDb, double sampleRate) {
        if (unshiftedDb.size() < 128 || !std::isfinite(sampleRate) || sampleRate <= 0) return false;
        if (sumDb.empty()) { sumDb.resize(unshiftedDb.size(), 0); sumSquares.resize(sumDb.size(), 0); rate = sampleRate; }
        if (sumDb.size() != unshiftedDb.size() || rate != sampleRate) return false;
        // Validate the complete frame before updating any accumulator.
        for (float x : unshiftedDb) if (!std::isfinite(x)) return false;
        const size_t n = sumDb.size();
        for (size_t i = 0; i < n; ++i) {
            const double x = unshiftedDb[(i + n / 2) % n];
            sumDb[i] += x; sumSquares[i] += x * x;
        }
        ++frames;
        return true;
    }
    int frameCount() const { return frames; }
    QVector<SpurMaskEntry> finish(double minimumProminenceDb = 6.0) const {
        QVector<SpurMaskEntry> found;
        if (frames < 4 || sumDb.empty()) return found;
        const int n = static_cast<int>(sumDb.size());
        const double bin = rate / n;
        const int inner = (std::max)(4, static_cast<int>(std::ceil(10000 / bin)));
        const int outer = (std::max)(inner + 12, static_cast<int>(std::ceil(50000 / bin)));
        const int maxBins = (std::max)(1, static_cast<int>(20000 / bin));
        if (n <= outer * 2 + 4) return found;
        std::vector<double> levels(n), prefix(n + 1, 0);
        for (int i = 0; i < n; ++i) { levels[i] = sumDb[i] / frames; prefix[i + 1] = prefix[i] + levels[i]; }
        struct Candidate { int first, last; double peak, baseline; };
        std::vector<Candidate> candidates;
        for (int i = outer; i < n - outer; ++i) {
            const double peak = levels[i];
            if (peak < levels[i - 1] || peak <= levels[i + 1]) continue;
            const double baseline = ((prefix[i - inner] - prefix[i - outer]) +
                (prefix[i + outer + 1] - prefix[i + inner + 1])) / (2.0 * (outer - inner));
            const double variance = (std::max)(0.0, sumSquares[i] / frames - peak * peak);
            const double meanError = std::sqrt(variance / (frames - 1));
            if (peak - baseline < (std::max)((std::clamp)(minimumProminenceDb, 3.0, 30.0), 8.0 * meanError)) continue;
            const double edge = baseline + 3.0;
            int left = i, right = i;
            while (left > 0 && i - left <= maxBins && levels[left - 1] > edge) --left;
            while (right + 1 < n && right - i <= maxBins && levels[right + 1] > edge) ++right;
            // Width is measured on the averaged spectrum. Broad VGA humps are
            // explicitly excluded, even when a noise ripple creates a local maximum.
            if ((right - left + 1) * bin > 20000) continue;
            left = (std::max)(0, left - 2); right = (std::min)(n - 1, right + 2);
            candidates.push_back({left, right, peak, baseline});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            return a.peak - a.baseline > b.peak - b.baseline;
        });
        for (const auto &c : candidates) {
            const double width = (std::min)(20000.0, (c.last - c.first + 1) * bin);
            SpurMaskEntry e{((c.first + c.last) * 0.5 - n / 2) * bin, width,
                static_cast<float>(c.peak - c.baseline), frames};
            bool overlap = false;
            for (const auto &old : found) if (std::abs(e.offsetHz - old.offsetHz) < (e.widthHz + old.widthHz) * 0.5) { overlap = true; break; }
            if (!overlap) found.append(e);
            if (found.size() >= 256) break;
        }
        std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.offsetHz < b.offsetHz; });
        return found;
    }

private:
    std::vector<double> sumDb;
    std::vector<double> sumSquares;
    double rate = 0;
    int frames = 0;
};

// The same correction is used for the spectrum and each raw waterfall FFT row.
// Width is in Hz; changing FFT size does not disable or move the mask.
inline int applyCalibratedSpurLines(std::vector<float> &unshiftedDb,
                                    const QVector<SpurMaskEntry> &mask, double sampleRate) {
    const int n = static_cast<int>(unshiftedDb.size());
    if (n < 16 || !std::isfinite(sampleRate) || sampleRate <= 0) return 0;
    const double bin = sampleRate / n;
    auto level = [&](int index) -> float & { return unshiftedDb[(index + n / 2) % n]; };
    int corrected = 0;
    for (const auto &e : mask) {
        if (!std::isfinite(e.offsetHz) || !std::isfinite(e.widthHz) || e.widthHz <= 0) continue;
        const double width = (std::max)(e.widthHz, 3.0 * bin);
        const double position = e.offsetHz / bin + n / 2;
        if (position < 0 || position >= n) continue;
        const int first = (std::max)(0, static_cast<int>(std::floor(position - width / (2 * bin))));
        const int last = (std::min)(n - 1, static_cast<int>(std::ceil(position + width / (2 * bin))));
        const int guard = (std::max)(3, last - first + 1);
        double baseline = 0; int count = 0;
        for (int side = 0; side < 2; ++side) {
            const int begin = side ? last + guard : first - guard * 3;
            const int end = side ? last + guard * 3 : first - guard;
            for (int i = (std::max)(0, begin); i < (std::min)(n, end); ++i)
                if (std::isfinite(level(i))) { baseline += level(i); ++count; }
        }
        if (count < 4) continue;
        baseline /= count;
        float peak = -std::numeric_limits<float>::infinity();
        for (int i = first; i <= last; ++i) peak = (std::max)(peak, level(i));
        if (peak - baseline < 3.0) continue; // no narrow line here; leave the noise/hump alone
        for (int i = first; i <= last; ++i) {
            float &value = level(i);
            if (std::isfinite(value) && value > baseline) value = static_cast<float>(baseline);
        }
        ++corrected;
    }
    return corrected;
}
