#include "esp.h"
#include "image.h"
#include "platform.h"
#include "stm32h7xx_hal.h"

#include <string.h>

#define ESP_ENABLE_PIN GPIO_PIN_9
#define ESP_UART_PINS (GPIO_PIN_6 | GPIO_PIN_7)
static UART_HandleTypeDef uart;

uint32_t boot_millis(void)
{
    return HAL_GetTick();
}

void boot_delay(uint32_t milliseconds)
{
    HAL_Delay(milliseconds);
}

bool boot_platform_init(void)
{
    if (HAL_Init() != HAL_OK)
        return false;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOA, ESP_ENABLE_PIN, GPIO_PIN_RESET);
    GPIO_InitTypeDef gpio = {.Pin = ESP_ENABLE_PIN, .Mode = GPIO_MODE_OUTPUT_PP,
                            .Pull = GPIO_NOPULL, .Speed = GPIO_SPEED_FREQ_LOW};
    HAL_GPIO_Init(GPIOA, &gpio);
    return true;
}

void boot_esp_power(bool enabled)
{
    HAL_GPIO_WritePin(GPIOA, ESP_ENABLE_PIN, enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

bool boot_uart_init(uint32_t baud)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_USART6_CLK_ENABLE();
    __HAL_RCC_USART16_CONFIG(RCC_USART16CLKSOURCE_D2PCLK2);
    __HAL_RCC_USART6_FORCE_RESET();
    __HAL_RCC_USART6_RELEASE_RESET();
    GPIO_InitTypeDef gpio = {.Pin = GPIO_PIN_6, .Mode = GPIO_MODE_AF_PP,
                             .Pull = GPIO_NOPULL, .Speed = GPIO_SPEED_FREQ_HIGH,
                             .Alternate = GPIO_AF7_USART6};
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = GPIO_PIN_7;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &gpio);
    uart = (UART_HandleTypeDef){
        .Instance = USART6,
        .Init = {.BaudRate = baud, .WordLength = UART_WORDLENGTH_8B,
                 .StopBits = UART_STOPBITS_1, .Parity = UART_PARITY_NONE,
                 .Mode = UART_MODE_TX_RX, .HwFlowCtl = UART_HWCONTROL_NONE,
                 .OverSampling = UART_OVERSAMPLING_16,
                 .OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE,
                 .ClockPrescaler = UART_PRESCALER_DIV1},
        .AdvancedInit = {.AdvFeatureInit = UART_ADVFEATURE_SWAP_INIT,
                         .Swap = UART_ADVFEATURE_SWAP_ENABLE}
    };
    return HAL_UART_Init(&uart) == HAL_OK;
}

bool boot_uart_send(const void *data, size_t length)
{
    return length <= UINT16_MAX &&
           HAL_UART_Transmit(&uart, data, (uint16_t)length, 3000) == HAL_OK;
}

int boot_uart_read(uint32_t deadline)
{
    while ((int32_t)(HAL_GetTick() - deadline) < 0) {
        uint32_t status = USART6->ISR;
        if (status & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
            USART6->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
            USART6->RQR = USART_RQR_RXFRQ;
        } else if (status & USART_ISR_RXNE_RXFNE) {
            return (uint8_t)USART6->RDR;
        }
    }
    return -1;
}

bool flash_erase_sector(uint32_t address)
{
    if ((FLASH->OPTSR_CUR & FLASH_OPTSR_SWAP_BANK_OPT) ||
        address < APP_FLASH_START || address >= APP_FLASH_END ||
        (address - BOOT_FLASH_START) % FLASH_SECTOR_BYTES != 0)
        return false;
    bool bank2 = address >= FLASH_BANK2_BASE;
    uint32_t bank_start = bank2 ? FLASH_BANK2_BASE : BOOT_FLASH_START;
    FLASH_EraseInitTypeDef erase = {.TypeErase = FLASH_TYPEERASE_SECTORS,
                                   .Banks = bank2 ? FLASH_BANK_2 : FLASH_BANK_1,
                                   .Sector = (address - bank_start) / FLASH_SECTOR_BYTES,
                                   .NbSectors = 1, .VoltageRange = FLASH_VOLTAGE_RANGE_3};
    if (HAL_FLASH_Unlock() != HAL_OK)
        return false;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK1 | FLASH_FLAG_EOP_BANK1);
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS_BANK2 | FLASH_FLAG_EOP_BANK2);
    uint32_t error;
    bool ok = HAL_FLASHEx_Erase(&erase, &error) == HAL_OK;
    HAL_FLASH_Lock();
    return ok;
}

bool flash_write_word(uint32_t address, const uint8_t data[FLASH_WORD_BYTES])
{
    if (address < APP_FLASH_START || address > APP_FLASH_END - FLASH_WORD_BYTES ||
        address % FLASH_WORD_BYTES != 0 || (uintptr_t)data % FLASH_WORD_BYTES != 0)
        return false;
    if (HAL_FLASH_Unlock() != HAL_OK)
        return false;
    bool ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, address, (uint32_t)data) == HAL_OK;
    HAL_FLASH_Lock();
    __DSB();
    return ok && memcmp((const void *)address, data, FLASH_WORD_BYTES) == 0;
}

bool boot_read_request(H753_BootRequest *request)
{
    if (!(RCC->RSR & RCC_RSR_SFTRSTF))
        return false;
    PWR->CR1 |= PWR_CR1_DBP;
    uint32_t started = boot_millis();
    while (!(PWR->CR1 & PWR_CR1_DBP)) {
        if (boot_millis() - started > 100u)
            return false;
    }
    __HAL_RCC_BKPRAM_CLK_ENABLE();
    volatile H753_BootRequest *mailbox = (volatile H753_BootRequest *)H753_BOOT_MAILBOX_ADDRESS;
    *request = *mailbox;
    if (request->magic != H753_BOOT_REQUEST_MAGIC)
        return false;

    *mailbox = (H753_BootRequest){0};
    __DSB();
    return request->check == H753_BootRequestCheck(request) &&
           request->port && request->ssid[0] && request->update_key[0] &&
           request->update_key[H753_UPDATE_KEY_LENGTH] == 0 &&
           memchr(request->ssid, 0, sizeof request->ssid) &&
           memchr(request->password, 0, sizeof request->password);
}

bool boot_app_is_valid(void)
{
    const volatile uint32_t *vectors = (const volatile uint32_t *)APP_FLASH_START;
    return image_vectors_valid(vectors[0], vectors[1], APP_FLASH_CAPACITY);
}

/* After changing MSP, nothing may access the old C stack. */
__attribute__((naked, noreturn))
static void enter_app(uint32_t msp __attribute__((unused)),
                      uint32_t reset __attribute__((unused)))
{
    __asm volatile("movs r2, #0\n"
                   "msr control, r2\n"
                   "msr basepri, r2\n"
                   "msr faultmask, r2\n"
                   "msr msp, r0\n"
                   "isb\n"
                   "cpsie i\n"
                   "bx r1");
}

_Noreturn void boot_jump_to_app(void)
{
    const volatile uint32_t *vectors = (const volatile uint32_t *)APP_FLASH_START;
    uint32_t msp = vectors[0], reset = vectors[1];
    boot_esp_power(false);
    if (uart.Instance) {
        __HAL_RCC_USART6_FORCE_RESET();
        __HAL_RCC_USART6_RELEASE_RESET();
        __HAL_RCC_USART6_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOC, ESP_UART_PINS);
    }
    HAL_GPIO_DeInit(GPIOA, ESP_ENABLE_PIN);
    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;
    for (unsigned i = 0; i < sizeof NVIC->ICER / sizeof NVIC->ICER[0]; ++i) {
        NVIC->ICER[i] = UINT32_MAX;
        NVIC->ICPR[i] = UINT32_MAX;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    SCB->VTOR = APP_FLASH_START;
    __DSB();
    __ISB();
    enter_app(msp, reset);
}
