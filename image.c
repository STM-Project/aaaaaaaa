#include "image.h"

#include <string.h>

bool image_vectors_valid(uint32_t msp, uint32_t reset, uint32_t length)
{
    /* The application linker locates the reset stack in AXI SRAM. */
    const uint32_t target = reset & ~UINT32_C(1);
    return msp > 0x24000000u && msp <= 0x24080000u && (msp & 7u) == 0 &&
           length >= FLASH_WORD_BYTES && length <= APP_FLASH_CAPACITY &&
           (reset & 1u) != 0 && target >= APP_FLASH_START + FLASH_WORD_BYTES &&
           target < APP_FLASH_START + length;
}

static uint32_t crc_byte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
    return crc;
}

bool image_begin(struct image_writer *writer, uint32_t length, uint32_t expected_crc)
{
    *writer = (struct image_writer){.length = length, .crc = UINT32_MAX,
                                    .expected_crc = expected_crc};
    writer->failed = length < FLASH_WORD_BYTES || length > APP_FLASH_CAPACITY;
    return !writer->failed;
}

bool image_write(struct image_writer *writer, const uint8_t *data, size_t length)
{
    if (writer->failed || length > writer->length - writer->received) {
        writer->failed = true;
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        uint32_t offset = writer->received++;
        unsigned slot = offset % FLASH_WORD_BYTES;
        writer->crc = crc_byte(writer->crc, data[i]);
        if (offset < FLASH_WORD_BYTES) {
            writer->vector[slot] = data[i];
            if (writer->received != FLASH_WORD_BYTES)
                continue;
            uint32_t msp, reset;
            memcpy(&msp, writer->vector, sizeof msp);
            memcpy(&reset, writer->vector + sizeof msp, sizeof reset);
            /* Invalidate the old image before touching any subsequent sector. */
            if (!image_vectors_valid(msp, reset, writer->length) ||
                !flash_erase_sector(APP_FLASH_START))
                goto fail;
        } else {
            if (offset % FLASH_SECTOR_BYTES == 0 &&
                !flash_erase_sector(APP_FLASH_START + offset))
                goto fail;
            writer->word[slot] = data[i];
            if (slot == FLASH_WORD_BYTES - 1 &&
                !flash_write_word(APP_FLASH_START + offset - slot, writer->word))
                goto fail;
        }
    }
    return true;

fail:
    writer->failed = true;
    return false;
}

bool image_finish(struct image_writer *writer)
{
    if (writer->failed || writer->received != writer->length ||
        (writer->crc ^ UINT32_MAX) != writer->expected_crc) {
        writer->failed = true;
        return false;
    }
    unsigned tail = writer->length % FLASH_WORD_BYTES;
    if (tail != 0) {
        memset(writer->word + tail, 0xff, FLASH_WORD_BYTES - tail);
        if (!flash_write_word(APP_FLASH_START + writer->length - tail, writer->word)) {
            writer->failed = true;
            return false;
        }
    }
    /* Publish the initial stack and reset vectors only after all checks pass. */
    writer->failed = true;
    if (flash_write_word(APP_FLASH_START, writer->vector))
        return true;
    (void)flash_erase_sector(APP_FLASH_START);
    return false;
}
