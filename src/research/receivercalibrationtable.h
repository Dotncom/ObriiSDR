#ifndef RECEIVERCALIBRATIONTABLE_H
#define RECEIVERCALIBRATIONTABLE_H

#include <QString>
#include <QVector>
#include <QJsonObject>

#include <vector>

struct ReceiverCalibrationContext {
    QString backendId;
    int inputMode = -1;
    double sampleRateHz = 0.0;
    int lnaGain = -1;
    int vgaGain = -1;
    int rtlGainTenthsDb = -1;
    int gainState = -1;
    QJsonObject toJson() const;
    static ReceiverCalibrationContext fromJson(const QJsonObject &object);
    QString description() const;
};

struct ReceiverCalibrationPoint {
    double frequencyHz = 0.0;
    double frequencyOffsetHz = 0.0;
    double amplitudeOffsetDb = 0.0;
    double uncertaintyDb = 0.0;
    QString note;
    ReceiverCalibrationContext conditions;
};

struct ReceiverCalibrationCorrection {
    bool valid = false;
    double frequencyOffsetHz = 0.0;
    double amplitudeOffsetDb = 0.0;
    double uncertaintyDb = 0.0;
};

class ReceiverCalibrationTable {
public:
    void setPoints(const QVector<ReceiverCalibrationPoint> &points);
    const QVector<ReceiverCalibrationPoint> &points() const { return calibrationPoints; }
    bool isEmpty() const { return calibrationPoints.isEmpty(); }

    ReceiverCalibrationCorrection correctionAt(double frequencyHz) const;
    ReceiverCalibrationCorrection correctionAt(double frequencyHz, const ReceiverCalibrationContext &context) const;
    void setContext(const ReceiverCalibrationContext &context);
    int activePointCount() const { return activePoints.size(); }
    void applyAmplitudeCorrection(const std::vector<float> &frequencies,
                                  std::vector<float> &primary,
                                  std::vector<float> *secondary,
                                  double baseOffsetDb) const;

private:
    double interpolatedAmplitudeOffset(double frequencyHz, int &upperIndex) const;

    QVector<ReceiverCalibrationPoint> calibrationPoints;
    QVector<ReceiverCalibrationPoint> activePoints;
    ReceiverCalibrationContext currentContext;
    bool contextInitialized = false;
    QVector<ReceiverCalibrationPoint> selectedPoints(const ReceiverCalibrationContext &context) const;
};

#endif // RECEIVERCALIBRATIONTABLE_H
