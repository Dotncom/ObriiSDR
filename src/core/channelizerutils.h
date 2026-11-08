#ifndef CHANNELIZERUTILS_H
#define CHANNELIZERUTILS_H

#include "radiosettings.h"

double channelizerTargetRate(const RadioSettings &settings);
double channelizerOutputRate(const RadioSettings &settings);
double channelizerCutoff(const RadioSettings &settings, double outputRate);
int channelizerFrameSamplesForRate(double outputRate, int baseFrameSamples);

#endif // CHANNELIZERUTILS_H
