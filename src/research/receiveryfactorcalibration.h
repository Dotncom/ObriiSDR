#ifndef RECEIVERYFACTORCALIBRATION_H
#define RECEIVERYFACTORCALIBRATION_H

#include "receivernoisebaseline.h"

#include <QByteArray>
#include <QVector>

#include <vector>

enum class YFactorSourceMode {
    Temperatures = 0,
    Enr = 1
};

enum class YFactorCaptureStage {
    None = 0,
    Cold = 1,
    Hot = 2
};

struct ReceiverYFactorProfile {
    ReceiverNoiseBaselineKey key;
    YFactorSourceMode sourceMode = YFactorSourceMode::Temperatures;
    double coldTemperatureKelvin = 290.0;
    double hotTemperatureKelvin = 0.0;
    double enrDb = 15.0;
    double minimumFrequencyHz = 0.0;
    double maximumFrequencyHz = 0.0;
    QVector<float> coldPower;
    QVector<float> hotPower;
    QVector<float> equivalentNoiseTemperatureKelvin;
    QVector<float> noiseFigureDb;
    int coldFrames = 0;
    int hotFrames = 0;
    int validBins = 0;
    qint64 capturedUtcMs = 0;

    bool isValid() const;
};

class ReceiverYFactorCalibration {
public:
    enum class CaptureResult {
        Ignored,
        Accepted,
        ColdCompleted,
        Completed,
        Rejected
    };

    void beginColdCapture(const ReceiverNoiseBaselineKey &key, int targetFrames);
    bool beginHotCapture(const ReceiverNoiseBaselineKey &key,
                         int targetFrames,
                         YFactorSourceMode sourceMode,
                         double coldTemperatureKelvin,
                         double hotTemperatureKelvin,
                         double enrDb,
                         QString *error = nullptr);
    CaptureResult addFrame(const std::vector<float> &frequencies,
                           const std::vector<float> &levelsDb,
                           const ReceiverNoiseBaselineKey &key);
    void cancelCapture();
    void clearPendingCold();

    bool isCapturing() const { return captureStage != YFactorCaptureStage::None; }
    YFactorCaptureStage stage() const { return captureStage; }
    int capturedFrames() const { return stageFrames; }
    int targetFrames() const { return stageTargetFrames; }
    bool coldReady(const ReceiverNoiseBaselineKey &key) const;

    bool applyInputReferred(const std::vector<float> &frequencies,
                            std::vector<float> &levelsDb,
                            const ReceiverNoiseBaselineKey &key,
                            double targetColdFloorDb) const;
    bool hasMatchingProfile(const ReceiverNoiseBaselineKey &key) const;
    ReceiverYFactorProfile matchingProfile(const ReceiverNoiseBaselineKey &key) const;
    int profileCount() const { return profiles.size(); }
    bool clearMatching(const ReceiverNoiseBaselineKey &key);
    void clearAll();

    QByteArray serialize() const;
    bool deserialize(const QByteArray &encoded, QString *error = nullptr);

private:
    int matchingProfileIndex(const ReceiverNoiseBaselineKey &key) const;
    bool appendCaptureFrame(const std::vector<float> &frequencies,
                            const std::vector<float> &levelsDb,
                            QString *error);
    void finishColdCapture();
    bool finishHotCapture(QString *error);

    QVector<ReceiverYFactorProfile> profiles;
    YFactorCaptureStage captureStage = YFactorCaptureStage::None;
    ReceiverNoiseBaselineKey captureKey;
    int stageFrames = 0;
    int stageTargetFrames = 0;
    double stageMinimumFrequencyHz = 0.0;
    double stageMaximumFrequencyHz = 0.0;
    QVector<double> stagePowerSum;

    ReceiverNoiseBaselineKey pendingColdKey;
    double pendingMinimumFrequencyHz = 0.0;
    double pendingMaximumFrequencyHz = 0.0;
    QVector<float> pendingColdPower;
    int pendingColdFrames = 0;

    YFactorSourceMode pendingSourceMode = YFactorSourceMode::Temperatures;
    double pendingColdTemperatureKelvin = 290.0;
    double pendingHotTemperatureKelvin = 580.0;
    double pendingEnrDb = 15.0;
};

#endif // RECEIVERYFACTORCALIBRATION_H
