#ifndef TUNINGUTILS_H
#define TUNINGUTILS_H

#include "radiosettings.h"

#include <QPair>
#include <QString>

double directMaxFrequency(double sampleRate);
double directMinFrequencyForMode(int inputMode, double sampleRate);
QPair<double, double> listeningScanVisibleSpanHz(const RadioSettings &settings);
double rfMinimumCenterFrequency(const RadioSettings &settings);
double rfMaximumCenterFrequency(const RadioSettings &settings);
double rfMinimumListeningFrequency(const RadioSettings &settings);
double rfMaximumFrequency(const RadioSettings &settings);
double autoTuneRoundingStepHz(double frequencyHz, double visibleSpanHz);
double roundAutoTuneFrequencyHz(double frequencyHz, double visibleSpanHz);
void normalizeTuning(RadioSettings &settings, bool preserveCenter = false);
bool shouldOffsetDmrCenterFromListening(const RadioSettings &settings);
bool offsetDmrCenterFromListening(RadioSettings &settings);

int scalePercentToSliderValue(double scalePercent);
double sliderValueToScalePercent(int sliderValue);
QString formatScalePercent(double scalePercent);
QString scaleLabelText(double scalePercent);
float sliderValueToLevel(int sliderValue);
int levelToSliderValue(float level);
QString levelLabelText(const QString &name, float level);

double clampAudioLowPassHz(double hz);
double clampAudioHighPassHz(double hz);
double audioLowPassSliderValueToHz(int value);
int audioLowPassHzToSliderValue(double hz);
double audioHighPassSliderValueToHz(int value);
int audioHighPassHzToSliderValue(double hz);
QString audioFilterFrequencyText(double hz);
QString formatSampleRate(double sampleRate);

double clampHfNoiseCancelDepth(double depth);
int hfNoiseCancelDepthToSliderValue(double depth);
double hfNoiseCancelSliderValueToDepth(int value);
QString hfNoiseCancelDepthLabelText(double depth);
double clampHfNoiseCancelRefGainDb(double gainDb);
int hfNoiseCancelRefGainToSliderValue(double gainDb);
double hfNoiseCancelSliderValueToRefGainDb(int value);
QString hfNoiseCancelRefGainLabelText(double gainDb);
double clampHfNoiseCancelRefDelayNs(double delayNs);
int hfNoiseCancelRefDelayToSliderValue(double delayNs);
double hfNoiseCancelSliderValueToRefDelayNs(int value);
QString hfNoiseCancelRefDelayLabelText(double delayNs);
double clampHfNoiseCancelRefTiltDb(double tiltDb);
int hfNoiseCancelRefTiltToSliderValue(double tiltDb);
double hfNoiseCancelSliderValueToRefTiltDb(int value);
QString hfNoiseCancelRefTiltLabelText(double tiltDb);

#endif // TUNINGUTILS_H
