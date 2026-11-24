/*
 * dodgit host co-simulation - internal header of the simulator library (libsim).
 * Architecture and fidelity limits: tests/sim/README.md.
 *
 * One process, several nodes. Every node is the UNMODIFIED firmware (compiled to its own dynamic
 * library, so that the file-scope statics of shared/uwb.c, main.c, ... exist once per node) running as
 * a stackful coroutine. The simulator owns VIRTUAL time and the radio "air":
 *
 *   - each node has a local time; shim calls that touch anything another node could observe (radio
 *     registers, GPIO, accelerometer) first "sync": the node yields until every event (frame
 *     arrival, scenario action, other node) earlier than its local time has been processed, so all
 *     observable operations happen in global time order. Calls that are node-local (writing the TX
 *     buffer, reading the clock) only advance the node's own time by a modelled CPU cost.
 *   - sleeping (k_msleep / k_usleep) always syncs, so every node loop yields.
 *   - events (frame start / lock / end, scenario script, node resume) live in one heap ordered by
 *     (time, insertion order): fully deterministic, no real time anywhere.
 */
#ifndef SIM_H
#define SIM_H

#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

typedef int64_t ps_t; /* global virtual time in picoseconds (106 days of range) */
#define PS_US 1000000LL
#define PS_MS 1000000000LL
#define PS_S 1000000000000LL

#define SIM_MAX_NODES 12
#define SIM_MAX_FRAME 127
#define SIM_TRACE_BYTES 32 /* first bytes of every frame kept in the air trace (the product's MAX_FRAME) */

/* ---------------------------------------------------------------------------------------------
 * Coroutines
 * ------------------------------------------------------------------------------------------- */
typedef struct ctx
{
  jmp_buf jb;
  ucontext_t uc;
  char *stack;
  struct node *node; /* NULL for the main (scheduler) context */
} ctx_t;

/* ---------------------------------------------------------------------------------------------
 * DW3110 model state (one per node)
 * ------------------------------------------------------------------------------------------- */
enum rx_state
{
  RX_OFF,
  RX_ARMED,  /* listening for a preamble (effective from rx_arm_eff) */
  RX_LOCKED, /* locked on a frame, receiving it */
};

enum fate /* what happened to a frame at one receiver (air trace) */
{
  FATE_PENDING = 0,
  FATE_GOOD,      /* received, CRC good */
  FATE_CRC,       /* received with a CRC error (random link corruption) */
  FATE_COLLISION, /* received, destroyed by an overlapping frame: CRC error */
  FATE_LOSS,      /* random link loss: never arrived */
  FATE_RULE,      /* dropped by a scenario drop rule */
  FATE_DEAF,      /* receiver not armed (or armed too late to lock) */
  FATE_TXBUSY,    /* receiver was transmitting */
  FATE_BUSY,      /* receiver was locked on another frame */
  FATE_OFF,       /* node powered off */
  FATE_ABORT,     /* reception cut short (receiver switched off / started TX / frame aborted) */
};

struct dw
{
  /* 40-bit device clock: clk(T) = ref_dtu + (T - ref_ps) * rate, drift in parts per billion */
  ps_t ref_ps;
  uint64_t ref_dtu;
  int32_t drift_ppb;

  uint32_t status;                 /* SYS_STATUS low word */
  uint16_t ant_tx_cfg, ant_rx_cfg; /* what the firmware configured */
  int32_t ant_tx_err, ant_rx_err;  /* (true delay) - (configured), in device time units */
  uint16_t plen_symbols;
  bool rate_850k;
  bool prf16;
  int sfd_symbols;

  /* TX */
  uint8_t txbuf[SIM_MAX_FRAME];
  uint16_t txlen;      /* from dwt_writetxfctrl: frame length including the 2 CRC bytes */
  uint32_t dx_time;    /* dwt_setdelayedtrxtime */
  bool tx_pending;     /* delayed/immediate TX accepted, preamble not on the air yet */
  bool tx_busy;        /* frame on the air */
  uint32_t tx_token;   /* bumped to cancel a pending / running TX */
  bool resp_expected;
  uint64_t tx_ts;      /* 40-bit timestamp of the last TX (RMARKER + configured antenna delay) */
  uint64_t tx_serial;  /* frame currently on the air */
  /* the pending TX (computed when dwt_starttx was called) */
  uint8_t ptx_buf[SIM_MAX_FRAME];
  uint16_t ptx_len;
  ps_t ptx_start, ptx_rm;

  /* RX */
  enum rx_state rx;
  ps_t rx_arm_eff;
  uint64_t rx_serial; /* frame being received (RX_LOCKED) */
  uint8_t rxbuf[SIM_MAX_FRAME];
  uint16_t rxlen;
  uint64_t rx_ts; /* 40-bit RX timestamp of the last good frame */
};

struct frame
{
  uint64_t serial; /* 0 = unused slot */
  int src;
  unsigned src_gen;
  ps_t t_start, t_rm, t_end; /* at the transmitter's antenna: first preamble symbol, RMARKER, last bit */
  uint16_t len;
  uint8_t data[SIM_MAX_FRAME];
  bool aborted;
  ps_t t_abort;              /* when the transmitter was switched off in the middle of the frame */
  int trace_idx;
  struct
  {
    ps_t a_start, a_rm, a_end; /* the same instants at this receiver (propagation added) */
    bool reach;                /* the signal arrives at all (false: link loss / rule / node off) */
    bool corrupt;              /* random CRC corruption on this link */
  } rx[SIM_MAX_NODES];
};

struct trace_rec
{
  ps_t t_start, t_end;
  int8_t src;
  uint8_t len;
  uint8_t aborted;
  uint8_t data[SIM_TRACE_BYTES];
  uint8_t fate[SIM_MAX_NODES];
};

/* ---------------------------------------------------------------------------------------------
 * Nodes
 * ------------------------------------------------------------------------------------------- */
enum role
{
  ROLE_BALL,
  ROLE_POUCH,
};

struct wp
{
  ps_t t;
  double p[3]; /* meters */
};

struct logline
{
  ps_t t;
  uint32_t off;
  uint16_t len;
};

struct ledrec
{
  ps_t t;
  uint8_t mask;
};

struct accel_ev
{
  int64_t time_ms;
  int32_t mg;
  bool stall;
};

#define ACCEL_QUEUE 4 /* the event queue of ball/src/impact.h: small, the OLDEST events are dropped on overflow */

#define RANGE_MAX 8
struct range_rec /* one uwb_ball_range_round() call, recorded by the mock layer (mocks/uwb_hook.c) */
{
  ps_t t;       /* when the round returned */
  int ret;      /* its return value: pouches heard, or -errno */
  int count;
  uint8_t slot[RANGE_MAX];
  uint8_t lives[RANGE_MAX];
  uint32_t uid[RANGE_MAX];
  int32_t mm[RANGE_MAX];
};

struct node
{
  int id;
  enum role role;
  bool powered;
  unsigned gen;  /* bumped at every power on and power off: stale events are ignored */
  int boots;
  ctx_t ctx;
  void *lib;
  char libpath[640];
  ps_t t;        /* node-local time (global timeline); may run ahead of sim.now between syncs */
  ps_t boot_ps;
  bool exited;   /* main() returned (the node is dead until the next power on) */

  uint8_t dev_id[8];
  uint64_t rng;
  uint64_t dwrng; /* separate stream for the radio model (clock offset, timestamp noise) */
  uint32_t rand_force[64];
  int rand_force_n, rand_force_i;

  /* scenario-settable parameters (survive power cycles) */
  int32_t drift_ppb;
  int64_t clk_offset_dtu; /* < 0: random at every chip reset */
  int32_t ant_tx_err, ant_rx_err;
  double ts_noise_ps;     /* Gaussian RX timestamp noise (sigma) */
  ps_t accel_cooldown;    /* suppress Hit events this soon after the last accepted one (0 = report all) */
  bool accel_fail;        /* impact_init() fails */

  struct wp *wps;
  int nwp, wp_cap;

  char *logbuf;
  size_t log_len, log_cap;
  struct logline *lines;
  int nlines, lines_cap;
  char line[1024];
  int line_len;
  ps_t line_t;

  struct ledrec *leds;
  int nleds, leds_cap;
  uint8_t led_pins;
  int button;

  int accel_level;
  ps_t accel_since;
  int accel_saved; /* level to return to after a stall */
  struct accel_ev evq[ACCEL_QUEUE];
  int ev_head, ev_n;
  uint32_t accel_resets;
  struct range_rec *ranges;
  int nranges, ranges_cap;
  uint32_t hits_injected, hits_suppressed, hits_dropped;
  ps_t last_hit_ps;
  bool hit_seen;
  int32_t last_hit_mg;

  struct dw dw;

  /* firmware getters (dlsym) */
  int (*f_phase)(void);
  int (*f_slot)(void);
  uint8_t (*f_lives)(void);
  uint32_t (*f_uid)(void);
  int (*f_count)(void);
  uint8_t (*f_mask)(void);
  uint32_t (*f_slot_uid)(int);
  int (*f_slot_lives)(int);
};

/* ---------------------------------------------------------------------------------------------
 * The simulation
 * ------------------------------------------------------------------------------------------- */
struct drop_rule
{
  int type; /* frame type byte, -1 any */
  int src;  /* transmitting node, -1 any */
  int dst;  /* receiving node, -1 any */
  int count; /* matches left, -1 unlimited */
  ps_t from, to;
  int hits;
};

#define SIM_MAX_RULES 64
#define SIM_POOL 256

struct sim
{
  bool inited;
  ps_t now;   /* time of the latest processed event / limit reached */
  ps_t limit; /* run_until target */
  uint64_t seq;
  uint64_t seed;
  uint64_t airrng;
  struct node nodes[SIM_MAX_NODES];
  double loss_p[SIM_MAX_NODES][SIM_MAX_NODES]; /* [src][dst] */
  double loss_crc[SIM_MAX_NODES][SIM_MAX_NODES]; /* of the lost frames, the fraction that arrives with a bad CRC */
  struct drop_rule rules[SIM_MAX_RULES];
  int nrules;
  ps_t console_char_ps; /* time one printk character costs the caller (0 = free) */
  char copy_dir[480];
  bool in_query; /* getters called from the scenario: no cost, no time */
  bool stop;     /* a watched log line appeared */
  char watch[160];
  int watch_node;

  struct frame pool[SIM_POOL];
  uint64_t next_serial;
  struct trace_rec *trace;
  int ntrace, trace_cap;
  bool trace_on;
  uint64_t collisions, frames_total;
};

extern struct sim S;
extern struct node *cur_node; /* the node whose code is running (NULL in the scheduler) */

/* ---- sim_core.c ------------------------------------------------------------------------------ */
void node_sync(struct node *n);                 /* barrier before an observable operation */
void node_cost(struct node *n, double us);      /* advance the node's own clock (CPU time) */
uint64_t rng_next(uint64_t *s);
double rng_unit(uint64_t *s);                   /* [0,1) */
struct node *sim_node(int id);

void sim_core_reset(uint64_t seed, const char *copy_dir);
ps_t sim_core_run(ps_t limit, const char *watch, int watch_node);
void sim_log_marker(struct node *n, ps_t t, const char *text);

/* ---- sim_dw.c -------------------------------------------------------------------------------- */
void dw_boot(struct node *n);                   /* power on: chip state, clock reference */
void dw_power_off(struct node *n);
void dw_event(int kind, struct node *n, uint64_t a, uint64_t b); /* TX start / done, frame lock / end */
void dw_position(struct node *n, ps_t t, double out[3]);
void sim_dw_reset_all(void);

/* event kinds shared between core and dw */
enum ev_kind
{
  EV_RESUME,
  EV_SCRIPT,
  EV_TX_START,   /* a = token */
  EV_TX_DONE,    /* a = token */
  EV_FRAME_LOCK, /* a = frame serial */
  EV_FRAME_END,  /* a = frame serial */
};
void sim_push(ps_t t, int kind, int node, unsigned gen, uint64_t a, uint64_t b);

/* Scenario actions, executed at their virtual time (sim_core.c). */
enum script_op
{
  OP_POWER_ON,  /* str = path of the node library */
  OP_POWER_OFF,
  OP_BUTTON,    /* a = level (1 = pressed) */
  OP_ACCEL_LEVEL,   /* a = enum sim_level */
  OP_ACCEL_HIT,     /* a = mg */
  OP_ACCEL_STALL,   /* a = ps until the sensor delivers data again */
  OP_ACCEL_RESTORE, /* internal: end of a stall; a = stall serial */
  OP_LOSS,          /* a = src | dst << 8, d = loss probability, b = crc fraction in ppm */
};
void sim_push_script(ps_t t, int node, int op, int64_t a, double d, int64_t b, const char *str);

#endif
