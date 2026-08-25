/**
 * @file    stm32_bridge.h
 * @brief   STM32 UART 通信协议封装
 *
 * UART 接线 (STM32 USART2 �?ESP32 Serial2):
 *   STM32 PA2(TX) �?ESP32 GPIO16(RX)
 *   STM32 PA3(RX) �?ESP32 GPIO17(TX)
 *   STM32 GND     �?ESP32 GND
 *
 * 负责:
 *   - 协议帧组装与发�? *   - ACK/NACK 等待与重�? *   - 超时处理
 */

#ifndef __STM32_BRIDGE_H
#define __STM32_BRIDGE_H

#include <Arduino.h>
#include "config.h"

/* ============================================================
 * 协议常量 (�?STM32 侧保持一�?
 * ============================================================ */
#define PROTOCOL_SOF            0xAA
#define PROTOCOL_MAX_PAYLOAD    512

/* 命令�?*/
#define CMD_QUERY_VERSION       0x01
#define CMD_REPORT_VERSION      0x02
#define CMD_OTA_START           0x10
#define CMD_OTA_START_ACK       0x11
#define CMD_DATA_PACKET         0x12
#define CMD_DATA_ACK            0x13
#define CMD_OTA_FINISH          0x20
#define CMD_OTA_RESULT          0x21
#define CMD_REBOOT              0x30
#define CMD_ERROR               0xFF

/* ACK 状�?*/
#define ACK_OK                  0x00
#define ACK_CRC_ERROR           0x01
#define ACK_FLASH_ERROR         0x02
#define ACK_SEQ_ERROR           0x03

/* ============================================================
 * OTA 进度回调
 * ============================================================ */
typedef void (*ota_progress_cb_t)(uint32_t bytes_sent, uint32_t total);

/* ============================================================
 * API
 * ============================================================ */
void    stm32_bridge_init(void);
bool    stm32_ping(void);
bool    stm32_rx_probe(uint32_t timeout_ms);

/* 版本查询 */
String  stm32_query_version(void);

/* OTA 流程 */
bool    stm32_ota_start(uint32_t fw_size, uint32_t fw_crc32,
                        const String &version);
bool    stm32_ota_send_packet(uint16_t seq, uint32_t offset,
                              const uint8_t *data, uint16_t len);
bool    stm32_ota_finish(uint32_t fw_size);
bool    stm32_ota_verify_and_reboot(void);
const char *stm32_last_error(void);

/* 设置进度回调 */
void    stm32_set_progress_callback(ota_progress_cb_t cb);

#endif /* __STM32_BRIDGE_H */
