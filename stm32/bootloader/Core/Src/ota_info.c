/**
 * @file    ota_info.c
 * @brief   Power-fail-safe OTA info storage.
 */

#include "ota_info.h"
#include "flash_if.h"
#include <string.h>

#define OTA_INFO_SLOT_SIZE      64U
#define OTA_INFO_SLOT0_ADDR     OTA_INFO_ADDR
#define OTA_INFO_SLOT1_ADDR     (OTA_INFO_ADDR + FLASH_PAGE_SIZE)

typedef struct {
    ota_info_t info;
    uint32_t sequence;
    uint32_t crc32;
    uint8_t  reserved[OTA_INFO_SLOT_SIZE - sizeof(ota_info_t) - 8U];
} ota_info_record_t;

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 1UL) {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static uint32_t record_crc32(const ota_info_record_t *record)
{
    uint32_t crc = 0xFFFFFFFFUL;

    crc = crc32_update(crc, (const uint8_t *)&record->info,
                       (uint32_t)sizeof(record->info));
    crc = crc32_update(crc, (const uint8_t *)&record->sequence,
                       (uint32_t)sizeof(record->sequence));
    return crc ^ 0xFFFFFFFFUL;
}

static void read_record(uint32_t addr, ota_info_record_t *record)
{
    flash_read(addr, (uint8_t *)record, sizeof(*record));
}

static int partition_valid(uint8_t partition)
{
    return partition == 'A' || partition == 'B';
}

static int record_valid(const ota_info_record_t *record)
{
    if (record->info.magic != OTA_INFO_MAGIC) {
        return 0;
    }
    if (!partition_valid(record->info.active_partition)) {
        return 0;
    }
    return record->crc32 == record_crc32(record);
}

static int sequence_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static int legacy_info_read(ota_info_t *info)
{
    flash_read(OTA_INFO_ADDR, (uint8_t *)info, sizeof(*info));
    return info->magic == OTA_INFO_MAGIC &&
           partition_valid(info->active_partition);
}

static int latest_record_read(ota_info_record_t *latest, uint8_t *slot)
{
    ota_info_record_t rec0;
    ota_info_record_t rec1;
    int valid0;
    int valid1;

    read_record(OTA_INFO_SLOT0_ADDR, &rec0);
    read_record(OTA_INFO_SLOT1_ADDR, &rec1);
    valid0 = record_valid(&rec0);
    valid1 = record_valid(&rec1);

    if (valid0 && (!valid1 || sequence_newer(rec0.sequence, rec1.sequence))) {
        if (latest) {
            *latest = rec0;
        }
        if (slot) {
            *slot = 0;
        }
        return 1;
    }

    if (valid1) {
        if (latest) {
            *latest = rec1;
        }
        if (slot) {
            *slot = 1;
        }
        return 1;
    }

    return 0;
}

void ota_info_init(void)
{
    ota_info_t info;

    memset(&info, 0, sizeof(info));
    info.active_partition = 'A';
    info.ota_state = OTA_STATE_IDLE;
    info.magic = OTA_INFO_MAGIC;
    ota_info_write(&info);
}

void ota_info_read(ota_info_t *info)
{
    ota_info_record_t latest;
    ota_info_t legacy;

    if (!info) {
        return;
    }

    if (latest_record_read(&latest, NULL)) {
        *info = latest.info;
        return;
    }

    if (legacy_info_read(&legacy)) {
        *info = legacy;
        return;
    }

    memset(info, 0, sizeof(*info));
}

void ota_info_write(const ota_info_t *info)
{
    ota_info_record_t latest;
    ota_info_record_t record;
    ota_info_t legacy;
    uint8_t latest_slot = 0;
    uint8_t target_slot = 0;
    uint32_t target_addr;
    uint32_t next_sequence = 1;

    if (!info) {
        return;
    }

    if (latest_record_read(&latest, &latest_slot)) {
        next_sequence = latest.sequence + 1U;
        target_slot = (latest_slot == 0) ? 1 : 0;
    } else if (legacy_info_read(&legacy)) {
        target_slot = 1;
    }

    target_addr = (target_slot == 0) ? OTA_INFO_SLOT0_ADDR : OTA_INFO_SLOT1_ADDR;

    memset(&record, 0, sizeof(record));
    record.info = *info;
    record.info.magic = OTA_INFO_MAGIC;
    record.sequence = next_sequence;
    record.crc32 = record_crc32(&record);

    flash_erase_page(target_addr);
    flash_write(target_addr, (const uint8_t *)&record, sizeof(record));
}

void ota_info_erase(void)
{
    flash_erase_range(OTA_INFO_ADDR, OTA_INFO_SIZE);
}
