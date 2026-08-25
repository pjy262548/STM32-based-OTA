#include "stm32f10x_it.h"
#include "uart_handler.h"

static void fault_reset(void)
{
  __disable_irq();
  NVIC_SystemReset();
  while (1) {
  }
}

void HardFault_Handler(void){fault_reset();}
void MemManage_Handler(void){fault_reset();}
void BusFault_Handler(void){fault_reset();}
void UsageFault_Handler(void){fault_reset();}

void USART2_IRQHandler(void){uart_irq_callback();}
void DMA1_Channel6_IRQHandler(void){
  if(DMA1->ISR&DMA_ISR_TCIF6)DMA1->IFCR=DMA_IFCR_CTCIF6;
  if(DMA1->ISR&DMA_ISR_HTIF6)DMA1->IFCR=DMA_IFCR_CHTIF6;
  if(DMA1->ISR&DMA_ISR_TEIF6)DMA1->IFCR=DMA_IFCR_CTEIF6;
}
