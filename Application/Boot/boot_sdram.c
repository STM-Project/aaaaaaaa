#include "platform.h"
#include "debug_uart.h"
#include "fmc.h"

static bool sdram_command(uint32_t mode, uint32_t count, uint32_t register_value)
{
    FMC_SDRAM_CommandTypeDef cmd = {.CommandMode = mode,
                                    .CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1,
                                    .AutoRefreshNumber = count,
                                    .ModeRegisterDefinition = register_value};
    return HAL_SDRAM_SendCommand(&hsdram1, &cmd, 100) == HAL_OK;
}

bool boot_sdram_init(void)
{
    /* Same timing, remap and mode register as the main H753 application. */
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
    if (HAL_SDRAM_Init(&hsdram1, &timing) != HAL_OK) {
        boot_debug("SDRAM: HAL_SDRAM_Init nieudany.");
        return false;
    }
    HAL_SetFMCMemorySwappingConfig(FMC_SWAPBMAP_SDRAM_SRAM);
    if (!sdram_command(FMC_SDRAM_CMD_CLK_ENABLE, 1, 0)) {
        boot_debug("SDRAM: blad wlaczenia zegara.");
        return false;
    }
    HAL_Delay(1);
    if (!sdram_command(FMC_SDRAM_CMD_PALL, 1, 0) ||
        !sdram_command(FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8, 0) ||
        !sdram_command(FMC_SDRAM_CMD_LOAD_MODE, 1, 0x0222) ||
        HAL_SDRAM_ProgramRefreshRate(&hsdram1, 1542) != HAL_OK) {
        boot_debug("SDRAM: blad sekwencji konfiguracji FMC.");
        return false;
    }

    /* Probe full words, matching the 32-bit board memory bus. */
    volatile uint32_t *probe = (volatile uint32_t *)(uintptr_t)SDRAM_MEMORY_ADDR;
    probe[0] = 0x55aa33ccu;
    probe[1] = 0xaa55cc33u;
    __DSB();
    bool valid = probe[0] == 0x55aa33ccu && probe[1] == 0xaa55cc33u;
    if (!valid)
        boot_debug("SDRAM: blad odczytu testu 32 bit.");
    else {
        probe[0] = 0;
        probe[1] = 0;
        __DSB();
    }
    return valid;
}
