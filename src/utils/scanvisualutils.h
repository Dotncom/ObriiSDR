#ifndef SCANVISUALUTILS_H
#define SCANVISUALUTILS_H

#include "scanvisualassembler.h"

#include <QJsonObject>
#include <QVector>

#include <vector>

int normalizedScanVisualMode(int value);
ScanVisualMode scanVisualModeFromInt(int value);
int nearestScanFrequencyIndex(const QVector<double> &frequencies, double centerHz);
QVector<ScanVisualSegment> scanSegmentsFromFrame(const QJsonObject &frame);
std::vector<float> actualFrequenciesFromScanSegments(const std::vector<float> &displayFrequencies,
                                                     const QVector<ScanVisualSegment> &segments);
double displayFrequencyForScanActual(double actualFrequencyHz,
                                     const QVector<ScanVisualSegment> &segments,
                                     double fallbackDisplayHz);
double actualFrequencyForScanDisplay(const ScanVisualSegment &segment, double displayFrequencyHz);
ScanVisualFrame windowedScanVisualFrame(const ScanVisualFrame &frame,
                                        double centerDisplayHz,
                                        double spanHz);
bool actualFrequencyInsideScanSegments(double frequencyHz, const QVector<ScanVisualSegment> &segments);
double fallbackActualFrequencyForScanSegments(const QVector<ScanVisualSegment> &segments,
                                              double fallbackHz);

#endif // SCANVISUALUTILS_H
