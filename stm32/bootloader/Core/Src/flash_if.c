/**
 * @file    flash_if.c
 * @brief   STM32F103 内部 Flash 操作实现
 *
 * 注意事项:
 *   - STM32F1 写 Flash 必须按 16-bit 半字操作
 *   - 写入前必须先擦除 (擦除后该页全部为 0xFFFF)
 *   - 擦除/写入期间 Flash 不可读, 代码必须运行在 SRAM 或使用
 *     __ramfunc 属性
 */

#include "flash_if.h"

/* ============================================================
 * 解锁 Flash 控制器
 * ============================================================ */
void flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123;
        FLASH->KEYR = 0xCDEF89AB;
    }
}

/* ============================================================
 * 锁定 Flash 控制器
 * ============================================================ */
void flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/* ============================================================
 * 擦除单个 Flash 页 (2KB)
 * @param page_addr: 页内任意地址
 * @return 0=成功, -1=失败
 * ============================================================ */
__attribute__((section(".ramfunc")))
int flash_erase_page(uint32_t page_addr)
{
    uint32_t result;

    flash_unlock();

    /* 等待 Flash 空闲 */
    while (FLASH->SR & FLASH_SR_BSY);

    /* 设置擦除操作 */
    FLASH->CR |= FLASH_CR_PER;                     /* Page Erase */
    FLASH->AR  = page_addr;                        /* 目标地址 */
    FLASH->CR |= FLASH_CR_STRT;                    /* 开始擦除 */

    /* 等待完成 */
    while (FLASH->SR & FLASH_SR_BSY);

    /* 检查结果 */
    result = (FLASH->SR & FLASH_SR_EOP) ? 0 : -1;

    /* 清除标志 */
    FLASH->SR = FLASH_SR_EOP;
    FLASH->CR &= ~FLASH_CR_PER;

    flash_lock();
    return result;
}

/* ============================================================
 * 按范围擦除 Flash
 * @param addr: 起始地址 (必须页对齐)
 * @param size: 擦除大小 (自动向上取整到页大小)
 * @return 0=成功, -1=失败
 * ============================================================ */
__attribute__((section(".ramfunc")))
int flash_erase_range(uint32_t addr, uint32_t size)
{
    uint32_t num_pages;
    uint32_t i;

    /* 检查地址对齐 */
    if (addr & (FLASH_PAGE_SIZE - 1)) {
        return -1;
    }

    /* 计算需要擦除的页数 */
    num_pages = (size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

    for (i = 0; i < num_pages; i++) {
        if (flash_erase_page(addr + i * FLASH_PAGE_SIZE) != 0) {
            return -1;
        }
    }

    return 0;
}

/* ============================================================
 * 写单个半字 (16-bit)
 * STM32F103 Flash 编程必须是 16-bit 操作
 * @param addr: 目标地址
 * @param data: 16-bit 数据
 * @return 0=成功, -1=失败
 *
 * 警告: 此函数必须放置在 SRAM 中执行, 或确保调用时 Flash
 *       不被占用。使用 __ramfunc 属性。
 * ============================================================ */
__attribute__((section(".ramfunc")))
int flash_write_halfword(uint32_t addr, uint16_t data)
{
    flash_unlock();

    /* 等待 Flash 空闲 */
    while (FLASH->SR & FLASH_SR_BSY);

    /* 设置编程操作 */
    FLASH->CR |= FLASH_CR_PG;                      /* Programming mode */
    *(volatile uint16_t *)addr = data;

    /* 等待完成 */
    while (FLASH->SR & FLASH_SR_BSY);

    /* 检查结果 */
    int result = 0;
    if (FLASH->SR & FLASH_SR_PGERR) {
        FLASH->SR |= FLASH_SR_PGERR;
        result = -1;
    }

    FLASH->CR &= ~FLASH_CR_PG;
    flash_lock();
    return result;
}

/* ============================================================
 * 批量写入数据到 Flash
 * @param addr: 目标地址
 * @param data: 数据缓冲区
 * @param len:  写入长度 (字节)
 * @return 0=成功, -1=失败
 *
 * 注意: 如果 len 为奇数, 最后一个字节会与 0xFF 组成半字写入
 * ============================================================ */
__attribute__((section(".ramfunc")))
int flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    uint16_t half_word;

    for (i = 0; i < len; i += 2) {
        if (i + 1 < len) {
            half_word = data[i] | ((uint16_t)data[i + 1] << 8);
        } else {
            /* 最后一个奇数字节, 与 0xFF 组合 */
            half_word = data[i] | 0xFF00;
        }

        if (flash_write_halfword(addr + i, half_word) != 0) {
            return -1;
        }
    }

    return 0;
}

/* ============================================================
 * 从 Flash 读取数据
 * ============================================================ */
void flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        buf[i] = *(volatile uint8_t *)(addr + i);
    }
}
