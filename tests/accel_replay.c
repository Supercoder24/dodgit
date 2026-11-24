/*
 * Replay an accelerometer trace through the classifier and print what it decides. For tuning
 * (docs/Accelerometer.md) and for tools/accel_replay_data.py.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Ishared -Iball/src tests/accel_replay.c ball/src/accel_class.c \
 *      -o /tmp/accel_replay
 *   /tmp/accel_replay [-f STEP_MS] < trace.txt
 *
 * Input, one sample per line (what tools/accel_trace.py extract writes): "t_ms mg"; extra columns
 * (x y z) are ignored, '#' lines and blank lines are skipped. Times are printed relative to the
 * first sample. Output, one event per line:
 *
 *   <t> ms LEVEL <from> -> <to> (<from> lasted <n> ms) mean=<mg> trend=<mg> masd=<mg> shape=<SHAPE>
 *   <t> ms HIT <mg> mg
 *   <t> ms HIT_SUPPRESSED <mg> mg          a crossing inside the cooldown
 *   <t> ms FEATURES ...                    only with -f: every STEP_MS, for threshold tuning
 *   SUMMARY samples=<n> span=<ms> UNKNOWN=<ms> REST=<ms> HELD=<ms> SPINNING=<ms> changes=<n>
 *           hits=<n> suppressed=<n> shortest=<ms>
 *
 * shortest is the length of the shortest level that was left again (the flicker check; -1 if the
 * level never changed after the first one).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "accel_class.h"

int main(int argc, char **argv)
{
  uint32_t step_ms = 0; /* 0 = no feature lines */
  if (argc == 3 && strcmp(argv[1], "-f") == 0)
  {
    step_ms = (uint32_t)atoi(argv[2]);
  }
  else if (argc != 1)
  {
    fprintf(stderr, "usage: %s [-f STEP_MS] < trace.txt\n", argv[0]);
    return 2;
  }

  struct accel_class ac;
  accel_class_reset(&ac);
  char line[256];
  uint32_t t0 = 0, prev_t = 0, level_start = 0, next_feat = 0;
  uint32_t level_ms[4] = {0};
  uint32_t samples = 0, changes = 0, hits = 0, suppressed = 0;
  int64_t shortest = -1;
  enum accel_level level = ACCEL_UNKNOWN;

  while (fgets(line, sizeof line, stdin))
  {
    long t_in, mg;
    if (line[0] == '#' || sscanf(line, "%ld %ld", &t_in, &mg) != 2)
    {
      continue;
    }
    uint32_t t = (uint32_t)t_in;
    if (samples == 0)
    {
      t0 = prev_t = t;
    }
    uint32_t rel = t - t0;
    level_ms[level] += t - prev_t; /* the interval since the previous sample belongs to its level */
    prev_t = t;

    struct accel_result r = accel_class_update(&ac, t, (int32_t)mg);
    struct accel_features f = accel_class_features(&ac);
    if (r.hit)
    {
      printf("%u ms HIT %d mg\n", rel, r.hit_mg);
      hits++;
    }
    if (r.hit_suppressed)
    {
      printf("%u ms HIT_SUPPRESSED %d mg\n", rel, r.hit_mg);
      suppressed++;
    }
    if (r.changed)
    {
      uint32_t lasted = rel - level_start;
      printf("%u ms LEVEL %s -> %s (%s lasted %u ms) mean=%d trend=%d masd=%d shape=%s\n", rel,
             accel_level_name(level), accel_level_name(r.level), accel_level_name(level), lasted,
             f.mean_mg, f.trend_mg, f.masd_mg, accel_shape_name(f.shape));
      if (shortest < 0 || lasted < shortest)
      {
        shortest = lasted;
      }
      level = r.level;
      level_start = rel;
      changes++;
    }
    if (step_ms && rel >= next_feat)
    {
      printf("%u ms FEATURES level=%s shape=%s mean=%d trend=%d masd=%d n=%u in_level=%u ms\n", rel,
             accel_level_name(r.level), accel_shape_name(f.shape), f.mean_mg, f.trend_mg, f.masd_mg,
             f.samples, accel_class_level_ms(&ac));
      next_feat = rel + step_ms;
    }
    samples++;
  }
  printf("SUMMARY samples=%u span=%u UNKNOWN=%u REST=%u HELD=%u SPINNING=%u changes=%u hits=%u "
         "suppressed=%u shortest=%lld\n",
         samples, prev_t - t0, level_ms[ACCEL_UNKNOWN], level_ms[ACCEL_REST], level_ms[ACCEL_HELD],
         level_ms[ACCEL_SPINNING], changes, hits, suppressed, (long long)shortest);
  return 0;
}
