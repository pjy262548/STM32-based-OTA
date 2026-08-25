#ifndef __UART_HANDLER_H
#define __UART_HANDLER_H
#include "stm32f10x.h"
#include "ota_protocol.h"
#define OTA_UART               USART2
#define OTA_UART_BAUDRATE      115200
#define OTA_UART_TX_PIN        GPIO_Pin_2
#define OTA_UART_RX_PIN        GPIO_Pin_3
#define OTA_UART_GPIO          GPIOA
#define OTA_UART_CLOCK_GPIO    RCC_APB2Periph_GPIOA
#define OTA_UART_CLOCK_UART    RCC_APB1Periph_USART2
#define OTA_UART_DMA_CHANNEL   DMA1_Channel6
#define OTA_UART_IRQ_CHANNEL   DMA1_Channel6_IRQn
#define OTA_UART_IRQN          USART2_IRQn
#define UART_RX_BUF_SIZE       1024
void uart_handler_init(void);
void uart_send_bytes(const uint8_t *data, uint16_t len);
void uart_send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len);
void uart_process_rx_data(void);
void uart_irq_callback(void);
void uart_diag_echo_pending(void);
uint32_t uart_rx_total(void);
#endif
