/*
 * dodgit - accelerometer log lines and buffers. See accel_log.h for the line formats and the
 * ownership rules. Pure C: everything here works on structs the caller provides.
 */
#include <stdarg.h>
#include <stdio.h>
#include "accel_log.h"

#if BURST_POST_SAMPLES < 1
#error "BURST_POST_SAMPLES must be at least 1"
#endif

/* The ring's head and count are 16 bits. */
_Static_assert(ACCEL_RAW_RING_SIZE <= UINT16_MAX, "ACCEL_RAW_CAPTURE_MS is too long for the raw ring");

/* ---- Helpers -------------------------------------------------------------------------------- */

/* A printf() into buf at offset *len that either fits completely (and advances *len) or leaves the
 * line exactly as it was, so a line never ends in the middle of a number. */
static bool append(char *buf, size_t size, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static bool append(char *buf, size_t size, size_t *len, const char *fmt, ...)
{
  if (*len + 1 >= size) /* no room for even one more character plus the NUL */
  {
    if (size > 0)
    {
      buf[*len] = '\0';
    }
    return false;
  }
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf + *len, size - *len, fmt, args);
  va_end(args);
  if (n < 0 || (size_t)n >= size - *len)
  {
    buf[*len] = '\0';
    return false;
  }
  *len += (size_t)n;
  return true;
}

/* The length of a line written by one snprintf() call that returned n. */
static size_t written(int n, size_t size)
{
  if (size == 0)
  {
    return 0;
  }
  if (n < 0)
  {
    return 0;
  }
  return (size_t)n < size ? (size_t)n : size - 1;
}

static int16_t clip16(int32_t v)
{
  return (int16_t)(v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v));
}

/* ---- Class / feature lines ----------------------------------------------------------------- */

size_t accel_log_level_line(char *buf, size_t size, uint32_t up_ms, enum accel_level from,
                            enum accel_level to, uint32_t after_ms)
{
  return written(snprintf(buf, size, "ACCEL level %s -> %s up=%u after=%u", accel_level_name(from),
                          accel_level_name(to), (unsigned)up_ms, (unsigned)after_ms),
                 size);
}

size_t accel_log_hit_line(char *buf, size_t size, uint32_t up_ms, int32_t mg, enum accel_level level)
{
  return written(snprintf(buf, size, "ACCEL hit up=%u mg=%d level=%s", (unsigned)up_ms, (int)mg,
                          accel_level_name(level)),
                 size);
}

size_t accel_log_stall_line(char *buf, size_t size, uint32_t up_ms, uint32_t reset_no, bool data_is_back)
{
  return written(snprintf(buf, size, "ACCEL stalled up=%u, sensor re-initialized (reset #%u, %s)",
                          (unsigned)up_ms, (unsigned)reset_no,
                          data_is_back ? "data is back" : "still no data"),
                 size);
}

size_t accel_log_feat_line(char *buf, size_t size, uint32_t up_ms, enum accel_level level,
                           const struct accel_features *f)
{
  return written(snprintf(buf, size, "FEAT up=%u level=%s shape=%s mean=%d trend=%d masd=%d samples=%u",
                          (unsigned)up_ms, accel_level_name(level), accel_shape_name(f->shape),
                          (int)f->mean_mg, (int)f->trend_mg, (int)f->masd_mg, (unsigned)f->samples),
                 size);
}

/* ---- BURST --------------------------------------------------------------------------------- */

/* Entry `age` steps back from the newest one (0 = newest) of the pre-trigger ring. */
static uint16_t pre_at(const struct accel_burst_rec *r, uint16_t age)
{
  return (uint16_t)((r->next + BURST_PRE_SAMPLES - 1 - age) % BURST_PRE_SAMPLES);
}

static void pre_push(struct accel_burst_rec *r, uint32_t t_ms, int32_t mg)
{
  r->pre[r->next].t = t_ms;
  r->pre[r->next].mg = clip16(mg);
  r->next = (uint16_t)((r->next + 1) % BURST_PRE_SAMPLES);
  if (r->len < BURST_PRE_SAMPLES)
  {
    r->len++;
  }
}

bool accel_burst_sample(struct accel_burst_rec *r, uint32_t t_ms, int32_t mg)
{
  bool complete = false;

  if (r->active)
  {
    struct accel_burst *w = &r->win;
    w->t_off[w->n] = (uint16_t)(t_ms - w->first_ms);
    w->mg[w->n++] = clip16(mg);
    if (--r->post_left == 0)
    {
      r->active = false;
      complete = true;
    }
  }
  pre_push(r, t_ms, mg);
  return complete;
}

bool accel_burst_trigger(struct accel_burst_rec *r, uint32_t t_ms, int32_t mg, enum accel_burst_kind kind,
                         enum accel_level level)
{
  struct accel_burst *w = &r->win;

  if (r->active)
  {
    if (w->extra < UINT8_MAX)
    {
      w->extra++;
    }
    return false;
  }
  if (r->len == 0) /* the crossing sample was not fed: nothing to build a window from */
  {
    return false;
  }

  w->seq = ++r->seq;
  w->trigger_ms = t_ms;
  w->trigger_mg = mg;
  w->kind = (uint8_t)kind;
  w->level = (uint8_t)level;
  w->extra = 0;
  w->first_ms = r->pre[pre_at(r, (uint16_t)(r->len - 1))].t;
  for (uint16_t k = 0; k < r->len; k++) /* oldest first, the crossing sample last */
  {
    uint16_t i = pre_at(r, (uint16_t)(r->len - 1 - k));
    w->t_off[k] = (uint16_t)(r->pre[i].t - w->first_ms);
    w->mg[k] = r->pre[i].mg;
  }
  w->n = w->n_pre = r->len;
  r->post_left = BURST_POST_SAMPLES;
  r->active = true;
  return true;
}

bool accel_burst_break(struct accel_burst_rec *r)
{
  bool was_active = r->active;

  r->next = 0;
  r->len = 0;
  r->active = false;
  return was_active;
}

size_t accel_burst_header_line(char *buf, size_t size, const struct accel_burst *b)
{
  return written(snprintf(buf, size,
                          "BURST #%u trigger up=%u mg=%d %s level=%s also=%u samples=%u pre=%u",
                          (unsigned)b->seq, (unsigned)b->trigger_ms, (int)b->trigger_mg,
                          b->kind == ACCEL_BURST_HIT ? "hit" : "cooldown",
                          accel_level_name((enum accel_level)b->level), (unsigned)b->extra,
                          (unsigned)b->n, (unsigned)b->n_pre),
                 size);
}

uint16_t accel_burst_data_line(char *buf, size_t size, const struct accel_burst *b, uint16_t first)
{
  if (first >= b->n)
  {
    buf[0] = '\0';
    return b->n;
  }
  uint16_t end = (uint16_t)(first + ACCEL_BURST_LINE_SAMPLES < b->n ? first + ACCEL_BURST_LINE_SAMPLES : b->n);
  size_t len = 0;

  /* 16 samples of at most 7 characters (" -32768") plus the prefix (at most 46) make 158 characters at
   * the very most, which fits ACCEL_LOG_LINE_SIZE. */
  append(buf, size, &len, "BURST #%u %+d..%+d ms:", (unsigned)b->seq,
         (int)(int32_t)(b->first_ms + b->t_off[first] - b->trigger_ms),
         (int)(int32_t)(b->first_ms + b->t_off[end - 1] - b->trigger_ms));
  uint16_t i = first;
  while (i < end && append(buf, size, &len, " %d", b->mg[i]))
  {
    i++;
  }
  return end;
}

/* ---- RAW ----------------------------------------------------------------------------------- */

enum raw_stage
{
  RAW_HEADER,
  RAW_DATA,
  RAW_END,
  RAW_DONE,
};

void accel_raw_reset(struct accel_raw_ring *r)
{
  r->head = 0;
  r->count = 0;
  r->t_newest = 0;
}

void accel_raw_push(struct accel_raw_ring *r, uint32_t t_ms, int32_t x, int32_t y, int32_t z)
{
  uint32_t dt = r->count ? (uint32_t)(t_ms - r->t_newest) : 0;
  struct accel_raw_sample *s = &r->s[r->head];

  s->x = clip16(x);
  s->y = clip16(y);
  s->z = clip16(z);
  s->dt_ms = (uint16_t)(dt > UINT16_MAX ? UINT16_MAX : dt);
  r->head = (uint16_t)((r->head + 1) % ACCEL_RAW_RING_SIZE);
  if (r->count < ACCEL_RAW_RING_SIZE)
  {
    r->count++;
  }
  r->t_newest = t_ms;
}

/* Sample i of the ring, 0 = the oldest. */
static const struct accel_raw_sample *raw_at(const struct accel_raw_ring *r, uint16_t i)
{
  return &r->s[(r->head + ACCEL_RAW_RING_SIZE - r->count + i) % ACCEL_RAW_RING_SIZE];
}

/* Time from the oldest to the newest sample (the dt of the oldest one refers to a sample that is gone). */
static uint32_t raw_span_ms(const struct accel_raw_ring *r)
{
  uint32_t span = 0;

  for (uint16_t i = 1; i < r->count; i++)
  {
    span += raw_at(r, i)->dt_ms;
  }
  return span;
}

void accel_raw_dump_begin(struct accel_raw_dump *d, uint32_t seq)
{
  d->seq = seq;
  d->next = 0;
  d->t_off = 0;
  d->stage = RAW_HEADER;
}

bool accel_raw_dump_line(struct accel_raw_dump *d, const struct accel_raw_ring *r, char *buf, size_t size)
{
  size_t len = 0;

  switch (d->stage)
  {
  case RAW_HEADER:
  {
    uint32_t span = raw_span_ms(r);
    uint32_t rate = (r->count > 1 && span > 0) ? ((uint32_t)(r->count - 1) * 1000 + span / 2) / span : 0;
    append(buf, size, &len, "RAW #%u begin up=%u rate=%u samples=%u", (unsigned)d->seq,
           (unsigned)(r->t_newest - span), (unsigned)rate, (unsigned)r->count);
    d->stage = r->count ? RAW_DATA : RAW_END;
    return true;
  }
  case RAW_DATA:
  {
    append(buf, size, &len, "RAW #%u %u:", (unsigned)d->seq, (unsigned)d->t_off);
    for (uint16_t n = 0; n < ACCEL_RAW_PER_LINE && d->next < r->count; n++)
    {
      const struct accel_raw_sample *s = raw_at(r, d->next);
      bool fits = append(buf, size, &len, " %d,%d,%d", s->x, s->y, s->z);

      if (!fits && n > 0)
      {
        break; /* this one starts the next line */
      }
      d->next++; /* (a buffer too small for even one sample drops it rather than looping forever) */
      if (d->next < r->count)
      {
        d->t_off += raw_at(r, d->next)->dt_ms;
      }
      if (!fits)
      {
        break;
      }
    }
    if (d->next >= r->count)
    {
      d->stage = RAW_END;
    }
    return true;
  }
  case RAW_END:
    append(buf, size, &len, "RAW #%u end", (unsigned)d->seq);
    d->stage = RAW_DONE;
    return true;
  case RAW_DONE:
    break;
  }
  if (size > 0)
  {
    buf[0] = '\0';
  }
  return false;
}

bool accel_raw_dump_done(const struct accel_raw_dump *d)
{
  return d->stage == RAW_DONE;
}
