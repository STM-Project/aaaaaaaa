#include "esp.h"
#include "http.h"
#include "platform.h"

#define UPDATE_WINDOW_MS 120000u

int main(void)
{
    if (!boot_platform_init())
        for (;;) {}

    H753_BootRequest request;
    bool requested = boot_read_request(&request);
    if (!requested && boot_app_is_valid())
        boot_jump_to_app();

    for (;;) {
        if (!esp_start(requested ? &request : 0)) {
            if (boot_app_is_valid())
                boot_jump_to_app();
            requested = false;
            boot_delay(1000);
            continue;
        }

        uint32_t deadline = boot_millis() + UPDATE_WINDOW_MS;
        for (;;) {
            if ((int32_t)(boot_millis() - deadline) >= 0 && boot_app_is_valid())
                boot_jump_to_app();

            int link = esp_pending();
            if (link == -2)
                break;
            if (link >= 0) {
                enum http_result result = http_serve((unsigned)link, requested ? request.update_key : 0);
                if (result == HTTP_INSTALLED) {
                    boot_delay(250);
                    boot_jump_to_app();
                }
            }
            boot_delay(50);
        }
        if (boot_app_is_valid())
            boot_jump_to_app();
    }
}
