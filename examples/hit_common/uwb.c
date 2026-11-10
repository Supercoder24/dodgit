/* Two-board DS-TWR with a fourth frame returning distance to the ball.
 * One radio owner (main thread); all TX/RX operations have bounded timeouts. */
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <errno.h>
#include <deca_device_api.h>
#include <deca_probe_interface.h>
#include <dw3000_hw.h>
#include <dw3000_spi.h>
#include "dw3000/dw3000_deca_regs.h"
#include "uwb.h"
#include "logic.h"
#include "impact.h"

#define BALL 1
#define WEARABLE 2
#define HEADER 12
#define MAX_FRAME 32
#define REPLY_UUS 5000ULL
#define UUS_TO_DTU 65536ULL
#define RX_TIMEOUT_UUS 15000
#define POLL 0x31
#define RESPONSE 0x32
#define FINAL 0x33
#define RESULT 0x34

static uint32_t last_status;
static dwt_config_t config = {
    5, DWT_PLEN_128, DWT_PAC8, 9, 9, 1, DWT_BR_6M8,
    DWT_PHRMODE_STD, DWT_PHRRATE_STD, 129 + 8 - 8,
    DWT_STS_MODE_OFF, DWT_STS_LEN_64, DWT_PDOA_M0,
};

uint32_t uwb_status(void) { return last_status; }

static void reset_rx(void)
{
    dwt_forcetrxoff();
    dwt_writesysstatuslo(SYS_STATUS_ALL_RX_GOOD | SYS_STATUS_ALL_RX_ERR |
                        SYS_STATUS_ALL_RX_TO | SYS_STATUS_TXFRS_BIT_MASK);
}

int uwb_init(void)
{
    if (dw3000_hw_init() < 0) return -ENODEV;
    dw3000_hw_reset();
    k_msleep(10);
    if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) != DWT_SUCCESS) return -ENODEV;
    printk("UWB_ID,0x%08x\n", dwt_readdevid());
    int64_t deadline = k_uptime_get() + 100;
    while (!dwt_checkidlerc()) {
        if (k_uptime_get() >= deadline) return -ETIMEDOUT;
        k_msleep(1);
    }
    if (dwt_initialise(0) != DWT_SUCCESS) return -EIO;
    dw3000_spi_speed_fast();
    if (dwt_configure(&config) != DWT_SUCCESS) return -EIO;
    dwt_txconfig_t tx = {0x34, 0xFDFDFDFD, 0};
    dwt_configuretxrf(&tx);
    dwt_settxantennadelay(ANTENNA_DELAY);
    dwt_setrxantennadelay(ANTENNA_DELAY);
    dwt_setrxaftertxdelay(0);
    dwt_setrxtimeout(RX_TIMEOUT_UUS);
    reset_rx();
    return 0;
}

static void header(uint8_t *f, uint8_t type, uint8_t src, uint8_t dst, uint16_t seq)
{
    memset(f, 0, MAX_FRAME);
    f[0] = 0x41; f[1] = 0x88; f[2] = (uint8_t)seq;
    sys_put_le16(0xd048, f + 3);
    sys_put_le16(dst, f + 5);
    sys_put_le16(src, f + 7);
    f[9] = type;
    sys_put_le16(seq, f + 10);
}

static bool matches(const uint8_t *f, uint8_t type, uint8_t src, uint8_t dst)
{
    return f[0] == 0x41 && f[1] == 0x88 && sys_get_le16(f + 3) == 0xd048 &&
           sys_get_le16(f + 5) == dst && sys_get_le16(f + 7) == src && f[9] == type;
}

static int transmit(uint8_t *f, unsigned payload, uint8_t mode)
{
    dwt_writesysstatuslo(SYS_STATUS_TXFRS_BIT_MASK);
    if (dwt_writetxdata(HEADER + payload + 2, f, 0) != DWT_SUCCESS) return -EIO;
    dwt_writetxfctrl(HEADER + payload + 2, 0, 1);
    return dwt_starttx(mode) == DWT_SUCCESS ? 0 : -ETIME;
}

static int wait_tx(void)
{
    int64_t deadline = k_uptime_get() + 25;
    do {
        last_status = dwt_readsysstatuslo();
        if (last_status & SYS_STATUS_TXFRS_BIT_MASK) return 0;
        k_usleep(100);
    } while (k_uptime_get() < deadline);
    reset_rx();
    return -ETIMEDOUT;
}

static int receive(uint8_t *f, unsigned payload, uint8_t type,
                   uint8_t src, uint8_t dst, int seq)
{
    int64_t deadline = k_uptime_get() + 25;
    do {
        last_status = dwt_readsysstatuslo();
        if (last_status & SYS_STATUS_RXFCG_BIT_MASK) {
            uint16_t len = dwt_getframelength(NULL);
            dwt_writesysstatuslo(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_TXFRS_BIT_MASK);
            if (len != HEADER + payload + 2 || len > MAX_FRAME) return -EBADMSG;
            dwt_readrxdata(f, len, 0);
            if (!matches(f, type, src, dst) ||
                (seq >= 0 && sys_get_le16(f + 10) != seq)) return -EBADMSG;
            return 0;
        }
        if (last_status & (SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO)) {
            reset_rx();
            return -EIO;
        }
        k_usleep(100);
    } while (k_uptime_get() < deadline);
    reset_rx();
    return -ETIMEDOUT;
}

static uint64_t rx_time(void)
{
    uint8_t ts[5];
    dwt_readrxtimestamp(ts, DWT_COMPAT_NONE);
    uint64_t value = 0;
    for (int i = 4; i >= 0; --i) value = (value << 8) | ts[i];
    return value;
}

int uwb_ball_range(uint16_t seq, uint32_t hit, uint32_t *ack, int32_t *distance_mm, struct peer_impact *peer)
{
    uint8_t f[MAX_FRAME];
    *ack = 0;
    *peer = (struct peer_impact){0};
    reset_rx();
    header(f, POLL, BALL, WEARABLE, seq);
    sys_put_le32(hit, f + HEADER);
    int rc = transmit(f, 4, DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);
    if (rc) return rc;
    rc = receive(f, 16, RESPONSE, WEARABLE, BALL, seq);
    if (rc) return rc;
    *ack = sys_get_le32(f + HEADER);
    uint32_t age = sys_get_le32(f + HEADER + 8);
    if (age <= WEARABLE_REPORT_MS) {
        peer->id = sys_get_le32(f + HEADER + 4);
        peer->time = k_uptime_get() - age;
        peer->mg = (int32_t)sys_get_le32(f + HEADER + 12);
    }
    uint32_t poll_tx = dwt_readtxtimestamplo32();
    uint64_t resp_rx = rx_time();
    uint32_t delayed = (uint32_t)((resp_rx + REPLY_UUS * UUS_TO_DTU) >> 8);
    uint32_t final_tx = (uint32_t)(((uint64_t)(delayed & 0xfffffffeUL) << 8) + ANTENNA_DELAY);
    header(f, FINAL, BALL, WEARABLE, seq);
    sys_put_le32(poll_tx, f + HEADER);
    sys_put_le32((uint32_t)resp_rx, f + HEADER + 4);
    sys_put_le32(final_tx, f + HEADER + 8);
    dwt_setdelayedtrxtime(delayed);
    rc = transmit(f, 12, DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
    if (rc) { reset_rx(); return rc; }
    rc = receive(f, 4, RESULT, WEARABLE, BALL, seq);
    if (rc) return rc;
    int32_t mm = (int32_t)sys_get_le32(f + HEADER);
    if (mm < 0 || mm > 50000) return -ERANGE;
    mm -= RANGE_OFFSET_MM;
    if (mm < 0 || mm > 50000) return -ERANGE;
    *distance_mm = mm;
    return 0;
}

int uwb_wearable_step(uint32_t *hit, int32_t *distance_mm)
{
    uint8_t f[MAX_FRAME];
    *hit = 0;
    reset_rx();
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) return -EIO;
    int rc = receive(f, 4, POLL, BALL, WEARABLE, -1);
    if (rc) return rc;
    uint64_t poll_rx = rx_time();
    uint16_t seq = sys_get_le16(f + 10);
    *hit = sys_get_le32(f + HEADER);
    header(f, RESPONSE, WEARABLE, BALL, seq);
    sys_put_le32(*hit, f + HEADER);
    struct impact_report r = impact_latest();
    sys_put_le32(r.id, f + HEADER + 4);
    sys_put_le32(r.age_ms, f + HEADER + 8);
    sys_put_le32((uint32_t)r.mg, f + HEADER + 12);
    rc = transmit(f, 16, DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);
    if (rc) return rc;
    rc = receive(f, 12, FINAL, BALL, WEARABLE, seq);
    if (rc) return rc;
    uint32_t final_rx = (uint32_t)rx_time();
    uint32_t resp_tx = dwt_readtxtimestamplo32();
    int32_t mm;
    if (!twr_distance(sys_get_le32(f + HEADER), sys_get_le32(f + HEADER + 4),
                      sys_get_le32(f + HEADER + 8), (uint32_t)poll_rx,
                      resp_tx, final_rx, &mm)) return -ERANGE;
    header(f, RESULT, WEARABLE, BALL, seq);
    sys_put_le32((uint32_t)mm, f + HEADER);
    rc = transmit(f, 4, DWT_START_TX_IMMEDIATE);
    if (rc) return rc;
    rc = wait_tx();
    if (!rc) *distance_mm = mm;
    return rc;
}
