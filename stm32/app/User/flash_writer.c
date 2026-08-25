#include "flash_writer.h"
#include "crc32.h"
#include <string.h>

#define OTA_INFO_SLOT_SIZE      OTA_INFO_STORE_SIZE
#define OTA_INFO_SLOT0_ADDR     OTA_INFO_ADDR
#define OTA_INFO_SLOT1_ADDR     (OTA_INFO_ADDR + FLASH_PAGE_SIZE)

typedef struct {
    ota_info_t info;
    uint32_t sequence;
    uint32_t crc32;
    uint8_t  reserved[OTA_INFO_SLOT_SIZE - sizeof(ota_info_t) - 8U];
} ota_info_record_t;

static void flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123;
        FLASH->KEYR = 0xCDEF89AB;
    }
}

static void flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

__attribute__((section(".ramfunc")))
static int flash_wait_ready(void)
{
    uint32_t timeout = 0x000FFFFFUL;
    while ((FLASH->SR & FLASH_SR_BSY) && --timeout) {
    }
    if (timeout == 0) {
        return -1;
    }
    if (FLASH->SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) {
        return -1;
    }
    return 0;
}

__attribute__((section(".ramfunc")))
static void flash_clear_status(void)
{
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
}

__attribute__((section(".ramfunc")))
static int flash_erase_page(uint32_t addr)
{
    int ret = 0;
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    flash_unlock();
    flash_clear_status();

    if (flash_wait_ready() != 0) {
        ret = -1;
    } else {
        FLASH->CR |= FLASH_CR_PER;
        FLASH->AR = addr;
        FLASH->CR |= FLASH_CR_STRT;
        if (flash_wait_ready() != 0) {
            ret = -1;
        }
        FLASH->CR &= ~FLASH_CR_PER;
        flash_clear_status();
    }

    flash_lock();
    if (!primask) {
        __enable_irq();
    }
    return ret;
}

__attribute__((section(".ramfunc")))
static int flash_program_halfword(uint32_t addr, uint16_t data)
{
    int ret = 0;
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    flash_unlock();
    flash_clear_status();

    if (flash_wait_ready() != 0) {
        ret = -1;
    } else {
        FLASH->CR |= FLASH_CR_PG;
        *(volatile uint16_t *)addr = data;
        if (flash_wait_ready() != 0 || *(volatile uint16_t *)addr != data) {
            ret = -1;
        }
        FLASH->CR &= ~FLASH_CR_PG;
        flash_clear_status();
    }

    flash_lock();
    if (!primask) {
        __enable_irq();
    }
    return ret;
}

static int download_range_valid(uint32_t addr, uint32_t len)
{
    uint32_t end;

    if (len == 0) {
        return 0;
    }
    end = addr + len;
    if (end < addr) {
        return 0;
    }
    return addr >= APP_DOWNLOAD_ADDR && end <= (APP_DOWNLOAD_ADDR + APP_PARTITION_SIZE);
}

void flash_writer_init(void)
{
}

int flash_erase_app_area(uint32_t addr, uint32_t size)
{
    if (addr != APP_DOWNLOAD_ADDR || !download_range_valid(addr, size)) {
        return -1;
    }
    if (size > APP_PARTITION_SIZE || (addr + size) > (addr + APP_PARTITION_SIZE)) {
        return -1;
    }

    uint32_t pages = (size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        if (flash_erase_page(addr + i * FLASH_PAGE_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

int flash_write_data(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!data || !download_range_valid(addr, len)) {
        return -1;
    }

    for (uint32_t i = 0; i < len; i += 2) {
        uint16_t halfword = data[i];
        if ((i + 1) < len) {
            halfword |= (uint16_t)data[i + 1] << 8;
        } else {
            halfword |= 0xFF00;
        }
        if (flash_program_halfword(addr + i, halfword) != 0) {
            return -1;
        }
    }
    return 0;
}

void flash_read_data(uint32_t addr, uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = *(volatile uint8_t *)(addr + i);
    }
}

uint32_t flash_calc_crc32(uint32_t addr, uint32_t size)
{
    uint8_t buf[256];
    uint32_t offset = 0;
    uint32_t crc = 0xFFFFFFFF;

    crc32_init();
    while (offset < size) {
        uint32_t chunk = (size - offset > sizeof(buf)) ? sizeof(buf) : (size - offset);
        flash_read_data(addr + offset, buf, chunk);
        crc = crc32_calc_continue(crc, buf, chunk);
        offset += chunk;
    }

    return crc ^ 0xFFFFFFFF;
}

static uint32_t ota_info_record_crc32(const ota_info_record_t *record)
{
    uint32_t crc = 0xFFFFFFFFUL;

    crc32_init();
    crc = crc32_calc_continue(crc, (const uint8_t *)&record->info,
                              (uint32_t)sizeof(record->info));
    crc = crc32_calc_continue(crc, (const uint8_t *)&record->sequence,
                              (uint32_t)sizeof(record->sequence));
    return crc ^ 0xFFFFFFFFUL;
}

static void ota_info_read_record(uint32_t addr, ota_info_record_t *record)
{
    flash_read_data(addr, (uint8_t *)record, sizeof(*record));
}

static int ota_info_partition_valid(uint8_t partition)
{
    return partition == 'A' || partition == 'B';
}

static int ota_info_record_valid(const ota_info_record_t *record)
{
    if (record->info.magic != OTA_INFO_MAGIC) {
        return 0;
    }
    if (!ota_info_partition_valid(record->info.active_partition)) {
        return 0;
    }
    return record->crc32 == ota_info_record_crc32(record);
}

static int ota_info_sequence_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static int ota_info_read_legacy(ota_info_t *info)
{
    flash_read_data(OTA_INFO_ADDR, (uint8_t *)info, sizeof(*info));
    return info->magic == OTA_INFO_MAGIC &&
           ota_info_partition_valid(info->active_partition);
}

static int ota_info_read_latest_record(ota_info_record_t *latest, uint8_t *slot)
{
    ota_info_record_t rec0;
    ota_info_record_t rec1;
    int valid0;
    int valid1;

    ota_info_read_record(OTA_INFO_SLOT0_ADDR, &rec0);
    ota_info_read_record(OTA_INFO_SLOT1_ADDR, &rec1);
    valid0 = ota_info_record_valid(&rec0);
    valid1 = ota_info_record_valid(&rec1);

    if (valid0 && (!valid1 || ota_info_sequence_newer(rec0.sequence, rec1.sequence))) {
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

void ota_info_read(void *info)
{
    ota_info_record_t latest;
    ota_info_t legacy;

    if (!info) {
        return;
    }

    if (ota_info_read_latest_record(&latest, NULL)) {
        memcpy(info, &latest.info, sizeof(latest.info));
        return;
    }

    if (ota_info_read_legacy(&legacy)) {
        memcpy(info, &legacy, sizeof(legacy));
        return;
    }

    memset(info, 0, sizeof(ota_info_t));
}

int ota_info_write(const void *info)
{
    ota_info_record_t latest;
    ota_info_record_t record;
    ota_info_t legacy;
    uint8_t latest_slot = 0;
    uint8_t target_slot = 0;
    uint32_t target_addr;
    uint32_t next_sequence = 1;
    ota_info_record_t verify;

    if (!info) {
        return -1;
    }

    if (ota_info_read_latest_record(&latest, &latest_slot)) {
        next_sequence = latest.sequence + 1U;
        target_slot = (latest_slot == 0) ? 1 : 0;
    } else if (ota_info_read_legacy(&legacy)) {
        target_slot = 1;
    }

    target_addr = (target_slot == 0) ? OTA_INFO_SLOT0_ADDR : OTA_INFO_SLOT1_ADDR;

    memset(&record, 0, sizeof(record));
    record.info = *(const ota_info_t *)info;
    record.info.magic = OTA_INFO_MAGIC;
    record.sequence = next_sequence;
    record.crc32 = ota_info_record_crc32(&record);

    if (flash_erase_page(target_addr) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < sizeof(record); i += 2) {
        const uint8_t *bytes = (const uint8_t *)&record;
        uint16_t halfword = bytes[i] | ((uint16_t)bytes[i + 1] << 8);
        if (flash_program_halfword(target_addr + i, halfword) != 0) {
            return -1;
        }
    }

    ota_info_read_record(target_addr, &verify);
    return ota_info_record_valid(&verify) ? 0 : -1;
}
