/**
 * @file    ota_info.h
 * @brief   OTA 状态信息管理 (存储在 Flash 的 OTA Info 区)
 *
 * OTA Info 区 (16KB, 位于 0x0807C000) 结构:
 *   ┌─────────────────────────────────┐
 *   │  ota_info_t   (前 64B)          │
 *   │  保留                          │
 *   │  Magic Number (末尾 4B)         │
 *   └─────────────────────────────────┘
 */

#ifndef __OTA_INFO_H
#define __OTA_INFO_H

#include "stm32f10x.h"
#include "boot.h"

/* ============================================================
 * 常量
 * ============================================================ */
#define OTA_INFO_MAGIC           0xAA55AA55UL      /* 数据有效标志 */

/* ============================================================
 * OTA 状态枚举
 * ============================================================ */
typedef enum {
    OTA_STATE_IDLE        = 0x00,   /* 空闲, 无 OTA 进行 */
    OTA_STATE_DOWNLOADING = 0x01,   /* 正在下载新固件 */
    OTA_STATE_COMPLETED   = 0x02,   /* 下载完成, 待 Bootloader 验证 */
    OTA_STATE_VERIFIED    = 0x03,   /* CRC 校验通过, 下次启动切换 */
    OTA_STATE_FAILED      = 0xFF    /* OTA 失败 */
} ota_state_t;

/* ============================================================
 * OTA Info 数据结构 (64 字节, 单页写入)
 * ============================================================ */
typedef struct {
    uint8_t  active_partition;       /* 'A' 或 'B', 当前运行的分区 */
    uint8_t  ota_state;              /* ota_state_t */
    uint8_t  reserved[2];            /* 对齐保留 */
    uint32_t new_fw_size;            /* 新固件大小 (字节) */
    uint32_t new_fw_crc32;           /* 新固件 CRC32 校验值 */
    uint8_t  new_version[16];        /* 新固件版本号字符串 */
    uint32_t ota_start_time;         /* OTA 开始时间戳 (可选) */
    uint32_t packet_count;           /* 已接收数据包数 (可选) */
    uint32_t magic;                  /* 魔数 0xAA55AA55 */
} ota_info_t;

/* ============================================================
 * API
 * ============================================================ */
void     ota_info_init(void);
void     ota_info_read(ota_info_t *info);
void     ota_info_write(const ota_info_t *info);
void     ota_info_erase(void);

#endif /* __OTA_INFO_H */
