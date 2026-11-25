#include "esp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *reply;
static size_t reply_size, read_index;
static unsigned mode, sends, baud, powers, tick, station, dhcp, static_ip, joined, port;

static void queue(const char *data, size_t length)
{
    reply = data;
    reply_size = length;
    read_index = 0;
}

uint32_t boot_millis(void) { return tick; }
void boot_delay(uint32_t milliseconds) { tick += milliseconds; }
void boot_esp_power(bool enabled) { (void)enabled; ++powers; }
bool boot_uart_init(uint32_t new_baud) { baud = new_baud; return true; }

int boot_uart_read(uint32_t deadline)
{
    if (read_index == reply_size) {
        tick = deadline;
        return -1;
    }
    return (uint8_t)reply[read_index++];
}

bool boot_uart_send(const void *data, size_t size)
{
    #define SENT(text) (size == sizeof(text) - 1 && !memcmp(data, text, size))
    station += SENT("AT+CWMODE=1\r\n");
    dhcp += SENT("AT+CWDHCP=1,1\r\n");
    static_ip += SENT("AT+CIPSTA=\"192.168.2.5\",\"192.168.2.1\",\"255.255.255.0\"\r\n");
    joined += SENT("AT+CWJAP=\"My\\,Network\",\"p\\\"ass\\\\word\"\r\n");
    port += SENT("AT+CIPSERVER=1,8180\r\n");
    #undef SENT
    ++sends;
    static const char lengths[] = "AT+CIPRECVLEN?\r\n";
    static const char packet[] = "AT+CIPRECVDATA=2,4\r\n";
    static const char send[] = "AT+CIPSEND=2,3\r\n";
    if (size == sizeof lengths - 1 && !memcmp(data, lengths, size)) {
        static const char list[] = "\r\n+IPD,2,4\r\n+CIPRECVLEN:0,0,4,0,0\r\n\r\nOK\r\n";
        queue(list, sizeof list - 1);
    } else if (size == sizeof packet - 1 && !memcmp(data, packet, size)) {
        static const char modern[] = "\r\n+CIPRECVDATA:4,\0\r\n>\r\nOK\r\n";
        static const char legacy[] = "\r\n+CIPRECVDATA,4:\0\r\n>\r\nOK\r\n";
        static const char bad[] = "\r\n+CIPRECVDATA:5,abcde\r\nOK\r\n";
        if (mode == 2)
            queue(bad, sizeof bad - 1);
        else
            queue(mode ? legacy : modern, sizeof modern - 1);
    } else if (size == sizeof send - 1 && !memcmp(data, send, size)) {
        queue("\r\nOK\r\n>", 7);
    } else if (size == 3 && !memcmp(data, "abc", 3)) {
        queue("\r\nSEND OK\r\n", 11);
    } else {
        assert(size >= 4 && !memcmp(data, "AT", 2));
        queue("\r\nOK\r\n", 6);
    }
    return true;
}

int main(void)
{
    assert(esp_start(NULL) && baud == 115200 && powers == 2 && sends >= 10);
    assert(esp_pending() == 2);
    uint8_t data[4];
    assert(esp_read(2, data, sizeof data) == 4 && !memcmp(data, "\0\r\n>", 4));
    mode = 1;
    assert(esp_read(2, data, sizeof data) == 4 && !memcmp(data, "\0\r\n>", 4));
    mode = 2;
    assert(esp_read(2, data, sizeof data) == -1);
    assert(esp_send(2, "abc", 3));
    esp_close(2);
    H753_BootRequest request = {.port=8180,.dhcp=1,.ssid="My,Network",
                                .password="p\"ass\\word", .ip=0xc0a80205,
                                .gateway=0xc0a80201,.mask=0xffffff00};
    assert(esp_start(&request) && station==1 && dhcp==1 && joined==1 && port==1);
    request.dhcp=0;
    assert(esp_start(&request) && station==2 && dhcp==1 && static_ip==1 && joined==2 && port==2);
    puts("PASS: ESP-AT passive receive, both binary response layouts and SEND OK");
    return 0;
}
