#ifndef RECEIVERNOISEBASELINE_H
#define RECEIVERNOISEBASELINE_H

#include "receivercalibrationtable.h"

#include <QByteArray>
#include <QVector>

#include <vector>

enum class ReceiverNoiseBaselineMode {
    Off = 0,
    RelativeFloor = 1,
    ExcessPower = 2,
    WideShape = 3
};

struct ReceiverNoiseBaselineKey {
    ReceiverCalibrationContext receiver;
    double centerFrequencyHz = 0.0;
    int fftLength = 0;
    int fftWindowType = 0;
    int rfBandwidthHz = 0;
    double amplitudeOffsetDb = 0.0;
    bool calibrationTableEnabled = false;
    int calibrationPointCount = 0;
    QString hardwareIdentity;

    bool matches(const ReceiverNoiseBaselineKey &other) const;
    QString description() const;
};

struct ReceiverNoiseBaselineProfile {
    ReceiverNoiseBaselineKey key;
    double minimumFrequencyHz = 0.0;
    double maximumFrequencyHz = 0.0;
    QVector<float> averagePower;
    int capturedFrames = 0;
    double spreadDb = 0.0;
    qint64 capturedUtcMs = 0;

    bool isValid() const;
};

class ReceiverNoiseBaseline {
public:
    enum class CaptureResult {
        Ignored,
        Accepted,
        Completed,
        Rejected
    };

    void beginCapture(const ReceiverNoiseBaselineKey &key, int targetFrames, int storedBins = 32768);
    CaptureResult addFrame(const std::vector<float> &frequencies,
                           const std::vector<float> &levelsDb,
                           const ReceiverNoiseBaselineKey &key);
    void cancelCapture();

    bool isCapturing() const { return capturing; }
    int capturedFrames() const { return captureFrames; }
    int targetFrames() const { return captureTargetFrames; }
    QString lastStatus() const { return status; }

    bool apply(const std::vector<float> &frequencies,
               std::vector<float> &levelsDb,
               const ReceiverNoiseBaselineKey &key,
               ReceiverNoiseBaselineMode mode,
               double targetFloorDb, double shapeWidthHz = 150000.0, double shapeStrength = 1.0) const;
    bool hasMatchingProfile(const ReceiverNoiseBaselineKey &key, bool wideShape = false) const;
    ReceiverNoiseBaselineProfile matchingProfile(const ReceiverNoiseBaselineKey &key, bool wideShape = false) const;
    int profileCount() const { return profiles.size(); }
    bool clearMatching(const ReceiverNoiseBaselineKey &key, bool wideShape = false);
    void clearAll();

    QByteArray serialize() const;
    bool deserialize(const QByteArray &encoded, QString *error = nullptr);

private:
    int matchingProfileIndex(const ReceiverNoiseBaselineKey &key, bool wideShape = false) const;
    void invalidateShapeCache() { shapeCacheIndex = -1; }
    void prepareShapeCache(int index, double widthHz) const;
    void finishCapture();

    QVector<ReceiverNoiseBaselineProfile> profiles;
    bool capturing = false;
    ReceiverNoiseBaselineKey captureKey;
    int captureFrames = 0;
    int captureTargetFrames = 0;
    double captureMinimumFrequencyHz = 0.0;
    double captureMaximumFrequencyHz = 0.0;
    QVector<double> capturePowerSum;
    QVector<double> capturePowerSquareSum;
    int captureStoredBins = 32768;
    int captureSourceBins = 0;
    mutable int shapeCacheIndex = -1;
    mutable double shapeCacheWidthHz = 0;
    mutable QVector<float> shapeCacheDb;
    mutable double shapeCacheFloorDb = 0;
    QString status;
};

#endif // RECEIVERNOISEBASELINE_H
