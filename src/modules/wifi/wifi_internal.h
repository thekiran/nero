#ifndef NERO_WIFI_INTERNAL_H
#define NERO_WIFI_INTERNAL_H

#include "nero/wifi.h"

typedef struct {
    int has_ssid;
    char ssid[NERO_WIFI_SSID_CAP];
    uint8_t ssid_len;
    int hidden;
    int has_ds_channel;
    int ds_channel;
    nero_wifi_security security;
} nero_wifi_ies;

/* Parse the information elements in a beacon/probe response/probe request.
 * Bodies shorter than the fixed management header are handled by the caller. */
void nero_wifi_parse_ies(nero_wifi_ies *out, const uint8_t *data, size_t len, int privacy);

/* Parse association/reassociation request capability fields. */
int nero_wifi_parse_assoc_request(const uint8_t *data, size_t len, nero_wifi_ies *out);

/* Internal band/channel normalization shared by parser and platform layer. */
nero_wifi_band nero_wifi_frequency_to_band(int frequency_mhz);
int nero_wifi_frequency_to_channel(int frequency_mhz, nero_wifi_band *band);

#endif
