/*
 * The two host services `src/shared/shared.c` expects, for offline tools.
 *
 * A validator that drives the engine's own player movement has to link the
 * engine's own vector math, and that file calls back into the host to print
 * and to abort. Reimplementing `AngleVectors` inside MAPGEN instead would be
 * the one thing the whole exercise is meant to avoid: a validator that models
 * the game rather than running it.
 *
 * So the math is the engine's and these two are the harness's. An error is
 * fatal here rather than longjmp'ing into a game loop that does not exist,
 * and it says what it was, because a tool that dies silently during a
 * qualification run is a tool that reports a pass.
 */

#include "shared/shared.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void Com_LPrintf(print_type_t type, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(type == PRINT_ALL ? stdout : stderr, fmt, args);
    va_end(args);
}

void Com_Error(error_type_t code, const char *fmt, ...)
{
    va_list args;
    fprintf(stderr, "fatal (%d): ", (int)code);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(3);
}
