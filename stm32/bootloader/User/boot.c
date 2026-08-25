/**
 * @file    boot.c
 * @brief   Single runnable App slot with download and rollback slots.
 */

#include "boot.h"
#include "flash_if.h"
#include "ota_info.h"

#define LED0_PIN                        5U
#define LED1_PIN                        5U
#define COPY_CHUNK_SIZE                 256U
#define OTA_MAX_PENDING_BOOT_ATTEMPTS   2U
#define INSTALL_OK                      0
#define INSTALL_INVALID_IMAGE          -1
#define INSTALL_RETRY_LATER            -2

static void system_clock_init(void);
static void led_init(void);
static void led0_blink(int times, uint32_t delay_ms);
static void led1_blink(int times, uint32_t delay_ms);
static void delay_ms(uint32_t ms);
static int image_size_valid(uint32_t size);
static int is_image_vector_valid(uint32_t image_addr, uint32_t exec_addr);
static int is_vector_table_valid(uint32_t addr);
static uint32_t calc_crc32(uint32_t addr, uint32_t size);
static int is_image_crc_valid(uint32_t image_addr, const ota_info_t *info);
static int copy_flash_area(uint32_t src_addr, uint32_t dst_addr, uint32_t size);
static int backup_current_app(ota_info_t *info);
static int backup_valid(const ota_info_t *info);
static int restore_backup(ota_info_t *info);
static int install_downloaded_app(ota_info_t *info);
static void mark_failed(ota_info_t *info);
static void handle_ota_state(ota_info_t *info);
__attribute__((noreturn)) static void jump_to_app(uint32_t app_addr);

static int g_boot_block_jump = 0;

void bootloader_main(void)
{
    ota_info_t info;

    system_clock_init();
    led_init();

    GPIOB->BRR = (1U << LED0_PIN);
    delay_ms(300);
    GPIOB->BSRR = (1U << LED0_PIN);

    ota_info_read(&info);
    if (info.magic != OTA_INFO_MAGIC) {
        ota_info_init();
        ota_info_read(&info);
        led0_blink(1, 200);
    } else {
        handle_ota_state(&info);
    }

    if (g_boot_block_jump || !is_vector_table_valid(APP_RUN_ADDR)) {
        if (restore_backup(&info) != 0 || !is_vector_table_valid(APP_RUN_ADDR)) {
            GPIOE->BRR = (1U << LED1_PIN);
            while (1) {
                led0_blink(1, 200);
            }
        }
    }

    led0_blink(2, 150);
    delay_ms(500);
    jump_to_app(APP_RUN_ADDR);
}

static void handle_ota_state(ota_info_t *info)
{
    int ret;

    if (!info || info->magic != OTA_INFO_MAGIC) {
        return;
    }

    switch (info->ota_state) {
    case OTA_STATE_VERIFIED:
        if (backup_current_app(info) != 0) {
            led1_blink(6, 100);
            return;
        }
        info->ota_state = OTA_STATE_INSTALLING;
        info->boot_attempts = 0;
        ota_info_write(info);
        ret = install_downloaded_app(info);
        if (ret == INSTALL_OK) {
            led1_blink(2, 200);
        } else if (ret == INSTALL_INVALID_IMAGE) {
            mark_failed(info);
            led1_blink(5, 100);
        } else {
            if (restore_backup(info) != 0) {
                mark_failed(info);
                g_boot_block_jump = 1;
            }
            led1_blink(6, 100);
        }
        break;

    case OTA_STATE_INSTALLING:
        ret = install_downloaded_app(info);
        if (ret == INSTALL_OK) {
            led1_blink(2, 200);
        } else if (ret != INSTALL_OK) {
            if (restore_backup(info) != 0) {
                mark_failed(info);
                g_boot_block_jump = 1;
            }
            led1_blink(5, 100);
        }
        break;

    case OTA_STATE_PENDING_CONFIRM:
        if (!is_vector_table_valid(APP_RUN_ADDR) ||
            !is_image_crc_valid(APP_RUN_ADDR, info) ||
            info->boot_attempts >= OTA_MAX_PENDING_BOOT_ATTEMPTS) {
            if (restore_backup(info) == 0) {
                led1_blink(7, 100);
            } else {
                mark_failed(info);
                g_boot_block_jump = 1;
                led1_blink(5, 100);
            }
            break;
        }
        info->boot_attempts++;
        ota_info_write(info);
        led1_blink(3, 100);
        break;

    case OTA_STATE_DOWNLOADING:
    case OTA_STATE_COMPLETED:
        mark_failed(info);
        led1_blink(4, 100);
        break;

    default:
        break;
    }
}

static int install_downloaded_app(ota_info_t *info)
{
    if (!info || !image_size_valid(info->new_fw_size)) {
        return INSTALL_INVALID_IMAGE;
    }
    if (!is_image_vector_valid(APP_DOWNLOAD_ADDR, APP_RUN_ADDR)) {
        return INSTALL_INVALID_IMAGE;
    }
    if (!is_image_crc_valid(APP_DOWNLOAD_ADDR, info)) {
        return INSTALL_INVALID_IMAGE;
    }

    if (copy_flash_area(APP_DOWNLOAD_ADDR, APP_RUN_ADDR, info->new_fw_size) != 0) {
        return INSTALL_RETRY_LATER;
    }
    if (!is_vector_table_valid(APP_RUN_ADDR)) {
        return INSTALL_RETRY_LATER;
    }
    if (!is_image_crc_valid(APP_RUN_ADDR, info)) {
        return INSTALL_RETRY_LATER;
    }

    info->active_partition = 'A';
    info->ota_state = OTA_STATE_PENDING_CONFIRM;
    info->boot_attempts = 0;
    ota_info_write(info);
    return INSTALL_OK;
}

static int backup_current_app(ota_info_t *info)
{
    uint32_t crc_run;
    uint32_t crc_backup;

    if (!info || !is_vector_table_valid(APP_RUN_ADDR)) {
        return -1;
    }

    crc_run = calc_crc32(APP_RUN_ADDR, APP_PARTITION_SIZE);
    if (copy_flash_area(APP_RUN_ADDR, APP_BACKUP_ADDR, APP_PARTITION_SIZE) != 0) {
        return -1;
    }
    if (!is_image_vector_valid(APP_BACKUP_ADDR, APP_RUN_ADDR)) {
        return -1;
    }

    crc_backup = calc_crc32(APP_BACKUP_ADDR, APP_PARTITION_SIZE);
    if (crc_backup != crc_run) {
        return -1;
    }

    info->active_partition = 'A';
    info->backup_fw_size = APP_PARTITION_SIZE;
    info->backup_fw_crc32 = crc_backup;
    return 0;
}

static int backup_valid(const ota_info_t *info)
{
    if (!info || info->backup_fw_size != APP_PARTITION_SIZE) {
        return 0;
    }
    if (!is_image_vector_valid(APP_BACKUP_ADDR, APP_RUN_ADDR)) {
        return 0;
    }
    return calc_crc32(APP_BACKUP_ADDR, APP_PARTITION_SIZE) == info->backup_fw_crc32;
}

static int restore_backup(ota_info_t *info)
{
    if (!backup_valid(info)) {
        return -1;
    }
    if (copy_flash_area(APP_BACKUP_ADDR, APP_RUN_ADDR, APP_PARTITION_SIZE) != 0) {
        return -1;
    }
    if (!is_vector_table_valid(APP_RUN_ADDR)) {
        return -1;
    }
    if (calc_crc32(APP_RUN_ADDR, APP_PARTITION_SIZE) != info->backup_fw_crc32) {
        return -1;
    }

    info->active_partition = 'A';
    info->ota_state = OTA_STATE_FAILED;
    info->boot_attempts = 0;
    ota_info_write(info);
    return 0;
}

static void mark_failed(ota_info_t *info)
{
    if (!info) {
        return;
    }
    info->active_partition = 'A';
    info->ota_state = OTA_STATE_FAILED;
    info->boot_attempts = 0;
    ota_info_write(info);
}

static void led_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPEEN;

    GPIOB->CRL &= ~(0xFU << (LED0_PIN * 4U));
    GPIOB->CRL |=  (0x3U << (LED0_PIN * 4U));
    GPIOB->BSRR = (1U << LED0_PIN);

    GPIOE->CRL &= ~(0xFU << (LED1_PIN * 4U));
    GPIOE->CRL |=  (0x3U << (LED1_PIN * 4U));
    GPIOE->BSRR = (1U << LED1_PIN);
}

static void led0_blink(int times, uint32_t delay_ms_value)
{
    for (int i = 0; i < times; i++) {
        GPIOB->BRR = (1U << LED0_PIN);
        delay_ms(delay_ms_value);
        GPIOB->BSRR = (1U << LED0_PIN);
        delay_ms(delay_ms_value);
    }
}

static void led1_blink(int times, uint32_t delay_ms_value)
{
    for (int i = 0; i < times; i++) {
        GPIOE->BRR = (1U << LED1_PIN);
        delay_ms(delay_ms_value);
        GPIOE->BSRR = (1U << LED1_PIN);
        delay_ms(delay_ms_value);
    }
}

static void delay_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 12000U; j++) {
            __NOP();
        }
    }
}

static int image_size_valid(uint32_t size)
{
    return size > 0 && size <= APP_PARTITION_SIZE;
}

static int is_image_vector_valid(uint32_t image_addr, uint32_t exec_addr)
{
    uint32_t sp = *((volatile uint32_t *)image_addr);
    uint32_t pc = *((volatile uint32_t *)(image_addr + 4));
    uint32_t reset_addr = pc & ~1UL;

    if (sp < SRAM_BASE || sp > SRAM_END) {
        return 0;
    }
    if ((pc & 1UL) == 0) {
        return 0;
    }
    if (reset_addr < exec_addr || reset_addr >= (exec_addr + APP_PARTITION_SIZE)) {
        return 0;
    }

    return 1;
}

static int is_vector_table_valid(uint32_t addr)
{
    return is_image_vector_valid(addr, addr);
}

static uint32_t calc_crc32(uint32_t addr, uint32_t size)
{
    uint32_t crc = 0xFFFFFFFFUL;

    for (uint32_t i = 0; i < size; i++) {
        crc ^= *((volatile uint8_t *)(addr + i));
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 1UL) {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc ^ 0xFFFFFFFFUL;
}

static int is_image_crc_valid(uint32_t image_addr, const ota_info_t *info)
{
    if (!info || !image_size_valid(info->new_fw_size)) {
        return 0;
    }
    return calc_crc32(image_addr, info->new_fw_size) == info->new_fw_crc32;
}

static int copy_flash_area(uint32_t src_addr, uint32_t dst_addr, uint32_t size)
{
    uint8_t buf[COPY_CHUNK_SIZE];
    uint32_t offset = 0;

    if (!image_size_valid(size)) {
        return -1;
    }
    if (flash_erase_range(dst_addr, APP_PARTITION_SIZE) != 0) {
        return -1;
    }

    while (offset < size) {
        uint32_t chunk = size - offset;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }

        flash_read(src_addr + offset, buf, chunk);
        if (flash_write(dst_addr + offset, buf, chunk) != 0) {
            return -1;
        }
        offset += chunk;
    }

    return 0;
}

__attribute__((noreturn))
static void jump_to_app(uint32_t app_addr)
{
    uint32_t sp = *((volatile uint32_t *)app_addr);
    uint32_t reset_handler = *((volatile uint32_t *)(app_addr + 4));

    __disable_irq();
    SCB->VTOR = app_addr;
    __set_MSP(sp);

    typedef void (*app_entry_t)(void);
    ((app_entry_t)reset_handler)();

    while (1) {
        __NOP();
    }
}

static void system_clock_init(void)
{
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY));

    FLASH->ACR |= FLASH_ACR_LATENCY_2;
    RCC->CFGR |= RCC_CFGR_PLLSRC;
    RCC->CFGR |= RCC_CFGR_PLLMULL9;
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));

    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);
}
