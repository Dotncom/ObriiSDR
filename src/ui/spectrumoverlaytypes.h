#ifndef SPECTRUMOVERLAYTYPES_H
#define SPECTRUMOVERLAYTYPES_H

#include <QString>

struct GraphBandMarker {
    double startHz = 0.0;
    double endHz = 0.0;
    QString label;
    bool amateur = false;
};

struct SpectrumScienceMarker {
    bool enabled = false;
    double frequencyHz = 0.0;
    float levelDb = -160.0f;
    QString label;
};

#endif // SPECTRUMOVERLAYTYPES_H
