#include "esp.h"
#include "http.h"
#include "platform.h"

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#define main boot_entry
#include "../main.c"
#undef main

static jmp_buf finished;
static unsigned start_count, jumps;
static uint32_t time_ms;
static bool has_app, has_request, esp_ok;

bool boot_platform_init(void) { return true; }
bool boot_app_is_valid(void) { return has_app; }

bool boot_read_request(H753_BootRequest *request)
{
    *request = (H753_BootRequest){.ssid = "selected", .port = 8180};
    return has_request;
}

_Noreturn void boot_jump_to_app(void)
{
    ++jumps;
    longjmp(finished, 1);
}

uint32_t boot_millis(void) { return time_ms; }
void boot_delay(uint32_t milliseconds) { time_ms += milliseconds; }

bool esp_start(const H753_BootRequest *request)
{
    ++start_count;
    assert(has_request == (request != NULL));
    if (has_request)
        assert(!strcmp(request->ssid, "selected") && request->port == 8180);
    return esp_ok;
}

int esp_pending(void) { return 2; }
enum http_result http_serve(unsigned link, const char *key)
{
    assert(link == 2 && (key != NULL) == has_request);
    return HTTP_INSTALLED;
}

static void verify(bool valid, bool requested, bool network, unsigned starts, uint32_t delay)
{
    has_app = valid;
    has_request = requested;
    esp_ok = network;
    start_count = jumps = time_ms = 0;
    if (!setjmp(finished))
        boot_entry();
    assert(jumps == 1 && start_count == starts && time_ms == delay);
}

int main(void)
{
    verify(true, false, true, 0, 0);
    verify(true, true, true, 1, 250);
    verify(true, true, false, 1, 0);
    verify(false, false, true, 1, 250);
    puts("PASS: normal jump without ESP, one-shot STA update and AP recovery");
    return 0;
}
