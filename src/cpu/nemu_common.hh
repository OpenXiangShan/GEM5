#ifndef __NEMI_COMMON_H
#define __NEMI_COMMON_H

#include <stdint.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define eprintf(...) fprintf(stdout, ## __VA_ARGS__)

#ifdef WITH_DRAMSIM3
#include "cosimulation.h"

#endif

#endif // __NEMI_COMMON_H
