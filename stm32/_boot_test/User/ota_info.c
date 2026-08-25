/**
 * @file    ota_info.c
 * @brief   OTA Info 区读写实现
 *
 * OTA Info 区位于 Flash 最后 16KB, 独立于 Bootloader / App 分区。
 * 使用最后一页 (2KB) 存储 ota_info_t 结构。
 */

#include "ota_info.h"
#include "flash_if.h"
#include <string.h>

/* OTA Info 结构存放在 OTA Info 区的开头 */
#define OTA_INFO_STRUCT_ADDR    OTA_INFO_ADDR

/* ============================================================
 * 初始化 OTA Info 区 (首次使用)
 * ============================================================ */
void ota_info_init(void)
{
    ota_info_t info;

    /* 擦除 OTA Info 区第一页 */
    flash_erase_page(OTA_INFO_ADDR);

    /* 填充默认值 */
    memset(&info, 0, sizeof(info));
    info.active_partition = 'A';
    info.ota_state        = OTA_STATE_IDLE;
    info.magic            = OTA_INFO_MAGIC;

    ota_info_write(&info);
}

/* ============================================================
 * 读取 OTA Info
 * ============================================================ */
void ota_info_read(ota_info_t *info)
{
    flash_read(OTA_INFO_STRUCT_ADDR, (uint8_t *)info, sizeof(ota_info_t));
}

/* ============================================================
 * 写入 OTA Info (先擦除再写入)
 * 注意: 频繁写入会消耗 Flash 寿命, 应仅在必要时调用
 * ============================================================ */
void ota_info_write(const ota_info_t *info)
{
    /* 必须确保 magic 正确 */
    ota_info_t write_info = *info;
    write_info.magic = OTA_INFO_MAGIC;

    /* 擦除所在页 */
    flash_erase_page(OTA_INFO_STRUCT_ADDR);

    /* 写入 */
    flash_write(OTA_INFO_STRUCT_ADDR, (const uint8_t *)&write_info,
                sizeof(ota_info_t));
}

/* ============================================================
 * 擦除整个 OTA Info 区
 * ============================================================ */
void ota_info_erase(void)
{
    flash_erase_range(OTA_INFO_ADDR, OTA_INFO_SIZE);
}
