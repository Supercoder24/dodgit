/*
 * dodgit - accelerometer BLE streamer (DWM3001CDK)
 *
 * Samples the on-board accelerometer at SAMPLE_RATE_HZ over +/-16 g and streams
 * the samples over the Nordic UART Service so impacts can be recorded while the
 * ball is thrown (see tools/capture.py).
 *
 * LEDs: led3 (blue) on = running, led0 (green) toggles for every packet sent.
 *
 * Packet (little endian, sent as one NUS notification):
 *   u16 dropped      total samples dropped so far (buffer overflow or send failure)
 *   N x sample       u32 t_us, i16 x_mg, i16 y_mg, i16 z_mg   (10 bytes each)
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>

#define SAMPLE_RATE_HZ 400
#define FULL_SCALE_G 16
#define SEND_PERIOD_MS 40
#define MAX_SAMPLES_PER_PACKET 20
#define QUEUE_DEPTH 400 /* 1 s of samples */

struct __packed sample
{
  uint32_t t_us;
  int16_t x_mg;
  int16_t y_mg;
  int16_t z_mg;
};

K_MSGQ_DEFINE(sample_q, sizeof(struct sample), QUEUE_DEPTH, 4);

static const struct device *const accel = DEVICE_DT_GET(DT_ALIAS(accel0));
static const struct gpio_dt_spec led_run = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
static const struct gpio_dt_spec led_tx = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static struct bt_conn *current_conn;
static volatile bool streaming;
static uint16_t dropped;

/* ---------------------------------------------------------------------------
 * Sensor
 * ------------------------------------------------------------------------- */
static bool accel_init(void)
{
  struct sensor_value val;

  if (!device_is_ready(accel))
  {
    printk("accel: device %s not ready\n", accel->name);
    return false;
  }

  /* Impacts are far above the +/-2 g default range. */
  sensor_g_to_ms2(FULL_SCALE_G, &val);
  if (sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &val) != 0)
  {
    printk("accel: failed to set +/-%d g range\n", FULL_SCALE_G);
    return false;
  }

  val.val1 = SAMPLE_RATE_HZ;
  val.val2 = 0;
  if (sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &val) != 0)
  {
    printk("accel: failed to set %d Hz sampling rate\n", SAMPLE_RATE_HZ);
    return false;
  }
  return true;
}

static int16_t to_mg(const struct sensor_value *v)
{
  return (int16_t)(sensor_value_to_double(v) / 9.80665 * 1000.0);
}

static void sampler_fn(void *p1, void *p2, void *p3)
{
  struct k_timer tick;
  k_timer_init(&tick, NULL, NULL);
  k_timer_start(&tick, K_NO_WAIT, K_USEC(1000000 / SAMPLE_RATE_HZ));

  while (1)
  {
    k_timer_status_sync(&tick);
    if (!streaming)
    {
      continue;
    }

    struct sensor_value v[3];
    struct sample s;

    if (sensor_sample_fetch(accel) != 0 || sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, v) != 0)
    {
      dropped++;
      continue;
    }
    s.t_us = (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
    s.x_mg = to_mg(&v[0]);
    s.y_mg = to_mg(&v[1]);
    s.z_mg = to_mg(&v[2]);

    if (k_msgq_put(&sample_q, &s, K_NO_WAIT) != 0)
    {
      /* Link is behind: drop the oldest sample so the newest data survives. */
      struct sample old;
      k_msgq_get(&sample_q, &old, K_NO_WAIT);
      k_msgq_put(&sample_q, &s, K_NO_WAIT);
      dropped++;
    }
  }
}
K_THREAD_DEFINE(sampler, 1024, sampler_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, 0);

/* ---------------------------------------------------------------------------
 * Bluetooth
 * ------------------------------------------------------------------------- */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

static void advertise(void)
{
  int rc = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
                                           BT_GAP_ADV_FAST_INT_MAX_2, NULL),
                           ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
  printk("Advertising %s (rc = %d)\n", CONFIG_BT_DEVICE_NAME, rc);
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
  if (err)
  {
    printk("Connection failed (err %u)\n", err);
    return;
  }
  printk("Connected\n");
  current_conn = bt_conn_ref(conn);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
  printk("Disconnected (reason %u)\n", reason);
  streaming = false;
  if (current_conn)
  {
    bt_conn_unref(current_conn);
    current_conn = NULL;
  }
}

/* The connection object is only reusable here, so restart advertising now. */
static void on_recycled(void)
{
  advertise();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .recycled = on_recycled,
};

static void on_send_enabled(bool enabled, void *ctx)
{
  printk("Notifications %s\n", enabled ? "enabled" : "disabled");
  if (enabled)
  {
    k_msgq_purge(&sample_q);
    dropped = 0;
    streaming = true;
  }
  else
  {
    streaming = false;
  }
}

static struct bt_nus_cb nus_callbacks = {
    .notif_enabled = on_send_enabled,
};

static void send_batch(void)
{
  uint8_t buf[2 + MAX_SAMPLES_PER_PACKET * sizeof(struct sample)];
  struct bt_conn *conn = current_conn;
  /* ATT notifications reserve three bytes for the opcode and handle. */
  uint32_t mtu = conn ? bt_gatt_get_mtu(conn) - 3U : 0;
  uint32_t max_n = mtu > 2 ? MIN((mtu - 2) / sizeof(struct sample), MAX_SAMPLES_PER_PACKET) : 0;

  if (max_n == 0)
  {
    return;
  }

  uint32_t n = 0;
  struct sample s;
  while (n < max_n && k_msgq_get(&sample_q, &s, K_NO_WAIT) == 0)
  {
    memcpy(&buf[2 + n * sizeof(s)], &s, sizeof(s));
    n++;
  }
  if (n == 0)
  {
    return;
  }

  sys_put_le16(dropped, buf);
  if (bt_nus_send(conn, buf, 2 + n * sizeof(s)) != 0)
  {
    dropped += n;
  }
  gpio_pin_toggle_dt(&led_tx);
}

int main(void)
{
  printk("\n===== dodgit accel BLE =====\n");

  if (!gpio_is_ready_dt(&led_run) || !gpio_is_ready_dt(&led_tx) ||
      gpio_pin_configure_dt(&led_run, GPIO_OUTPUT_ACTIVE) < 0 ||
      gpio_pin_configure_dt(&led_tx, GPIO_OUTPUT_INACTIVE) < 0)
  {
    printk("LED init failed\n");
    return 0;
  }

  if (!accel_init())
  {
    return 0;
  }

  if (bt_enable(NULL) != 0 || bt_nus_cb_register(&nus_callbacks, NULL) != 0)
  {
    printk("Bluetooth init failed\n");
    return 0;
  }
  advertise();

  while (1)
  {
    k_msleep(SEND_PERIOD_MS);
    if (streaming)
    {
      send_batch();
    }
  }
  return 0;
}
