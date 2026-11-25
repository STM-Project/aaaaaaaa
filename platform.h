#ifndef BOOT_PLATFORM_H
#define BOOT_PLATFORM_H

#include <stdbool.h>
#include "h753_boot_request.h"

bool boot_platform_init(void);
bool boot_app_is_valid(void);
bool boot_read_request(H753_BootRequest *request);
_Noreturn void boot_jump_to_app(void);

#endif
