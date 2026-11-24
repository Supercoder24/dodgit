/*
 * dodgit - categorized logging, shared by the ball and the pouch.
 *
 * Every log line belongs to a category with a compile-time switch in shared/settings.h
 * ("Logging" section, LOG_* = 0 or 1). A disabled category compiles to nothing: no formatting,
 * no console print, no Bluetooth queue slot. That matters because the console is slow (115200 baud:
 * a ~100 character line takes ~9 ms, about one Ready ranging round) and the Bluetooth queue drops
 * lines when it is full.
 *
 *   LOG_CAT(LOG_STATE, "State: %s -> %s", from, to);
 *
 * dodgit_log() itself is provided by the application: the ball sends the line to the console and
 * to Bluetooth (ball/src/ble_log.c), the pouch only prints it (pouch/src/main.c).
 */
#ifndef DODGIT_LOG_H
#define DODGIT_LOG_H

#include "settings.h"

/* printf-style; a newline is appended. */
void dodgit_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* flag is one of the LOG_* macros from settings.h (a constant, so the dead branch disappears). */
#define LOG_CAT(flag, ...)       \
  do                             \
  {                              \
    if (flag)                    \
    {                            \
      dodgit_log(__VA_ARGS__);   \
    }                            \
  } while (0)

#endif
