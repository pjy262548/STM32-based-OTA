#include "stm32f10x.h"
#include "ota_protocol.h"
#include "flash_writer.h"
#include "uart_handler.h"
#include "version.h"

#define USART2_DIAG_ENABLE 0
#define APP_CONFIRM_DELAY_MS 3000UL

static void delay_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 12000; j++) {
            __NOP();
        }
    }
}

static void system_clock_init(void)
{
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY));

    FLASH->ACR |= FLASH_ACR_LATENCY_2;
    RCC->CFGR |= RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2;

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));

    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);
}

static void vector_table_init(void)
{
    SCB->VTOR = APP_RUN_ADDR;
}

static void led_init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOE, ENABLE);

    GPIO_InitTypeDef gpio;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Pin = GPIO_Pin_5;
    GPIO_Init(GPIOB, &gpio);
    GPIO_SetBits(GPIOB, GPIO_Pin_5);

    gpio.GPIO_Pin = GPIO_Pin_5;
    GPIO_Init(GPIOE, &gpio);
    GPIO_ResetBits(GPIOE, GPIO_Pin_5);
}

static void led_boot_blink(void)
{
    for (int i = 0; i < 3; i++) {
        GPIO_ResetBits(GPIOB, GPIO_Pin_5);
        delay_ms(100);
        GPIO_SetBits(GPIOB, GPIO_Pin_5);
        delay_ms(100);
    }
}

int main(void)
{
    system_clock_init();
    vector_table_init();
    led_init();
    led_boot_blink();

    flash_writer_init();
    ota_protocol_init();
    uart_handler_init();

    __enable_irq();

    uint32_t uptime_ms = 0;
    uint32_t heartbeat_ms = 0;
    uint32_t diag_tx_ms = 0;
    uint32_t last_rx_total = uart_rx_total();
    uint8_t app_confirmed = 0;

    while (1) {
        uart_process_rx_data();

        if (uart_rx_total() != last_rx_total) {
            last_rx_total = uart_rx_total();
            uart_diag_echo_pending();
        }

        delay_ms(1);
        uptime_ms++;
        heartbeat_ms++;
        diag_tx_ms++;
        ota_protocol_tick(uptime_ms);

        if (!app_confirmed && uptime_ms >= APP_CONFIRM_DELAY_MS) {
            ota_confirm_running_app();
            app_confirmed = 1;
        }

#if USART2_DIAG_ENABLE
        if (!ota_context_get()->active && diag_tx_ms >= 500) {
            diag_tx_ms = 0;
            static const uint8_t diag_msg[] = "PA2 OK\r\n";
            uart_send_bytes(diag_msg, sizeof(diag_msg) - 1);
        }
#endif

        if (heartbeat_ms >= 1000) {
            heartbeat_ms = 0;
            if (GPIO_ReadOutputDataBit(GPIOB, GPIO_Pin_5)) {
                GPIO_ResetBits(GPIOB, GPIO_Pin_5);
            } else {
                GPIO_SetBits(GPIOB, GPIO_Pin_5);
            }
        }

        GPIO_ResetBits(GPIOE, GPIO_Pin_5);
    }
}
