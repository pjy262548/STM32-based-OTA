#ifndef __STM32F10x_CONF_H
#define __STM32F10x_CONF_H

/* 参数校验宏 — 标准外设库必需, 未启用全断言时为空操作 */
#ifdef USE_STDPERIPH_DRIVER
  #ifdef USE_FULL_ASSERT
    void assert_failed(uint8_t *file, uint32_t line);
    #define assert_param(expr) ((expr) ? (void)0 : assert_failed((uint8_t *)__FILE__, __LINE__))
  #else
    #define assert_param(expr) ((void)0)
  #endif
#endif

#include "stm32f10x_flash.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_usart.h"
#include "stm32f10x_dma.h"
#include "misc.h"
#endif
