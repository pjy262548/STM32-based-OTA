#ifndef __FLASH_WRITER_H
#define __FLASH_WRITER_H

#include "stm32f10x.h"
#include "ota_protocol.h"

#define FLASH_PAGE_SIZE         0x800UL

void     flash_writer_init(void);
int      flash_erase_app_area(uint32_t addr, uint32_t size);
int      flash_write_data(uint32_t addr, const uint8_t *data, uint32_t len);
void     flash_read_data(uint32_t addr, uint8_t *buf, uint32_t len);
uint32_t flash_calc_crc32(uint32_t addr, uint32_t size);
void     ota_info_read(void *info);
int      ota_info_write(const void *info);

#endif
