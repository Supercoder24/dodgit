#ifndef DODGIT_TELEMETRY_H
#define DODGIT_TELEMETRY_H
#include <stdint.h>
#include <stdbool.h>
int telemetry_init(void);
void telemetry(const char *fmt, ...);
uint32_t telemetry_dropped(void);
bool telemetry_connected(void);
#endif
