/*
 * Host simulator shim for the Qorvo decadriver API (deca_device_api.h): the subset used by
 * shared/uwb.c. The behaviour behind these functions is the DW3110 model in tests/sim/src/sim_dw.c.
 * Names, types and constant VALUES follow the real driver.
 */
#ifndef SIM_DECA_DEVICE_API_H
#define SIM_DECA_DEVICE_API_H

#include <stdbool.h>
#include <stdint.h>
#include "dw3000/dw3000_deca_regs.h"

#define DWT_SUCCESS 0
#define DWT_ERROR (-1)

typedef enum { DWT_BR_850K = 0, DWT_BR_6M8 = 1, DWT_BR_NODATA = 2 } dwt_uwb_bit_rate_e;
typedef enum { DWT_PAC8 = 0, DWT_PAC16 = 1, DWT_PAC32 = 2, DWT_PAC4 = 3 } dwt_pac_size_e;
typedef enum { DWT_SFD_IEEE_4A = 0, DWT_SFD_DW_8 = 1, DWT_SFD_DW_16 = 2, DWT_SFD_IEEE_4Z = 3 } dwt_sfd_type_e;
typedef enum { DWT_PHRMODE_STD = 0x0, DWT_PHRMODE_EXT = 0x1 } dwt_phr_mode_e;
typedef enum { DWT_PHRRATE_STD = 0x0, DWT_PHRRATE_DTA = 0x1 } dwt_phr_rate_e;
typedef enum { DWT_PDOA_M0 = 0x0, DWT_PDOA_M1 = 0x1, DWT_PDOA_M3 = 0x3 } dwt_pdoa_mode_e;
typedef enum { DWT_STS_MODE_OFF = 0x0, DWT_STS_MODE_1 = 0x1 } dwt_sts_mode_e;
typedef enum { DWT_STS_LEN_32 = 0, DWT_STS_LEN_64 = 1, DWT_STS_LEN_128 = 2 } dwt_sts_lengths_e;
typedef enum { DWT_IP_M = 0x0, DWT_COMPAT_NONE = 0xFF } dwt_ip_sts_segment_e;

#define DWT_PLEN_4096 (0x1FFU)
#define DWT_PLEN_2048 (0xFFU)
#define DWT_PLEN_1536 (0xBFU)
#define DWT_PLEN_1024 (0x7FU)
#define DWT_PLEN_512 (0x3FU)
#define DWT_PLEN_256 (0x1FU)
#define DWT_PLEN_128 (0x0FU)
#define DWT_PLEN_72 (0x08U)
#define DWT_PLEN_64 (0x07U)
#define DWT_PLEN_32 (0x03U)

typedef struct
{
  uint8_t chan;
  uint16_t txPreambLength;
  dwt_pac_size_e rxPAC;
  uint8_t txCode;
  uint8_t rxCode;
  dwt_sfd_type_e sfdType;
  dwt_uwb_bit_rate_e dataRate;
  dwt_phr_mode_e phrMode;
  dwt_phr_rate_e phrRate;
  uint16_t sfdTO;
  dwt_sts_mode_e stsMode;
  dwt_sts_lengths_e stsLength;
  dwt_pdoa_mode_e pdoaMode;
} dwt_config_t;

typedef struct
{
  uint8_t PGdly;
  uint32_t power;
  uint16_t PGcount;
} dwt_txconfig_t;

struct dwt_probe_s
{
  int unused;
};

/* dwt_starttx() modes */
#define DWT_START_TX_IMMEDIATE 0x00
#define DWT_START_TX_DELAYED 0x01
#define DWT_RESPONSE_EXPECTED 0x02
/* dwt_rxenable() modes */
#define DWT_START_RX_IMMEDIATE 0x00

/* Interrupt / status events (same bits as SYS_STATUS). */
#define DWT_INT_RXFR_BIT_MASK SYS_STATUS_RXFR_BIT_MASK
#define DWT_INT_RXFCG_BIT_MASK SYS_STATUS_RXFCG_BIT_MASK
#define DWT_INT_RXPRD_BIT_MASK SYS_STATUS_RXPRD_BIT_MASK
#define DWT_INT_RXSFDD_BIT_MASK SYS_STATUS_RXSFDD_BIT_MASK
#define DWT_INT_RXPHD_BIT_MASK SYS_STATUS_RXPHD_BIT_MASK
#define DWT_INT_CIADONE_BIT_MASK SYS_STATUS_CIADONE_BIT_MASK
#define DWT_INT_RXPHE_BIT_MASK SYS_STATUS_RXPHE_BIT_MASK
#define DWT_INT_RXFCE_BIT_MASK SYS_STATUS_RXFCE_BIT_MASK
#define DWT_INT_RXFSL_BIT_MASK SYS_STATUS_RXFSL_BIT_MASK
#define DWT_INT_RXSTO_BIT_MASK SYS_STATUS_RXSTO_BIT_MASK
#define DWT_INT_ARFE_BIT_MASK SYS_STATUS_ARFE_BIT_MASK
#define DWT_INT_CIAERR_BIT_MASK SYS_STATUS_CIAERR_BIT_MASK
#define DWT_INT_CPERR_BIT_MASK SYS_STATUS_CPERR_BIT_MASK
#define DWT_INT_RXFTO_BIT_MASK SYS_STATUS_RXFTO_BIT_MASK
#define DWT_INT_RXPTO_BIT_MASK SYS_STATUS_RXPTO_BIT_MASK

#define SYS_STATUS_ALL_RX_TO \
  ((uint32_t)DWT_INT_RXFTO_BIT_MASK | (uint32_t)DWT_INT_RXPTO_BIT_MASK | (uint32_t)DWT_INT_CPERR_BIT_MASK)
#define SYS_STATUS_ALL_RX_ERR                                                                   \
  ((uint32_t)DWT_INT_RXPHE_BIT_MASK | (uint32_t)DWT_INT_RXFCE_BIT_MASK |                        \
   (uint32_t)DWT_INT_RXFSL_BIT_MASK | (uint32_t)DWT_INT_RXSTO_BIT_MASK |                        \
   (uint32_t)DWT_INT_ARFE_BIT_MASK | (uint32_t)DWT_INT_CIAERR_BIT_MASK |                        \
   (uint32_t)DWT_INT_CPERR_BIT_MASK)
#define SYS_STATUS_ALL_RX_GOOD                                                                  \
  ((uint32_t)DWT_INT_RXFR_BIT_MASK | (uint32_t)DWT_INT_RXFCG_BIT_MASK |                         \
   (uint32_t)DWT_INT_RXPRD_BIT_MASK | (uint32_t)DWT_INT_RXSFDD_BIT_MASK |                       \
   (uint32_t)DWT_INT_RXPHD_BIT_MASK | (uint32_t)DWT_INT_CIADONE_BIT_MASK)

int32_t dwt_probe(struct dwt_probe_s *probe_interf);
uint32_t dwt_readdevid(void);
uint8_t dwt_checkidlerc(void);
int32_t dwt_initialise(int32_t mode);
int32_t dwt_configure(dwt_config_t *config);
void dwt_configuretxrf(dwt_txconfig_t *config);
void dwt_settxantennadelay(uint16_t antennaDly);
void dwt_setrxantennadelay(uint16_t antennaDly);
void dwt_setrxaftertxdelay(uint32_t rxDelayTime);
void dwt_setrxtimeout(uint32_t rx_time);

void dwt_forcetrxoff(void);
void dwt_writesysstatuslo(uint32_t mask);
uint32_t dwt_readsysstatuslo(void);
int32_t dwt_rxenable(int32_t mode);
int32_t dwt_writetxdata(uint16_t txDataLength, uint8_t *txDataBytes, uint16_t txBufferOffset);
void dwt_writetxfctrl(uint16_t txFrameLength, uint16_t txBufferOffset, uint8_t ranging);
int32_t dwt_starttx(uint8_t mode);
void dwt_setdelayedtrxtime(uint32_t starttime);
uint16_t dwt_getframelength(uint8_t *rng);
void dwt_readrxdata(uint8_t *buffer, uint16_t length, uint16_t rxBufferOffset);
void dwt_readrxtimestamp(uint8_t *timestamp, dwt_ip_sts_segment_e segment);
uint32_t dwt_readtxtimestamplo32(void);
uint32_t dwt_readsystimestamphi32(void);

#endif
