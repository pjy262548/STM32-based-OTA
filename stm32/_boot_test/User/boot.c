/**
 * @file    boot.c
 * @brief   Bootloader 主逻辑 - 上电后判断启动哪个分区
 *
 * 启动流程:
 *   1. 初始化基础硬件 (时钟, 看门狗)
 *   2. 读取 OTA Info 区
 *   3. 如果 ota_state == VERIFIED, 切换 active_partition
 *   4. 检查目标分区 SP 是否合法 (必须在 SRAM 范围内)
 *   5. 设置 MSP + 跳转到 Application
 */

#include "boot.h"
#include "flash_if.h"
#include "ota_info.h"

/* ============================================================
 * 内部函数
 * ============================================================ */
static void system_clock_init(void);
static uint32_t get_boot_partition_addr(void);
static int is_vector_table_valid(uint32_t addr);
__attribute__((noreturn)) static void jump_to_app(uint32_t app_addr);

/* ============================================================
 * Bootloader 主入口
 * ============================================================ */
void bootloader_main(void)
{
    ota_info_t info;

    /* ---- 1. 基础初始化 ---- */
    system_clock_init();

    /* ---- 2. 读取 OTA Info ---- */
    ota_info_read(&info);

    /* ---- 3. 处理 OTA 状态 ---- */
    if (info.magic == OTA_INFO_MAGIC) {
        if (info.ota_state == OTA_STATE_VERIFIED) {
            /* 新固件已验证通过 → 切换启动分区 */
            info.active_partition = (info.active_partition == 'A') ? 'B' : 'A';
            info.ota_state = OTA_STATE_IDLE;
            ota_info_write(&info);
        }
        /* 其他状态 (IDLE / DOWNLOADING / FAILED) 保持当前分区不变 */
    } else {
        /* 首次上电, 初始化 OTA Info 区, 默认从 App A 启动 */
        ota_info_init();
        info.active_partition = 'A';
    }

    /* ---- 4. 确定目标分区地址 ---- */
    uint32_t app_addr = get_boot_partition_addr();

    /* ---- 5. 检查向量表是否有效 ---- */
    if (!is_vector_table_valid(app_addr)) {
        /* 当前分区损坏, 尝试另一个分区 */
        uint32_t alt_addr = (app_addr == APP_A_ADDR) ? APP_B_ADDR : APP_A_ADDR;
        if (is_vector_table_valid(alt_addr)) {
            app_addr = alt_addr;
        } else {
            /* 两个分区都无效, 死循环等待看门狗复位 */
            while (1) {
                __NOP();
            }
        }
    }

    /* ---- 6. 跳转到 Application ---- */
    jump_to_app(app_addr);
}

/* ============================================================
 * 获取启动分区地址
 * ============================================================ */
static uint32_t get_boot_partition_addr(void)
{
    ota_info_t info;
    ota_info_read(&info);

    if (info.magic == OTA_INFO_MAGIC && info.active_partition == 'B') {
        return APP_B_ADDR;
    }
    /* 默认从 App A 启动 */
    return APP_A_ADDR;
}

/* ============================================================
 * 检查向量表是否有效
 * 条件: 初始 SP 值必须在 SRAM 范围内
 * ============================================================ */
static int is_vector_table_valid(uint32_t addr)
{
    uint32_t sp = *((volatile uint32_t *)addr);
    uint32_t pc = *((volatile uint32_t *)(addr + 4));

    /* SP 必须在 SRAM 范围内 */
    if (sp < SRAM_BASE || sp > SRAM_END) {
        return 0;
    }

    /* PC (Reset_Handler) 必须在 Flash 范围内 */
    if (pc < FLASH_BASE || pc > (FLASH_BASE + 0x80000UL)) {
        return 0;
    }

    return 1;
}

/* ============================================================
 * 跳转到 Application
 * 步骤:
 *   1. 关闭全局中断
 *   2. 设置主堆栈指针 MSP = 目标分区向量表第一个字
 *   3. 跳转到 Reset_Handler (向量表第二个字)
 *
 *   注意: App 启动后需要自行设置 SCB->VTOR
 * ============================================================ */
__attribute__((noreturn))
static void jump_to_app(uint32_t app_addr)
{
    uint32_t sp = *((volatile uint32_t *)app_addr);
    uint32_t reset_handler = *((volatile uint32_t *)(app_addr + 4));

    /* 关闭所有外设中断 */
    __disable_irq();

    /* 设置向量表偏移 (Bootloader 自己先设好) */
    SCB->VTOR = app_addr;

    /* 设置 MSP */
    __set_MSP(sp);

    /* 跳转到 Application 的 Reset_Handler */
    typedef void (*app_entry_t)(void);
    app_entry_t app_entry = (app_entry_t)reset_handler;
    app_entry();

    /* 永远不会到达这里 */
    while (1) {
        __NOP();
    }
}

/* ============================================================
 * 系统时钟初始化 (HSE 8MHz → PLL 72MHz)
 * ============================================================ */
static void system_clock_init(void)
{
    /* 使能 HSE */
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY));

    /* 配置 Flash 等待周期 (72MHz 需要 2 个等待周期) */
    FLASH->ACR |= FLASH_ACR_LATENCY_2;

    /* PLL: HSE × 9 = 72MHz */
    /* PLLSRC=HSE, PLLMUL=9 */
    RCC->CFGR |= RCC_CFGR_PLLSRC;                  /* HSE as PLL source */
    RCC->CFGR |= RCC_CFGR_PLLMULL9;                 /* ×9 */

    /* APB1 预分频器 /2 (max 36MHz) */
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;

    /* 使能 PLL */
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));

    /* 切换到 PLL */
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);
}
