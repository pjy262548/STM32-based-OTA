/**
 * CRC16-CCITT 和 CRC32 工具函数
 * 供 ESP32 和 Python 工具使用
 */

#pragma once

#include <cstdint>

/* CRC16-CCITT */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len);

/* CRC32 (ZIP/PNG 兼容) */
void     crc32_init();
uint32_t crc32_begin();
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);
uint32_t crc32_finish(uint32_t crc);
uint32_t crc32_calc(const uint8_t *data, uint32_t len);
