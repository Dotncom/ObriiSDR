#ifndef MODULATIONUTILS_H
#define MODULATIONUTILS_H

#include <QImage>
#include <QString>

double defaultBandwidthForModulation(int modulationType);
QString formatBandwidthHz(double bandwidth);
double recommendedFpvDemodBandwidthHz(double detectedWidthHz);
double recommendedDigitalVideoBandwidthHz(double detectedWidthHz);
QImage createSstvTestPattern();

#endif // MODULATIONUTILS_H
