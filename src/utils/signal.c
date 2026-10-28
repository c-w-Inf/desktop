#include "signal.h"

#include <signal.h>

static volatile sig_atomic_t stopRequested;

static void OnStopSignal (int signal) {
    (void)signal;
    stopRequested = 1;
}

void UtilsInstallStopHandlers (void) {
    signal (SIGINT, OnStopSignal);
#ifdef SIGTERM
    signal (SIGTERM, OnStopSignal);
#endif
#ifdef SIGHUP
    signal (SIGHUP, OnStopSignal);
#endif
#ifdef SIGBREAK
    signal (SIGBREAK, OnStopSignal);
#endif
}

bool UtilsStopRequested (void) {
    return stopRequested != 0;
}
