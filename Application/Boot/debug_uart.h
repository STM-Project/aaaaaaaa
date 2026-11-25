#ifndef BOOT_DEBUG_UART_H
#define BOOT_DEBUG_UART_H

void boot_debug_init(void);
void boot_debug_deinit(void);
void boot_debug(const char *message);

#endif
