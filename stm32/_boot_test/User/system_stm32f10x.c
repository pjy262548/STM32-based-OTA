/**
 * @file    system_stm32f10x.c
 * @brief   Bootloader 系统时钟配置
 *
 * 最小化实现, 只配置时钟, 不依赖 HAL
 */

#include "stm32f10x.h"

uint32_t SystemCoreClock = 72000000;

void SystemInit(void)
{
    /* HSE ON */
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY));

    /* Flash: 2 wait states */
    FLASH->ACR |= FLASH_ACR_LATENCY_2;

    /* AHB = SYSCLK, APB1 = HCLK/2, APB2 = HCLK */
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1;
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;
    RCC->CFGR |= RCC_CFGR_PPRE2_DIV1;

    /* PLL = HSE × 9 */
    RCC->CFGR |= RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9;

    /* PLL ON */
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));

    /* Switch to PLL */
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);
}

void SystemCoreClockUpdate(void)
{
    SystemCoreClock = 72000000;
}
