#include "boot_lcd.h"
#include "fmc.h"
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
static uint8_t framebuffer[WIDTH * HEIGHT * 3]
    __attribute__((section(".lcd_framebuffer"), aligned(32), used));
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
                        rectangle(x + col * scale, y + row * scale, scale, scale, 0xf2f5fa);
    }
}

static bool sdram_command(uint32_t mode, uint32_t count, uint32_t register_value)
{
    FMC_SDRAM_CommandTypeDef cmd = {.CommandMode = mode,
                                    .CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1,
                                    .AutoRefreshNumber = count,
                                    .ModeRegisterDefinition = register_value};
    return HAL_SDRAM_SendCommand(&hsdram1, &cmd, 100) == HAL_OK;
}

static bool framebuffer_init(void)
{
    /* Board timing from MX_FMC_Init. Skip the application's full 16 MiB test. */
    hsdram1.Instance = FMC_SDRAM_DEVICE;
    hsdram1.Init = (FMC_SDRAM_InitTypeDef){
        .SDBank = FMC_SDRAM_BANK1, .ColumnBitsNumber = FMC_SDRAM_COLUMN_BITS_NUM_8,
        .RowBitsNumber = FMC_SDRAM_ROW_BITS_NUM_12, .MemoryDataWidth = FMC_SDRAM_MEM_BUS_WIDTH_32,
        .InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4, .CASLatency = FMC_SDRAM_CAS_LATENCY_2,
        .WriteProtection = FMC_SDRAM_WRITE_PROTECTION_DISABLE,
        .SDClockPeriod = FMC_SDRAM_CLOCK_PERIOD_2, .ReadBurst = FMC_SDRAM_RBURST_ENABLE,
        .ReadPipeDelay = FMC_SDRAM_RPIPE_DELAY_1
    };
    FMC_SDRAM_TimingTypeDef timing = {.LoadToActiveDelay = 2, .ExitSelfRefreshDelay = 7,
        .SelfRefreshTime = 5, .RowCycleDelay = 7, .WriteRecoveryTime = 3,
        .RPDelay = 2, .RCDDelay = 2};
    if (HAL_SDRAM_Init(&hsdram1, &timing) != HAL_OK)
        return false;
    HAL_SetFMCMemorySwappingConfig(FMC_SWAPBMAP_SDRAM_SRAM);
    if (!sdram_command(FMC_SDRAM_CMD_CLK_ENABLE, 1, 0))
        return false;
    HAL_Delay(1);
    if (!sdram_command(FMC_SDRAM_CMD_PALL, 1, 0) ||
        !sdram_command(FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8, 0) ||
        !sdram_command(FMC_SDRAM_CMD_LOAD_MODE, 1, 0x0222) ||
        HAL_SDRAM_ProgramRefreshRate(&hsdram1, 1542) != HAL_OK)
        return false;

    volatile uint8_t *probe = framebuffer;
    probe[0] = 0x5a;
    probe[sizeof framebuffer - 1] = 0xa5;
    __DSB();
    return probe[0] == 0x5a && probe[sizeof framebuffer - 1] == 0xa5;
}

static void paint_log(void)
{
    if (!ready)
        return;
    rectangle(16, LOG_Y, WIDTH - 32, LOG_ROWS * LOG_LINE_HEIGHT, 0x0b1828);
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
    boot_debug("LCD: inicjalizacja SDRAM.");
    if (!framebuffer_init()) {
        boot_debug("LCD: blad SDRAM.");
        return;
    }

    rectangle(0, 0, WIDTH, HEIGHT, 0x112134);
    label(24, 25, 4, "AKTUALIZACJA H7");
    label(24, 213, 2, "LOG BOOTLOADERA");
    boot_debug("LCD: start LTDC.");
    __DSB();
    MX_LTDC_Init();
    ready = true;
    boot_lcd_status("START BOOTLOADERA", 0);
    boot_debug("LCD gotowy.");
}

void boot_lcd_status(const char *status, unsigned percent)
{
    if (!ready || (last_status == status && last_percent == percent))
        return;
    if (last_status != status) {
        rectangle(24, 75, WIDTH - 48, 32, 0x112134);
        rectangle(24, 153, WIDTH - 48, 53, 0x112134);
        label(24, 75, 4, status);
    }
    last_status = status;
    last_percent = percent;
    rectangle(24, 118, 622, 20, 0x293e58);
    rectangle(24, 118, 622u * percent / 100u, 20, 0x368cd1);
    char value[8];
    snprintf(value, sizeof value, "%u%%", percent);
    rectangle(672, 116, 92, 24, 0x112134);
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
