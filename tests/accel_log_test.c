/* Host test of the accelerometer log lines and buffers (ball/src/accel_log.[ch]): line formats, the
 * BURST window recorder and the RAW ring and its paced dump.
 *   cc -std=c11 -Wall -Wextra -Werror -Ishared -Iball/src tests/accel_log_test.c ball/src/accel_log.c \
 *      ball/src/accel_class.c -o /tmp/accel_log_test && /tmp/accel_log_test */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "accel_log.h"

static char line[ACCEL_LOG_LINE_SIZE];

static void test_class_lines(void)
{
  size_t n = accel_log_level_line(line, sizeof(line), 12345, ACCEL_HELD, ACCEL_SPINNING, 678);
  assert(strcmp(line, "ACCEL level HELD -> SPINNING up=12345 after=678") == 0);
  assert(n == strlen(line));

  accel_log_level_line(line, sizeof(line), 5, ACCEL_UNKNOWN, ACCEL_REST, 0);
  assert(strcmp(line, "ACCEL level UNKNOWN -> REST up=5 after=0") == 0);

  accel_log_hit_line(line, sizeof(line), 4000, 32767, ACCEL_SPINNING);
  assert(strcmp(line, "ACCEL hit up=4000 mg=32767 level=SPINNING") == 0);

  accel_log_stall_line(line, sizeof(line), 9000, 3, false);
  assert(strcmp(line, "ACCEL stalled up=9000, sensor re-initialized (reset #3, still no data)") == 0);
  accel_log_stall_line(line, sizeof(line), 9250, 4, true);
  assert(strcmp(line, "ACCEL stalled up=9250, sensor re-initialized (reset #4, data is back)") == 0);

  struct accel_features f = {.mean_mg = 1003, .trend_mg = 12, .masd_mg = 7, .samples = 19,
                             .shape = ACCEL_SHAPE_CALM};
  accel_log_feat_line(line, sizeof(line), 777, ACCEL_REST, &f);
  assert(strcmp(line, "FEAT up=777 level=REST shape=CALM mean=1003 trend=12 masd=7 samples=19") == 0);

  /* the longest possible values still fit a log queue slot, and a short buffer truncates cleanly */
  f = (struct accel_features){.mean_mg = -2147483647, .trend_mg = 2147483647, .masd_mg = 2147483647,
                              .samples = 65535, .shape = ACCEL_SHAPE_MIXED};
  n = accel_log_feat_line(line, sizeof(line), 4294967295u, ACCEL_SPINNING, &f);
  assert(n < ACCEL_LOG_LINE_SIZE && n == strlen(line));
  n = accel_log_level_line(line, 10, 1, ACCEL_REST, ACCEL_HELD, 1);
  assert(n == 9 && strlen(line) == 9);
}

/* ---- BURST ---------------------------------------------------------------------------------- */

static void feed(struct accel_burst_rec *r, uint32_t t, int32_t mg, bool *completed)
{
  if (accel_burst_sample(r, t, mg))
  {
    *completed = true;
  }
}

static void test_burst_window(void)
{
  static struct accel_burst_rec r; /* zero = ready */
  bool done = false;
  uint32_t t = 1000;

  /* 30 samples of history (less than BURST_PRE_SAMPLES), 5 ms apart, mg = 1000 + index */
  for (int i = 0; i < 30; i++, t += 5)
  {
    feed(&r, t, 1000 + i, &done);
  }
  assert(!done && !r.active);

  /* the crossing sample: 4000 mg */
  feed(&r, t, 4000, &done);
  uint32_t trigger_t = t;
  t += 5;
  assert(accel_burst_trigger(&r, trigger_t, 4000, ACCEL_BURST_HIT, ACCEL_SPINNING));
  assert(r.active && r.win.n == 31 && r.win.n_pre == 31 && r.win.seq == 1);
  assert(r.win.first_ms == 1000 && r.win.trigger_ms == trigger_t);
  assert(r.win.mg[0] == 1000 && r.win.mg[29] == 1029 && r.win.mg[30] == 4000);
  assert(r.win.t_off[0] == 0 && r.win.t_off[30] == 150);

  /* a second crossing inside the window is only counted */
  feed(&r, t, 3500, &done);
  assert(!accel_burst_trigger(&r, t, 3500, ACCEL_BURST_COOLDOWN, ACCEL_SPINNING));
  t += 5;
  assert(r.win.extra == 1 && r.win.seq == 1);

  /* the post samples complete the window, at exactly BURST_POST_SAMPLES (one was fed already) */
  int fed = 1;
  while (!done)
  {
    feed(&r, t, 1100, &done);
    t += 5;
    fed++;
  }
  assert(fed == BURST_POST_SAMPLES && !r.active);
  assert(r.win.n == 31 + BURST_POST_SAMPLES && r.win.n_pre == 31);
  assert(r.win.mg[31] == 3500 && r.win.t_off[31] == 155);

  /* header and data lines */
  accel_burst_header_line(line, sizeof(line), &r.win);
  char expect[160];
  snprintf(expect, sizeof(expect), "BURST #1 trigger up=%u mg=4000 hit level=SPINNING also=1 samples=%d pre=31",
           (unsigned)trigger_t, 31 + BURST_POST_SAMPLES);
  assert(strcmp(line, expect) == 0);

  uint16_t pos = 0;
  int lines = 0;
  size_t values = 0;
  while (pos < r.win.n)
  {
    pos = accel_burst_data_line(line, sizeof(line), &r.win, pos);
    assert(strncmp(line, "BURST #1 ", 9) == 0 && strlen(line) < ACCEL_LOG_LINE_SIZE);
    for (const char *p = strchr(line, ':'); p && *p; p++)
    {
      values += (*p == ' ');
    }
    lines++;
  }
  assert(pos == r.win.n && values == r.win.n);
  assert(lines == (r.win.n + ACCEL_BURST_LINE_SAMPLES - 1) / ACCEL_BURST_LINE_SAMPLES);

  /* first data line: offsets are relative to the trigger and the values are the oldest samples */
  accel_burst_data_line(line, sizeof(line), &r.win, 0);
  assert(strncmp(line, "BURST #1 -150..-75 ms: 1000 1001 1002", 37) == 0);
  /* a window can follow immediately (the next crossing starts window 2) */
  feed(&r, t, 3000, &done);
  assert(accel_burst_trigger(&r, t, 3000, ACCEL_BURST_COOLDOWN, ACCEL_HELD));
  assert(r.win.seq == 2 && r.win.kind == ACCEL_BURST_COOLDOWN && r.win.extra == 0);
  accel_burst_header_line(line, sizeof(line), &r.win);
  assert(strstr(line, "BURST #2 trigger") == line && strstr(line, " cooldown level=HELD also=0 "));
}

static void test_burst_ring_wrap_and_break(void)
{
  static struct accel_burst_rec r;
  bool done = false;
  uint32_t t = 0;

  /* more samples than the history holds: only the newest BURST_PRE_SAMPLES are kept, in order */
  for (int i = 0; i < 3 * BURST_PRE_SAMPLES + 7; i++, t += 5)
  {
    feed(&r, t, i, &done);
  }
  assert(accel_burst_trigger(&r, t - 5, 9999, ACCEL_BURST_HIT, ACCEL_REST));
  assert(r.win.n == BURST_PRE_SAMPLES);
  int newest = 3 * BURST_PRE_SAMPLES + 6;
  assert(r.win.mg[BURST_PRE_SAMPLES - 1] == newest);
  assert(r.win.mg[0] == newest - (BURST_PRE_SAMPLES - 1));
  for (int k = 1; k < BURST_PRE_SAMPLES; k++)
  {
    assert(r.win.mg[k] == r.win.mg[k - 1] + 1 && r.win.t_off[k] == r.win.t_off[k - 1] + 5);
  }

  /* a stall while collecting: the window ends with what it has, the history is forgotten */
  feed(&r, t, 1000, &done);
  feed(&r, t + 5, 1000, &done);
  assert(!done && r.active);
  assert(accel_burst_break(&r));
  assert(!r.active && r.len == 0 && r.win.n == BURST_PRE_SAMPLES + 2);
  assert(!accel_burst_break(&r));
  assert(!accel_burst_trigger(&r, t + 400, 4000, ACCEL_BURST_HIT, ACCEL_HELD)); /* no history yet */

  /* values beyond int16 are clipped in the window, not in the header */
  feed(&r, t + 400, 70000, &done);
  assert(accel_burst_trigger(&r, t + 400, 70000, ACCEL_BURST_HIT, ACCEL_HELD));
  assert(r.win.mg[0] == INT16_MAX && r.win.trigger_mg == 70000);
  accel_burst_header_line(line, sizeof(line), &r.win);
  assert(strstr(line, "mg=70000 hit"));
}

/* ---- RAW ------------------------------------------------------------------------------------ */

static struct accel_raw_ring ring;

/* Dump the ring and parse the lines back: returns the number of samples read, checks the format. */
static unsigned parse_dump(uint32_t seq, uint32_t *up, uint32_t *rate, unsigned *header_samples,
                           int16_t (*xyz)[3], uint32_t *t_of_sample, unsigned max)
{
  struct accel_raw_dump d;
  unsigned n = 0, lines = 0;
  bool began = false, ended = false;
  uint32_t prev_offset_end = 0;

  accel_raw_dump_begin(&d, seq);
  while (accel_raw_dump_line(&d, &ring, line, sizeof(line)))
  {
    assert(strlen(line) < ACCEL_LOG_LINE_SIZE);
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "RAW #%u ", (unsigned)seq);
    assert(strncmp(line, prefix, strlen(prefix)) == 0);
    const char *rest = line + strlen(prefix);
    if (!began)
    {
      assert(sscanf(rest, "begin up=%u rate=%u samples=%u", up, rate, header_samples) == 3);
      began = true;
    }
    else if (strcmp(rest, "end") == 0)
    {
      assert(accel_raw_dump_done(&d));
      ended = true;
    }
    else
    {
      assert(!ended && !accel_raw_dump_done(&d));
      char *end;
      uint32_t offset = (uint32_t)strtoul(rest, &end, 10);
      assert(*end == ':' && offset >= prev_offset_end);
      uint32_t samples_on_line = 0;
      for (const char *p = end + 1; *p == ' ';)
      {
        int x, y, z, used;
        assert(sscanf(p + 1, "%d,%d,%d%n", &x, &y, &z, &used) == 3);
        assert(n < max);
        xyz[n][0] = (int16_t)x;
        xyz[n][1] = (int16_t)y;
        xyz[n][2] = (int16_t)z;
        if (samples_on_line == 0)
        {
          t_of_sample[n] = offset;
        }
        n++;
        samples_on_line++;
        p += 1 + used;
      }
      assert(samples_on_line >= 1 && samples_on_line <= ACCEL_RAW_PER_LINE);
      prev_offset_end = offset;
    }
    lines++;
  }
  assert(began && ended && accel_raw_dump_done(&d) && lines >= 2);
  assert(!accel_raw_dump_line(&d, &ring, line, sizeof(line)));
  return n;
}

static void test_raw_dump(void)
{
  static int16_t xyz[ACCEL_RAW_RING_SIZE][3];
  static uint32_t first_t[ACCEL_RAW_RING_SIZE];
  uint32_t up, rate;
  unsigned header_samples;

  /* empty ring: header and end only */
  accel_raw_reset(&ring);
  assert(parse_dump(1, &up, &rate, &header_samples, xyz, first_t, ACCEL_RAW_RING_SIZE) == 0);
  assert(header_samples == 0 && rate == 0);

  /* a few samples, 5 ms apart: 3 data lines (8 + 8 + 4), rate 200 Hz */
  uint32_t t = 100000;
  for (int i = 0; i < 20; i++, t += 5)
  {
    accel_raw_push(&ring, t, i, -i, 1000 + i);
  }
  assert(parse_dump(2, &up, &rate, &header_samples, xyz, first_t, ACCEL_RAW_RING_SIZE) == 20);
  assert(up == 100000 && rate == 200 && header_samples == 20);
  for (int i = 0; i < 20; i++)
  {
    assert(xyz[i][0] == i && xyz[i][1] == -i && xyz[i][2] == 1000 + i);
  }
  assert(first_t[0] == 0 && first_t[8] == 40 && first_t[16] == 80);

  /* the exact lines */
  struct accel_raw_dump d;
  accel_raw_dump_begin(&d, 7);
  accel_raw_dump_line(&d, &ring, line, sizeof(line));
  assert(strcmp(line, "RAW #7 begin up=100000 rate=200 samples=20") == 0);
  accel_raw_dump_line(&d, &ring, line, sizeof(line));
  assert(strcmp(line, "RAW #7 0: 0,0,1000 1,-1,1001 2,-2,1002 3,-3,1003 4,-4,1004 5,-5,1005 6,-6,1006 7,-7,1007") == 0);

  /* the ring wraps: after 3 rings' worth only the newest ACCEL_RAW_RING_SIZE samples remain, in order,
   * with the jittery spacing (5 or 6 ms) preserved */
  accel_raw_reset(&ring);
  uint32_t total_ms = 0;
  t = 50;
  unsigned pushed = 3 * ACCEL_RAW_RING_SIZE + 13;
  uint32_t times[3 * ACCEL_RAW_RING_SIZE + 13];
  for (unsigned i = 0; i < pushed; i++)
  {
    times[i] = t;
    accel_raw_push(&ring, t, (int32_t)i, 0, 0);
    t += (i % 3) ? 5 : 6;
  }
  unsigned n = parse_dump(3, &up, &rate, &header_samples, xyz, first_t, ACCEL_RAW_RING_SIZE);
  assert(n == ACCEL_RAW_RING_SIZE && header_samples == ACCEL_RAW_RING_SIZE);
  unsigned oldest = pushed - ACCEL_RAW_RING_SIZE;
  assert(up == times[oldest]);
  for (unsigned i = 0; i < n; i += ACCEL_RAW_PER_LINE) /* the offset of each line's first sample is exact */
  {
    assert((uint16_t)xyz[i][0] == (uint16_t)(oldest + i));
    assert(first_t[i] == times[oldest + i] - times[oldest]);
  }
  total_ms = times[pushed - 1] - times[oldest];
  assert(total_ms >= ACCEL_RAW_CAPTURE_MS - ACCEL_SAMPLE_PERIOD_MS); /* the ring covers the capture time */
  assert(rate >= 180 && rate <= 190); /* 5,5,6 ms spacing: 3 samples per 16 ms = 187.5 Hz */
}

static void test_raw_extremes(void)
{
  /* the widest possible samples: lines are shorter than the log slot, and every sample is printed
   * exactly once (a line simply holds fewer than 8 of them) */
  static int16_t xyz[ACCEL_RAW_RING_SIZE][3];
  static uint32_t first_t[ACCEL_RAW_RING_SIZE];
  uint32_t up, rate;
  unsigned header_samples;

  accel_raw_reset(&ring);
  for (int i = 0; i < 100; i++)
  {
    accel_raw_push(&ring, 4294967200u + (uint32_t)i * 2, (i & 1) ? -40000 : 40000, -32768, -16000);
  }
  assert(parse_dump(4, &up, &rate, &header_samples, xyz, first_t, ACCEL_RAW_RING_SIZE) == 100);
  assert(xyz[0][0] == INT16_MAX && xyz[1][0] == INT16_MIN && xyz[0][1] == INT16_MIN && xyz[0][2] == -16000);
  assert(up == 4294967200u && rate == 500); /* the clock wraps inside this capture */

  struct accel_raw_dump d;
  accel_raw_dump_begin(&d, 4);
  accel_raw_dump_line(&d, &ring, line, sizeof(line));
  accel_raw_dump_line(&d, &ring, line, sizeof(line));
  assert(strlen(line) <= 159);
  unsigned per_line = 0;
  for (const char *p = line; (p = strchr(p, ',')); p++)
  {
    per_line++;
  }
  assert(per_line / 2 < ACCEL_RAW_PER_LINE); /* 8 samples of this width do not fit */

  /* a stall: a gap longer than 16 bits saturates but the other offsets stay right */
  accel_raw_reset(&ring);
  accel_raw_push(&ring, 1000, 1, 2, 3);
  accel_raw_push(&ring, 1010, 1, 2, 3);
  accel_raw_push(&ring, 1010 + 200000, 4, 5, 6);
  accel_raw_push(&ring, 1015 + 200000, 4, 5, 6);
  assert(parse_dump(5, &up, &rate, &header_samples, xyz, first_t, ACCEL_RAW_RING_SIZE) == 4);
  assert(ring.s[2].dt_ms == UINT16_MAX && up == 1015 + 200000 - (10 + 65535 + 5));

  /* a buffer too small for any sample cannot make the dump loop forever */
  accel_raw_reset(&ring);
  for (int i = 0; i < 5; i++)
  {
    accel_raw_push(&ring, (uint32_t)i * 5, 1000, 1000, 1000);
  }
  accel_raw_dump_begin(&d, 6);
  char tiny[12];
  int calls = 0;
  while (accel_raw_dump_line(&d, &ring, tiny, sizeof(tiny)))
  {
    assert(strlen(tiny) < sizeof(tiny) && ++calls < 100);
  }
  assert(accel_raw_dump_done(&d));
}

static void test_raw_pacing(void)
{
  /* the pacing arithmetic documented in accel_log.h: a full ring, 8 samples per line, one line per
   * ACCEL_RAW_LINE_MS, never more than 4 KB/s of the longest possible line */
  unsigned lines = (ACCEL_RAW_RING_SIZE + ACCEL_RAW_PER_LINE - 1) / ACCEL_RAW_PER_LINE + 2;
  unsigned dump_ms = lines * ACCEL_RAW_LINE_MS;
  assert(dump_ms >= ACCEL_RAW_CAPTURE_MS * 9 / 10 && dump_ms <= ACCEL_RAW_CAPTURE_MS * 12 / 10);
  assert(159u * 1000u / ACCEL_RAW_LINE_MS <= 4096);
  assert(sizeof(struct accel_raw_sample) == 8);
}

int main(void)
{
  test_class_lines();
  test_burst_window();
  test_burst_ring_wrap_and_break();
  test_raw_dump();
  test_raw_extremes();
  test_raw_pacing();
  printf("accel_log_test: OK\n");
  return 0;
}
