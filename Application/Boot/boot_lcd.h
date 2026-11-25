#ifndef BOOT_LCD_H
#define BOOT_LCD_H
#include <stdbool.h>
void boot_lcd_init(void);
void boot_lcd_log(const char *message);
void boot_lcd_status(const char *status, unsigned percent);
void boot_lcd_recovery(const char *password);
bool boot_recovery_password(char password[17]);
#endif
