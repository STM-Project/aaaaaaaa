#include "stm32h7xx_hal.h"

extern unsigned char _estack;
void Reset_Handler(void);
void TIM8_TRG_COM_TIM14_IRQHandler(void);

static void Boot_Fault(void)
{
    for (;;) {}
}

/* Only the HAL time base generates interrupts in the bootloader. */
__attribute__((section(".boot_vectors"), used, aligned(1024)))
void (*const BootVectors[166])(void) = {
    [0] = (void (*)(void)) &_estack,
    [1] = Reset_Handler,
    [2] = Boot_Fault, [3] = Boot_Fault, [4] = Boot_Fault,
    [5] = Boot_Fault, [6] = Boot_Fault,
    [16 + TIM8_TRG_COM_TIM14_IRQn] = TIM8_TRG_COM_TIM14_IRQHandler,
};
