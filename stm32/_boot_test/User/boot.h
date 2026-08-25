/**
 * @file    boot.h
 * @brief   STM32F103 (ZET6/VET6, 512KB Flash) Bootloader - 启动跳转逻辑
 * @note    正点原子精英版 / 通用 STM32F103 高密度系列
 *          Flash 分区:
 *          - Bootloader: 0x08000000, 16KB
 *          - App A:      0x08004000, 240KB
 *          - App B:      0x08040000, 240KB
 *          - OTA Info:   0x0807C000, 16KB
 */

#ifndef __BOOT_H
#define __BOOT_H

#include "stm32f10x.h"

/* ============================================================
 * Flash 分区地址定义
 * 注: FLASH_BASE, SRAM_BASE 已在 stm32f10x.h 中定义
 * ============================================================ */
#define BOOTLOADER_SIZE          0x4000UL        /*  16 KB */
#define APP_PARTITION_SIZE       0x3C000UL       /* 240 KB */
#define OTA_INFO_SIZE            0x4000UL        /*  16 KB */

#define APP_A_ADDR               (FLASH_BASE + BOOTLOADER_SIZE)           /* 0x08004000 */
#define APP_B_ADDR               (APP_A_ADDR + APP_PARTITION_SIZE)        /* 0x08040000 */
#define OTA_INFO_ADDR            (APP_B_ADDR + APP_PARTITION_SIZE)        /* 0x0807C000 */

#define SRAM_END                 (SRAM_BASE + 0x10000UL)

/* ============================================================
 * API
 * ============================================================ */
void bootloader_main(void);

#endif /* __BOOT_H */
