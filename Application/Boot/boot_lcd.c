#include "boot_lcd.h"
#include "fmc.h"
#include "ltdc.h"
#include "rng.h"

#include <stddef.h>
#include <stdio.h>

enum { WIDTH = 800, HEIGHT = 480, SCALE = 5 };
static uint8_t framebuffer[WIDTH * HEIGHT * 3]
    __attribute__((section(".lcd_framebuffer"), aligned(32), used));
static bool ready, sdram_error;

void ERROR_Sdram(void)
{
    sdram_error = true;
}
static unsigned last_percent = 101;
static const char *last_status;

/* Five pixels across, seven rows per glyph. This screen can run with erased SD. */
static const uint8_t letters[26][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
};
static const uint8_t digits[10][7] = {
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14},
};
static const uint8_t percent_glyph[7] = {17,2,4,8,17,0,0};

static void rectangle(unsigned x, unsigned y, unsigned width, unsigned height, uint32_t rgb)
{
    for (unsigned row = y; row < y + height; ++row) {
        uint8_t *p = framebuffer + (row * WIDTH + x) * 3;
        for (unsigned col = 0; col < width; ++col) {
            *p++ = (uint8_t)rgb;
            *p++ = (uint8_t)(rgb >> 8);
            *p++ = (uint8_t)(rgb >> 16);
        }
    }
}

static void label(unsigned x, unsigned y, const char *text)
{
    for (; *text && x + SCALE * 5 < WIDTH; ++text, x += SCALE * 6) {
        const uint8_t *bits = NULL;
        if (*text >= 'A' && *text <= 'Z')
            bits = letters[(unsigned)(*text - 'A')];
        else if (*text >= '0' && *text <= '9')
            bits = digits[(unsigned)(*text - '0')];
        else if (*text == '%')
            bits = percent_glyph;
        if (bits)
            for (unsigned row = 0; row < 7; ++row)
                for (unsigned col = 0; col < 5; ++col)
                    if (bits[row] & (16u >> col))
                        rectangle(x + col * SCALE, y + row * SCALE, SCALE, SCALE, 0xf2f5fa);
    }
}

void boot_lcd_init(void)
{
    MX_FMC_Init();
    if (sdram_error)
        return;
    rectangle(0, 0, WIDTH, HEIGHT, 0x112134);
    MX_LTDC_Init();
    ready = true;
    label(42, 65, "AKTUALIZACJA H7");
}

void boot_lcd_status(const char *status, unsigned percent)
{
    if (!ready || (last_status == status && last_percent == percent))
        return;
    if (last_status != status) {
        rectangle(42, 158, WIDTH - 84, 48, 0x112134);
        rectangle(42, 315, WIDTH - 84, 92, 0x112134);
        label(42, 158, status);
    }
    last_status = status;
    last_percent = percent;
    rectangle(42, 223, 716, 28, 0x293e58);
    rectangle(42, 223, 716u * percent / 100u, 28, 0x368cd1);
    char value[8];
    snprintf(value, sizeof value, "%u%%", percent);
    rectangle(42, 268, 132, 38, 0x112134);
    label(42, 268, value);
    __DSB();
}

void boot_lcd_recovery(const char *password)
{
    boot_lcd_status("SIEC STM32H753 BOOT", 0);
    label(42, 325, "HASLO");
    label(42, 369, password);
    __DSB();
}

bool boot_recovery_password(char password[17])
{
    MX_RNG_Init();
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t random;
        if (HAL_RNG_GenerateRandomNumber(&hrng, &random) != HAL_OK)
            return false;
        for (unsigned j = 0; j < 8; ++j) {
            password[i * 8 + j] = "0123456789ABCDEF"[random & 15u];
            random >>= 4;
        }
    }
    password[16] = 0;
    return true;
}
