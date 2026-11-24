/*
 * dodgit host co-simulation - DW3110 transceiver model and the radio "air".
 *
 * What is modelled (and what is not: tests/sim/README.md, "Fidelity limits"):
 *   - a 40-bit device clock per node, 15.65 ps per tick, own random offset at every chip reset (so it
 *     wraps at arbitrary moments), optional drift in ppb;
 *   - immediate and delayed TX. A delayed time is the instant the RMARKER leaves the antenna (upper 32
 *     of the 40 clock bits, lowest of those ignored, as the real chip does); a time in the past (more than
 *     half a clock period ahead) or too close to start the preamble fails dwt_starttx() like HPDWARN;
 *   - TX / RX timestamps of the RMARKER, the antenna delay (configured value; a scenario may make the
 *     true delay differ), DWT_RESPONSE_EXPECTED (RX is armed right after the TX), single-shot RX
 *     (the receiver is off after every frame, good or bad);
 *   - frame airtime computed from the configured preamble / data rate (~0.19 ms for the product's
 *     frames), propagation delay from the 3D distance, a receiver locks onto a frame only if it was
 *     armed RX_LOCK_MARGIN_US before the RMARKER arrived, and two frames overlapping in time at a
 *     receiver destroy each other (the one it locked onto ends with a CRC error, SYS_STATUS RXFCE);
 *   - random per-link loss (silent, or with a CRC error), drop rules by message type;
 *   - a CPU cost per call (SPI transfers), see the COST_* values.
 */
#include "sim.h"

#include <math.h>
#include <stdlib.h>

#include "deca_device_api.h"
#include "deca_probe_interface.h"
#include "dw3000_hw.h"
#include "dw3000_spi.h"
#include "zephyr/kernel.h"

const struct dwt_probe_s dw3000_probe_interf = {0};

#define MASK40 ((1ULL << 40) - 1)
#define C_AIR 299702547.0 /* m/s, the speed of light in air that shared/twr.h uses */

/* ---- model constants (microseconds of virtual time) ---------------------------------------------- */
#define RX_WARMUP_US 10.0        /* rxenable -> receiver really listening */
#define RX_LOCK_MARGIN_US 16.0   /* the receiver must be on this long before the RMARKER (8 PAC + 8 SFD symbols) */
#define TX_IMM_LATENCY_US 8.0    /* immediate TX command -> first preamble symbol */
#define TX_SETUP_US 20.0         /* a delayed TX must be commanded at least this long before its preamble starts */
#define RX_END_LATENCY_US 15.0   /* last bit on the air -> RXFCG / RXFCE visible (RS decode, CRC) */
#define TX_DONE_LATENCY_US 2.0   /* last bit on the air -> TXFRS visible */

/* CPU cost of the driver calls (SPI at ~8 MHz plus driver overhead) */
#define COST_STATUS 8.0
#define COST_RXENABLE 25.0
#define COST_STARTTX 40.0
#define COST_TXDATA 22.0
#define COST_TXFCTRL 12.0
#define COST_RXDATA 28.0
#define COST_FRAMELEN 10.0
#define COST_TIMESTAMP 12.0
#define COST_DXTIME 8.0
#define COST_FORCEOFF 15.0

/* ---- the 40-bit device clock ------------------------------------------------------------------- */
#define DTU_NUM 638976ULL   /* device time units per picosecond = 638976 / 10^7 (499.2 MHz * 128) */
#define DTU_DEN 10000000ULL

static uint64_t dw_clock(const struct node *n, ps_t T)
{
  __int128 el = (__int128)(T - n->dw.ref_ps);
  __int128 d = el * DTU_NUM * (1000000000LL + n->dw.drift_ppb) / ((__int128)DTU_DEN * 1000000000LL);
  return (uint64_t)((__int128)n->dw.ref_dtu + d) & MASK40;
}

static ps_t dtu_to_ps(const struct node *n, uint64_t dtu)
{
  __int128 num = (__int128)dtu * DTU_DEN * 1000000000LL;
  __int128 den = (__int128)DTU_NUM * (1000000000LL + n->dw.drift_ppb);
  return (ps_t)((num + den - 1) / den);
}

/* ---- node position ------------------------------------------------------------------------------ */
void dw_position(struct node *n, ps_t t, double out[3])
{
  if (n->nwp == 0)
  {
    out[0] = out[1] = out[2] = 0;
    return;
  }
  if (t <= n->wps[0].t || n->nwp == 1)
  {
    memcpy(out, n->wps[0].p, sizeof(double) * 3);
    return;
  }
  if (t >= n->wps[n->nwp - 1].t)
  {
    memcpy(out, n->wps[n->nwp - 1].p, sizeof(double) * 3);
    return;
  }
  int i = 0;
  while (i + 1 < n->nwp && n->wps[i + 1].t <= t)
  {
    i++;
  }
  const struct wp *a = &n->wps[i], *b = &n->wps[i + 1];
  double f = (double)(t - a->t) / (double)(b->t - a->t);
  for (int k = 0; k < 3; k++)
  {
    out[k] = a->p[k] + (b->p[k] - a->p[k]) * f;
  }
}

/* ---- frame timing -------------------------------------------------------------------------------- */
static double symbol_ps(const struct node *n)
{
  return n->dw.prf16 ? 993590.0 : 1017630.0;
}

static ps_t shr_ps(const struct node *n) /* preamble + SFD: first symbol to RMARKER */
{
  return (ps_t)((n->dw.plen_symbols + n->dw.sfd_symbols) * symbol_ps(n));
}

static ps_t frame_ps(const struct node *n, int len) /* total airtime of a frame of `len` bytes incl. CRC */
{
  double bit_ps = n->dw.rate_850k ? 1176470.0 : 146780.0;
  int bits = len * 8;
  int blocks = (bits + 329) / 330;
  double phr = 21.0 * 1176470.0; /* PHR at 850 kb/s */
  return shr_ps(n) + (ps_t)(phr + (bits + 48 * blocks) * bit_ps);
}

static uint16_t crc16(const uint8_t *p, int n)
{
  uint16_t crc = 0;
  while (n--)
  {
    crc ^= (uint16_t)(*p++) << 8;
    for (int i = 0; i < 8; i++)
    {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

/* ---- chip state --------------------------------------------------------------------------------- */
static void set_fate(const struct frame *f, int node, enum fate fate)
{
  if (S.trace_on && f->trace_idx >= 0)
  {
    S.trace[f->trace_idx].fate[node] = (uint8_t)fate;
  }
}

static struct frame *frame_of(uint64_t serial)
{
  struct frame *f = &S.pool[serial % SIM_POOL];
  return f->serial == serial ? f : NULL;
}

static void abort_tx(struct node *n)
{
  struct dw *d = &n->dw;

  d->tx_token++; /* cancels the TX_START / TX_DONE events in the heap */
  d->tx_pending = false;
  if (d->tx_busy)
  {
    struct frame *f = frame_of(d->tx_serial);
    if (f)
    {
      f->aborted = true;
      f->t_abort = S.now;
      if (S.trace_on && f->trace_idx >= 0)
      {
        S.trace[f->trace_idx].aborted = 1;
      }
    }
    d->tx_busy = false;
  }
}

static void abort_rx(struct node *n)
{
  struct dw *d = &n->dw;

  if (d->rx == RX_LOCKED)
  {
    struct frame *f = frame_of(d->rx_serial);
    if (f)
    {
      set_fate(f, n->id, FATE_ABORT);
    }
  }
  d->rx = RX_OFF;
}

static void chip_reset(struct node *n, ps_t T)
{
  struct dw *d = &n->dw;

  abort_rx(n);
  abort_tx(n);
  d->status = 0;
  d->resp_expected = false;
  d->ref_ps = T;
  if (n->clk_offset_dtu >= 0)
  {
    d->ref_dtu = (uint64_t)n->clk_offset_dtu & MASK40;
  }
  else
  {
    d->ref_dtu = rng_next(&n->dwrng) & MASK40;
  }
}

void dw_boot(struct node *n)
{
  memset(&n->dw, 0, sizeof(n->dw));
  n->dw.drift_ppb = n->drift_ppb;
  n->dw.ant_tx_err = n->ant_tx_err;
  n->dw.ant_rx_err = n->ant_rx_err;
  n->dw.plen_symbols = 128;
  n->dw.sfd_symbols = 8;
  chip_reset(n, S.now);
}

void dw_power_off(struct node *n)
{
  abort_rx(n);
  abort_tx(n);
  n->dw.rx = RX_OFF;
}

/* ---- the air ------------------------------------------------------------------------------------ */
static bool rule_drops(int type, int src, int dst, ps_t T)
{
  for (int i = 0; i < S.nrules; i++)
  {
    struct drop_rule *r = &S.rules[i];

    if (r->count == 0 || T < r->from || T >= r->to)
    {
      continue;
    }
    if ((r->type >= 0 && r->type != type) || (r->src >= 0 && r->src != src) ||
        (r->dst >= 0 && r->dst != dst))
    {
      continue;
    }
    if (r->count > 0)
    {
      r->count--;
    }
    r->hits++;
    return true;
  }
  return false;
}

static void tx_start(struct node *n, uint64_t token)
{
  struct dw *d = &n->dw;

  if (!d->tx_pending || token != d->tx_token)
  {
    return;
  }
  d->tx_pending = false;
  d->tx_busy = true;
  abort_rx(n); /* the transmitter takes over: the receiver is off (and a frame it was locked on is lost) */

  struct frame *f = &S.pool[(++S.next_serial) % SIM_POOL];
  memset(f, 0, sizeof(*f));
  f->serial = S.next_serial;
  f->src = n->id;
  f->src_gen = n->gen;
  f->len = d->ptx_len;
  memcpy(f->data, d->ptx_buf, f->len);
  f->t_start = d->ptx_start;
  f->t_rm = d->ptx_rm;
  f->t_end = f->t_start + frame_ps(n, f->len);
  f->trace_idx = -1;
  d->tx_serial = f->serial;
  d->tx_ts = d->tx_ts; /* already computed by dwt_starttx() */
  S.frames_total++;

  if (S.trace_on)
  {
    if (S.ntrace == S.trace_cap)
    {
      S.trace_cap = S.trace_cap ? S.trace_cap * 2 : 4096;
      S.trace = realloc(S.trace, sizeof(*S.trace) * S.trace_cap);
    }
    struct trace_rec *tr = &S.trace[S.ntrace];
    memset(tr, 0, sizeof(*tr));
    tr->t_start = f->t_start;
    tr->t_end = f->t_end;
    tr->src = (int8_t)n->id;
    tr->len = (uint8_t)(f->len > 255 ? 255 : f->len);
    memcpy(tr->data, f->data, f->len < SIM_TRACE_BYTES ? f->len : SIM_TRACE_BYTES);
    f->trace_idx = S.ntrace++;
  }

  int type = f->len > 9 ? f->data[9] : -1;
  double ps[3];
  dw_position(n, f->t_rm, ps);
  for (int j = 0; j < SIM_MAX_NODES; j++)
  {
    struct node *r = &S.nodes[j];

    if (j == n->id)
    {
      continue;
    }
    if (!r->powered)
    {
      set_fate(f, j, FATE_OFF);
      continue;
    }
    double pr[3];
    dw_position(r, f->t_rm, pr);
    double dist = sqrt((ps[0] - pr[0]) * (ps[0] - pr[0]) + (ps[1] - pr[1]) * (ps[1] - pr[1]) +
                       (ps[2] - pr[2]) * (ps[2] - pr[2]));
    ps_t prop = (ps_t)(dist / C_AIR * 1e12);
    f->rx[j].a_start = f->t_start + prop;
    f->rx[j].a_rm = f->t_rm + prop;
    f->rx[j].a_end = f->t_end + prop;
    f->rx[j].reach = true;

    if (rule_drops(type, n->id, j, f->t_start))
    {
      f->rx[j].reach = false;
      set_fate(f, j, FATE_RULE);
      continue;
    }
    double p = S.loss_p[n->id][j];
    if (p > 0 && rng_unit(&S.airrng) < p)
    {
      if (rng_unit(&S.airrng) < S.loss_crc[n->id][j])
      {
        f->rx[j].corrupt = true; /* arrives, but the CRC fails */
      }
      else
      {
        f->rx[j].reach = false;
        set_fate(f, j, FATE_LOSS);
        continue;
      }
    }
    sim_push(f->rx[j].a_rm, EV_FRAME_LOCK, j, r->gen, f->serial, 0);
    sim_push(f->rx[j].a_end + (ps_t)(RX_END_LATENCY_US * PS_US), EV_FRAME_END, j, r->gen, f->serial, 0);
  }
  sim_push(f->t_end + (ps_t)(TX_DONE_LATENCY_US * PS_US), EV_TX_DONE, n->id, n->gen, token, 0);
}

static void tx_done(struct node *n, uint64_t token)
{
  struct dw *d = &n->dw;

  if (!d->tx_busy || token != d->tx_token)
  {
    return;
  }
  d->tx_busy = false;
  d->status |= SYS_STATUS_TXFRS_BIT_MASK | SYS_STATUS_TXPHS_BIT_MASK | SYS_STATUS_TXPRS_BIT_MASK |
               SYS_STATUS_TXFRB_BIT_MASK;
  if (d->resp_expected)
  {
    d->rx = RX_ARMED; /* dwt_setrxaftertxdelay(0): the receiver starts right after the frame */
    d->rx_arm_eff = S.now + (ps_t)(RX_WARMUP_US * PS_US);
    d->resp_expected = false;
  }
}

static void frame_lock(struct node *n, uint64_t serial)
{
  struct dw *d = &n->dw;
  struct frame *f = frame_of(serial);

  if (!f)
  {
    return;
  }
  if (f->aborted && f->t_abort < f->t_rm)
  {
    /* The transmitter was switched off before it got past the preamble: nothing to lock onto, and nothing
     * that makes the receiver report an error either. */
    set_fate(f, n->id, FATE_ABORT);
    return;
  }
  if (d->tx_busy || d->tx_pending)
  {
    set_fate(f, n->id, d->tx_busy ? FATE_TXBUSY : FATE_DEAF);
    return;
  }
  if (d->rx == RX_LOCKED)
  {
    set_fate(f, n->id, FATE_BUSY); /* destroyed with the frame the receiver is on: see frame_end() */
    return;
  }
  if (d->rx == RX_ARMED && f->rx[n->id].a_rm - d->rx_arm_eff >= (ps_t)(RX_LOCK_MARGIN_US * PS_US))
  {
    d->rx = RX_LOCKED;
    d->rx_serial = serial;
    return;
  }
  set_fate(f, n->id, FATE_DEAF);
}

static bool overlapped_at(const struct frame *f, int j)
{
  for (int i = 0; i < SIM_POOL; i++)
  {
    const struct frame *g = &S.pool[i];

    if (g->serial == 0 || g == f || g->src == j || !g->rx[j].reach)
    {
      continue;
    }
    if (g->rx[j].a_start < f->rx[j].a_end && g->rx[j].a_end > f->rx[j].a_start)
    {
      return true;
    }
  }
  return false;
}

static double gauss(uint64_t *s)
{
  double u1 = rng_unit(s) + 1e-12, u2 = rng_unit(s);
  return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

static void frame_end(struct node *n, uint64_t serial)
{
  struct dw *d = &n->dw;
  struct frame *f = frame_of(serial);

  if (!f || d->rx != RX_LOCKED || d->rx_serial != serial)
  {
    return;
  }
  d->rx = RX_OFF; /* single-shot: off after every frame, good or bad */
  if (f->aborted)
  {
    d->status |= SYS_STATUS_RXFCE_BIT_MASK | SYS_STATUS_RXPRD_BIT_MASK | SYS_STATUS_RXSFDD_BIT_MASK;
    set_fate(f, n->id, FATE_ABORT);
    return;
  }
  bool collided = overlapped_at(f, n->id);
  if (collided || f->rx[n->id].corrupt)
  {
    d->status |= SYS_STATUS_RXFCE_BIT_MASK | SYS_STATUS_RXPRD_BIT_MASK | SYS_STATUS_RXSFDD_BIT_MASK |
                 SYS_STATUS_RXPHD_BIT_MASK;
    set_fate(f, n->id, collided ? FATE_COLLISION : FATE_CRC);
    if (collided)
    {
      S.collisions++;
    }
    return;
  }
  d->rxlen = f->len;
  memcpy(d->rxbuf, f->data, f->len);
  ps_t a_rm = f->rx[n->id].a_rm;
  if (n->ts_noise_ps > 0)
  {
    a_rm += (ps_t)(gauss(&n->dwrng) * n->ts_noise_ps);
  }
  /* The chip subtracts the configured RX antenna delay from the time it detected the RMARKER. */
  d->rx_ts = (uint64_t)((int64_t)dw_clock(n, a_rm) + d->ant_rx_err) & MASK40;
  d->status |= SYS_STATUS_ALL_RX_GOOD;
  set_fate(f, n->id, FATE_GOOD);
}

void dw_event(int kind, struct node *n, uint64_t a, uint64_t b)
{
  (void)b;
  switch (kind)
  {
  case EV_TX_START: tx_start(n, a); break;
  case EV_TX_DONE: tx_done(n, a); break;
  case EV_FRAME_LOCK: frame_lock(n, a); break;
  case EV_FRAME_END: frame_end(n, a); break;
  }
}

/* ===========================================================================================
 * The driver API the firmware calls
 * ========================================================================================= */
/* Cost first, then the barrier: the operation takes effect (or is sampled) at the END of the SPI
 * transfer, at the node's local time. */
#define OBSERVABLE(n, us) \
  do                      \
  {                       \
    node_cost((n), (us)); \
    node_sync(n);         \
  } while (0)

int dw3000_hw_init(void)
{
  node_cost(cur_node, 200);
  return 0;
}

void dw3000_hw_reset(void)
{
  struct node *n = cur_node;

  OBSERVABLE(n, 50);
  chip_reset(n, n->t); /* the clock restarts from a fresh offset, like the real counter after a reset */
}

void dw3000_spi_speed_fast(void)
{
  node_cost(cur_node, 5);
}

int32_t dwt_probe(struct dwt_probe_s *probe_interf)
{
  (void)probe_interf;
  node_cost(cur_node, 300);
  return DWT_SUCCESS;
}

uint32_t dwt_readdevid(void)
{
  node_cost(cur_node, COST_STATUS);
  return 0xDECA0302; /* DW3110 */
}

uint8_t dwt_checkidlerc(void)
{
  node_cost(cur_node, COST_STATUS);
  return 1;
}

int32_t dwt_initialise(int32_t mode)
{
  (void)mode;
  node_cost(cur_node, 2000);
  return DWT_SUCCESS;
}

int32_t dwt_configure(dwt_config_t *config)
{
  struct dw *d = &cur_node->dw;

  node_cost(cur_node, 1500);
  d->plen_symbols = (uint16_t)((config->txPreambLength + 1) * 8);
  d->prf16 = config->txCode <= 8;
  d->rate_850k = config->dataRate == DWT_BR_850K;
  d->sfd_symbols = config->sfdType == DWT_SFD_DW_16 ? 16 : 8;
  return DWT_SUCCESS;
}

void dwt_configuretxrf(dwt_txconfig_t *config)
{
  (void)config;
  node_cost(cur_node, 30);
}

void dwt_settxantennadelay(uint16_t antennaDly)
{
  cur_node->dw.ant_tx_cfg = antennaDly;
  node_cost(cur_node, COST_DXTIME);
}

void dwt_setrxantennadelay(uint16_t antennaDly)
{
  cur_node->dw.ant_rx_cfg = antennaDly;
  node_cost(cur_node, COST_DXTIME);
}

void dwt_setrxaftertxdelay(uint32_t rxDelayTime)
{
  (void)rxDelayTime; /* the product only uses 0 */
  node_cost(cur_node, COST_DXTIME);
}

void dwt_setrxtimeout(uint32_t rx_time)
{
  (void)rx_time; /* the product only uses 0 = no hardware timeout */
  node_cost(cur_node, COST_DXTIME);
}

void dwt_forcetrxoff(void)
{
  struct node *n = cur_node;

  OBSERVABLE(n, COST_FORCEOFF);
  abort_rx(n);
  abort_tx(n);
  n->dw.resp_expected = false;
}

void dwt_writesysstatuslo(uint32_t mask)
{
  struct node *n = cur_node;

  OBSERVABLE(n, COST_STATUS);
  n->dw.status &= ~mask; /* write 1 to clear */
}

uint32_t dwt_readsysstatuslo(void)
{
  struct node *n = cur_node;

  OBSERVABLE(n, COST_STATUS);
  return n->dw.status;
}

int32_t dwt_rxenable(int32_t mode)
{
  struct node *n = cur_node;
  struct dw *d = &n->dw;

  (void)mode;
  OBSERVABLE(n, COST_RXENABLE);
  if (d->rx == RX_OFF && !d->tx_busy && !d->tx_pending)
  {
    d->rx = RX_ARMED;
    d->rx_arm_eff = n->t + (ps_t)(RX_WARMUP_US * PS_US);
  }
  return DWT_SUCCESS;
}

int32_t dwt_writetxdata(uint16_t txDataLength, uint8_t *txDataBytes, uint16_t txBufferOffset)
{
  struct dw *d = &cur_node->dw;

  node_cost(cur_node, COST_TXDATA);
  if ((unsigned)txDataLength + txBufferOffset > SIM_MAX_FRAME)
  {
    return DWT_ERROR;
  }
  memcpy(d->txbuf + txBufferOffset, txDataBytes, txDataLength);
  return DWT_SUCCESS;
}

void dwt_writetxfctrl(uint16_t txFrameLength, uint16_t txBufferOffset, uint8_t ranging)
{
  (void)txBufferOffset;
  (void)ranging;
  cur_node->dw.txlen = txFrameLength;
  node_cost(cur_node, COST_TXFCTRL);
}

void dwt_setdelayedtrxtime(uint32_t starttime)
{
  cur_node->dw.dx_time = starttime;
  node_cost(cur_node, COST_DXTIME);
}

int32_t dwt_starttx(uint8_t mode)
{
  struct node *n = cur_node;
  struct dw *d = &n->dw;

  OBSERVABLE(n, COST_STARTTX);
  if (d->tx_pending || d->tx_busy || d->txlen < 3 || d->txlen > SIM_MAX_FRAME)
  {
    return DWT_ERROR;
  }
  ps_t T = n->t;
  uint64_t ant_true = (uint64_t)((int64_t)d->ant_tx_cfg + d->ant_tx_err);
  ps_t shr = shr_ps(n);
  ps_t t_start, t_rm;
  uint64_t trig;

  if (mode & DWT_START_TX_DELAYED)
  {
    trig = ((uint64_t)(d->dx_time & 0xFFFFFFFEu)) << 8; /* the lowest bit of the 32 is ignored */
    uint64_t now_c = dw_clock(n, T);
    uint64_t c_rm = (trig + ant_true) & MASK40;
    uint64_t lead = (c_rm - now_c) & MASK40;

    if (lead >= (1ULL << 39)) /* HPDWARN: the time has already passed */
    {
      d->status |= SYS_STATUS_HPDWARN_BIT_MASK;
      return DWT_ERROR;
    }
    t_rm = T + dtu_to_ps(n, lead);
    t_start = t_rm - shr;
    if (t_start < T + (ps_t)(TX_SETUP_US * PS_US)) /* too late to start the preamble */
    {
      d->status |= SYS_STATUS_HPDWARN_BIT_MASK;
      return DWT_ERROR;
    }
  }
  else
  {
    t_start = T + (ps_t)(TX_IMM_LATENCY_US * PS_US);
    ps_t t_nom = t_start + shr;
    uint64_t c_nom = dw_clock(n, t_nom);
    uint64_t x = (c_nom - ant_true) & MASK40;

    trig = ((x + 511) & ~511ULL) & MASK40; /* immediate TX starts on the 8 ns grid of the chip */
    uint64_t c_rm = (trig + ant_true) & MASK40;
    t_rm = t_nom + dtu_to_ps(n, (c_rm - c_nom) & MASK40);
    t_start = t_rm - shr;
  }
  d->tx_ts = (trig + d->ant_tx_cfg) & MASK40; /* what dwt_readtxtimestamplo32() reports afterwards */
  d->ptx_len = d->txlen;
  memcpy(d->ptx_buf, d->txbuf, d->ptx_len);
  uint16_t crc = crc16(d->ptx_buf, d->ptx_len - 2);
  d->ptx_buf[d->ptx_len - 2] = (uint8_t)crc;
  d->ptx_buf[d->ptx_len - 1] = (uint8_t)(crc >> 8);
  d->ptx_start = t_start;
  d->ptx_rm = t_rm;
  d->tx_pending = true;
  d->tx_token++;
  d->resp_expected = (mode & DWT_RESPONSE_EXPECTED) != 0;
  sim_push(t_start, EV_TX_START, n->id, n->gen, d->tx_token, 0);
  return DWT_SUCCESS;
}

uint16_t dwt_getframelength(uint8_t *rng)
{
  struct node *n = cur_node;

  (void)rng;
  OBSERVABLE(n, COST_FRAMELEN);
  return n->dw.rxlen;
}

void dwt_readrxdata(uint8_t *buffer, uint16_t length, uint16_t rxBufferOffset)
{
  struct node *n = cur_node;

  OBSERVABLE(n, COST_RXDATA);
  if ((unsigned)length + rxBufferOffset <= SIM_MAX_FRAME)
  {
    memcpy(buffer, n->dw.rxbuf + rxBufferOffset, length);
  }
}

void dwt_readrxtimestamp(uint8_t *timestamp, dwt_ip_sts_segment_e segment)
{
  struct node *n = cur_node;

  (void)segment;
  OBSERVABLE(n, COST_TIMESTAMP);
  for (int i = 0; i < 5; i++)
  {
    timestamp[i] = (uint8_t)(n->dw.rx_ts >> (8 * i));
  }
}

uint32_t dwt_readtxtimestamplo32(void)
{
  struct node *n = cur_node;

  OBSERVABLE(n, COST_TIMESTAMP);
  return (uint32_t)n->dw.tx_ts;
}

uint32_t dwt_readsystimestamphi32(void)
{
  struct node *n = cur_node;

  OBSERVABLE(n, COST_TIMESTAMP);
  return (uint32_t)(dw_clock(n, n->t) >> 8);
}
