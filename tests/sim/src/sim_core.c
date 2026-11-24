/*
 * dodgit host co-simulation - scheduler, coroutines, nodes, and the Zephyr kernel / GPIO / random /
 * hwinfo shims. The radio model is in sim_dw.c, the scenario-facing API in sim_api.c.
 */
#include "sim.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>

#include "zephyr/kernel.h"
#include "zephyr/random/random.h"
#include "zephyr/drivers/hwinfo.h"
#include "zephyr/drivers/gpio.h"
#include "sim_host.h"

struct sim S;
struct node *cur_node;

/* ---------------------------------------------------------------------------------------------
 * Random numbers (deterministic)
 * ------------------------------------------------------------------------------------------- */
static uint64_t splitmix(uint64_t *s)
{
  uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

uint64_t rng_next(uint64_t *s)
{
  return splitmix(s);
}

double rng_unit(uint64_t *s)
{
  return (double)(rng_next(s) >> 11) * (1.0 / 9007199254740992.0);
}

/* ---------------------------------------------------------------------------------------------
 * Event heap, ordered by (time, insertion order)
 * ------------------------------------------------------------------------------------------- */
struct event
{
  ps_t t;
  uint64_t seq;
  int kind;
  int node;
  unsigned gen;
  uint64_t a;
  uint64_t b;
  double d;
  char *str;
};

static struct event *heap;
static int heap_n, heap_cap;

static bool ev_less(const struct event *x, const struct event *y)
{
  return x->t < y->t || (x->t == y->t && x->seq < y->seq);
}

static void heap_push(struct event e)
{
  if (heap_n == heap_cap)
  {
    heap_cap = heap_cap ? heap_cap * 2 : 4096;
    heap = realloc(heap, sizeof(*heap) * heap_cap);
  }
  int i = heap_n++;
  while (i > 0)
  {
    int p = (i - 1) / 2;
    if (!ev_less(&e, &heap[p]))
    {
      break;
    }
    heap[i] = heap[p];
    i = p;
  }
  heap[i] = e;
}

static struct event heap_pop(void)
{
  struct event top = heap[0];
  struct event last = heap[--heap_n];
  int i = 0;
  for (;;)
  {
    int c = 2 * i + 1;
    if (c >= heap_n)
    {
      break;
    }
    if (c + 1 < heap_n && ev_less(&heap[c + 1], &heap[c]))
    {
      c++;
    }
    if (!ev_less(&heap[c], &last))
    {
      break;
    }
    heap[i] = heap[c];
    i = c;
  }
  if (heap_n > 0)
  {
    heap[i] = last;
  }
  return top;
}

void sim_push(ps_t t, int kind, int node, unsigned gen, uint64_t a, uint64_t b)
{
  struct event e = {.t = t, .seq = ++S.seq, .kind = kind, .node = node, .gen = gen, .a = a, .b = b};
  heap_push(e);
}

void sim_push_script(ps_t t, int node, int op, int64_t a, double d, int64_t b, const char *str)
{
  if (t < S.now)
  {
    t = S.now;
  }
  struct event e = {.t = t, .seq = ++S.seq, .kind = EV_SCRIPT, .node = node, .gen = (unsigned)op,
                    .a = (uint64_t)a, .b = (uint64_t)b, .d = d, .str = str ? strdup(str) : NULL};
  heap_push(e);
}

struct node *sim_node(int id)
{
  return (id >= 0 && id < SIM_MAX_NODES) ? &S.nodes[id] : NULL;
}

/* ---------------------------------------------------------------------------------------------
 * Coroutines. Stacks are created with makecontext (once), every later switch is _setjmp/_longjmp:
 * about 40 ns instead of a signal-mask system call.
 * ------------------------------------------------------------------------------------------- */
#define NODE_STACK_BYTES (256 * 1024)

static ctx_t main_ctx;
static ctx_t *running = &main_ctx;

static void transfer(ctx_t *to)
{
  ctx_t *from = running;
  if (from == to)
  {
    return;
  }
  running = to;
  cur_node = to->node;
  if (_setjmp(from->jb) == 0)
  {
    _longjmp(to->jb, 1);
  }
}

static void dispatch(ctx_t *self);
static void handle_script(struct event *e);

static ctx_t boot_from;
static ctx_t *booting;
static void (*booting_fn)(void);

static void node_entry(void)
{
  ctx_t *me = booting;
  void (*fn)(void) = booting_fn;

  if (_setjmp(me->jb) == 0)
  {
    swapcontext(&me->uc, &boot_from.uc); /* back to ctx_make(); resumed later by transfer() */
  }
  fn();
  /* main() returned (e.g. hw_init failed): the node is dead until the next power on. Keep this stack
   * as a scheduler stack; it is never resumed because no RESUME event is queued for it. */
  struct node *n = me->node;
  n->exited = true;
  for (;;)
  {
    dispatch(me);
  }
}

static void ctx_make(ctx_t *c, void (*fn)(void))
{
  c->stack = malloc(NODE_STACK_BYTES);
  getcontext(&c->uc);
  c->uc.uc_stack.ss_sp = c->stack;
  c->uc.uc_stack.ss_size = NODE_STACK_BYTES;
  c->uc.uc_link = NULL;
  booting = c;
  booting_fn = fn;
  makecontext(&c->uc, node_entry, 0);
  swapcontext(&boot_from.uc, &c->uc);
}

/* The scheduler loop. Runs on whatever stack is current: the scenario's (main) or a node's. It returns
 * when it is `self`'s turn again, or (main only) when nothing is due before S.limit. */
static void dispatch(ctx_t *self)
{
  for (;;)
  {
    if (heap_n == 0 || heap[0].t > S.limit || S.stop)
    {
      if (self == &main_ctx)
      {
        return;
      }
      transfer(&main_ctx);
      return; /* resumed: our RESUME event was popped by the scheduler */
    }
    struct event e = heap_pop();
    if (e.t > S.now)
    {
      S.now = e.t;
    }
    switch (e.kind)
    {
    case EV_RESUME:
    {
      struct node *n = &S.nodes[e.node];
      if (e.gen != n->gen || !n->powered)
      {
        break;
      }
      if (&n->ctx == self)
      {
        return;
      }
      transfer(&n->ctx);
      return; /* somebody resumed us because our own RESUME fired */
    }
    case EV_SCRIPT:
      handle_script(&e);
      free(e.str);
      break;
    default:
    {
      struct node *n = &S.nodes[e.node];
      dw_event(e.kind, n, e.a, e.b);
      break;
    }
    }
  }
}

void node_sync(struct node *n)
{
  if (S.in_query)
  {
    return;
  }
  if ((heap_n == 0 || heap[0].t > n->t) && n->t <= S.limit && !S.stop)
  {
    if (n->t > S.now)
    {
      S.now = n->t;
    }
    return;
  }
  sim_push(n->t, EV_RESUME, n->id, n->gen, 0, 0);
  dispatch(&n->ctx);
}

void node_cost(struct node *n, double us)
{
  if (!S.in_query)
  {
    n->t += (ps_t)(us * PS_US);
  }
}

/* ---------------------------------------------------------------------------------------------
 * Time as the firmware sees it: Zephyr ticks at 32768 Hz since boot
 * ------------------------------------------------------------------------------------------- */
static int64_t ticks_of(const struct node *n, ps_t t)
{
  __int128 e = (__int128)(t - n->boot_ps) * 32768;
  return (int64_t)(e / PS_S);
}

static ps_t tick_start(const struct node *n, int64_t tick)
{
  __int128 e = (__int128)tick * PS_S;
  return n->boot_ps + (ps_t)((e + 32767) / 32768);
}

static int64_t uptime_ms_at(const struct node *n, ps_t t)
{
  return ticks_of(n, t) * 1000 / 32768;
}

int64_t k_uptime_get(void)
{
  struct node *n = cur_node;
  node_cost(n, 1);
  return uptime_ms_at(n, n->t);
}

uint32_t k_uptime_get_32(void)
{
  return (uint32_t)k_uptime_get();
}

static int32_t sleep_us(int64_t us)
{
  struct node *n = cur_node;

  node_cost(n, 2);
  if (us < 0)
  {
    us = 0;
  }
  int64_t ticks = (us * 32768 + 999999) / 1000000;
  /* Zephyr adds one tick to a relative sleep to cover the partial tick that has already elapsed. */
  n->t = tick_start(n, ticks_of(n, n->t) + ticks + 1);
  node_sync(n);
  return 0;
}

int32_t k_msleep(int32_t ms)
{
  return sleep_us((int64_t)ms * 1000);
}

int32_t k_usleep(int32_t us)
{
  return sleep_us(us);
}

/* ---------------------------------------------------------------------------------------------
 * Console and log capture
 * ------------------------------------------------------------------------------------------- */
static void log_append(struct node *n, ps_t t, const char *text, int len)
{
  if (n->log_len + len + 1 > n->log_cap)
  {
    n->log_cap = n->log_cap ? n->log_cap * 2 : 1 << 16;
    while (n->log_len + len + 1 > n->log_cap)
    {
      n->log_cap *= 2;
    }
    n->logbuf = realloc(n->logbuf, n->log_cap);
  }
  if (n->nlines == n->lines_cap)
  {
    n->lines_cap = n->lines_cap ? n->lines_cap * 2 : 1024;
    n->lines = realloc(n->lines, sizeof(*n->lines) * n->lines_cap);
  }
  memcpy(n->logbuf + n->log_len, text, len);
  n->logbuf[n->log_len + len] = '\0';
  n->lines[n->nlines++] = (struct logline){.t = t, .off = (uint32_t)n->log_len, .len = (uint16_t)len};
  n->log_len += len + 1;
  if (S.watch[0] && S.watch_node == n->id && strstr(n->logbuf + n->lines[n->nlines - 1].off, S.watch))
  {
    S.stop = true;
  }
}

void sim_log_marker(struct node *n, ps_t t, const char *text)
{
  log_append(n, t, text, (int)strlen(text));
}

void vprintk(const char *fmt, va_list ap)
{
  struct node *n = cur_node;
  char tmp[1024];
  int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);

  if (len < 0)
  {
    return;
  }
  if (len >= (int)sizeof(tmp))
  {
    len = sizeof(tmp) - 1;
  }
  for (int i = 0; i < len; i++)
  {
    if (n->line_len == 0)
    {
      n->line_t = n->t;
    }
    if (tmp[i] == '\n')
    {
      log_append(n, n->line_t, n->line, n->line_len);
      n->line_len = 0;
    }
    else if (n->line_len < (int)sizeof(n->line) - 1)
    {
      n->line[n->line_len++] = tmp[i];
    }
  }
  node_cost(n, 3.0 + (double)len * (double)S.console_char_ps / (double)PS_US);
}

void printk(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintk(fmt, ap);
  va_end(ap);
}

/* ---------------------------------------------------------------------------------------------
 * random, hwinfo, GPIO
 * ------------------------------------------------------------------------------------------- */
uint32_t sys_rand32_get(void)
{
  struct node *n = cur_node;

  node_cost(n, 3);
  if (n->rand_force_i < n->rand_force_n)
  {
    return n->rand_force[n->rand_force_i++];
  }
  return (uint32_t)(rng_next(&n->rng) >> 16);
}

ssize_t hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
  struct node *n = cur_node;

  node_cost(n, 5);
  if (length > sizeof(n->dev_id))
  {
    length = sizeof(n->dev_id);
  }
  memcpy(buffer, n->dev_id, length);
  return (ssize_t)length;
}

static void led_record(struct node *n, int pin, int value)
{
  uint8_t pins = n->led_pins;

  if (value)
  {
    pins |= (uint8_t)(1U << pin);
  }
  else
  {
    pins &= (uint8_t)~(1U << pin);
  }
  if (pins == n->led_pins && n->nleds > 0)
  {
    return;
  }
  n->led_pins = pins;
  if (n->nleds > 0 && n->leds[n->nleds - 1].t == n->t)
  {
    n->leds[n->nleds - 1].mask = pins; /* several pins written in one go: keep the final mask */
    return;
  }
  if (n->nleds == n->leds_cap)
  {
    n->leds_cap = n->leds_cap ? n->leds_cap * 2 : 256;
    n->leds = realloc(n->leds, sizeof(*n->leds) * n->leds_cap);
  }
  n->leds[n->nleds++] = (struct ledrec){.t = n->t, .mask = pins};
}

bool gpio_is_ready_dt(const struct gpio_dt_spec *spec)
{
  (void)spec;
  return true;
}

int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags)
{
  if (spec->pin < 4 && (flags & GPIO_OUTPUT_INACTIVE))
  {
    led_record(cur_node, spec->pin, 0);
  }
  return 0;
}

int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value)
{
  if (spec->pin < 4)
  {
    led_record(cur_node, spec->pin, value != 0);
  }
  return 0;
}

int gpio_pin_get_dt(const struct gpio_dt_spec *spec)
{
  struct node *n = cur_node;

  node_sync(n);
  node_cost(n, 1);
  return spec->pin == 8 ? n->button : (n->led_pins >> spec->pin) & 1;
}

/* ---------------------------------------------------------------------------------------------
 * Accelerometer service (the contract of ball/src/impact.h; mocks/impact.c forwards to these)
 * ------------------------------------------------------------------------------------------- */
int sim_accel_init(void)
{
  struct node *n = cur_node;

  node_cost(n, 500);
  return n->accel_fail ? -EIO : 0;
}

int sim_accel_level(void)
{
  struct node *n = cur_node;

  node_sync(n);
  node_cost(n, 1);
  return n->accel_level;
}

uint32_t sim_accel_level_ms(void)
{
  struct node *n = cur_node;

  node_sync(n);
  node_cost(n, 1);
  return (uint32_t)((n->t - n->accel_since) / PS_MS);
}

bool sim_accel_pop(int64_t *time_ms, int32_t *mg, bool *stall)
{
  struct node *n = cur_node;

  node_sync(n);
  node_cost(n, 2);
  if (n->ev_n == 0)
  {
    return false;
  }
  struct accel_ev *e = &n->evq[n->ev_head];
  *time_ms = e->time_ms;
  *mg = e->mg;
  *stall = e->stall;
  n->ev_head = (n->ev_head + 1) % ACCEL_QUEUE;
  n->ev_n--;
  return true;
}

int32_t sim_accel_peak(void)
{
  struct node *n = cur_node;

  node_sync(n);
  switch (n->accel_level)
  {
  case SIM_LEVEL_HELD: return 1400;
  case SIM_LEVEL_SPINNING: return 1200;
  case SIM_LEVEL_REST: return 1010;
  default: return -1;
  }
}

int32_t sim_accel_min(void)
{
  struct node *n = cur_node;

  node_sync(n);
  switch (n->accel_level)
  {
  case SIM_LEVEL_HELD: return 700;
  case SIM_LEVEL_SPINNING: return 900;
  case SIM_LEVEL_REST: return 990;
  default: return -1;
  }
}

uint32_t sim_accel_resets(void)
{
  return cur_node->accel_resets;
}

static void accel_enqueue(struct node *n, int32_t mg, bool stall)
{
  if (n->ev_n == ACCEL_QUEUE)
  {
    n->hits_dropped++; /* queue full: the oldest event is dropped, the newest always gets through (impact.h) */
    n->ev_head = (n->ev_head + 1) % ACCEL_QUEUE;
    n->ev_n--;
  }
  struct accel_ev *e = &n->evq[(n->ev_head + n->ev_n) % ACCEL_QUEUE];
  *e = (struct accel_ev){.time_ms = uptime_ms_at(n, S.now), .mg = mg, .stall = stall};
  n->ev_n++;
}

/* ---------------------------------------------------------------------------------------------
 * Node life cycle
 * ------------------------------------------------------------------------------------------- */
static struct
{
  void *lib;
  char *stack;
} zombies[64];
static int nzombies;

static void reap(void)
{
  for (int i = 0; i < nzombies; i++)
  {
    if (zombies[i].lib)
    {
      dlclose(zombies[i].lib);
    }
    free(zombies[i].stack);
  }
  nzombies = 0;
}

static void node_kill(struct node *n)
{
  if (!n->powered)
  {
    return;
  }
  dw_power_off(n);
  n->powered = false;
  n->gen++;
  if (n->line_len > 0)
  {
    log_append(n, n->line_t, n->line, n->line_len); /* a half-printed line */
    n->line_len = 0;
  }
  if (nzombies < 64)
  {
    zombies[nzombies].lib = n->lib;
    zombies[nzombies].stack = n->ctx.stack;
    nzombies++;
  }
  n->lib = NULL;
  n->ctx.stack = NULL;
}

static int copy_file(const char *from, const char *to)
{
  FILE *in = fopen(from, "rb");
  FILE *out = in ? fopen(to, "wb") : NULL;
  char buf[65536];
  size_t k;

  if (!in || !out)
  {
    if (in)
    {
      fclose(in);
    }
    return -1;
  }
  while ((k = fread(buf, 1, sizeof(buf), in)) > 0)
  {
    fwrite(buf, 1, k, out);
  }
  fclose(in);
  fclose(out);
  return 0;
}

static void node_boot(struct node *n, const char *path)
{
  char copy[640];
  char note[96];

  if (n->powered)
  {
    node_kill(n);
  }
  n->gen++;
  n->boots++;
  n->powered = true;
  n->exited = false;
  n->boot_ps = S.now;
  n->t = S.now;
  n->line_len = 0;
  n->led_pins = 0;
  n->ev_n = n->ev_head = 0;
  uint64_t seed = S.seed ^ ((uint64_t)(n->id + 1) * 0x1000193ULL) ^ ((uint64_t)n->boots * 0x9E3779B1ULL);
  n->rng = seed;
  n->dwrng = seed ^ 0xD00D;
  dw_boot(n);

  snprintf(copy, sizeof(copy), "%s/n%d_%d.simlib", S.copy_dir, n->id, n->boots);
  if (copy_file(path, copy) != 0)
  {
    fprintf(stderr, "sim: cannot copy %s to %s\n", path, copy);
    abort();
  }
  n->lib = dlopen(copy, RTLD_NOW | RTLD_LOCAL);
  if (!n->lib)
  {
    fprintf(stderr, "sim: dlopen(%s): %s\n", copy, dlerror());
    abort();
  }
  void (*fn)(void) = (void (*)(void))dlsym(n->lib, "node_main");
  if (!fn)
  {
    fprintf(stderr, "sim: %s has no node_main\n", copy);
    abort();
  }
  n->f_phase = (int (*)(void))dlsym(n->lib, "uwb_pouch_phase");
  n->f_slot = (int (*)(void))dlsym(n->lib, "uwb_pouch_slot");
  n->f_lives = (uint8_t(*)(void))dlsym(n->lib, "uwb_pouch_lives");
  n->f_uid = (uint32_t(*)(void))dlsym(n->lib, "uwb_pouch_uid");
  n->f_count = (int (*)(void))dlsym(n->lib, "uwb_ball_pouch_count");
  n->f_mask = (uint8_t(*)(void))dlsym(n->lib, "uwb_ball_slot_mask");
  n->f_slot_uid = (uint32_t(*)(int))dlsym(n->lib, "uwb_ball_slot_uid");
  n->f_slot_lives = (int (*)(int))dlsym(n->lib, "uwb_ball_slot_lives");

  n->ctx.node = n;
  ctx_make(&n->ctx, fn);
  sim_push(n->t, EV_RESUME, n->id, n->gen, 0, 0);
  snprintf(note, sizeof(note), "SIM: power on (boot #%d)", n->boots);
  sim_log_marker(n, n->t, note);
}

static void handle_script(struct event *e)
{
  int op = (int)e->gen;
  struct node *n = (e->node >= 0 && e->node < SIM_MAX_NODES) ? &S.nodes[e->node] : NULL;

  switch (op)
  {
  case OP_POWER_ON:
    node_boot(n, e->str);
    break;
  case OP_POWER_OFF:
    if (n->powered)
    {
      sim_log_marker(n, S.now, "SIM: power off");
    }
    node_kill(n);
    break;
  case OP_BUTTON:
    n->button = (int)e->a;
    break;
  case OP_ACCEL_LEVEL:
    if (n->accel_level == SIM_LEVEL_UNKNOWN && n->accel_saved >= 0)
    {
      n->accel_saved = (int)e->a; /* during a stall: applies when the sensor is back */
    }
    else if (n->accel_level != (int)e->a)
    {
      n->accel_level = (int)e->a;
      n->accel_since = S.now;
    }
    break;
  case OP_ACCEL_HIT:
    n->hits_injected++;
    if (n->hit_seen && n->accel_cooldown > 0 && S.now - n->last_hit_ps < n->accel_cooldown)
    {
      n->hits_suppressed++; /* inside the cooldown: part of the previous impact */
      break;
    }
    n->hit_seen = true;
    n->last_hit_ps = S.now;
    n->last_hit_mg = (int32_t)e->a;
    accel_enqueue(n, (int32_t)e->a, false);
    break;
  case OP_ACCEL_STALL:
    n->accel_resets++;
    accel_enqueue(n, 0, true);
    if (n->accel_level != SIM_LEVEL_UNKNOWN)
    {
      n->accel_saved = n->accel_level;
      n->accel_level = SIM_LEVEL_UNKNOWN;
      n->accel_since = S.now;
    }
    sim_push_script(S.now + (ps_t)e->a, e->node, OP_ACCEL_RESTORE, 0, 0, 0, NULL);
    break;
  case OP_ACCEL_RESTORE:
    if (n->accel_level == SIM_LEVEL_UNKNOWN && n->accel_saved >= 0)
    {
      n->accel_level = n->accel_saved;
      n->accel_since = S.now;
      n->accel_saved = -1;
    }
    break;
  case OP_LOSS:
  {
    int src = (int)(e->a & 0xFF), dst = (int)((e->a >> 8) & 0xFF);
    S.loss_p[src][dst] = e->d;
    S.loss_crc[src][dst] = (double)(int64_t)e->b / 1e6;
    break;
  }
  }
}

/* ---------------------------------------------------------------------------------------------
 * Library control (called through sim_api.c)
 * ------------------------------------------------------------------------------------------- */
void sim_core_reset(uint64_t seed, const char *copy_dir)
{
  reap();
  for (int i = 0; i < SIM_MAX_NODES; i++)
  {
    struct node *n = &S.nodes[i];
    if (n->powered)
    {
      node_kill(n);
    }
  }
  reap();
  for (int i = 0; i < SIM_MAX_NODES; i++)
  {
    struct node *n = &S.nodes[i];
    free(n->logbuf);
    free(n->lines);
    free(n->leds);
    free(n->wps);
    free(n->ranges);
  }
  free(S.trace);
  for (int i = 0; i < heap_n; i++)
  {
    free(heap[i].str);
  }
  heap_n = 0;
  memset(&S, 0, sizeof(S));
  running = &main_ctx;
  cur_node = NULL;
  S.seed = seed;
  S.airrng = seed ^ 0xA1A1A1A1ULL;
  S.console_char_ps = 86806000; /* 115200 baud, 10 bits per character */
  S.trace_on = true;
  snprintf(S.copy_dir, sizeof(S.copy_dir), "%s", copy_dir);
  for (int i = 0; i < SIM_MAX_NODES; i++)
  {
    struct node *n = &S.nodes[i];
    n->id = i;
    n->role = i == 0 ? ROLE_BALL : ROLE_POUCH;
    n->clk_offset_dtu = -1;
    n->accel_level = SIM_LEVEL_REST;
    n->accel_saved = -1;
    /* distinct, stable 64-bit "factory" IDs */
    uint64_t s = 0xD0D61700ULL + (uint64_t)i * 7919;
    uint64_t v = splitmix(&s);
    memcpy(n->dev_id, &v, 8);
  }
  S.inited = true;
}

ps_t sim_core_run(ps_t limit, const char *watch, int watch_node)
{
  if (limit < S.now)
  {
    limit = S.now;
  }
  S.limit = limit;
  S.stop = false;
  snprintf(S.watch, sizeof(S.watch), "%s", watch ? watch : "");
  S.watch_node = watch_node;
  dispatch(&main_ctx);
  reap();
  if (!S.stop && S.now < limit)
  {
    S.now = limit;
  }
  S.watch[0] = '\0';
  return S.now;
}
