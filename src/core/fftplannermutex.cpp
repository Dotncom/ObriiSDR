#include "fftplannermutex.h"
std::mutex &fftwPlannerGlobalMutex() {
    static std::mutex mutex;
    return mutex;
}
