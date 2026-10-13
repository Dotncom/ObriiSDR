#include "radiosettings.h"
#include <vector>

int globalMode = INPUT_RF;
int fftLength = 0;
double globalFrequency = 0.0;
double globalSampleRate = 0.0;
std::vector<float> fftMagnitudes;
std::vector<float> fftFrequencies;
