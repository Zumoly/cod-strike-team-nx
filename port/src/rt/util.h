// What selfproc.c expects from the android32 runtime's logging.
#ifndef PORT_RT_UTIL_H
#define PORT_RT_UTIL_H

#include <stdio.h>

#define debugPrintf(...) fprintf(stderr, __VA_ARGS__)

#endif
