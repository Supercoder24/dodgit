/*
 * dodgit host co-simulation - the scenario-facing C API of libsim (called from Python through ctypes,
 * tests/sim/simlib.py) and the hooks the mock layer calls (shim/sim_host.h).
 *
 * Times are int64 picoseconds of global virtual time. Scenario actions (power, button, accelerometer,
 * loss) are queued as events at their time; the run call then advances the virtual clock.
 */
#include "sim.h"

#include <stdlib.h>

#include "sim_host.h"

#define API __attribute__((visibility("default")))

/* ---- control ---------------------------------------------------------------------------------- */
API void sim_reset(uint64_t seed, const char *copy_dir)
{
  sim_core_reset(seed, copy_dir);
}

/* Advance to `limit` (or until a line containing `watch` appears in the log of `watch_node`). Returns the
 * virtual time reached. */
API int64_t sim_run(int64_t limit, const char *watch, int watch_node)
{
  return sim_core_run(limit, watch, watch_node);
}

API int64_t sim_now(void)
{
  return S.now;
}

API void sim_set_console_baud(int baud)
{
  S.console_char_ps = baud > 0 ? (ps_t)(10.0 * 1e12 / baud) : 0;
}

API void sim_set_trace(int on)
{
  S.trace_on = on != 0;
}

/* ---- scenario actions ------------------------------------------------------------------------- */
API void sim_power_on(int node, int64_t t, const char *lib)
{
  sim_push_script(t, node, OP_POWER_ON, 0, 0, 0, lib);
}

API void sim_power_off(int node, int64_t t)
{
  sim_push_script(t, node, OP_POWER_OFF, 0, 0, 0, NULL);
}

API void sim_button(int node, int64_t t, int level)
{
  sim_push_script(t, node, OP_BUTTON, level, 0, 0, NULL);
}

API void sim_accel_level_at(int node, int64_t t, int level)
{
  sim_push_script(t, node, OP_ACCEL_LEVEL, level, 0, 0, NULL);
}

API void sim_accel_hit_at(int node, int64_t t, int mg)
{
  sim_push_script(t, node, OP_ACCEL_HIT, mg, 0, 0, NULL);
}

API void sim_accel_stall_at(int node, int64_t t, int64_t duration)
{
  sim_push_script(t, node, OP_ACCEL_STALL, duration, 0, 0, NULL);
}

/* Random frame loss on the link src -> dst from time t on: probability p; of the lost frames the
 * fraction crc_frac arrives with a CRC error instead of vanishing (the receiver then reports RXFCE). */
API void sim_loss_at(int src, int dst, int64_t t, double p, double crc_frac)
{
  sim_push_script(t, 0, OP_LOSS, (int64_t)(src | (dst << 8)), p, (int64_t)(crc_frac * 1e6), NULL);
}

/* Drop `count` frames (-1: all) of message type `type` (-1: any) sent by node src (-1: any) as heard by
 * node dst (-1: any), that start within [from, to). Returns the rule index (sim_drop_hits). */
API int sim_drop(int type, int src, int dst, int count, int64_t from, int64_t to)
{
  if (S.nrules >= SIM_MAX_RULES)
  {
    return -1;
  }
  S.rules[S.nrules] = (struct drop_rule){.type = type, .src = src, .dst = dst, .count = count, .from = from,
                                          .to = to, .hits = 0};
  return S.nrules++;
}

API int sim_drop_hits(int rule)
{
  return (rule >= 0 && rule < S.nrules) ? S.rules[rule].hits : 0;
}

/* Position waypoints, linear interpolation between them (meters). Keep them in time order. */
API void sim_waypoint(int node, int64_t t, double x, double y, double z)
{
  struct node *n = sim_node(node);

  if (n->nwp == n->wp_cap)
  {
    n->wp_cap = n->wp_cap ? n->wp_cap * 2 : 16;
    n->wps = realloc(n->wps, sizeof(*n->wps) * n->wp_cap);
  }
  int i = n->nwp++;
  while (i > 0 && n->wps[i - 1].t > t)
  {
    n->wps[i] = n->wps[i - 1];
    i--;
  }
  n->wps[i] = (struct wp){.t = t, .p = {x, y, z}};
}

API void sim_clear_waypoints(int node)
{
  sim_node(node)->nwp = 0;
}

API void sim_position_at(int node, int64_t t, double *out)
{
  dw_position(sim_node(node), t, out);
}

/* ---- node parameters (survive power cycles) ---------------------------------------------------- */
API void sim_set_clock(int node, int32_t drift_ppb, int64_t offset_dtu)
{
  struct node *n = sim_node(node);

  n->drift_ppb = drift_ppb;
  n->clk_offset_dtu = offset_dtu;
}

API void sim_set_antenna_error(int node, int32_t tx_err_dtu, int32_t rx_err_dtu)
{
  struct node *n = sim_node(node);

  n->ant_tx_err = tx_err_dtu;
  n->ant_rx_err = rx_err_dtu;
}

API void sim_set_ts_noise(int node, double sigma_ps)
{
  sim_node(node)->ts_noise_ps = sigma_ps;
}

API void sim_set_accel_params(int node, int64_t cooldown_ps, int fail)
{
  struct node *n = sim_node(node);

  n->accel_cooldown = cooldown_ps;
  n->accel_fail = fail != 0;
}

/* The next draws of sys_rand32_get() on this node return these values (then the random stream goes on). */
API void sim_rand_force(int node, const uint32_t *vals, int count)
{
  struct node *n = sim_node(node);

  if (count > 64)
  {
    count = 64;
  }
  memcpy(n->rand_force, vals, sizeof(uint32_t) * (size_t)count);
  n->rand_force_n = count;
  n->rand_force_i = 0;
}

/* ---- queries ----------------------------------------------------------------------------------- */
API int sim_node_state(int node) /* bit0 powered, bit1 main() returned */
{
  struct node *n = sim_node(node);

  return (n->powered ? 1 : 0) | (n->exited ? 2 : 0);
}

API int sim_node_boots(int node)
{
  return sim_node(node)->boots;
}

API int sim_log_count(int node)
{
  return sim_node(node)->nlines;
}

API const char *sim_log_line(int node, int i, int64_t *t)
{
  struct node *n = sim_node(node);

  if (i < 0 || i >= n->nlines)
  {
    return NULL;
  }
  *t = n->lines[i].t;
  return n->logbuf + n->lines[i].off;
}

API int sim_led_count(int node)
{
  return sim_node(node)->nleds;
}

API int sim_led_get(int node, int i, int64_t *t)
{
  struct node *n = sim_node(node);

  if (i < 0 || i >= n->nleds)
  {
    return -1;
  }
  *t = n->leds[i].t;
  return n->leds[i].mask;
}

/* Firmware getters: called from the scenario context with the node "current" and without time cost. */
#define WITH_NODE(n, expr)                  \
  do                                        \
  {                                         \
    struct node *saved_ = cur_node;         \
    bool q_ = S.in_query;                   \
    cur_node = (n);                         \
    S.in_query = true;                      \
    expr;                                   \
    cur_node = saved_;                      \
    S.in_query = q_;                        \
  } while (0)

API int sim_pouch_phase(int node)
{
  struct node *n = sim_node(node);
  int v = -1;

  if (n->powered && n->f_phase)
  {
    WITH_NODE(n, v = n->f_phase());
  }
  return v;
}

API int sim_pouch_slot(int node)
{
  struct node *n = sim_node(node);
  int v = -2;

  if (n->powered && n->f_slot)
  {
    WITH_NODE(n, v = n->f_slot());
  }
  return v;
}

API int sim_pouch_lives(int node)
{
  struct node *n = sim_node(node);
  int v = -1;

  if (n->powered && n->f_lives)
  {
    WITH_NODE(n, v = n->f_lives());
  }
  return v;
}

/* The node's 64-bit "factory" device ID (what hwinfo_get_device_id() returns; the firmware folds it
 * into the 32-bit uid with FNV-1a, simlib.py does the same). */
API uint64_t sim_dev_id(int node)
{
  uint64_t v;

  memcpy(&v, sim_node(node)->dev_id, 8);
  return v;
}

API int sim_ball_count(int node)
{
  struct node *n = sim_node(node);
  int v = -1;

  if (n->powered && n->f_count)
  {
    WITH_NODE(n, v = n->f_count());
  }
  return v;
}

API int sim_ball_mask(int node)
{
  struct node *n = sim_node(node);
  int v = -1;

  if (n->powered && n->f_mask)
  {
    WITH_NODE(n, v = n->f_mask());
  }
  return v;
}

API uint32_t sim_ball_slot_uid(int node, int slot)
{
  struct node *n = sim_node(node);
  uint32_t v = 0;

  if (n->powered && n->f_slot_uid)
  {
    WITH_NODE(n, v = n->f_slot_uid(slot));
  }
  return v;
}

API int sim_ball_slot_lives(int node, int slot)
{
  struct node *n = sim_node(node);
  int v = -1;

  if (n->powered && n->f_slot_lives)
  {
    WITH_NODE(n, v = n->f_slot_lives(slot));
  }
  return v;
}

/* ---- ranging rounds recorded by the mock layer ------------------------------------------------- */
void sim_note_range_round(int ret, int count, const uint8_t *slot, const uint32_t *uid, const int32_t *mm,
                          const uint8_t *lives)
{
  struct node *n = cur_node;

  if (n->nranges == n->ranges_cap)
  {
    n->ranges_cap = n->ranges_cap ? n->ranges_cap * 2 : 1024;
    n->ranges = realloc(n->ranges, sizeof(*n->ranges) * n->ranges_cap);
  }
  struct range_rec *r = &n->ranges[n->nranges++];
  memset(r, 0, sizeof(*r));
  r->t = n->t;
  r->ret = ret;
  r->count = count;
  for (int i = 0; i < count && i < RANGE_MAX; i++)
  {
    r->slot[i] = slot[i];
    r->uid[i] = uid[i];
    r->mm[i] = mm[i];
    r->lives[i] = lives[i];
  }
}

API int sim_range_count(int node)
{
  return sim_node(node)->nranges;
}

API const struct range_rec *sim_range_base(int node)
{
  return sim_node(node)->ranges;
}

API int sim_range_rec_size(void)
{
  return (int)sizeof(struct range_rec);
}

/* ---- the air trace ----------------------------------------------------------------------------- */
API int sim_trace_count(void)
{
  return S.ntrace;
}

API const struct trace_rec *sim_trace_base(void)
{
  return S.trace;
}

API int sim_trace_rec_size(void)
{
  return (int)sizeof(struct trace_rec);
}

API uint64_t sim_stat(int which)
{
  switch (which)
  {
  case 0: return S.frames_total;
  case 1: return S.collisions;
  case 2: return (uint64_t)S.nrules;
  default: return 0;
  }
}

API uint32_t sim_accel_stat(int node, int which)
{
  struct node *n = sim_node(node);

  switch (which)
  {
  case 0: return n->hits_injected;
  case 1: return n->hits_suppressed;
  case 2: return n->hits_dropped;
  default: return 0;
  }
}
