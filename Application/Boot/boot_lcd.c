#include "boot_lcd.h"
#include "debug_uart.h"
#include "ltdc.h"
#include "rng.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
    WIDTH = 800, HEIGHT = 480, LOG_ROWS = 11, LOG_COLUMNS = 62,
    LOG_Y = 242, LOG_LINE_HEIGHT = 20
};
/* The existing linker reserves the application's RGB888 SDRAM region.
 * Keep the reservation for its linker assertion. LTDC reads the L8 AXI buffer. */
static uint8_t unused_sdram_reservation[WIDTH * HEIGHT * 3]
    __attribute__((section(".lcd_framebuffer"), aligned(32), used));
static uint8_t framebuffer[WIDTH * HEIGHT] __attribute__((aligned(32)));

enum lcd_color { BACKGROUND, CONSOLE, TEXT, TRACK, PROGRESS };
static const uint32_t palette[] = {0x112134, 0x0b1828, 0xf2f5fa, 0x293e58, 0x368cd1};
static bool ready;
static char log_rows[LOG_ROWS][LOG_COLUMNS + 1];
static unsigned log_next, log_count;

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

static void rectangle(unsigned x, unsigned y, unsigned width, unsigned height, enum lcd_color color)
{
    for (unsigned row = y; row < y + height; ++row)
        memset(framebuffer + row * WIDTH + x, color, width);
}

static void label(unsigned x, unsigned y, unsigned scale, const char *text)
{
    for (; *text && x + scale * 5 < WIDTH; ++text, x += scale * 6) {
        static const uint8_t punctuation[][7] = {
            {0,0,0,0,0,6,6}, {0,6,6,0,6,6,0}, {1,1,2,4,8,16,16},
            {0,0,0,31,0,0,0}, {14,17,1,2,4,0,4}, {0,4,4,31,4,4,0},
            {0,0,0,0,0,0,31}, {0,0,31,0,31,0,0},
        };
        const char *marks = ".:/-?+_=";
        unsigned char c = (unsigned char)*text;
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        const uint8_t *bits = NULL;
        if (c >= 'A' && c <= 'Z')
            bits = letters[(unsigned)(c - 'A')];
        else if (c >= '0' && c <= '9')
            bits = digits[(unsigned)(c - '0')];
        else if (c == '%')
            bits = percent_glyph;
        else {
            const char *mark = strchr(marks, c);
            if (mark)
                bits = punctuation[mark - marks];
        }
        if (bits)
            for (unsigned row = 0; row < 7; ++row)
                for (unsigned col = 0; col < 5; ++col)
                    if (bits[row] & (16u >> col))
                        rectangle(x + col * scale, y + row * scale, scale, scale, TEXT);
    }
}

static bool start_ltdc(void)
{
    /* The panel timing is identical to MX_LTDC_Init. HAL_LTDC_MspInit
     * supplies the project's PLL3 pixel clock and GPIO pin mapping. */
    hltdc.Instance = LTDC;
    hltdc.Init = (LTDC_InitTypeDef){
        .HSPolarity = LTDC_HSPOLARITY_AL, .VSPolarity = LTDC_VSPOLARITY_AL,
        .DEPolarity = LTDC_DEPOLARITY_AL, .PCPolarity = LTDC_PCPOLARITY_IPC,
        .HorizontalSync = 38, .VerticalSync = 2,
        .AccumulatedHBP = 45, .AccumulatedVBP = 4,
        .AccumulatedActiveW = 845, .AccumulatedActiveH = 484,
        .TotalWidth = 851, .TotalHeigh = 486
    };
    LTDC_LayerCfgTypeDef layer = {
        .WindowX0 = 0, .WindowX1 = WIDTH, .WindowY0 = 0, .WindowY1 = HEIGHT,
        .PixelFormat = LTDC_PIXEL_FORMAT_L8, .Alpha = 255,
        .BlendingFactor1 = LTDC_BLENDING_FACTOR1_PAxCA,
        .BlendingFactor2 = LTDC_BLENDING_FACTOR2_PAxCA,
        .FBStartAdress = (uint32_t)(uintptr_t)framebuffer,
        .ImageWidth = WIDTH, .ImageHeight = HEIGHT
    };
    if (HAL_LTDC_Init(&hltdc) != HAL_OK)
        return false;
    __HAL_LTDC_DISABLE_IT(&hltdc, LTDC_IT_TE | LTDC_IT_FU);
    return HAL_LTDC_ConfigLayer(&hltdc, &layer, 0) == HAL_OK &&
           HAL_LTDC_ConfigCLUT(&hltdc, palette, sizeof palette / sizeof *palette, 0) == HAL_OK &&
           HAL_LTDC_EnableCLUT(&hltdc, 0) == HAL_OK;
}

static void paint_log(void)
{
    if (!ready)
        return;
    rectangle(16, LOG_Y, WIDTH - 32, LOG_ROWS * LOG_LINE_HEIGHT, CONSOLE);
    unsigned first = (log_next + LOG_ROWS - log_count) % LOG_ROWS;
    for (unsigned i = 0; i < log_count; ++i)
        label(24, LOG_Y + 3 + i * LOG_LINE_HEIGHT, 2, log_rows[(first + i) % LOG_ROWS]);
    __DSB();
}

void boot_lcd_log(const char *message)
{
    do {
        size_t length = 0;
        while (length < LOG_COLUMNS && message[length])
            ++length;
        memcpy(log_rows[log_next], message, length);
        log_rows[log_next][length] = 0;
        log_next = (log_next + 1) % LOG_ROWS;
        if (log_count < LOG_ROWS)
            ++log_count;
        message += length;
    } while (*message);
    paint_log();
}

void boot_lcd_init(void)
{
    boot_debug("LCD: bufor AXI SRAM, format L8.");
    rectangle(0, 0, WIDTH, HEIGHT, BACKGROUND);
    label(24, 25, 4, "AKTUALIZACJA H7");
    label(24, 213, 2, "LOG BOOTLOADERA");
    boot_debug("LCD: start LTDC.");
    __DSB();
    if (!start_ltdc()) {
        boot_debug("LCD: blad konfiguracji LTDC.");
        return;
    }
    ready = true;
    boot_lcd_status("START BOOTLOADERA", 0);
    boot_debug("LCD gotowy: LTDC korzysta z AXI SRAM.");
}

void boot_lcd_status(const char *status, unsigned percent)
{
    if (!ready || (last_status == status && last_percent == percent))
        return;
    if (last_status != status) {
        rectangle(24, 75, WIDTH - 48, 32, BACKGROUND);
        rectangle(24, 153, WIDTH - 48, 53, BACKGROUND);
        label(24, 75, 4, status);
    }
    last_status = status;
    last_percent = percent;
    rectangle(24, 118, 622, 20, TRACK);
    rectangle(24, 118, 622u * percent / 100u, 20, PROGRESS);
    char value[8];
    snprintf(value, sizeof value, "%u%%", percent);
    rectangle(672, 116, 92, 24, BACKGROUND);
    label(672, 116, 3, value);
    __DSB();
}

void boot_lcd_recovery(const char *password)
{
    boot_lcd_status("SIEC STM32H753 BOOT", 0);
    label(24, 159, 3, "HASLO:");
    label(150, 159, 3, password);
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
