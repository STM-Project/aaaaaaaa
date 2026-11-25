CROSS_COMPILE ?= arm-none-eabi-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE := $(CROSS_COMPILE)size
PYTHON ?= python3

# The export includes the HAL and device startup. A checkout can reuse its copies.
BOARD_ROOT := $(if $(wildcard vendor/Drivers),vendor,..)
HAL := $(BOARD_ROOT)/Drivers/STM32H7xx_HAL_Driver
CMSIS := $(BOARD_ROOT)/Drivers/CMSIS
CPU := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
CPPFLAGS := -I. -Ibuild -I$(HAL)/Inc -I$(CMSIS)/Include -I$(CMSIS)/Device/ST/STM32H7xx/Include \
            -DSTM32H753xx -DUSE_HAL_DRIVER -DUSE_PWR_LDO_SUPPLY
CFLAGS := $(CPU) -Os -g3 -std=c11 -ffunction-sections -fdata-sections -Wall -Wextra -MMD -MP
HAL_MODULES := hal hal_cortex hal_gpio hal_rcc hal_rcc_ex hal_flash hal_flash_ex hal_uart hal_uart_ex
LOCAL := main platform esp http image
OBJECTS := $(addprefix build/,$(addsuffix .o,$(LOCAL))) \
           $(addprefix build/stm32h7xx_,$(addsuffix .o,$(HAL_MODULES))) \
           build/system_stm32h7xx.o build/startup.o
vpath %.c $(HAL)/Src $(BOARD_ROOT)/Core/Src

.PHONY: all clean check
all: build/bootloader.hex build/bootloader.bin

build:
	mkdir -p $@

build/wifi_config.h: wifi_config.py $(wildcard wifi-password) | build
	$(PYTHON) wifi_config.py $@

build/esp.o: build/wifi_config.h

build/%.o: %.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/startup.o: $(BOARD_ROOT)/Core/Startup/startup_stm32h753bitx.s | build
	$(CC) $(CPPFLAGS) $(CPU) -c $< -o $@

build/bootloader.elf: $(OBJECTS) boot.ld
	$(CC) $(CPU) -Tboot.ld --specs=nano.specs --specs=nosys.specs \
		-Wl,--gc-sections,-Map=build/bootloader.map $(OBJECTS) -o $@
	$(SIZE) $@

build/bootloader.hex: build/bootloader.elf
	$(OBJCOPY) -O ihex $< $@

build/bootloader.bin: build/bootloader.elf
	$(OBJCOPY) -O binary $< $@

check: build/wifi_config.h | build
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -I. image.c http.c tests/test_upload.c -o build/test_upload
	./build/test_upload
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -I. -Ibuild esp.c tests/test_esp.c -o build/test_esp
	./build/test_esp
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -I. tests/test_boot_mode.c -o build/test_boot_mode
	./build/test_boot_mode

HOSTCC ?= cc
clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
