#include "netutils.h"

#if defined(__EMSCRIPTEN__)
#include "netutils_web.inc"
#else
#include "netutils_native.inc"
#endif
