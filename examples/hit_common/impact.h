#ifndef DODGIT_IMPACT_H
#define DODGIT_IMPACT_H
#include <stdint.h>
#include <stdbool.h>
struct impact_event { int64_t time; uint32_t id; int32_t mg; };
struct impact_report { uint32_t id; uint32_t age_ms; int32_t mg; };
struct impact_status { int32_t peak_mg; uint32_t errors, dropped; bool ready; };
int impact_init(int32_t threshold_mg, int cooldown_ms, bool queue_events);
bool impact_get(struct impact_event *event);
struct impact_report impact_latest(void);
struct impact_status impact_status(void);
#endif
