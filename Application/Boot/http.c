#include "http.h"
#include "boot_lcd.h"
#include "esp.h"
#include "image.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define REQUEST_TIMEOUT_MS 15000u

static const char upload_page[] =
    "<!doctype html><html lang=pl><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Aktualizacja STM32H753</title>"
    "<h1>Aktualizacja STM32H753</h1>"
    "<p>Wybierz firmware.bin dla adresu 0x08020000.</p>"
    "<input type=file id=file accept='.bin'><button id=send>Wgraj i uruchom</button>"
    "<p><label>Klucz aktualizacji <input type=password id=key maxlength=32></label></p>"
    "<p><progress id=progress max=100 value=0></progress></p><p id=status role=status></p>"
    "<script>"
    "const file=document.getElementById('file'),send=document.getElementById('send'),"
    "status=document.getElementById('status'),key=document.getElementById('key');"
    "key.value=sessionStorage.getItem('h753-update-key')||'';"
    "sessionStorage.removeItem('h753-update-key');"
    "send.onclick=async()=>{"
    "if(!file.files.length){status.textContent='Wybierz plik BIN.';return;}"
    "const f=file.files[0];"
    "if(f.size<32||f.size>1966080){status.textContent='Niepoprawny rozmiar pliku.';return;}"
    "send.disabled=true;try{"
    "let c=-1;for(const b of new Uint8Array(await f.arrayBuffer())){"
    "c^=b;for(let k=0;k<8;k++)c=(c>>>1)^((c&1)?0xedb88320:0);}"
    "const crc=((~c)>>>0).toString(16).padStart(8,'0');"
    "const progress=document.getElementById('progress');progress.value=0;"
    "status.textContent='Przesylanie pliku.';"
    "const r=new XMLHttpRequest();r.open('POST','/uploader.php?crc='+crc);"
    "r.setRequestHeader('Content-Type','application/octet-stream');"
    "r.setRequestHeader('X-H753-Update-Key',key.value);"
    "r.upload.onprogress=e=>{if(e.lengthComputable)progress.value=Math.floor(e.loaded*100/e.total);};"
    "r.upload.onload=()=>{status.textContent='Zapis i weryfikacja w STM32.';};"
    "await new Promise((ok,fail)=>{r.onload=ok;r.onerror=fail;r.onabort=fail;r.send(f);});"
    "status.textContent=r.responseText;"
    "}catch(e){status.textContent='Utracono polaczenie. Sprawdz, czy aplikacja wystartowala.';}"
    "finally{send.disabled=false;}};"
    "</script></html>";

struct request {
    bool upload;
    uint32_t length;
    uint32_t crc;
    const char *candidate;
};

static bool parse_length(const char *text, uint32_t *length)
{
    uint32_t value = 0;
    if (!*text)
        return false;
    while (*text) {
        if (*text < '0' || *text > '9')
            return false;
        value = value * 10u + (unsigned)(*text++ - '0');
        if (value > APP_FLASH_CAPACITY)
            return false;
    }
    *length = value;
    return true;
}

static bool parse_crc(const char *text, uint32_t *crc)
{
    if (strlen(text) != 8)
        return false;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned char c = (unsigned char)text[i];
        unsigned value;
        if (c >= '0' && c <= '9')
            value = c - '0';
        else if ((c | 32u) >= 'a' && (c | 32u) <= 'f')
            value = (c | 32u) - 'a' + 10u;
        else
            return false;
        *crc = (*crc << 4) | value;
    }
    return true;
}

static bool parse_request(char *head, struct request *request)
{
    char *line = strstr(head, "\r\n");
    if (!line)
        return false;
    *line = 0;
    char *target = strchr(head, ' ');
    if (!target)
        return false;
    *target++ = 0;
    char *version = strchr(target, ' ');
    if (!version)
        return false;
    *version++ = 0;
    if (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))
        return false;
    if (strcmp(head, "GET") == 0)
        return strcmp(target, "/") == 0 || strcmp(target, "/upload") == 0 || strcmp(target, "/ulpad") == 0;

    static const char upload_route[] = "/uploader.php?crc=";
    if (strcmp(head, "POST") || strncmp(target, upload_route, sizeof upload_route - 1) ||
        !parse_crc(target + sizeof upload_route - 1, &request->crc))
        return false;

    bool has_length = false, has_type = false;
    for (line += 2; *line; ) {
        char *next = strstr(line, "\r\n");
        if (!next)
            return false;
        if (next == line)
            break;
        *next = 0;
        char *value = strchr(line, ':');
        if (!value || *line == ' ' || *line == '\t')
            return false;
        *value++ = 0;
        while (*value == ' ' || *value == '\t')
            ++value;
        char *end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = 0;
        if (strcasecmp(line, "Transfer-Encoding") == 0 ||
            strcasecmp(line, "Expect") == 0)
            return false;
        if (strcasecmp(line, "Content-Length") == 0) {
            if (has_length || !parse_length(value, &request->length))
                return false;
            has_length = true;
        } else if (strcasecmp(line, "Content-Type") == 0) {
            if (has_type || strcasecmp(value, "application/octet-stream") != 0)
                return false;
            has_type = true;
        } else if (strcasecmp(line, "X-H753-Update-Key") == 0) {
            if (request->candidate)
                return false;
            request->candidate = value;
        }
        line = next + 2;
    }
    request->upload = has_length && has_type && request->length >= FLASH_WORD_BYTES;
    return request->upload;
}

static void respond(unsigned link, const char *status, const char *type, const char *body)
{
    char header[192];
    size_t length = strlen(body);
    int size = snprintf(header, sizeof header,
                        "HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\n"
                        "Content-Length: %u\r\nCache-Control: no-store\r\nX-H753-Bootloader: 1\r\n"
                        "Connection: close\r\n\r\n", status, type, (unsigned)length);
    if (size <= 0 || (size_t)size >= sizeof header || !esp_send(link, header, (size_t)size))
        return;
    for (size_t offset = 0; offset < length;) {
        size_t remaining = length - offset;
        size_t count = remaining > 1024u ? 1024u : remaining;
        if (!esp_send(link, body + offset, count))
            return;
        offset += count;
    }
}

static int next_data(unsigned link, uint8_t *buffer, size_t capacity)
{
    uint32_t deadline = boot_millis() + REQUEST_TIMEOUT_MS;
    do {
        int count = esp_read(link, buffer, capacity);
        if (count != 0)
            return count;
        boot_delay(20);
    } while ((int32_t)(boot_millis() - deadline) < 0);
    return -1;
}

enum http_result http_serve(unsigned link, const char *update_key)
{
    uint8_t bytes[1024];
    char head[2049];
    size_t used = 0;
    struct request request = {0};
    struct image_writer image;
    enum http_result result = HTTP_OTHER;

    for (;;) {
        int count = next_data(link, bytes, sizeof bytes);
        if (count < 0)
            goto bad_request;
        size_t offset = 0;
        while (offset < (size_t)count) {
            if (used == sizeof head - 1 || bytes[offset] == 0)
                goto bad_request;
            head[used++] = (char)bytes[offset++];
            if (used >= 4 && memcmp(head + used - 4, "\r\n\r\n", 4) == 0)
                break;
        }
        if (used < 4 || memcmp(head + used - 4, "\r\n\r\n", 4) != 0)
            continue;

        head[used] = 0;
        if (!parse_request(head, &request))
            goto bad_request;
        if (!request.upload) {
            respond(link, "200 OK", "text/html", upload_page);
            result = HTTP_PAGE;
            break;
        }
        if (update_key && (!request.candidate ||
                           !H753_UpdateKeyMatches(update_key, request.candidate,
                                                 strlen(request.candidate)))) {
            respond(link, "403 Forbidden", "text/plain", "Niepoprawny klucz aktualizacji.\n");
            break;
        }
        boot_lcd_status("ZAPIS OPROGRAMOWANIA", 0);
        if (!image_begin(&image, request.length, request.crc) ||
            !image_write(&image, bytes + offset, (size_t)count - offset))
            goto bad_image;
        while (image.received < image.length) {
            boot_lcd_status("ZAPIS OPROGRAMOWANIA", image.received * 100u / image.length);
            size_t remaining = image.length - image.received;
            size_t wanted = remaining < sizeof bytes ? remaining : sizeof bytes;
            count = next_data(link, bytes, wanted);
            if (count < 0 || !image_write(&image, bytes, (size_t)count))
                goto bad_image;
        }
        boot_lcd_status("WERYFIKACJA", 99);
        if (!image_finish(&image))
            goto bad_image;
        boot_lcd_status("ZAPISANO START H7", 100);
        respond(link, "200 OK", "text/plain", "Firmware zapisany. Uruchamiam aplikacje.\n");
        result = HTTP_INSTALLED;
        break;
    }
    esp_close(link);
    return result;

bad_request:
    respond(link, "400 Bad Request", "text/plain", "Nieobslugiwane zadanie HTTP.\n");
    esp_close(link);
    return HTTP_OTHER;

bad_image:
    boot_lcd_status("BLAD AKTUALIZACJI", 0);
    respond(link, "422 Unprocessable Content", "text/plain",
            "Wgrywanie nieudane. Sprawdz plik H753, sume CRC i sprobuj ponownie.\n");
    esp_close(link);
    return HTTP_OTHER;
}
