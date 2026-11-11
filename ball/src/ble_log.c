/*
 * dodgit - ball debug log over Bluetooth. See ble_log.h.
 *
 * Uses the nRF Connect SDK Nordic UART Service library (CONFIG_BT_NUS, <bluetooth/services/nus.h>),
 * exactly like examples/accel_ble. (NOT the Zephyr-upstream NUS used by accel_ble_nus.)
 * Connection / advertising / notification handling follows accel_ble/src/main.c.
 *
 * Flow: ble_log() formats a line, prints it with printk and, if notifications are enabled,
 * puts it in a queue. A low-priority thread sends queued lines to the laptop, split into
 * pieces that fit the negotiated MTU (bt_nus_get_mtu). Connection and advertising are
 * handled by callbacks, so the main thread never waits for Bluetooth.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <bluetooth/services/nus.h> /* nRF Connect SDK library, not zephyr/bluetooth/services/ */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include "ble_log.h"

struct log_line
{
  char data[160];
};

K_MSGQ_DEFINE(log_q, sizeof(struct log_line), 48, 4);
K_MUTEX_DEFINE(conn_lock);
static struct bt_conn *peer;
static atomic_t subscribed;
static atomic_t dropped;
static atomic_t ready;

uint32_t ble_log_dropped(void) { return (uint32_t)atomic_get(&dropped); }
bool ble_log_connected(void) { return atomic_get(&subscribed) != 0; }

void ble_log(const char *fmt, ...)
{
  struct log_line line;
  va_list args;

  va_start(args, fmt);
  vsnprintf(line.data, sizeof(line.data) - 1, fmt, args);
  va_end(args);
  size_t n = strlen(line.data);
  line.data[n++] = '\n';
  line.data[n] = '\0';
  printk("%s", line.data);
  if (atomic_get(&subscribed) && k_msgq_put(&log_q, &line, K_NO_WAIT))
  {
    atomic_inc(&dropped);
  }
}

/* ---------------------------------------------------------------------------
 * Advertising and connection handling
 * ------------------------------------------------------------------------- */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
static const struct bt_data sd[] = {BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL)};

static void adv_work_fn(struct k_work *work)
{
  int rc = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
                                           BT_GAP_ADV_FAST_INT_MAX_2, NULL),
                           ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
  printk("BLE advertising: rc=%d\n", rc);
}
K_WORK_DEFINE(adv_work, adv_work_fn);

static void connected(struct bt_conn *conn, uint8_t err)
{
  if (err)
  {
    return;
  }
  k_mutex_lock(&conn_lock, K_FOREVER);
  peer = bt_conn_ref(conn);
  k_mutex_unlock(&conn_lock);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
  atomic_clear(&subscribed);
  k_mutex_lock(&conn_lock, K_FOREVER);
  if (peer)
  {
    bt_conn_unref(peer);
  }
  peer = NULL;
  k_mutex_unlock(&conn_lock);
  k_msgq_purge(&log_q);
}

static void recycled(void)
{
  k_work_submit(&adv_work); /* advertise again so the laptop can reconnect */
}

BT_CONN_CB_DEFINE(conn_cbs) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
};

/* The laptop enabled/disabled notifications on the NUS TX characteristic. */
static void on_send_enabled(enum bt_nus_send_status status)
{
  k_msgq_purge(&log_q);
  atomic_set(&subscribed, status == BT_NUS_SEND_STATUS_ENABLED);
}
static struct bt_nus_cb nus_cbs = {.send_enabled = on_send_enabled};

int ble_log_init(void)
{
  int rc = bt_enable(NULL);
  if (rc)
  {
    return rc;
  }
  rc = bt_nus_init(&nus_cbs);
  if (rc)
  {
    return rc;
  }
  atomic_set(&ready, 1);
  k_work_submit(&adv_work);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Sender thread
 * ------------------------------------------------------------------------- */
static void sender(void *a, void *b, void *c)
{
  struct log_line line;

  while (1)
  {
    /* Purging on subscription/disconnect wakes waiters with an error. */
    if (k_msgq_get(&log_q, &line, K_FOREVER) || !atomic_get(&ready))
    {
      continue;
    }
    size_t len = strlen(line.data), pos = 0;
    int64_t deadline = k_uptime_get() + 1500;

    while (pos < len && atomic_get(&subscribed) && k_uptime_get() < deadline)
    {
      k_mutex_lock(&conn_lock, K_FOREVER);
      struct bt_conn *conn = peer ? bt_conn_ref(peer) : NULL;
      k_mutex_unlock(&conn_lock);
      if (!conn)
      {
        break;
      }
      /* Largest payload the link currently allows (0 until the connection is ready). */
      uint32_t mtu = bt_nus_get_mtu(conn);
      size_t chunk = MIN(len - pos, mtu ? mtu : 20);
      int rc = bt_nus_send(conn, line.data + pos, chunk);
      bt_conn_unref(conn);
      if (!rc)
      {
        pos += chunk;
      }
      else if (rc != -ENOMEM && rc != -EAGAIN)
      {
        break;
      }
      k_msleep(5);
    }
    if (pos < len)
    {
      atomic_inc(&dropped);
    }
  }
}
K_THREAD_DEFINE(ble_sender, 2048, sender, NULL, NULL, NULL, 7, 0, 0);
