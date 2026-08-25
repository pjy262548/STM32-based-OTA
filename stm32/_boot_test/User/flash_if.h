/**
 * @file    flash_if.h
 * @brief   STM32F103 内部 Flash 读写驱动
 */

#ifndef __FLASH_IF_H
#define __FLASH_IF_H

#include "stm32f10x.h"

/* STM32F103 高密度产品 (ZET6/VET6, 512KB Flash), Flash 页大小 2KB */
#define FLASH_PAGE_SIZE          0x800UL         /* 2 KB */

/* ============================================================
 * API
 * ============================================================ */
void     flash_unlock(void);
void     flash_lock(void);
int      flash_erase_page(uint32_t page_addr);
int      flash_erase_range(uint32_t addr, uint32_t size);
int      flash_write_halfword(uint32_t addr, uint16_t data);
int      flash_write(uint32_t addr, const uint8_t *data, uint32_t len);
void     flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

#endif /* __FLASH_IF_H */
