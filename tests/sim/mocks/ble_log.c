/*
 * Mock of ball/src/ble_log.c: the Bluetooth stack is not simulated. dodgit_log() behaves like the real
 * one towards the console (format into a 160-byte line, printk it), so the console time it costs the
 * ball is the same; nobody is ever connected, so nothing is queued and nothing is dropped.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "ble_log.h"

int ble_log_init(void) { return 0; }
bool ble_log_connected(void) { return false; }
uint32_t ble_log_dropped(void) { return 0; }

void dodgit_log(const char *fmt, ...)
{
  char line[160];
  va_list args;

  va_start(args, fmt);
  vsnprintf(line, sizeof(line) - 1, fmt, args);
  va_end(args);
  size_t n = strlen(line);
  line[n++] = '\n';
  line[n] = '\0';
  printk("%s", line);
}
