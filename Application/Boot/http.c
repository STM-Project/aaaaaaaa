#include "http.h"
#include "boot_lcd.h"
#include "debug_uart.h"
#include "esp.h"
#include "image.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define REQUEST_TIMEOUT_MS 15000u
#define UPLOAD_CHUNK_BYTES 16384u

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
    "let offset=0;while(offset<f.size){"
    "const end=Math.min(f.size,offset+16384);"
    "const url='/uploader.php?crc='+crc+'&size='+f.size+'&offset='+offset;"
    "let reply;for(let attempt=0;attempt<4;attempt++){"
    "try{const headers={'Content-Type':'application/octet-stream','X-H753-Update-Key':key.value};"
    "const r=await fetch(url,{method:'POST',headers,body:f.slice(offset,end),"
    "signal:AbortSignal.timeout(45000)});"
    "reply={ok:r.ok,status:r.status,text:await r.text()};"
    "if(r.status!==408)break;"
    "}catch(e){reply=undefined;}"
    "status.textContent='Ponawiam porcje od '+offset+' B.';"
    "await new Promise(r=>setTimeout(r,750));}"
    "if(!reply)throw new Error('Utracono polaczenie przy '+offset+' B.');"
    "if(!reply.ok)throw new Error(reply.text);"
    "if(reply.text.trim()!==String(end))throw new Error('Brak potwierdzenia zapisu STM32.');"
    "offset=end;progress.value=Math.floor(offset*100/f.size);"
    "status.textContent='Potwierdzono '+offset+' / '+f.size+' B.';}"
    "status.textContent='Firmware zapisany. Uruchamiam aplikacje.';"
    "}catch(e){status.textContent=e.message;}"
    "finally{send.disabled=false;}};"
    "</script></html>";

struct request {
    bool upload;
    uint32_t length;
    uint32_t crc, image_size, image_offset;
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

static bool parse_upload_target(char *target, struct request *request)
{
    static const char upload_route[] = "/uploader.php?crc=";
    if (strncmp(target, upload_route, sizeof upload_route - 1))
        return false;
    char *crc = target + sizeof upload_route - 1;
    char *size = strchr(crc, '&');
    if (!size)
        return parse_crc(crc, &request->crc);
    if (strncmp(size, "&size=", 6))
        return false;
    *size = 0;
    size += 6;
    char *offset = strchr(size, '&');
    if (!offset || strncmp(offset, "&offset=", 8))
        return false;
    *offset = 0;
    return parse_crc(crc, &request->crc) &&
           parse_length(size, &request->image_size) &&
           request->image_size >= FLASH_WORD_BYTES &&
           parse_length(offset + 8, &request->image_offset) &&
           request->image_offset < request->image_size;
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

    if (strcmp(head, "POST") || !parse_upload_target(target, request))
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
    request->upload = has_length && has_type &&
        (request->image_size ? request->length &&
                               request->length <= UPLOAD_CHUNK_BYTES &&
                               request->length <= request->image_size - request->image_offset
                             : request->length >= FLASH_WORD_BYTES);
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

/* Buffer a complete request before programming Flash. This lets a client
 * repeat the same offset after the connection closes during receive. */
static enum http_result serve_chunk(unsigned link, const struct request *request,
                                    const uint8_t *initial, size_t received)
{
    static uint8_t chunk[UPLOAD_CHUNK_BYTES];
    static struct image_writer transfer;
    static uint32_t previous_offset, previous_length;
    static bool active;

    if (received > request->length) {
        respond(link, "400 Bad Request", "text/plain", "Niepoprawna dlugosc porcji.\n");
        return HTTP_OTHER;
    }
    memcpy(chunk, initial, received);
    while (received < request->length) {
        size_t wanted = request->length - received;
        if (wanted > 1024u)
            wanted = 1024u;
        int count = next_data(link, chunk + received, wanted);
        if (count < 0) {
            boot_debug("HTTP: przerwano odbior porcji. Mozna ponowic.");
            respond(link, "408 Request Timeout", "text/plain", "Ponow przesylanie porcji.\n");
            return HTTP_OTHER;
        }
        received += (size_t)count;
    }

    bool same_image = active && request->image_size == transfer.length &&
                      request->crc == transfer.expected_crc;
    bool repeated = same_image && transfer.received != 0 &&
                    request->image_offset == previous_offset &&
                    request->length == previous_length;
    if (!repeated) {
        if (request->image_offset == 0) {
            active = image_begin(&transfer, request->image_size, request->crc);
            boot_lcd_status("ZAPIS OPROGRAMOWANIA", 0);
            boot_debug("HTTP: rozpoczecie zapisu aplikacji.");
        } else if (!same_image || request->image_offset != transfer.received) {
            respond(link, "409 Conflict", "text/plain", "Niezgodny offset. Uruchom wgrywanie od poczatku.\n");
            return HTTP_OTHER;
        }
        if (!active || !image_write(&transfer, chunk, received))
            goto failed;
        previous_offset = request->image_offset;
        previous_length = request->length;
    }

    bool complete = transfer.received == transfer.length;
    if (complete) {
        boot_lcd_status("WERYFIKACJA", 99);
        if (!image_finish(&transfer))
            goto failed;
        active = false;
        boot_lcd_status("ZAPISANO START H7", 100);
    } else {
        boot_lcd_status("ZAPIS OPROGRAMOWANIA", transfer.received * 100u / transfer.length);
    }
    char ack[16];
    snprintf(ack, sizeof ack, "%u\n", (unsigned)transfer.received);
    respond(link, "200 OK", "text/plain", ack);
    return complete ? HTTP_INSTALLED : HTTP_OTHER;

failed:
    active = false;
    boot_debug("HTTP: blad zapisu lub weryfikacji obrazu.");
    boot_lcd_status("BLAD AKTUALIZACJI", 0);
    respond(link, "422 Unprocessable Content", "text/plain",
            "Wgrywanie nieudane. Sprawdz plik H753 i sume CRC.\n");
    return HTTP_OTHER;
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
        if (request.image_size) {
            result = serve_chunk(link, &request, bytes + offset, (size_t)count - offset);
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
