#include "esp.h"
#include "http.h"
#include "platform.h"
#include "boot_lcd.h"
#include "debug_uart.h"
#include "main.h"

#define UPDATE_WINDOW_MS 120000u

void Boot_Main(void)
{
    if (!boot_platform_init())
        for (;;) {}

    boot_debug_init();
    boot_debug("Start STM32H753");

    H753_BootRequest request;
    bool requested = boot_read_request(&request);
    if (!requested && boot_app_is_valid()) {
        boot_debug("Brak zadania aktualizacji. Start aplikacji 0x08020000.");
        boot_debug_deinit();
        boot_jump_to_app();
    }

    extern void SystemClock_Config(void);
    SystemClock_Config();
    boot_debug_init();
    boot_debug(requested ? "Przyjeto zadanie /upload. Tryb STA."
                         : "Brak poprawnej aplikacji. Tryb serwisowy AP.");
    boot_lcd_init();
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
            if (boot_app_is_valid())
                NVIC_SystemReset();
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
                    boot_debug("Firmware zapisany. Reset i start aplikacji.");
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
