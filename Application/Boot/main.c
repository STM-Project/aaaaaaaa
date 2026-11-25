#include "esp.h"
#include "http.h"
#include "image.h"
#include "platform.h"
#include "boot_lcd.h"
#include "debug_uart.h"
#include "main.h"

#include <stdio.h>

#define UPDATE_WINDOW_MS 120000u

void Boot_Main(void)
{
    H753_BootRequest request;
    bool requested = boot_read_request(&request);
    if (!requested && boot_app_is_valid())
        boot_jump_to_app();

    if (!boot_platform_init())
        for (;;) {}

    boot_debug_init();
    boot_debug("Start STM32H753");

    extern void SystemClock_Config(void);
    SystemClock_Config();
    boot_debug_init();
    boot_debug(requested ? "Przyjeto zadanie /upload. Tryb STA."
                         : "Brak poprawnej aplikacji. Tryb serwisowy AP.");
    boot_lcd_init();
    boot_debug("SDRAM: inicjalizacja przed ESP32.");
    if (boot_sdram_init())
        boot_debug("SDRAM gotowy.");
    else
        boot_debug("SDRAM: blad; LCD pozostaje w AXI SRAM.");
    /* Give LTDC time to show the title before starting ESP32. */
    boot_delay(100);
    char recovery_password[17] = {0};
    for (;;) {
        if (!requested && !recovery_password[0] && !boot_recovery_password(recovery_password)) {
            recovery_password[0] = 0;
            boot_lcd_status("BLAD RNG", 0);
            boot_debug("Blad RNG. Ponawiam generowanie hasla AP.");
            boot_delay(1000);
            continue;
        }
        boot_lcd_status(requested ? "LACZENIE WIFI" : "SIEC SERWISOWA", 0);
        if (!esp_start(requested ? &request : 0, recovery_password)) {
            boot_debug("Start ESP32/HTTP nieudany.");
            boot_lcd_status("BLAD WIFI LUB HTTP", 0);
            if (boot_app_is_valid()) {
                boot_delay(3000);
                NVIC_SystemReset();
            }
            requested = false;
            boot_delay(1000);
            continue;
        }

        if (requested)
            boot_lcd_status("WYBIERZ BIN W HTTP", 0);
        else
            boot_lcd_recovery(recovery_password);
        uint32_t deadline = boot_millis() + UPDATE_WINDOW_MS;
        for (;;) {
            if ((int32_t)(boot_millis() - deadline) >= 0 && boot_app_is_valid())
                NVIC_SystemReset();

            int link = esp_pending();
            if (link == -2) {
                boot_debug("Brak odpowiedzi ESP32 na AT+CIPRECVLEN?.");
                break;
            }
            if (link >= 0) {
                enum http_result result = http_serve((unsigned)link, requested ? request.update_key : 0);
                if (result == HTTP_INSTALLED) {
                    const volatile uint32_t *vectors = (const volatile uint32_t *)APP_FLASH_START;
                    char message[76];
                    snprintf(message, sizeof message, "Firmware zapisany. MSP=%08lX Reset_Handler=%08lX.",
                             (unsigned long)vectors[0], (unsigned long)vectors[1]);
                    boot_debug(message);
                    boot_debug("Reset i start aplikacji.");
                    boot_delay(1200);
                    NVIC_SystemReset();
                }
            }
            boot_delay(50);
        }
        if (boot_app_is_valid())
            NVIC_SystemReset();
    }
}
