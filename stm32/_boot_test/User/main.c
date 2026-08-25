/**
 * @file    main.c
 * @brief   Bootloader 入口点
 *
 * 这是 STM32F103ZET6 上电后第一个执行的代码。
 * Bootloader 地址: 0x08000000, 大小 16KB
 *
 * Keil µVision 配置:
 *   - Options → Target → IROM1: Start=0x08000000, Size=0x4000
 *   - Options → Linker → Scatter File 指向 STM32F103ZETX_BOOT.sct
 *
 * 或者直接在 Linker 选项中设置:
 *   - R/O Base: 0x08000000
 */

#include "boot.h"

int main(void)
{
    /* 进入 Bootloader 主逻辑, 此函数不会返回 */
    bootloader_main();

    /* 永远不会执行到这里 */
    while (1) {
        __NOP();
    }
}
