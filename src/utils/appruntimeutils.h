#ifndef APPRUNTIMEUTILS_H
#define APPRUNTIMEUTILS_H

#include "radiosettings.h"

#include <QJsonValue>
#include <QString>
#include <QVector>

bool spectrumFftSettingsMatch(const RadioSettings &a, const RadioSettings &b);
qint64 agileRfLiveSettleMs(double sampleRate, bool sampleRateChange);
double agileRfAutoBandwidthRatio(double sampleRate);
const char *runStateName(RadioRunState state);
bool firmwareLooksAgile(const QString &firmwareVersion);
bool frequencyListChanged(const QVector<double> &a, const QVector<double> &b, double toleranceHz = 0.5);
bool jsonValuesEquivalent(const QJsonValue &left, const QJsonValue &right);

#endif // APPRUNTIMEUTILS_H
