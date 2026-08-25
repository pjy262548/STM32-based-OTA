#ifndef __CRC32_H
#define __CRC32_H

#include "stm32f10x.h"

/* ============================================================
 * CRC32 API (Ethernet polynomial: 0x04C11DB7)
 * ============================================================ */
void     crc32_init(void);
uint32_t crc32_calc_continue(uint32_t crc, const uint8_t *data, uint32_t len);

#endif /* __CRC32_H */
