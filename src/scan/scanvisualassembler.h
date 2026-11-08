#ifndef SCANVISUALASSEMBLER_H
#define SCANVISUALASSEMBLER_H

#include <QVector>
#include <QString>

#include <vector>

struct ScanVisualSegment {
    double startHz = 0.0;
    double endHz = 0.0;
    double centerHz = 0.0;
    double actualStartHz = 0.0;
    double actualEndHz = 0.0;
    double actualCenterHz = 0.0;
    QString label;
};

struct ScanVisualFrame {
    bool valid = false;
    bool mosaic = false;
    bool fresh = true;
    bool showSegmentMarkers = false;
    double minFrequency = 0.0;
    double maxFrequency = 0.0;
    double centerFrequency = 0.0;
    int fftLength = 0;
    int sectorCount = 0;
    std::vector<float> frequencies;
    std::vector<float> actualFrequencies;
    std::vector<float> magnitudes;
    std::vector<float> referenceMagnitudes;
    QVector<ScanVisualSegment> segments;
};

enum class ScanVisualMode {
    CompressedMosaic = 0,
    FloatingTrueAxis = 1,
    PassComposite = 2
};

class ScanVisualAssembler {
public:
    void reset();

    bool configure(const QVector<double> &centerFrequenciesHz,
                   double sampleRateHz,
                   int targetBins,
                   ScanVisualMode mode);

    ScanVisualFrame update(double frameCenterFrequencyHz,
                           const std::vector<float> &frequencies,
                           const std::vector<float> &magnitudes,
                           const std::vector<float> &referenceMagnitudes);

    bool isConfigured() const;

private:
    struct Sector {
        double actualCenterHz = 0.0;
        double actualStartHz = 0.0;
        double actualEndHz = 0.0;
        double displayCenterHz = 0.0;
        double displayStartHz = 0.0;
        double displayEndHz = 0.0;
    };

    int nearestSector(double centerHz) const;
    void rebuildOutputGrid();
    void resetPassBuffers();
    ScanVisualFrame composeFrame(bool includeReference,
                                 const std::vector<float> *levels = nullptr,
                                 const std::vector<float> *referenceLevels = nullptr,
                                 bool fresh = true) const;
    int binForDisplayFrequency(double frequencyHz) const;
    int binForSectorFrequency(const Sector &sector, double frequencyHz) const;
    static float shiftedValueAt(const std::vector<float> &values, int index, int count);

    std::vector<Sector> sectors;
    std::vector<float> outputFrequencies;
    std::vector<float> outputActualFrequencies;
    std::vector<float> outputLevels;
    std::vector<float> outputReferenceLevels;
    std::vector<float> passLevels;
    std::vector<float> passReferenceLevels;
    std::vector<float> completePassLevels;
    std::vector<float> completePassReferenceLevels;
    std::vector<bool> passSectorSeen;
    double configuredSampleRateHz = 0.0;
    double minFrequencyHz = 0.0;
    double maxFrequencyHz = 0.0;
    int outputBinCount = 0;
    ScanVisualMode configuredMode = ScanVisualMode::CompressedMosaic;
    int lastPassSectorIndex = -1;
    bool haveCompletePass = false;
};

#endif // SCANVISUALASSEMBLER_H
