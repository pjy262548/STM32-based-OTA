#include "uart_handler.h"
#include "ota_protocol.h"
#include <string.h>

static uint8_t rw[UART_RX_BUF_SIZE];
static volatile uint16_t rl = 0;
static volatile uint8_t rc = 0;
static volatile uint8_t rover = 0;
static volatile uint32_t rx_total = 0;

#ifndef UART_RX_ECHO_DIAG
#define UART_RX_ECHO_DIAG 0
#endif

#if UART_RX_ECHO_DIAG
#define ECHO_BUF_SIZE 64
static volatile uint8_t echo_buf[ECHO_BUF_SIZE];
static volatile uint8_t echo_head = 0;
static volatile uint8_t echo_tail = 0;
#endif

static void uart_append_rx_byte(uint8_t b)
{
    if (rl >= UART_RX_BUF_SIZE) {
        rl = 0;
        rc = 0;
        rover = 1;
    }

    rw[rl++] = b;
    rx_total++;
    rc = 1;
#if UART_RX_ECHO_DIAG
    uint8_t next_head = (uint8_t)((echo_head + 1) % ECHO_BUF_SIZE);
    if (next_head != echo_tail) {
        echo_buf[echo_head] = b;
        echo_head = next_head;
    }
#endif
}

void uart_handler_init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;
    NVIC_InitTypeDef nvic;

    RCC_APB1PeriphClockCmd(OTA_UART_CLOCK_UART, ENABLE);
    RCC_APB2PeriphClockCmd(OTA_UART_CLOCK_GPIO, ENABLE);

    gpio.GPIO_Pin = OTA_UART_TX_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(OTA_UART_GPIO, &gpio);

    gpio.GPIO_Pin = OTA_UART_RX_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(OTA_UART_GPIO, &gpio);

    USART_DeInit(OTA_UART);
    usart.USART_BaudRate = OTA_UART_BAUDRATE;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(OTA_UART, &usart);

    rl = 0;
    rc = 0;
    rover = 0;

    USART_ITConfig(OTA_UART, USART_IT_RXNE, ENABLE);
    USART_Cmd(OTA_UART, ENABLE);

    nvic.NVIC_IRQChannel = OTA_UART_IRQN;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
}

void uart_send_bytes(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint32_t timeout = 100000UL;
        while (!(OTA_UART->SR & USART_SR_TXE) && --timeout) {
        }
        if (timeout == 0) {
            return;
        }
        OTA_UART->DR = data[i];
    }
    uint32_t timeout = 100000UL;
    while (!(OTA_UART->SR & USART_SR_TC) && --timeout) {
    }
}

void uart_send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    if (len > PROTOCOL_MAX_PAYLOAD) {
        return;
    }

    uint8_t header[4];
    header[0] = PROTOCOL_SOF;
    header[1] = cmd;
    header[2] = (uint8_t)(len >> 8);
    header[3] = (uint8_t)(len & 0xFF);

    uint16_t crc = frame_crc16_calc(payload, len);
    uart_send_bytes(header, sizeof(header));
    if (len > 0) {
        uart_send_bytes(payload, len);
    }

    uint8_t crc_bytes[2];
    crc_bytes[0] = (uint8_t)(crc >> 8);
    crc_bytes[1] = (uint8_t)(crc & 0xFF);
    uart_send_bytes(crc_bytes, sizeof(crc_bytes));
}

void uart_irq_callback(void)
{
    volatile uint32_t sr = OTA_UART->SR;

    if (sr & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) {
        volatile uint32_t tmp = sr;
        tmp = OTA_UART->DR;
        (void)tmp;
        return;
    }

    if (sr & USART_SR_RXNE) {
        uart_append_rx_byte((uint8_t)OTA_UART->DR);
    }
}


uint32_t uart_rx_total(void)
{
    return rx_total;
}

void uart_diag_echo_pending(void)
{
#if UART_RX_ECHO_DIAG
    uint8_t b;
    static const char hex[] = "0123456789ABCDEF";
    uint8_t msg[5];

    __disable_irq();
    if (echo_tail == echo_head) {
        __enable_irq();
        return;
    }
    b = echo_buf[echo_tail];
    echo_tail = (uint8_t)((echo_tail + 1) % ECHO_BUF_SIZE);
    __enable_irq();

    msg[0] = 'R';
    msg[1] = 'X';
    msg[2] = hex[(b >> 4) & 0x0F];
    msg[3] = hex[b & 0x0F];
    msg[4] = '\n';
    uart_send_bytes(msg, sizeof(msg));
#endif
}

void uart_process_rx_data(void)
{
    static uint8_t state = 0;
    static uint16_t payload_index = 0;
    static protocol_frame_t frame;

    uint8_t buf[UART_RX_BUF_SIZE];
    uint16_t len;
    uint8_t overflow;

    if (!rc) {
        return;
    }

    __disable_irq();
    len = rl;
    overflow = rover;
    if (len > 0) {
        memcpy(buf, rw, len);
    }
    rl = 0;
    rc = 0;
    rover = 0;
    __enable_irq();

    if (overflow) {
        state = 0;
        payload_index = 0;
    }

    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        switch (state) {
        case 0:
            if (b == PROTOCOL_SOF) {
                state = 1;
            }
            break;
        case 1:
            frame.cmd = b;
            state = 2;
            break;
        case 2:
            frame.len = (uint16_t)b << 8;
            state = 3;
            break;
        case 3:
            frame.len |= b;
            if (frame.len > PROTOCOL_MAX_PAYLOAD) {
                state = 0;
            } else if (frame.len == 0) {
                state = 5;
            } else {
                payload_index = 0;
                state = 4;
            }
            break;
        case 4:
            frame.payload[payload_index++] = b;
            if (payload_index >= frame.len) {
                state = 5;
            }
            break;
        case 5:
            frame.crc = (uint16_t)b << 8;
            state = 6;
            break;
        case 6:
            frame.crc |= b;
            ota_process_frame(&frame);
            state = 0;
            payload_index = 0;
            break;
        default:
            state = 0;
            payload_index = 0;
            break;
        }
    }
}
