#ifndef BOOT_IMAGE_H
#define BOOT_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_FLASH_START UINT32_C(0x08000000)
#define APP_FLASH_START UINT32_C(0x08020000)
#define APP_FLASH_END UINT32_C(0x08200000)
#define APP_FLASH_CAPACITY (APP_FLASH_END - APP_FLASH_START)
#define FLASH_SECTOR_BYTES UINT32_C(0x20000)
#define FLASH_WORD_BYTES 32u

struct image_writer {
    uint32_t length;
    uint32_t received;
    uint32_t crc;
    uint32_t expected_crc;
    bool failed;
    _Alignas(32) uint8_t vector[FLASH_WORD_BYTES];
    _Alignas(32) uint8_t word[FLASH_WORD_BYTES];
};

bool image_vectors_valid(uint32_t msp, uint32_t reset, uint32_t length);
bool image_begin(struct image_writer *writer, uint32_t length, uint32_t expected_crc);
bool image_write(struct image_writer *writer, const uint8_t *data, size_t length);
bool image_finish(struct image_writer *writer);

bool flash_erase_sector(uint32_t address);
bool flash_write_word(uint32_t address, const uint8_t data[FLASH_WORD_BYTES]);

#endif
