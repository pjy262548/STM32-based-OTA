/**
 * @file    ota_info.h
 * @brief   Power-fail-safe OTA state stored in Flash.
 */

#ifndef __OTA_INFO_H
#define __OTA_INFO_H

#include "stm32f10x.h"
#include "boot.h"

#define OTA_INFO_MAGIC           0xAA55AA55UL

typedef enum {
    OTA_STATE_IDLE        = 0x00,
    OTA_STATE_DOWNLOADING = 0x01,
    OTA_STATE_COMPLETED   = 0x02,
    OTA_STATE_VERIFIED    = 0x03,
    OTA_STATE_PENDING_CONFIRM = 0x04,
    OTA_STATE_INSTALLING  = 0x05,
    OTA_STATE_FAILED      = 0xFF
} ota_state_t;

typedef struct {
    uint8_t  active_partition;       /* Always 'A' in single-run-slot mode. */
    uint8_t  ota_state;              /* ota_state_t */
    uint8_t  reserved[2];
    uint32_t new_fw_size;
    uint32_t new_fw_crc32;
    uint8_t  new_version[16];
    uint32_t ota_start_time;
    uint32_t packet_count;
    uint32_t backup_fw_size;         /* Full backup slot size. */
    uint32_t backup_fw_crc32;        /* CRC32 of the full backup slot. */
    uint32_t boot_attempts;          /* Boots since PENDING_CONFIRM. */
    uint32_t magic;
} ota_info_t;

void     ota_info_init(void);
void     ota_info_read(ota_info_t *info);
void     ota_info_write(const ota_info_t *info);
void     ota_info_erase(void);

#endif /* __OTA_INFO_H */
