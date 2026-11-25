#include "esp.h"
#include "http.h"
#include "image.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[APP_FLASH_CAPACITY];
static uint8_t firmware[0x0e0021];
static unsigned sectors, words, closes;
static uint32_t last_write, tick;
static bool fail_write;
static const uint8_t *request_data;
static size_t request_size;
static char response[2048];
static size_t response_size;

bool flash_erase_sector(uint32_t address)
{
    assert(address >= APP_FLASH_START && address <= APP_FLASH_END - FLASH_SECTOR_BYTES);
    assert((address - APP_FLASH_START) % FLASH_SECTOR_BYTES == 0);
    memset(flash + address - APP_FLASH_START, 0xff, FLASH_SECTOR_BYTES);
    ++sectors;
    return true;
}

bool flash_write_word(uint32_t address, const uint8_t data[FLASH_WORD_BYTES])
{
    assert(address >= APP_FLASH_START && address <= APP_FLASH_END - FLASH_WORD_BYTES);
    assert(address % FLASH_WORD_BYTES == 0 && (uintptr_t)data % FLASH_WORD_BYTES == 0);
    if (fail_write)
        return false;
    uint8_t *dest = flash + address - APP_FLASH_START;
    for (unsigned i = 0; i < FLASH_WORD_BYTES; ++i)
        assert(dest[i] == 0xff);
    memcpy(dest, data, FLASH_WORD_BYTES);
    last_write = address;
    ++words;
    return true;
}

uint32_t boot_millis(void) { return tick; }
void boot_delay(uint32_t milliseconds) { tick += milliseconds; }

int esp_read(unsigned link, uint8_t *data, size_t capacity)
{
    assert(link == 2);
    if (capacity > 19)
        capacity = 19;
    if (capacity > request_size)
        capacity = request_size;
    memcpy(data, request_data, capacity);
    request_data += capacity;
    request_size -= capacity;
    return (int)capacity;
}

bool esp_send(unsigned link, const void *data, size_t length)
{
    assert(link == 2 && response_size + length < sizeof response);
    memcpy(response + response_size, data, length);
    response_size += length;
    response[response_size] = 0;
    return true;
}
void esp_close(unsigned link) { assert(link == 2); ++closes; }

static enum http_result serve(const void *data, size_t length, const char *update_key)
{
    request_data = data;
    request_size = length;
    response_size = 0;
    return http_serve(2, update_key);
}

int main(void)
{
    /* Test data and CRCs cross-checked with Python zlib.crc32. */
    firmware[2] = 0x08;
    firmware[3] = 0x24;
    firmware[4] = 0x21;
    firmware[6] = 0x02;
    firmware[7] = 0x08;
    for (size_t i = FLASH_WORD_BYTES; i < sizeof firmware; ++i)
        firmware[i] = (uint8_t)(i * 7u);
    struct image_writer writer;
    assert(image_begin(&writer, sizeof firmware, UINT32_C(0x63fbb949)));
    assert(image_write(&writer, firmware, 17));
    assert(sectors == 0 && words == 0);
    for (size_t offset = 17; offset < sizeof firmware;) {
        size_t count = sizeof firmware - offset;
        if (count > 777)
            count = 777;
        assert(image_write(&writer, firmware + offset, count));
        offset += count;
    }
    assert(sectors == 8 && flash[0] == 0xff && flash[3] == 0xff);
    assert(image_finish(&writer) && last_write == APP_FLASH_START);
    assert(memcmp(firmware, flash, sizeof firmware) == 0);
    assert(flash[sizeof firmware] == 0xff);
    assert(!image_finish(&writer));

    assert(image_begin(&writer, 257, UINT32_C(0x6be7f33a)));
    assert(image_write(&writer, firmware, 40));
    assert(!image_finish(&writer) && flash[3] == 0xff);
    assert(!image_begin(&writer, APP_FLASH_CAPACITY + 1, 0));
    const unsigned before = sectors;
    assert(image_begin(&writer, 257, 0));
    const uint8_t bad_vector[32] = {0};
    assert(!image_write(&writer, bad_vector, sizeof bad_vector) && sectors == before);
    assert(image_begin(&writer, 257, 0));
    fail_write = true;
    assert(!image_write(&writer, firmware, 257) && !image_finish(&writer));
    fail_write = false;
    assert(flash[3] == 0xff);

    static const char get[] = "GET /upload HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n";
    assert(serve(get, sizeof get - 1, NULL) == HTTP_PAGE && strstr(response, "type=file"));
    static const char duplicate[] = "POST /uploader.php?crc=12345678 HTTP/1.1\r\n"
        "Content-Length: 257\r\nContent-Length: 258\r\n"
        "Content-Type: application/octet-stream\r\n\r\n";
    assert(serve(duplicate, sizeof duplicate - 1, NULL) == HTTP_OTHER && strstr(response, "400"));
    uint8_t post[600];
    int header = snprintf((char *)post, sizeof post,
                          "POST /uploader.php?crc=%08x HTTP/1.1\r\n"
                          "Content-Length: 257\r\nContent-Type: application/octet-stream\r\n\r\n",
                          (unsigned)UINT32_C(0x6be7f33a));
    assert(header > 0 && (size_t)header + 257 <= sizeof post);
    memcpy(post + header, firmware, 257);
    assert(serve(post, (size_t)header + 80, NULL) == HTTP_OTHER && flash[3] == 0xff);
    post[header + 100] ^= 1;
    assert(serve(post, (size_t)header + 257, NULL) == HTTP_OTHER && flash[3] == 0xff);
    post[header + 100] ^= 1;
    assert(serve(post, (size_t)header + 257, NULL) == HTTP_INSTALLED && closes == 5);
    assert(strstr(response, "Firmware zapisany") && !memcmp(firmware, flash, 257));
    static const char key[] = "0123456789abcdef0123456789abcdef";
    const unsigned erased_before = sectors;
    assert(serve(post, (size_t)header + 257, key) == HTTP_OTHER && strstr(response, "403"));
    assert(sectors == erased_before);
    header = snprintf((char *)post, sizeof post,
                      "POST /uploader.php?crc=%08x HTTP/1.1\r\n"
                      "Content-Length: 257\r\nContent-Type: application/octet-stream\r\n"
                      "X-H753-Update-Key: %s\r\n\r\n",
                      (unsigned)UINT32_C(0x6be7f33a), key);
    assert(header > 0 && (size_t)header + 257 <= sizeof post);
    memcpy(post + header, firmware, 257);
    assert(serve(post, (size_t)header + 257, key) == HTTP_INSTALLED);
    puts("PASS: fragmented HTTP, bank crossing, CRC, interrupted upload, flash failure and final vector commit");
    return 0;
}
