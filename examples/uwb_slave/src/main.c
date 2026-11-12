/*
 * dodgit - UWB ranging demo, SLAVE (DWM3001CDK)
 *
 * Power this board from a battery and leave it. It never starts anything: it
 * listens for a POLL from examples/uwb_master and answers the DS-TWR exchange
 * (see the diagram in uwb_master/src/main.c). It only records timestamps; the
 * master calculates the distance.
 *
 *   MASTER                                        SLAVE
 *   POLL     ------------------------------->    poll_rx  = RX time
 *   RESPONSE <-------------------------------    resp_tx  = TX time
 *   FINAL    ------------------------------->    final_rx = RX time
 *   REPORT   <-------------------------------    carries poll_rx, resp_tx, final_rx
 *
 * LEDs: led0 (green) toggles after every completed exchange,
 *       led1 + led2 (red) steady = radio init failed.
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
 * Settings (must match uwb_master)
 * ------------------------------------------------------------------------- */
#define ANTENNA_DELAY 16385  /* generic default, not calibrated per board */
#define RX_TIMEOUT_UUS 15000 /* radio gives up listening after ~15 ms, then we listen again */
#define SW_TIMEOUT_MS 25     /* software guard around every TX/RX wait */

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

#define POLL_PAYLOAD 0
#define RESPONSE_PAYLOAD 0
#define FINAL_PAYLOAD 12  /* poll_tx, resp_rx, final_tx (unused by the slave) */
#define REPORT_PAYLOAD 12 /* poll_rx, resp_tx, final_rx */

static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_red1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_red2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

/* ---------------------------------------------------------------------------
 * DW3000 radio setup (identical to the master)
 * ------------------------------------------------------------------------- */
static uint32_t last_status;

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

  dwt_txconfig_t tx_rf = {0x34, 0xFDFDFDFD, 0};
  dwt_configuretxrf(&tx_rf);
  dwt_settxantennadelay(ANTENNA_DELAY);
  dwt_setrxantennadelay(ANTENNA_DELAY);
  dwt_setrxaftertxdelay(0);
  dwt_setrxtimeout(RX_TIMEOUT_UUS);
  radio_reset_state();
  return 0;
}

/* ---------------------------------------------------------------------------
 * Frame helpers (identical to the master, except frame_receive takes seq < 0
 * to accept any sequence number: the slave learns it from the POLL)
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

/* Wait until the last frame has really left the antenna. */
static int frame_wait_sent(void)
{
  int64_t deadline = k_uptime_get() + SW_TIMEOUT_MS;

  do
  {
    last_status = dwt_readsysstatuslo();
    if (last_status & SYS_STATUS_TXFRS_BIT_MASK)
    {
      return 0;
    }
    k_usleep(100);
  } while (k_uptime_get() < deadline);

  radio_reset_state();
  return -ETIMEDOUT;
}

static int frame_receive(uint8_t *f, unsigned payload_len, uint8_t type,
                         uint8_t src, uint8_t dst, int seq)
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
          f[9] != type || (seq >= 0 && f[2] != seq))
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
 * Answer one ranging exchange. Returns 0 if all four frames were handled;
 * a negative errno means "nothing (valid) heard" and the caller just listens again.
 * ------------------------------------------------------------------------- */
static int respond_once(void)
{
  uint8_t f[MAX_FRAME];
  int rc;

  /* 1. Listen for a POLL from the master. */
  radio_reset_state();
  if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS)
  {
    return -EIO;
  }
  rc = frame_receive(f, POLL_PAYLOAD, MSG_POLL, MASTER_ID, SLAVE_ID, -1);
  if (rc)
  {
    return rc;
  }
  uint32_t poll_rx = (uint32_t)rx_timestamp();
  uint8_t seq = f[2];

  /* 2. RESPONSE right away; the radio switches to RX afterwards to catch FINAL.
   * The delay between POLL and RESPONSE does not have to be fixed: DS-TWR uses
   * the real timestamps (poll_rx, resp_tx), whatever the delay turned out to be. */
  frame_header(f, MSG_RESPONSE, SLAVE_ID, MASTER_ID, seq);
  rc = frame_send(f, RESPONSE_PAYLOAD, DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);
  if (rc)
  {
    radio_reset_state();
    return rc;
  }

  /* 3. Wait for FINAL, then read our two remaining timestamps. */
  rc = frame_receive(f, FINAL_PAYLOAD, MSG_FINAL, MASTER_ID, SLAVE_ID, seq);
  if (rc)
  {
    return rc;
  }
  uint32_t final_rx = (uint32_t)rx_timestamp();
  uint32_t resp_tx = dwt_readtxtimestamplo32(); /* last TX was the RESPONSE */

  /* 4. REPORT our three timestamps to the master, which calculates the distance. */
  frame_header(f, MSG_REPORT, SLAVE_ID, MASTER_ID, seq);
  sys_put_le32(poll_rx, f + HEADER_LEN);
  sys_put_le32(resp_tx, f + HEADER_LEN + 4);
  sys_put_le32(final_rx, f + HEADER_LEN + 8);
  rc = frame_send(f, REPORT_PAYLOAD, DWT_START_TX_IMMEDIATE);
  if (rc)
  {
    radio_reset_state();
    return rc;
  }
  return frame_wait_sent();
}

/* ---------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */
int main(void)
{
  if (!gpio_is_ready_dt(&led_green) || !gpio_is_ready_dt(&led_red1) ||
      !gpio_is_ready_dt(&led_red2) ||
      gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE) < 0 ||
      gpio_pin_configure_dt(&led_red1, GPIO_OUTPUT_INACTIVE) < 0 ||
      gpio_pin_configure_dt(&led_red2, GPIO_OUTPUT_INACTIVE) < 0)
  {
    printk("LED init failed\n");
    return 0;
  }

  int rc = radio_init();
  if (rc)
  {
    printk("UWB init failed: %d\n", rc);
    gpio_pin_set_dt(&led_red1, 1);
    gpio_pin_set_dt(&led_red2, 1);
    return 0;
  }
  printk("UWB slave ready: waiting for POLL\n");

  while (1)
  {
    if (respond_once() == 0)
    {
      gpio_pin_toggle_dt(&led_green);
    }
  }
}
