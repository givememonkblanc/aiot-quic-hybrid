#define _GNU_SOURCE
#include "netlog.h"
#include <stdio.h>
#include <string.h>
#include <sys/reent.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_link.h"
#include "lwip/sockets.h"

#define NETLOG_RING (64U * 1024U)
#define NETLOG_PACKET 1400U

static FILE *console;
static uint8_t *ring;
static size_t head, tail;           /* bytes in [tail, head) modulo the ring are waiting */
static uint32_t dropped;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static struct sockaddr_in peer;

static size_t used(void) { return (head - tail + NETLOG_RING) % NETLOG_RING; }

/* The socket layer exists only after the network stack has started; a socket created
 * before that stops the device. The station must also have an address. */
static bool network_up(void)
{
    wifi_link_status_t st;
    wifi_link_get_status(&st);
    return st.ready && st.ipv4 != 0;
}

static ssize_t tee_write(void *cookie, const char *buf, size_t n)
{
    (void)cookie;
    fwrite(buf, 1, n, console);
    fflush(console);
    portENTER_CRITICAL(&lock);
    if (used() + n >= NETLOG_RING) {
        dropped += n;   /* the newest output gives way; the count is reported */
    } else {
        for (size_t i = 0; i < n; i++) ring[(head + i) % NETLOG_RING] = (uint8_t)buf[i];
        head = (head + n) % NETLOG_RING;
    }
    portEXIT_CRITICAL(&lock);
    return (ssize_t)n;
}

static void sender(void *unused)
{
    (void)unused;
    static char packet[NETLOG_PACKET + 64];
    uint32_t boot = esp_random(), seq = 0, reported = 0;
    int sock = -1;
    while (!network_up()) vTaskDelay(pdMS_TO_TICKS(200));
    for (;;) {
        if (sock < 0) sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        size_t take = 0, available;
        portENTER_CRITICAL(&lock);
        available = used();
        /* whole lines only, at most one packet */
        size_t last_newline = 0;
        for (size_t i = 0; i < available && i < NETLOG_PACKET; i++)
            if (ring[(tail + i) % NETLOG_RING] == '\n') last_newline = i + 1;
        take = last_newline ? last_newline : (available >= NETLOG_PACKET ? NETLOG_PACKET : 0);
        int h = snprintf(packet, 64, "NL1 %08lx %lu\n", (unsigned long)boot, (unsigned long)seq);
        for (size_t i = 0; i < take; i++) packet[h + i] = (char)ring[(tail + i) % NETLOG_RING];
        uint32_t lost = dropped;
        portEXIT_CRITICAL(&lock);
        if (!take) {
            if (lost != reported && sock >= 0) {
                int m = snprintf(packet, sizeof(packet), "NL1 %08lx %lu\n[NETLOG_DROPPED] {\"bytes\":%lu}\n",
                                 (unsigned long)boot, (unsigned long)seq, (unsigned long)lost);
                if (sendto(sock, packet, m, 0, (struct sockaddr *)&peer, sizeof(peer)) == m) { reported = lost; seq++; }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (sock >= 0 && sendto(sock, packet, h + take, 0, (struct sockaddr *)&peer, sizeof(peer)) == (int)(h + take)) {
            portENTER_CRITICAL(&lock);
            tail = (tail + take) % NETLOG_RING;
            portEXIT_CRITICAL(&lock);
            seq++;
            if (used() < NETLOG_PACKET) vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));   /* no network yet: the lines wait */
        }
    }
}

bool netlog_start(const char *host, uint16_t port)
{
    if (!host || !host[0]) return false;
    ring = heap_caps_malloc(NETLOG_RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring) return false;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (inet_aton(host, &peer.sin_addr) == 0) return false;
    console = stdout;
    cookie_io_functions_t io = {.write = tee_write};
    FILE *tee = fopencookie(NULL, "w", io);
    if (!tee) return false;
    setvbuf(tee, NULL, _IOLBF, 256);
    _GLOBAL_REENT->_stdout = tee;
    stdout = tee;
    if (xTaskCreatePinnedToCore(sender, "netlog", 4096, NULL, 1, NULL, 0) != pdPASS) return false;
    printf("[NETLOG] {\"host\":\"%s\",\"port\":%u}\n", host, (unsigned)port);
    return true;
}
