/*
 * dodgit - UWB ranging demo, MASTER (DWM3001CDK)
 *
 * Plug this board into the laptop and watch the console (J-Link USB serial).
 * The other board runs examples/uwb_slave (on a battery) and only answers.
 *
 * Ranging mode (RANGING_MODE below):
 *   MODE_POLLING : range to the slave POLLING_RATE_HZ times per second.
 *   MODE_TRIGGER : range once every time the user button (SW0) is pressed.
 *
 * For every attempt the master measures how long the exchange took (from just
 * before the first frame is sent until the distance has been calculated) and
 * prints it with the distance:
 *   RANGE seq=7 distance=1234 mm duration=6.812 ms
 *   RANGE_FAIL seq=8 err=-116 duration=15.020 ms     (e.g. slave off / out of range)
 *
 * LEDs: led3 (blue) on = master, led0 (green) flashes on every good range,
 *       led1 (red) flashes on a failed range, led1 + led2 steady = init error.
 *
 * Ranging method: double-sided two-way ranging (DS-TWR) with 4 frames.
 *
 *   MASTER                                        SLAVE
 *   POLL     ------------------------------->    (poll_rx   = RX time)
 *   (resp_rx = RX time)  <-------------------     RESPONSE   (resp_tx = TX time)
 *   FINAL    ------------------------------->    (final_rx  = RX time)
 *     carries poll_tx, resp_rx, final_tx
 *   REPORT   <-------------------------------     carries poll_rx, resp_tx, final_rx
 *
 * The slave only timestamps. The master ends up with all six timestamps and
 * calculates the distance itself, so the printed duration covers the full
 * measurement. All timestamps come from each radio's own clock; the clocks are
 * never synchronised (the DS-TWR formula cancels their offset and most drift).
 */

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/gpio.h>
#include <deca_device_api.h>
#include <deca_probe_interface.h>
#include <dw3000_hw.h>
#include <dw3000_spi.h>
#include "dw3000/dw3000_deca_regs.h"

/* ---------------------------------------------------------------------------
 * Settings
 * ------------------------------------------------------------------------- */
#define MODE_POLLING 0
#define MODE_TRIGGER 1

/* >>> GLOBAL PARAMETER: pick MODE_POLLING or MODE_TRIGGER <<<
 * (or build with -DRANGING_MODE=MODE_TRIGGER) */
#ifndef RANGING_MODE
#define RANGING_MODE MODE_TRIGGER
#endif

#define POLLING_RATE_HZ 5       /* MODE_POLLING: ranges per second */
#define BUTTON_DEBOUNCE_MS 20   /* MODE_TRIGGER */

#define ANTENNA_DELAY 16385     /* generic default, not calibrated per board */
#define RANGE_OFFSET_MM 0       /* subtracted from the result; calibrate if biased */

/* Timing of the exchange */
#define REPLY_UUS 5000ULL       /* master sends FINAL this long after RESPONSE (~5.1 ms) */
#define UUS_TO_DTU 65536ULL     /* 1 UUS (~1.026 us) = 65536 device time units (~15.65 ps) */
#define RX_TIMEOUT_UUS 15000    /* radio gives up listening after ~15 ms */
#define SW_TIMEOUT_MS 25        /* software guard around every TX/RX wait */

/* ---------------------------------------------------------------------------
 * Frame format (simplified IEEE 802.15.4 data frame)
 *   [0..1] frame control  [2] sequence  [3..4] PAN  [5..6] dest  [7..8] source
 *   [9] message type      [10..] payload       (+2 CRC bytes added by the radio)
 * ------------------------------------------------------------------------- */
#define MASTER_ID 1
#define SLAVE_ID 2
#define PAN_ID 0xD048
#define HEADER_LEN 10
#define CRC_LEN 2
#define MAX_FRAME 32

#define MSG_POLL 0x31
#define MSG_RESPONSE 0x32
#define MSG_FINAL 0x33
#define MSG_REPORT 0x34

/* Payload sizes in bytes */
#define POLL_PAYLOAD 0
#define RESPONSE_PAYLOAD 0
#define FINAL_PAYLOAD 12  /* poll_tx, resp_rx, final_tx          (3 x u32) */
#define REPORT_PAYLOAD 12 /* poll_rx, resp_tx, final_rx          (3 x u32) */

/* ---------------------------------------------------------------------------
 * LEDs and button (aliases from the decawave_dwm3001cdk board DTS)
 * ------------------------------------------------------------------------- */
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_red1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_red2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static bool hw_init(void)
{
  const struct gpio_dt_spec *leds[] = {&led_green, &led_red1, &led_red2, &led_blue};
  for (int i = 0; i < ARRAY_SIZE(leds); i++)
  {
    if (!gpio_is_ready_dt(leds[i]) || gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE) < 0)
    {
      printk("LED %d init failed\n", i);
      return false;
    }
  }
  if (!gpio_is_ready_dt(&button) || gpio_pin_configure_dt(&button, GPIO_INPUT) < 0)
  {
    printk("Button init failed\n");
    return false;
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * DW3000 radio setup
 * ------------------------------------------------------------------------- */
static uint32_t last_status; /* last SYS_STATUS register value read (for debugging) */

/* Channel 5, 128-symbol preamble (code 9), 6.8 Mbps, no STS / PDoA: plain ranging. */
static dwt_config_t radio_config = {
    5, DWT_PLEN_128, DWT_PAC8, 9, 9, 1, DWT_BR_6M8,
    DWT_PHRMODE_STD, DWT_PHRRATE_STD, 129,
    DWT_STS_MODE_OFF, DWT_STS_LEN_64, DWT_PDOA_M0,
};

/* Stop whatever the radio is doing and clear all RX/TX status flags. */
static void radio_reset_state(void)
{
  dwt_forcetrxoff();
  dwt_writesysstatuslo(SYS_STATUS_ALL_RX_GOOD | SYS_STATUS_ALL_RX_ERR |
                       SYS_STATUS_ALL_RX_TO | SYS_STATUS_TXFRS_BIT_MASK);
}

static int radio_init(void)
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
  printk("DW3000 device ID: 0x%08x\n", dwt_readdevid());

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
  dwt_setrxaftertxdelay(0);               /* "response expected": RX starts right after TX */
  dwt_setrxtimeout(RX_TIMEOUT_UUS);
  radio_reset_state();
  return 0;
}

/* ---------------------------------------------------------------------------
 * Frame helpers
 * ------------------------------------------------------------------------- */
static void frame_header(uint8_t *f, uint8_t type, uint8_t src, uint8_t dst, uint8_t seq)
{
  memset(f, 0, MAX_FRAME);
  f[0] = 0x41; /* data frame, PAN ID compression, short addresses */
  f[1] = 0x88;
  f[2] = seq;
  sys_put_le16(PAN_ID, f + 3);
  sys_put_le16(dst, f + 5);
  sys_put_le16(src, f + 7);
  f[9] = type;
}

/* Send a frame (header already filled, payload_len payload bytes after it).
 * mode: DWT_START_TX_IMMEDIATE or DWT_START_TX_DELAYED, optionally
 * OR-ed with DWT_RESPONSE_EXPECTED to switch to RX automatically afterwards. */
static int frame_send(uint8_t *f, unsigned payload_len, uint8_t mode)
{
  unsigned len = HEADER_LEN + payload_len + CRC_LEN;

  dwt_writesysstatuslo(SYS_STATUS_TXFRS_BIT_MASK);
  if (dwt_writetxdata(len, f, 0) != DWT_SUCCESS)
  {
    return -EIO;
  }
  dwt_writetxfctrl(len, 0, 1); /* last arg 1 = this is a ranging frame */
  return dwt_starttx(mode) == DWT_SUCCESS ? 0 : -ETIME;
}

/* Wait for a good frame of the expected type from src to dst with this
 * sequence number (the radio has to be in RX already). */
static int frame_receive(uint8_t *f, unsigned payload_len, uint8_t type,
                         uint8_t src, uint8_t dst, uint8_t seq)
{
  int64_t deadline = k_uptime_get() + SW_TIMEOUT_MS;

  do
  {
    last_status = dwt_readsysstatuslo();

    if (last_status & SYS_STATUS_RXFCG_BIT_MASK) /* frame received, CRC good */
    {
      uint16_t len = dwt_getframelength(NULL);
      dwt_writesysstatuslo(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_TXFRS_BIT_MASK);
      if (len != HEADER_LEN + payload_len + CRC_LEN || len > MAX_FRAME)
      {
        return -EBADMSG;
      }
      dwt_readrxdata(f, len, 0);
      if (f[0] != 0x41 || f[1] != 0x88 || sys_get_le16(f + 3) != PAN_ID ||
          sys_get_le16(f + 5) != dst || sys_get_le16(f + 7) != src ||
          f[9] != type || f[2] != seq)
      {
        return -EBADMSG;
      }
      return 0;
    }
    if (last_status & (SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO))
    {
      radio_reset_state();
      return -EIO; /* hardware RX error or RX timeout */
    }
    k_usleep(100);
  } while (k_uptime_get() < deadline);

  radio_reset_state();
  return -ETIMEDOUT;
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

/* ---------------------------------------------------------------------------
 * DS-TWR distance calculation
 *
 * All timestamps are the low 32 bits of the 40-bit device time. Unsigned
 * subtraction gives the right interval even across the 32-bit rollover
 * (every ~67 ms) as long as each interval is shorter than that.
 *
 *   Ra = resp_rx - poll_tx   master: POLL sent -> RESPONSE received
 *   Da = final_tx - resp_rx  master: RESPONSE received -> FINAL sent
 *   Rb = final_rx - resp_tx  slave:  RESPONSE sent -> FINAL received
 *   Db = resp_tx - poll_rx   slave:  POLL received -> RESPONSE sent
 *
 *   time of flight = (Ra*Rb - Da*Db) / (Ra + Rb + Da + Db)
 * ------------------------------------------------------------------------- */
static bool twr_distance(uint32_t poll_tx, uint32_t resp_rx, uint32_t final_tx,
                         uint32_t poll_rx, uint32_t resp_tx, uint32_t final_rx,
                         int32_t *distance_mm)
{
  double ra = (uint32_t)(resp_rx - poll_tx);
  double da = (uint32_t)(final_tx - resp_rx);
  double rb = (uint32_t)(final_rx - resp_tx);
  double db = (uint32_t)(resp_tx - poll_rx);

  /* Sanity check: every interval must be positive and below 30 ms. */
  const double max_interval = 30000.0 * 65536.0;
  if (ra <= 0 || da <= 0 || rb <= 0 || db <= 0 ||
      ra > max_interval || da > max_interval || rb > max_interval || db > max_interval)
  {
    return false;
  }

  double tof_dtu = (ra * rb - da * db) / (ra + rb + da + db);

  /* 1 device time unit = 1 / (499.2 MHz * 128); c in air = 299702547 m/s. */
  double mm = tof_dtu * (299702547.0 * 1000.0 / (499200000.0 * 128.0));
  if (mm < -1000.0 || mm > 50000.0)
  {
    return false;
  }
  *distance_mm = (int32_t)(mm + (mm >= 0 ? 0.5 : -0.5)) - RANGE_OFFSET_MM;
  return true;
}

/* ---------------------------------------------------------------------------
 * One complete ranging exchange. Returns 0 and the distance, or a negative errno.
 * ------------------------------------------------------------------------- */
static int range_once(uint8_t seq, int32_t *distance_mm)
{
  uint8_t f[MAX_FRAME];
  int rc;

  radio_reset_state();

  /* 1. POLL: immediate TX, then the radio goes to RX by itself. */
  frame_header(f, MSG_POLL, MASTER_ID, SLAVE_ID, seq);
  rc = frame_send(f, POLL_PAYLOAD, DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);
  if (rc)
  {
    return rc;
  }

  /* 2. Wait for RESPONSE, then take the two timestamps we need from it. */
  rc = frame_receive(f, RESPONSE_PAYLOAD, MSG_RESPONSE, SLAVE_ID, MASTER_ID, seq);
  if (rc)
  {
    return rc;
  }
  uint32_t poll_tx = dwt_readtxtimestamplo32(); /* last TX was the POLL */
  uint64_t resp_rx = rx_timestamp();

  /* 3. FINAL: sent at a precisely scheduled time (resp_rx + REPLY_UUS). The radio
   * only takes the upper 32 of the 40 time bits (>> 8) and ignores the lowest of
   * those, so we compute the exact timestamp the radio will use, add the TX
   * antenna delay, and put it in the frame before it is sent. */
  uint32_t delayed = (uint32_t)((resp_rx + REPLY_UUS * UUS_TO_DTU) >> 8);
  uint32_t final_tx = (uint32_t)(((uint64_t)(delayed & 0xFFFFFFFEUL) << 8) + ANTENNA_DELAY);

  frame_header(f, MSG_FINAL, MASTER_ID, SLAVE_ID, seq);
  sys_put_le32(poll_tx, f + HEADER_LEN);
  sys_put_le32((uint32_t)resp_rx, f + HEADER_LEN + 4);
  sys_put_le32(final_tx, f + HEADER_LEN + 8);
  dwt_setdelayedtrxtime(delayed);
  rc = frame_send(f, FINAL_PAYLOAD, DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
  if (rc)
  {
    radio_reset_state();
    return rc;
  }

  /* 4. Wait for REPORT: the slave's three timestamps. */
  rc = frame_receive(f, REPORT_PAYLOAD, MSG_REPORT, SLAVE_ID, MASTER_ID, seq);
  if (rc)
  {
    return rc;
  }
  uint32_t poll_rx = sys_get_le32(f + HEADER_LEN);
  uint32_t resp_tx = sys_get_le32(f + HEADER_LEN + 4);
  uint32_t final_rx = sys_get_le32(f + HEADER_LEN + 8);

  /* 5. All six timestamps known: calculate the distance. */
  if (!twr_distance(poll_tx, (uint32_t)resp_rx, final_tx, poll_rx, resp_tx, final_rx,
                    distance_mm))
  {
    return -ERANGE;
  }
  return 0;
}

/* Run one exchange, time it, and print the result to the console. */
static void range_and_print(uint8_t seq)
{
  int32_t distance_mm = 0;

  /* The timer starts when ranging is initiated and stops once the distance has
   * been calculated (or the attempt failed). The system timer ticks at
   * 32.768 kHz, so the resolution is ~30 us. */
  uint32_t start = k_cycle_get_32();
  int rc = range_once(seq, &distance_mm);
  uint32_t duration_us = k_cyc_to_us_floor32(k_cycle_get_32() - start);

  if (rc == 0)
  {
    printk("RANGE seq=%u distance=%d mm duration=%u.%03u ms\n", seq, distance_mm,
           duration_us / 1000, duration_us % 1000);
    gpio_pin_toggle_dt(&led_green);
  }
  else
  {
    printk("RANGE_FAIL seq=%u err=%d duration=%u.%03u ms\n", seq, rc,
           duration_us / 1000, duration_us % 1000);
    gpio_pin_set_dt(&led_red1, 1);
    k_msleep(20);
    gpio_pin_set_dt(&led_red1, 0);
  }
}

/* ---------------------------------------------------------------------------
 * Trigger mode helper: returns true once per button press.
 * ------------------------------------------------------------------------- */
static bool button_pressed(void)
{
  if (gpio_pin_get_dt(&button) <= 0)
  {
    return false;
  }
  k_msleep(BUTTON_DEBOUNCE_MS);
  if (gpio_pin_get_dt(&button) <= 0)
  {
    return false; /* was just contact bounce */
  }
  while (gpio_pin_get_dt(&button) > 0) /* wait for release: one press = one range */
  {
    k_msleep(5);
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */
int main(void)
{
  if (!hw_init())
  {
    return 0;
  }
  gpio_pin_set_dt(&led_blue, 1); /* blue = master */

  int rc = radio_init();
  if (rc)
  {
    printk("UWB init failed: %d\n", rc);
    gpio_pin_set_dt(&led_red1, 1);
    gpio_pin_set_dt(&led_red2, 1);
    return 0;
  }

#if RANGING_MODE == MODE_POLLING
  printk("UWB master ready: POLLING mode, %d Hz\n", POLLING_RATE_HZ);
#else
  printk("UWB master ready: TRIGGER mode, press SW0 to range\n");
#endif

  uint8_t seq = 0;

  while (1)
  {
#if RANGING_MODE == MODE_POLLING
    int64_t next = k_uptime_get() + 1000 / POLLING_RATE_HZ;
    range_and_print(seq++);
    int64_t wait = next - k_uptime_get();
    if (wait > 0)
    {
      k_msleep(wait);
    }
#else
    if (button_pressed())
    {
      range_and_print(seq++);
    }
    k_msleep(10);
#endif
  }
}
