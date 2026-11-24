/*
 * Mock of the ball's accelerometer service (ball/src/impact.c needs the real sensor and is not compiled
 * by the simulator). It honours the contract in ball/src/impact.h: a LEVEL (Rest / Held / Spinning,
 * Unknown while the sensor is stalled) plus a queue of EVENTS (Hit, stall). The scenario drives the
 * level and injects the events (set_accel / hit / stall in tests/sim/simlib.py); nothing here
 * classifies anything (the classifier has its own tests: tests/accel_class_test.c).
 */
#include "impact.h"
#include "sim_host.h"

int impact_init(void)
{
  return sim_accel_init();
}

enum accel_level impact_level(void)
{
  switch (sim_accel_level())
  {
  case SIM_LEVEL_REST: return ACCEL_REST;
  case SIM_LEVEL_HELD: return ACCEL_HELD;
  case SIM_LEVEL_SPINNING: return ACCEL_SPINNING;
  default: return ACCEL_UNKNOWN;
  }
}

uint32_t impact_level_ms(void)
{
  return sim_accel_level_ms();
}

bool impact_get(struct impact_event *event)
{
  int64_t t;
  int32_t mg;
  bool stall;

  if (!sim_accel_pop(&t, &mg, &stall))
  {
    return false;
  }
  event->time = t;
  event->mg = mg;
  event->stall = stall;
  return true;
}

int32_t impact_take_peak(void) { return sim_accel_peak(); }
int32_t impact_take_min(void) { return sim_accel_min(); }
uint32_t impact_errors(void) { return 0; }
uint32_t impact_resets(void) { return sim_accel_resets(); }

bool impact_log_line(char *buf, size_t size)
{
  (void)buf;
  (void)size;
  return false; /* no LOG_ACCEL_* lines: the scenario knows what it injected */
}

void impact_raw_capture(void) {}
