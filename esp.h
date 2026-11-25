#ifndef BOOT_ESP_H
#define BOOT_ESP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "h753_boot_request.h"

bool esp_start(const H753_BootRequest *request);
int esp_pending(void);
int esp_read(unsigned link, uint8_t *data, size_t capacity);
bool esp_send(unsigned link, const void *data, size_t length);
void esp_close(unsigned link);

uint32_t boot_millis(void);
void boot_delay(uint32_t milliseconds);
void boot_esp_power(bool enabled);
bool boot_uart_init(uint32_t baud);
bool boot_uart_send(const void *data, size_t length);
int boot_uart_read(uint32_t deadline);

#endif
