#ifndef NERO_WIFI_CAPTURE_H
#define NERO_WIFI_CAPTURE_H

/* Read-only packet-capture backend contract.
 *
 * The macOS and Linux implementation dynamically loads libpcap so a build does
 * not require libpcap headers or link-time flags. Other platforms get a stub
 * that reports unsupported. Capture never injects frames and never changes
 * channel or regulatory settings through private APIs. */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define NERO_WIFI_IFNAME_MAX_CAPTURE 64

typedef struct nero_wifi_capture nero_wifi_capture;

typedef struct {
    const uint8_t *data;
    size_t caplen;
    size_t len;
    int64_t timestamp_us;
    int linktype;
} nero_wifi_capture_packet;

/* Return 0 to continue, -1 to abort capture. */
typedef int (*nero_wifi_capture_callback)(void *context, const nero_wifi_capture_packet *packet);

typedef struct {
    int backend_available;   /* libpcap could be loaded */
    int can_request_monitor; /* pcap_can_set_rfmon() result, if available */
    int monitor_requested;   /* caller asked for monitor mode */
    int monitor_set;         /* pcap_set_rfmon() accepted the request */
    int link_fallback;       /* monitor was unavailable or delivered nothing */
    int linktype;
    uint64_t packets;
    uint64_t bytes;
    uint64_t received;
    uint64_t dropped;
    char backend[64];
    char interface[NERO_WIFI_IFNAME_MAX_CAPTURE];
    char error[256];
} nero_wifi_capture_summary;

/* Probe without activating the interface. Safe in unprivileged processes. */
int nero_wifi_capture_probe(const char *interface_name, int *backend_available,
                            int *can_request_monitor, char *backend_name, size_t backend_name_len,
                            char *error, size_t error_len);

nero_wifi_capture *nero_wifi_capture_open(const char *interface_name, int request_monitor,
                                          char *error, size_t error_len);
/* Runs until the callback aborts, *stop becomes non-zero, or an error occurs.
 * Returns 0 for a clean stop, 1 when *stop was observed, -1 on error. */
int nero_wifi_capture_run(nero_wifi_capture *capture, nero_wifi_capture_callback callback,
                          void *context, _Atomic int *stop, int poll_ms,
                          nero_wifi_capture_summary *summary, char *error, size_t error_len);
void nero_wifi_capture_close(nero_wifi_capture *capture);

#endif
