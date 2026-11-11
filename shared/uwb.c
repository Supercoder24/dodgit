/*
 * dodgit - UWB protocol implementation (DW3110 transceiver, Zephyr, decadriver).
 *
 * Message types (header byte 9) and their payloads:
 *   JOIN      pouch -> ball       u32 uid
 *   ASSIGN    ball  -> pouch      u32 uid, u8 slot
 *   POLL      ball  -> broadcast  u8 n_slots
 *   RESPONSE  pouch -> ball       u8 lives                      (in pouch's timeslot)
 *   FINAL     ball  -> broadcast  u8 mask of slots that answered
 *   REPORT    pouch -> ball       u32 poll_rx, resp_tx, final_rx (in pouch's timeslot)
 *   HIT       ball  -> pouch      u16 hit_seq
 *   HIT_ACK   pouch -> ball       u16 hit_seq                   (in pouch's timeslot)
 *   RESET     ball  -> broadcast  -
 *   RESET_ACK pouch -> ball       -                             (in pouch's timeslot)
 *
 * Ranging is double-sided two-way ranging (DS-TWR): POLL / RESPONSE / FINAL /
 * REPORT, with the ball calculating the distance (see twr.h). Replies are sent
 * with the radio's delayed-transmit feature at
 *     trigger_rx_time + SLOT_BASE_UUS + slot * SLOT_UUS
 * so every pouch answers in its own, non-overlapping timeslot.
 */
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/random/random.h>
#include <zephyr/drivers/hwinfo.h>
#include <deca_device_api.h>
#include <deca_probe_interface.h>
#include <dw3000_hw.h>
#include <dw3000_spi.h>
#include "dw3000/dw3000_deca_regs.h"
#include "uwb.h"
#include "twr.h"

/* ---------------------------------------------------------------------------
 * Addresses and frame format (simplified IEEE 802.15.4 data frame)
 *   [0..1] frame control  [2] sequence  [3..4] PAN  [5..6] dest  [7..8] source
 *   [9] message type      [10..] payload       (+2 CRC bytes added by the radio)
 * ------------------------------------------------------------------------- */
#define PAN_ID 0xD048
#define BALL_ADDR 0x0001
#define BROADCAST_ADDR 0xFFFF
#define UNASSIGNED_ADDR 0xFFFE /* a pouch that has no timeslot yet */
#define POUCH_ADDR_BASE 0x0100 /* pouch address = base + timeslot */

#define HEADER_LEN 10
#define CRC_LEN 2
#define MAX_FRAME 32

#define MSG_JOIN 0x20
#define MSG_ASSIGN 0x21
#define MSG_POLL 0x31
#define MSG_RESPONSE 0x32
#define MSG_FINAL 0x33
#define MSG_REPORT 0x34
#define MSG_HIT 0x35
#define MSG_HIT_ACK 0x36
#define MSG_RESET 0x37
#define MSG_RESET_ACK 0x38

#define UUS_TO_DTU 65536ULL /* 1 UUS (~1.026 us) = 65536 device time units (~15.65 ps) */
#define SW_TIMEOUT_MS 25    /* software guard around single TX waits */

/* Payload size in bytes for each message type, or -1 if unknown. */
static int payload_size(uint8_t type)
{
  switch (type)
  {
  case MSG_JOIN: return 4;
  case MSG_ASSIGN: return 5;
  case MSG_POLL: return 1;
  case MSG_RESPONSE: return 1;
  case MSG_FINAL: return 1;
  case MSG_REPORT: return 12;
  case MSG_HIT: return 2;
  case MSG_HIT_ACK: return 2;
  case MSG_RESET: return 0;
  case MSG_RESET_ACK: return 0;
  }
  return -1;
}

static inline uint16_t frame_src(const uint8_t *f) { return sys_get_le16(f + 7); }
static inline uint16_t frame_dst(const uint8_t *f) { return sys_get_le16(f + 5); }
static inline uint8_t frame_type(const uint8_t *f) { return f[9]; }

/* ---------------------------------------------------------------------------
 * Radio setup
 * ------------------------------------------------------------------------- */
/* Channel 5, 128-symbol preamble (code 9), 6.8 Mbps, no STS / PDoA: plain ranging. */
static dwt_config_t radio_config = {
    5, DWT_PLEN_128, DWT_PAC8, 9, 9, 1, DWT_BR_6M8,
    DWT_PHRMODE_STD, DWT_PHRRATE_STD, 129,
    DWT_STS_MODE_OFF, DWT_STS_LEN_64, DWT_PDOA_M0,
};

static uint8_t tx_seq;  /* 8-bit sequence number of the frames we send */
static bool rx_on;      /* true while the receiver is armed (single-shot: off after each frame) */

/* Stop whatever the radio is doing and clear all RX/TX status flags. */
static void radio_reset_state(void)
{
  dwt_forcetrxoff();
  dwt_writesysstatuslo(SYS_STATUS_ALL_RX_GOOD | SYS_STATUS_ALL_RX_ERR |
                       SYS_STATUS_ALL_RX_TO | SYS_STATUS_TXFRS_BIT_MASK);
  rx_on = false;
}

/* Make sure the receiver is listening. Frames are received one at a time; callers
 * clear rx_on after handling a frame so the next call re-arms it. */
static void rx_ensure(void)
{
  if (!rx_on)
  {
    dwt_rxenable(DWT_START_RX_IMMEDIATE);
    rx_on = true;
  }
}

int uwb_init(void)
{
  if (dw3000_hw_init() < 0)
  {
    return -ENODEV;
  }
  dw3000_hw_reset();
  k_msleep(10);

  if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) != DWT_SUCCESS)
  {
    return -ENODEV;
  }
  uint32_t dev_id = dwt_readdevid();
  printk("DW3110 device ID: 0x%08x\n", dev_id);
  if ((dev_id & 0xFFFF0000) != 0xDECA0000)
  {
    return -ENODEV;
  }

  /* Wait (max 100 ms) for the chip to reach its IDLE_RC state. */
  int64_t deadline = k_uptime_get() + 100;
  while (!dwt_checkidlerc())
  {
    if (k_uptime_get() >= deadline)
    {
      return -ETIMEDOUT;
    }
    k_msleep(1);
  }
  if (dwt_initialise(0) != DWT_SUCCESS)
  {
    return -EIO;
  }
  dw3000_spi_speed_fast();
  if (dwt_configure(&radio_config) != DWT_SUCCESS)
  {
    return -EIO;
  }

  dwt_txconfig_t tx_rf = {0x34, 0xFDFDFDFD, 0}; /* pulse generator delay, TX power */
  dwt_configuretxrf(&tx_rf);
  dwt_settxantennadelay(ANTENNA_DELAY);
  dwt_setrxantennadelay(ANTENNA_DELAY);
  dwt_setrxaftertxdelay(0); /* "response expected": RX starts right after TX */
  dwt_setrxtimeout(0);      /* no hardware RX timeout: every wait uses a software deadline */
  radio_reset_state();
  return 0;
}

/* ---------------------------------------------------------------------------
 * Frame helpers
 * ------------------------------------------------------------------------- */
static void frame_header(uint8_t *f, uint8_t type, uint16_t src, uint16_t dst)
{
  memset(f, 0, MAX_FRAME);
  f[0] = 0x41; /* data frame, PAN ID compression, short addresses */
  f[1] = 0x88;
  f[2] = tx_seq++;
  sys_put_le16(PAN_ID, f + 3);
  sys_put_le16(dst, f + 5);
  sys_put_le16(src, f + 7);
  f[9] = type;
}

/* Send a prepared frame (the payload size follows from its type).
 * mode: DWT_START_TX_IMMEDIATE or DWT_START_TX_DELAYED, optionally OR-ed with
 * DWT_RESPONSE_EXPECTED to switch to RX automatically after the frame is sent. */
static int frame_send(uint8_t *f, uint8_t mode)
{
  unsigned len = HEADER_LEN + payload_size(frame_type(f)) + CRC_LEN;

  dwt_writesysstatuslo(SYS_STATUS_TXFRS_BIT_MASK);
  if (dwt_writetxdata(len, f, 0) != DWT_SUCCESS)
  {
    return -EIO;
  }
  dwt_writetxfctrl(len, 0, 1); /* last arg 1 = this is a ranging frame */
  if (dwt_starttx(mode) != DWT_SUCCESS)
  {
    return -ETIME; /* delayed TX time was already in the past */
  }
  rx_on = (mode & DWT_RESPONSE_EXPECTED) != 0;
  return 0;
}

/* Wait until the last frame has really left the antenna. */
static int wait_tx(void)
{
  int64_t deadline = k_uptime_get() + SW_TIMEOUT_MS;

  do
  {
    if (dwt_readsysstatuslo() & SYS_STATUS_TXFRS_BIT_MASK)
    {
      return 0;
    }
    k_usleep(100);
  } while (k_uptime_get() < deadline);

  radio_reset_state();
  return -ETIMEDOUT;
}

/* Non-blocking: look for a received frame.
 *   0        a valid frame (good CRC, our PAN, known type and length) is in f
 *   -EAGAIN  nothing yet (receiver still armed)
 *   other    a bad frame or RX error was cleared; the receiver is no longer armed
 * Any result other than -EAGAIN ends the single-shot RX: re-arm with rx_ensure(). */
static int frame_poll(uint8_t *f)
{
  uint32_t status = dwt_readsysstatuslo();

  if (status & SYS_STATUS_RXFCG_BIT_MASK) /* frame received, CRC good */
  {
    uint16_t len = dwt_getframelength(NULL);
    int rc = -EBADMSG;

    if (len <= MAX_FRAME)
    {
      dwt_readrxdata(f, len, 0);
      int p = payload_size(frame_type(f));
      if (p >= 0 && len == HEADER_LEN + p + CRC_LEN && f[0] == 0x41 && f[1] == 0x88 &&
          sys_get_le16(f + 3) == PAN_ID)
      {
        rc = 0;
      }
    }
    dwt_writesysstatuslo(SYS_STATUS_ALL_RX_GOOD | SYS_STATUS_ALL_RX_ERR);
    rx_on = false;
    return rc;
  }
  if (status & (SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO))
  {
    dwt_writesysstatuslo(SYS_STATUS_ALL_RX_GOOD | SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO);
    rx_on = false;
    return -EIO;
  }
  return -EAGAIN;
}

/* 40-bit RX timestamp of the last received frame, in device time units. */
static uint64_t rx_timestamp(void)
{
  uint8_t ts[5];
  uint64_t value = 0;

  dwt_readrxtimestamp(ts, DWT_COMPAT_NONE);
  for (int i = 4; i >= 0; i--)
  {
    value = (value << 8) | ts[i];
  }
  return value;
}

/* How long to listen for replies in timeslots 0 .. n-1, in ms (UUS -> ms, plus slack for
 * the 1 ms clock granularity and software latency). */
static int64_t window_ms(int n_slots)
{
  return ((int64_t)(SLOT_BASE_UUS + n_slots * SLOT_UUS) * 1026) / 1000000 + 4;
}

/* The delayed-TX value (upper 32 of the 40 time bits) for a reply in `slot`, timed from
 * the RX timestamp of the frame that triggered it. */
static uint32_t slot_tx_time(uint64_t trigger_rx, int slot)
{
  return (uint32_t)((trigger_rx + (uint64_t)(SLOT_BASE_UUS + slot * SLOT_UUS) * UUS_TO_DTU) >> 8);
}

/* The 32-bit TX timestamp the radio will actually stamp for a delayed transmit:
 * the radio ignores the lowest bit of the delayed time and adds the TX antenna delay. */
static uint32_t delayed_tx_timestamp(uint32_t delayed)
{
  return (uint32_t)(((uint64_t)(delayed & 0xFFFFFFFEUL) << 8) + ANTENNA_DELAY);
}

/* ===========================================================================
 * Ball side
 * ========================================================================= */
static uint32_t table_uid[MAX_POUCHES];
static bool table_used[MAX_POUCHES];
static int table_count;
static int highest_slot = -1;

int uwb_ball_pouch_count(void) { return table_count; }

uint8_t uwb_ball_slot_mask(void)
{
  uint8_t mask = 0;
  for (int i = 0; i < MAX_POUCHES; i++)
  {
    if (table_used[i])
    {
      mask |= 1U << i;
    }
  }
  return mask;
}

uint32_t uwb_ball_slot_uid(int slot)
{
  return (slot >= 0 && slot < MAX_POUCHES && table_used[slot]) ? table_uid[slot] : 0;
}

void uwb_ball_idle(void)
{
  radio_reset_state();
}

void uwb_ball_rollcall_start(void)
{
  radio_reset_state();
  rx_ensure();
}

enum rollcall_result uwb_ball_rollcall_step(int *slot, uint32_t *uid)
{
  uint8_t f[MAX_FRAME];

  rx_ensure();
  int rc = frame_poll(f);
  if (rc == -EAGAIN)
  {
    return RC_NONE;
  }
  if (rc || frame_type(f) != MSG_JOIN || frame_dst(f) != BALL_ADDR ||
      frame_src(f) != UNASSIGNED_ADDR)
  {
    return RC_NONE;
  }

  uint32_t id = sys_get_le32(f + HEADER_LEN);
  enum rollcall_result result = RC_FULL;
  int s = -1;

  for (int i = 0; i < MAX_POUCHES; i++) /* already registered? same slot again */
  {
    if (table_used[i] && table_uid[i] == id)
    {
      s = i;
      result = RC_REPEAT;
      break;
    }
  }
  if (s < 0) /* first come, first served: lowest free slot */
  {
    for (int i = 0; i < MAX_POUCHES; i++)
    {
      if (!table_used[i])
      {
        table_used[i] = true;
        table_uid[i] = id;
        table_count++;
        if (i > highest_slot)
        {
          highest_slot = i;
        }
        s = i;
        result = RC_NEW;
        break;
      }
    }
  }
  *slot = s;
  *uid = id;
  if (s < 0)
  {
    return RC_FULL; /* ninth pouch: ignored, no reply */
  }

  frame_header(f, MSG_ASSIGN, BALL_ADDR, UNASSIGNED_ADDR);
  sys_put_le32(id, f + HEADER_LEN);
  f[HEADER_LEN + 4] = s;
  radio_reset_state();
  if (frame_send(f, DWT_START_TX_IMMEDIATE) == 0)
  {
    wait_tx();
  }
  return result;
}

/* Per-round data collected from the replies. */
static uint64_t resp_rx_ts[MAX_POUCHES]; /* ball: RX time of each RESPONSE */
static uint8_t resp_lives[MAX_POUCHES];
static uint32_t rep_poll_rx[MAX_POUCHES]; /* from each pouch's REPORT */
static uint32_t rep_resp_tx[MAX_POUCHES];
static uint32_t rep_final_rx[MAX_POUCHES];

static void on_response(int slot, uint64_t rx_ts, const uint8_t *f)
{
  resp_rx_ts[slot] = rx_ts;
  resp_lives[slot] = f[HEADER_LEN];
}

static void on_report(int slot, uint64_t rx_ts, const uint8_t *f)
{
  (void)rx_ts;
  rep_poll_rx[slot] = sys_get_le32(f + HEADER_LEN);
  rep_resp_tx[slot] = sys_get_le32(f + HEADER_LEN + 4);
  rep_final_rx[slot] = sys_get_le32(f + HEADER_LEN + 8);
}

/* Listen (receiver must already be armed) for frames of `type` from the slots in `want`,
 * for at most window ms. Calls cb once per slot. Returns the mask of slots heard.
 * Leaves the radio off. */
static uint8_t collect(uint8_t type, uint8_t want, int64_t window,
                       void (*cb)(int slot, uint64_t rx_ts, const uint8_t *f))
{
  uint8_t got = 0;
  uint8_t f[MAX_FRAME];
  int64_t deadline = k_uptime_get() + window;

  while (k_uptime_get() < deadline && (got & want) != want)
  {
    int rc = frame_poll(f);
    if (rc == -EAGAIN)
    {
      k_usleep(100);
      continue;
    }
    if (rc == 0 && frame_type(f) == type && frame_dst(f) == BALL_ADDR &&
        frame_src(f) >= POUCH_ADDR_BASE)
    {
      int slot = frame_src(f) - POUCH_ADDR_BASE;
      if (slot < MAX_POUCHES && (want & (1U << slot)) && !(got & (1U << slot)))
      {
        cb(slot, rx_timestamp(), f); /* read the timestamp before re-arming RX */
        got |= 1U << slot;
      }
    }
    dwt_rxenable(DWT_START_RX_IMMEDIATE); /* single-shot RX: listen for the next slot */
    rx_on = true;
  }
  radio_reset_state();
  return got;
}

int uwb_ball_range_round(struct pouch_range *out, int max)
{
  uint8_t f[MAX_FRAME];
  int n = highest_slot + 1; /* only listen through the highest slot that is in use */
  uint8_t registered = uwb_ball_slot_mask();

  if (n <= 0)
  {
    return 0;
  }

  /* 1. POLL (broadcast): immediate TX, then the radio goes to RX by itself. */
  radio_reset_state();
  frame_header(f, MSG_POLL, BALL_ADDR, BROADCAST_ADDR);
  f[HEADER_LEN] = n;
  int rc = frame_send(f, DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);
  if (rc)
  {
    radio_reset_state();
    return rc;
  }
  rc = wait_tx();
  if (rc)
  {
    return rc;
  }
  uint32_t poll_tx = dwt_readtxtimestamplo32();

  /* 2. RESPONSEs: each registered pouch answers in its own timeslot. */
  uint8_t heard = collect(MSG_RESPONSE, registered, window_ms(n), on_response);
  if (!heard)
  {
    return 0;
  }

  /* 3. FINAL (broadcast) at a precisely scheduled time a little after "now". The radio
   * takes the upper 32 of its 40 time bits, so we schedule in those units (1 UUS = 256
   * units) and compute the exact timestamp it will stamp. The mask tells the pouches
   * which of them were heard and must send a REPORT. */
  uint32_t delayed = dwt_readsystimestamphi32() + (uint32_t)(FINAL_MARGIN_UUS * 256ULL);
  uint32_t final_tx = delayed_tx_timestamp(delayed);
  frame_header(f, MSG_FINAL, BALL_ADDR, BROADCAST_ADDR);
  f[HEADER_LEN] = heard;
  dwt_setdelayedtrxtime(delayed);
  rc = frame_send(f, DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
  if (rc)
  {
    radio_reset_state();
    return rc;
  }

  /* 4. REPORTs: each heard pouch sends its three timestamps in its timeslot. */
  uint8_t reported = collect(MSG_REPORT, heard,
                             window_ms(n) + (FINAL_MARGIN_UUS * 1026) / 1000000 + 2, on_report);

  /* 5. All six timestamps known per pouch: calculate the distances. */
  int count = 0;
  for (int s = 0; s < MAX_POUCHES && count < max; s++)
  {
    int32_t mm;
    if ((reported & (1U << s)) &&
        twr_distance(poll_tx, (uint32_t)resp_rx_ts[s], final_tx, rep_poll_rx[s],
                     rep_resp_tx[s], rep_final_rx[s], &mm))
    {
      mm -= RANGE_OFFSET_MM;
      if (mm >= 0)
      {
        out[count++] = (struct pouch_range){s, table_uid[s], mm, resp_lives[s]};
      }
    }
  }
  return count;
}

int uwb_ball_send_hit(int slot, uint16_t hit_seq)
{
  uint8_t f[MAX_FRAME];
  uint16_t pouch = POUCH_ADDR_BASE + slot;

  radio_reset_state();
  frame_header(f, MSG_HIT, BALL_ADDR, pouch);
  sys_put_le16(hit_seq, f + HEADER_LEN);
  int rc = frame_send(f, DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);
  if (rc)
  {
    radio_reset_state();
    return rc;
  }

  /* The pouch acknowledges in its timeslot (relative to receiving our HIT). */
  int64_t deadline = k_uptime_get() + window_ms(slot + 1);
  while (k_uptime_get() < deadline)
  {
    rc = frame_poll(f);
    if (rc == -EAGAIN)
    {
      k_usleep(100);
      continue;
    }
    if (rc == 0 && frame_type(f) == MSG_HIT_ACK && frame_src(f) == pouch &&
        frame_dst(f) == BALL_ADDR && sys_get_le16(f + HEADER_LEN) == hit_seq)
    {
      radio_reset_state();
      return 0;
    }
    dwt_rxenable(DWT_START_RX_IMMEDIATE);
    rx_on = true;
  }
  radio_reset_state();
  return -ETIMEDOUT;
}

static void on_ignore(int slot, uint64_t rx_ts, const uint8_t *f)
{
  (void)slot; (void)rx_ts; (void)f;
}

uint8_t uwb_ball_send_reset(void)
{
  uint8_t f[MAX_FRAME];
  int n = highest_slot + 1;

  if (n <= 0)
  {
    return 0;
  }
  radio_reset_state();
  frame_header(f, MSG_RESET, BALL_ADDR, BROADCAST_ADDR);
  if (frame_send(f, DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED))
  {
    radio_reset_state();
    return 0;
  }
  return collect(MSG_RESET_ACK, uwb_ball_slot_mask(), window_ms(n), on_ignore);
}

/* ===========================================================================
 * Pouch side
 * ========================================================================= */
static uint32_t my_uid;
static enum pouch_phase phase = POUCH_JOINING;
static int my_slot = -1;
static int64_t next_join_ms;
static bool uid_ready;

/* 32-bit ID folded from the 64-bit factory-programmed nRF52 DEVICEID (FNV-1a hash of
 * all 8 bytes). 0 is reserved as "no ID". */
static uint32_t compute_uid(void)
{
  uint8_t id[8] = {0};
  uint32_t h = 2166136261UL;

  hwinfo_get_device_id(id, sizeof(id));
  for (int i = 0; i < 8; i++)
  {
    h = (h ^ id[i]) * 16777619UL;
  }
  return h ? h : 1;
}

uint32_t uwb_pouch_uid(void)
{
  if (!uid_ready)
  {
    my_uid = compute_uid();
    uid_ready = true;
  }
  return my_uid;
}

enum pouch_phase uwb_pouch_phase(void) { return phase; }
int uwb_pouch_slot(void) { return my_slot; }

static uint16_t my_addr(void)
{
  return my_slot >= 0 ? POUCH_ADDR_BASE + my_slot : UNASSIGNED_ADDR;
}

static void pouch_send_join(void)
{
  uint8_t f[MAX_FRAME];

  radio_reset_state();
  frame_header(f, MSG_JOIN, UNASSIGNED_ADDR, BALL_ADDR);
  sys_put_le32(uwb_pouch_uid(), f + HEADER_LEN);
  /* Response expected: the radio stays in RX afterwards to hear the ASSIGN. */
  if (frame_send(f, DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED))
  {
    radio_reset_state();
  }
  /* Random delay before the next attempt, so pouches turned on together don't keep colliding. */
  next_join_ms = k_uptime_get() + JOIN_RETRY_MIN_MS +
                 sys_rand32_get() % (JOIN_RETRY_MAX_MS - JOIN_RETRY_MIN_MS + 1);
}

/* Reply `f` in this pouch's timeslot, relative to the RX time of the frame just received. */
static void pouch_reply_in_slot(uint8_t *f, uint64_t trigger_rx)
{
  dwt_setdelayedtrxtime(slot_tx_time(trigger_rx, my_slot));
  if (frame_send(f, DWT_START_TX_DELAYED) == 0)
  {
    wait_tx();
  }
  else
  {
    radio_reset_state();
  }
}

/* A POLL arrived: run the pouch's half of one ranging exchange. */
static void pouch_handle_poll(const uint8_t *poll, uint8_t lives)
{
  uint8_t f[MAX_FRAME];
  uint64_t poll_rx = rx_timestamp();
  uint8_t n_slots = poll[HEADER_LEN];

  /* RESPONSE in our timeslot. We know its exact TX timestamp up front. */
  uint32_t delayed = slot_tx_time(poll_rx, my_slot);
  uint32_t resp_tx = delayed_tx_timestamp(delayed);
  frame_header(f, MSG_RESPONSE, my_addr(), BALL_ADDR);
  f[HEADER_LEN] = lives;
  dwt_setdelayedtrxtime(delayed);
  if (frame_send(f, DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED))
  {
    radio_reset_state();
    return;
  }

  /* Wait for FINAL. The radio is idle until the delayed TX happens, then listens. */
  int64_t deadline = k_uptime_get() + window_ms(n_slots) + 10;
  while (k_uptime_get() < deadline)
  {
    int rc = frame_poll(f);
    if (rc == -EAGAIN)
    {
      k_usleep(100);
      continue;
    }
    if (rc == 0 && frame_type(f) == MSG_FINAL && frame_src(f) == BALL_ADDR)
    {
      uint64_t final_rx = rx_timestamp();
      if (f[HEADER_LEN] & (1U << my_slot)) /* ball heard us: send our timestamps */
      {
        uint8_t r[MAX_FRAME];
        frame_header(r, MSG_REPORT, my_addr(), BALL_ADDR);
        sys_put_le32((uint32_t)poll_rx, r + HEADER_LEN);
        sys_put_le32(resp_tx, r + HEADER_LEN + 4);
        sys_put_le32((uint32_t)final_rx, r + HEADER_LEN + 8);
        pouch_reply_in_slot(r, final_rx);
      }
      break;
    }
    dwt_rxenable(DWT_START_RX_IMMEDIATE); /* unrelated frame: keep listening */
    rx_on = true;
  }
  radio_reset_state();
}

static void pouch_dispatch(uint8_t *f, uint8_t lives, struct pouch_event *ev)
{
  if (frame_src(f) != BALL_ADDR)
  {
    return;
  }
  uint16_t dst = frame_dst(f);

  switch (frame_type(f))
  {
  case MSG_ASSIGN:
    if (phase == POUCH_JOINING && sys_get_le32(f + HEADER_LEN) == uwb_pouch_uid() &&
        f[HEADER_LEN + 4] < MAX_POUCHES)
    {
      my_slot = f[HEADER_LEN + 4];
      phase = POUCH_JOINED;
    }
    break;

  case MSG_POLL:
    if (dst != BROADCAST_ADDR)
    {
      break;
    }
    if (phase == POUCH_JOINING)
    {
      /* The ball is already ranging, so roll call is over and we have no timeslot. */
      phase = POUCH_JOIN_FAILED;
      radio_reset_state();
    }
    else
    {
      phase = POUCH_ACTIVE;
      pouch_handle_poll(f, lives);
    }
    break;

  case MSG_HIT:
    if (phase != POUCH_JOINING && dst == my_addr())
    {
      phase = POUCH_ACTIVE;
      uint64_t hit_rx = rx_timestamp();
      uint16_t seq = sys_get_le16(f + HEADER_LEN);
      frame_header(f, MSG_HIT_ACK, my_addr(), BALL_ADDR);
      sys_put_le16(seq, f + HEADER_LEN);
      pouch_reply_in_slot(f, hit_rx);
      ev->type = EV_HIT;
      ev->hit_seq = seq;
    }
    break;

  case MSG_RESET:
    if (phase != POUCH_JOINING && dst == BROADCAST_ADDR)
    {
      phase = POUCH_ACTIVE;
      uint64_t reset_rx = rx_timestamp();
      frame_header(f, MSG_RESET_ACK, my_addr(), BALL_ADDR);
      pouch_reply_in_slot(f, reset_rx);
      ev->type = EV_RESET;
    }
    break;
  }
}

void uwb_pouch_step(uint8_t lives, struct pouch_event *ev)
{
  uint8_t f[MAX_FRAME];

  ev->type = EV_NONE;
  if (phase == POUCH_JOIN_FAILED)
  {
    k_msleep(5);
    return;
  }
  if (phase == POUCH_JOINING && k_uptime_get() >= next_join_ms)
  {
    pouch_send_join();
  }

  rx_ensure();
  int rc = frame_poll(f);
  if (rc == -EAGAIN)
  {
    k_usleep(100);
    return;
  }
  if (rc == 0)
  {
    pouch_dispatch(f, lives, ev);
  }
  /* The single-shot RX is over; rx_ensure() re-arms it on the next step. */
}
