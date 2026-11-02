#ifndef AIOT_NETLOG_H
#define AIOT_NETLOG_H
/* Copy of the console over UDP, for a device without a USB connection.
 *
 * Everything written to stdout, including ESP_LOG output, still goes to the console and is
 * also kept in a ring buffer in PSRAM. A low-priority task sends the buffer to the host in
 * datagrams of whole lines once the network is up, so the lines from boot onward arrive.
 * Each datagram starts with "NL1 <boot> <seq>\n"; the host detects loss from the sequence.
 * Output written with ets_printf bypasses stdout and is not copied. */
#include <stdbool.h>
#include <stdint.h>
/* Call first in app_main, before other tasks are created: tasks take stdout from the
 * global state at their creation. */
bool netlog_start(const char *host, uint16_t port);
#endif
