/* Host simulator shim for <zephyr/sys/byteorder.h>: little-endian accessors (the host is LE too,
 * but the code stays byte-wise so that it behaves the same on any host). */
#ifndef SIM_ZEPHYR_BYTEORDER_H
#define SIM_ZEPHYR_BYTEORDER_H

#include <stdint.h>

static inline uint16_t sys_get_le16(const uint8_t src[2])
{
  return (uint16_t)(src[0] | (src[1] << 8));
}

static inline uint32_t sys_get_le32(const uint8_t src[4])
{
  return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
         ((uint32_t)src[3] << 24);
}

static inline void sys_put_le16(uint16_t val, uint8_t dst[2])
{
  dst[0] = (uint8_t)val;
  dst[1] = (uint8_t)(val >> 8);
}

static inline void sys_put_le32(uint32_t val, uint8_t dst[4])
{
  dst[0] = (uint8_t)val;
  dst[1] = (uint8_t)(val >> 8);
  dst[2] = (uint8_t)(val >> 16);
  dst[3] = (uint8_t)(val >> 24);
}

#endif
