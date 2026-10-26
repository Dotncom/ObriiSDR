#ifndef SPECTRUMMOSAICENGINE_H
#define SPECTRUMMOSAICENGINE_H

#include "analyzerframechannel.h"

#include <QVector>

#include <optional>

struct SpectrumMosaicSourceSettings {
    bool enabled = true;
    double edgeDiscardFraction = 0.05;
    double levelOffsetDb = 0.0;
};

struct SpectrumMosaicResult {
    AnalyzerSpectrumFrame frame;
    QVector<int> contributingSources;
    QVector<int> staleSources;
    int uncoveredBins = 0;
    int overlapBins = 0;
};

class SpectrumMosaicEngine final {
public:
    void setMaximumOutputBins(int bins);
    void setMaximumFrameAgeMs(qint64 ageMs);
    void setSourceSettings(int sourceId, const SpectrumMosaicSourceSettings &settings);
    void updateFrame(int sourceId, const AnalyzerSpectrumFrame &frame);
    void clear();

    std::optional<SpectrumMosaicResult> build() const;

private:
    struct SourceState {
        int sourceId = -1;
        SpectrumMosaicSourceSettings settings;
        AnalyzerSpectrumFrame frame;
        bool hasFrame = false;
    };

    SourceState *findOrCreate(int sourceId);
    const SourceState *find(int sourceId) const;

    QVector<SourceState> sources;
    int maximumOutputBins = 65536;
    qint64 maximumFrameAgeMs = 750;
};

#endif
