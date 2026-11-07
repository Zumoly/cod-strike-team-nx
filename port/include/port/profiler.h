#ifndef PORT_PROFILER_H
#define PORT_PROFILER_H

#include "port/bionic_thread.h"

// Takes one sample of the thread.  Call from another thread, at a steady rate.
void profiler_sample(b_pthread_t t);

// Writes what was sampled through put, one line per call, and starts over.
void profiler_report(void (*put)(const char *));

#endif
