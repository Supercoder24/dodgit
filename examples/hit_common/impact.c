#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <math.h>
#include "impact.h"
#include "logic.h"

static const struct device *accel = DEVICE_DT_GET(DT_ALIAS(accel0));
K_MUTEX_DEFINE(sensor_lock);
K_MSGQ_DEFINE(impact_q, sizeof(struct impact_event), 8, 8);
static atomic_t running;
static struct impact_gate gate;
static struct impact_event latest;
static int32_t threshold, peak;
static int cooldown;
static bool enqueue;
static uint32_t event_id, errors, dropped;
static int64_t last_valid, warm_until;

int impact_init(int32_t threshold_mg, int cooldown_ms, bool queue_events)
{
    if (!device_is_ready(accel)) return -ENODEV;
    struct sensor_value v;
    sensor_g_to_ms2(16, &v);
    int rc = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &v);
    if (rc) return rc;
    v.val1 = 400; v.val2 = 0;
    rc = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &v);
    if (rc) return rc;
    threshold = threshold_mg;
    cooldown = cooldown_ms;
    enqueue = queue_events;
    event_id = sys_rand32_get() | 1U;
    warm_until = k_uptime_get() + 500;
    gate.ready = true;
    atomic_set(&running, 1);
    return 0;
}

bool impact_get(struct impact_event *event)
{
    return k_msgq_get(&impact_q, event, K_NO_WAIT) == 0;
}

struct impact_report impact_latest(void)
{
    struct impact_report r = {0};
    int64_t now = k_uptime_get();
    k_mutex_lock(&sensor_lock, K_FOREVER);
    if (latest.id && now - latest.time <= WEARABLE_REPORT_MS && now - last_valid <= 25) {
        r.id = latest.id;
        r.age_ms = (uint32_t)(now - latest.time);
        r.mg = latest.mg;
    }
    k_mutex_unlock(&sensor_lock);
    return r;
}

struct impact_status impact_status(void)
{
    k_mutex_lock(&sensor_lock, K_FOREVER);
    struct impact_status s = {peak, errors, dropped,
        atomic_get(&running) && k_uptime_get() - last_valid <= 25 && k_uptime_get() >= warm_until};
    peak = 0;
    k_mutex_unlock(&sensor_lock);
    return s;
}

static void sample_thread(void *a, void *b, void *c)
{
    int64_t next = 0;
    while (1) {
        if (!atomic_get(&running)) { k_msleep(10); continue; }
        int64_t now = k_uptime_get();
        if (!next || now - next > SAMPLE_PERIOD_MS * 2) next = now;
        struct sensor_value xyz[3];
        int rc = sensor_sample_fetch(accel);
        if (!rc) rc = sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, xyz);
        k_mutex_lock(&sensor_lock, K_FOREVER);
        if (rc) {
            if (rc != -ENODATA) errors++;
            if (rc != -ENODATA || now - last_valid > 25) {
                latest.id = 0;
                gate.ready = false;
            }
        } else {
            if (last_valid && now - last_valid > 25) {
                latest.id = 0;
                gate.ready = false;
            }
            last_valid = now;
            double x = sensor_value_to_double(&xyz[0]) * 1000.0 / 9.80665;
            double y = sensor_value_to_double(&xyz[1]) * 1000.0 / 9.80665;
            double z = sensor_value_to_double(&xyz[2]) * 1000.0 / 9.80665;
            int32_t mg = (int32_t)sqrt(x*x + y*y + z*z);
            if (mg > peak) peak = mg;
            if (now >= warm_until && impact_sample(&gate, now, mg, threshold, cooldown)) {
                if (!++event_id) ++event_id;
                latest = (struct impact_event){now, event_id, mg};
                if (enqueue && k_msgq_put(&impact_q, &latest, K_NO_WAIT)) dropped++;
            }
        }
        k_mutex_unlock(&sensor_lock);
        next += SAMPLE_PERIOD_MS;
        int64_t delay = next - k_uptime_get();
        if (delay > 0) k_msleep(delay);
        else k_yield();
    }
}
K_THREAD_DEFINE(sensor_thread, 2048, sample_thread, NULL, NULL, NULL, 5, 0, 0);
