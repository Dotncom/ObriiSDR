#ifdef _MSC_VER
#include <intrin.h>
#define __builtin_popcount(value) __popcnt(static_cast<unsigned int>(value))
#endif
