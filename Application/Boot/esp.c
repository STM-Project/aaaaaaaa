#include "esp.h"

#include <stdio.h>
#include <string.h>

#define LINK_COUNT 5u
#define AT_TIMEOUT 1500u
static unsigned pending[LINK_COUNT];

static bool send_command(const char *command)
{
    return boot_uart_send(command, strlen(command));
}

static bool read_line(char *line, size_t capacity, uint32_t deadline)
{
    size_t used = 0;
    for (;;) {
        int byte = boot_uart_read(deadline);
        if (byte < 0)
            return false;
        if (byte == '\n') {
            if (used && line[used - 1] == '\r')
                --used;
            line[used] = 0;
            if (used)
                return true;
        } else if (used + 1 < capacity) {
            line[used++] = (char)byte;
        }
    }
}

static bool wait_line(const char *expected, uint32_t timeout)
{
    uint32_t deadline = boot_millis() + timeout;
    char line[96];
    while (read_line(line, sizeof line, deadline)) {
        if (strcmp(line, expected) == 0)
            return true;
        if (strcmp(line, "ERROR") == 0 || strcmp(line, "FAIL") == 0 ||
            strcmp(line, "SEND FAIL") == 0)
            break;
    }
    return false;
}

static bool command(const char *text)
{
    return send_command(text) && wait_line("OK", AT_TIMEOUT);
}

static void drain(uint32_t duration)
{
    uint32_t deadline = boot_millis() + duration;
    while (boot_uart_read(deadline) >= 0) {}
}

static bool quote_at(char *escaped, size_t capacity, const char *text)
{
    while (*text) {
        unsigned char byte = (unsigned char)*text++;
        if (byte < 32 || byte == 127 || capacity < 3)
            return false;
        if (byte == ',' || byte == '"' || byte == '\\') {
            *escaped++ = '\\';
            --capacity;
        }
        *escaped++ = (char)byte;
        --capacity;
    }
    *escaped = 0;
    return true;
}

static void format_ip(char out[16], uint32_t ip)
{
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24),
             (unsigned)((ip >> 16) & 255u), (unsigned)((ip >> 8) & 255u),
             (unsigned)(ip & 255u));
}

static bool join_wifi(const H753_BootRequest *request)
{
    char ssid[129], password[129], cmd[288];
    if (!quote_at(ssid, sizeof ssid, request->ssid) ||
        !quote_at(password, sizeof password, request->password) ||
        !command("AT+CWMODE=1\r\n") ||
        !command("AT+CWHOSTNAME=\"Elektronika_STM\"\r\n"))
        return false;
    if (request->dhcp) {
        if (!command("AT+CWDHCP=1,1\r\n"))
            return false;
    } else {
        char ip[16], gateway[16], mask[16];
        format_ip(ip, request->ip);
        format_ip(gateway, request->gateway);
        format_ip(mask, request->mask);
        snprintf(cmd, sizeof cmd, "AT+CIPSTA=\"%s\",\"%s\",\"%s\"\r\n", ip, gateway, mask);
        if (!command("AT+CWDHCP=0,1\r\n") || !command(cmd))
            return false;
    }
    snprintf(cmd, sizeof cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, password);
    return send_command(cmd) && wait_line("OK", 22000u);
}

bool esp_start(const H753_BootRequest *request, const char *recovery_password)
{
    boot_esp_power(false);
    boot_delay(100);
    boot_esp_power(true);
    if (!boot_uart_init(115200))
        return false;
    drain(900);
    if (!command("AT\r\n")) {
        if (!boot_uart_init(2000000) || !command("AT\r\n"))
            return false;
        if (!send_command("AT+UART_CUR=115200,8,1,0,0\r\n"))
            return false;
        drain(100);
        if (!boot_uart_init(115200) || !command("AT\r\n"))
            return false;
    }
    if (!command("ATE0\r\n") || !command("AT+SYSSTORE=0\r\n"))
        return false;
    if (request) {
        if (!join_wifi(request))
            return false;
    } else {
        char access_point[96];
        snprintf(access_point, sizeof access_point,
                 "AT+CWSAP=\"STM32H753-Boot\",\"%s\",5,3,1\r\n", recovery_password);
        if (!command("AT+CWMODE=2\r\n") || !command(access_point) ||
            !command("AT+CIPAP=\"192.168.4.1\",\"192.168.4.1\",\"255.255.255.0\"\r\n"))
            return false;
    }
    char server[32];
    snprintf(server, sizeof server, "AT+CIPSERVER=1,%u\r\n", request ? request->port : 80u);
    return command("AT+CIPMUX=1\r\n") &&
           command("AT+CIPDINFO=0\r\n") &&
           command("AT+CIPRECVMODE=1\r\n") &&
           command("AT+CIPSERVERMAXCONN=1\r\n") &&
           command(server);
}

static bool parse_lengths(const char *line)
{
    static const char prefix[] = "+CIPRECVLEN:";
    if (strncmp(line, prefix, sizeof prefix - 1))
        return false;
    line += sizeof prefix - 1;
    for (unsigned link = 0; link < LINK_COUNT; ++link) {
        unsigned length = 0;
        if (*line < '0' || *line > '9')
            return false;
        do {
            length = length * 10u + (unsigned)(*line++ - '0');
            if (length > 1000000u)
                return false;
        } while (*line >= '0' && *line <= '9');
        pending[link] = length;
        if (*line != (link + 1 < LINK_COUNT ? ',' : '\0'))
            return false;
        ++line;
    }
    return true;
}

static bool receive_lengths(void)
{
    if (!send_command("AT+CIPRECVLEN?\r\n"))
        return false;
    uint32_t deadline = boot_millis() + AT_TIMEOUT;
    char line[96];
    bool got_lengths = false;
    while (read_line(line, sizeof line, deadline)) {
        if (parse_lengths(line))
            got_lengths = true;
        else if (strcmp(line, "OK") == 0)
            return got_lengths;
        else if (strcmp(line, "ERROR") == 0)
            break;
    }
    return false;
}

/* -1 means idle, -2 means the AT command failed. */
int esp_pending(void)
{
    if (!receive_lengths())
        return -2;
    for (unsigned link = 0; link < LINK_COUNT; ++link)
        if (pending[link])
            return (int)link;
    return -1;
}

int esp_read(unsigned link, uint8_t *data, size_t capacity)
{
    if (link >= LINK_COUNT || !capacity || !receive_lengths())
        return -1;
    if (!pending[link])
        return 0;
    size_t requested = pending[link] < capacity ? pending[link] : capacity;
    char cmd[48];
    snprintf(cmd, sizeof cmd, "AT+CIPRECVDATA=%u,%u\r\n", link, (unsigned)requested);
    if (!send_command(cmd))
        return -1;

    /* ESP-AT revisions use either +CIPRECVDATA:<n>, or +CIPRECVDATA,<n>:. */
    static const char prefix[] = "+CIPRECVDATA";
    size_t matched = 0;
    uint32_t deadline = boot_millis() + AT_TIMEOUT;
    int byte;
    while (matched < sizeof prefix - 1) {
        byte = boot_uart_read(deadline);
        if (byte < 0)
            return -1;
        if (byte == prefix[matched])
            ++matched;
        else
            matched = byte == prefix[0] ? 1u : 0u;
    }
    byte = boot_uart_read(deadline);
    if (byte != ':' && byte != ',')
        return -1;
    int separator = byte == ':' ? ',' : ':';
    size_t count = 0;
    bool digits = false;
    while ((byte = boot_uart_read(deadline)) >= '0' && byte <= '9') {
        digits = true;
        count = count * 10u + (unsigned)(byte - '0');
        if (count > requested)
            return -1;
    }
    if (!digits || byte != separator)
        return -1;
    for (size_t i = 0; i < count; ++i) {
        byte = boot_uart_read(deadline);
        if (byte < 0)
            return -1;
        data[i] = (uint8_t)byte;
    }
    return wait_line("OK", AT_TIMEOUT) ? (int)count : -1;
}

bool esp_send(unsigned link, const void *data, size_t length)
{
    char cmd[48];
    snprintf(cmd, sizeof cmd, "AT+CIPSEND=%u,%u\r\n", link, (unsigned)length);
    if (!send_command(cmd))
        return false;
    uint32_t deadline = boot_millis() + AT_TIMEOUT;
    int byte;
    do {
        byte = boot_uart_read(deadline);
    } while (byte >= 0 && byte != '>');
    return byte == '>' && boot_uart_send(data, length) && wait_line("SEND OK", 3000);
}

void esp_close(unsigned link)
{
    char cmd[32];
    snprintf(cmd, sizeof cmd, "AT+CIPCLOSE=%u\r\n", link);
    (void)command(cmd);
}
