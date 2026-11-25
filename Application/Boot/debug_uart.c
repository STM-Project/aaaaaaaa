#include "debug_uart.h"
#include "boot_lcd.h"
#include "stm32h7xx_hal.h"

#define DEBUG_BAUD 2000000u
#define DEBUG_TIMEOUT_MS 10u

static uint8_t debug_enabled;

void boot_debug_init(void)
{
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_UART7_CONFIG(RCC_USART234578CLKSOURCE_D2PCLK1);
    __HAL_RCC_UART7_CLK_ENABLE();
    __HAL_RCC_UART7_FORCE_RESET();
    __HAL_RCC_UART7_RELEASE_RESET();

    GPIO_InitTypeDef pin = {.Pin = GPIO_PIN_7, .Mode = GPIO_MODE_AF_PP,
                            .Pull = GPIO_NOPULL, .Speed = GPIO_SPEED_FREQ_LOW,
                            .Alternate = GPIO_AF7_UART7};
    HAL_GPIO_Init(GPIOF, &pin);

    /* Same UART7 TX and 8N1 format as the application. Polling needs no DMA. */
    UART7->BRR = (HAL_RCC_GetPCLK1Freq() + DEBUG_BAUD / 2u) / DEBUG_BAUD;
    UART7->CR1 = USART_CR1_TE | USART_CR1_UE;
    debug_enabled = 1;
}

static void write_text(const char *text, uint32_t start)
{
    for (; *text; ++text) {
        while (!(UART7->ISR & USART_ISR_TXE_TXFNF)) {
            if (HAL_GetTick() - start >= DEBUG_TIMEOUT_MS)
                return;
        }
        UART7->TDR = (uint8_t)*text;
    }
}

void boot_debug(const char *message)
{
    boot_lcd_log(message);
    if (!debug_enabled)
        return;
    uint32_t start = HAL_GetTick();
    write_text("[BOOT] ", start);
    write_text(message, start);
    write_text("\r\n", start);
    while (!(UART7->ISR & USART_ISR_TC) && HAL_GetTick() - start < DEBUG_TIMEOUT_MS) {}
}

void boot_debug_deinit(void)
{
    if (!debug_enabled)
        return;
    UART7->CR1 = 0;
    HAL_GPIO_DeInit(GPIOF, GPIO_PIN_7);
    __HAL_RCC_UART7_CLK_DISABLE();
    debug_enabled = 0;
}
