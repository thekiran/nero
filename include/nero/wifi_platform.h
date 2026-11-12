#ifndef NERO_WIFI_PLATFORM_H
#define NERO_WIFI_PLATFORM_H

/* Platform Wi-Fi discovery (CoreWLAN on macOS).
 *
 * CoreWLAN scanning is *active*: it asks nearby APs to identify themselves. It
 * never joins a network and is separate from passive monitor-mode capture.
 * macOS may redact SSID/BSSID unless the calling application has Location
 * Services permission; redacted values are reported as unknown, never invented. */

#include "nero/wifi.h"

typedef struct {
    int corewlan_available;
    int active_scan_available;
    int identifiers_redacted;
    char backend[64];
} nero_wifi_platform_capabilities;

typedef struct {
    char os_name[32];
    char os_version[64];
    char model[64];
    char chip[64];
    char architecture[32];
    char interface[NERO_WIFI_IFNAME_MAX];
    char driver[128];
    int wifi_powered;
    int service_active;
    int has_channel;
    int channel;
    nero_wifi_band band;
    int has_rssi;
    int rssi;
    nero_wifi_platform_capabilities capabilities;
} nero_wifi_platform_status;

typedef struct {
    char name[NERO_WIFI_IFNAME_MAX];
    char hardware_address[32];
    int powered;
    int service_active;
    int has_rssi;
    int rssi;
    int has_channel;
    int channel;
    nero_wifi_band band;
    int mode;
} nero_wifi_interface_info;

int nero_wifi_platform_get_status(nero_wifi_platform_status *status, char *error, size_t error_len);
int nero_wifi_platform_list_interfaces(nero_wifi_interface_info *out, int capacity, int *count,
                                       char *error, size_t error_len);
/* Active CoreWLAN scan. Each result has source fields populated; bssid may be
 * unavailable. Returns 0 on success, -1 on failure. */
int nero_wifi_platform_scan(const char *interface_name, nero_wifi_ap *out, int capacity, int *count,
                            nero_wifi_platform_capabilities *capabilities, char *error,
                            size_t error_len);

#endif
