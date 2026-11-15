#ifndef NERO_WIFI_HELPER_PROTOCOL_H
#define NERO_WIFI_HELPER_PROTOCOL_H
/* Private, versioned JSON protocol. Included by the macOS client and offline
 * parser test; cJSON is already part of the production CLI. */
#include "cJSON.h"
#include <math.h>

static int helper_integer(const cJSON *object, const char *key, int low, int high, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < low ||
        v->valuedouble > high || v->valuedouble != (int)v->valuedouble) return -1;
    *out = (int)v->valuedouble;
    return 0;
}
static int helper_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static int helper_parse(const char *data, size_t length, nero_wifi_ap *out, int capacity,
                        int *count, nero_wifi_platform_capabilities *caps,
                        char *error, size_t error_len)
{
    *count = 0;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(data, length, &end, 1);
    const char *message = "invalid Wi-Fi helper response";
    int result = -1, version = 0;
    nero_wifi_ap *parsed = NULL;
    if (!root || !cJSON_IsObject(root) || cJSON_GetArraySize(root) != 3 ||
        helper_integer(root, "version", 1, 1, &version)) goto done;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "aps");
    if (!cJSON_IsString(status) || !cJSON_IsArray(rows)) goto done;
    if (strcmp(status->valuestring, "authorized") != 0) {
        if (cJSON_GetArraySize(rows) != 0) goto done;
        if (!strcmp(status->valuestring, "not_determined"))
            message = "Location permission not requested: open NEROWiFi.app and click Allow Wi-Fi Identifier Access";
        else if (!strcmp(status->valuestring, "denied"))
            message = "Location permission denied: enable NERO Wi-Fi in System Settings > Privacy & Security > Location Services";
        else if (!strcmp(status->valuestring, "restricted"))
            message = "Location permission restricted by system policy";
        else if (!strcmp(status->valuestring, "disabled"))
            message = "Location Services disabled: enable them in System Settings";
        else if (!strcmp(status->valuestring, "no_interface")) message = "Wi-Fi interface not found";
        else if (!strcmp(status->valuestring, "powered_off")) message = "Wi-Fi is powered off";
        else if (!strcmp(status->valuestring, "scan_failed")) message = "CoreWLAN scan failed";
        goto done;
    }
    int total = cJSON_GetArraySize(rows);
    if (total > NERO_WIFI_MAX_APS || capacity <= 0) goto done;
    parsed = calloc(NERO_WIFI_MAX_APS, sizeof(*parsed));
    if (!parsed) goto done;
    int redacted = 0;
    for (int i = 0; i < total; ++i) {
        const cJSON *row = cJSON_GetArrayItem(rows, i);
        nero_wifi_ap *ap = &parsed[i];
        int band = 0, security = 0;
        if (!cJSON_IsObject(row) || cJSON_GetArraySize(row) != 6 ||
            helper_integer(row, "channel", 0, 233, &ap->channel) ||
            helper_integer(row, "band", 0, 3, &band) ||
            helper_integer(row, "rssi", -127, 0, &ap->rssi) ||
            helper_integer(row, "security", 0, 7, &security)) goto done;
        const cJSON *bssid = cJSON_GetObjectItemCaseSensitive(row, "bssid");
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(row, "ssid_hex");
        if (cJSON_IsString(bssid)) {
            if (strlen(bssid->valuestring) != 17 || nero_wifi_parse_mac(bssid->valuestring, ap->bssid)) goto done;
            ap->has_bssid = 1;
        } else if (cJSON_IsNull(bssid)) ap->redacted = 1;
        else goto done;
        if (cJSON_IsString(ssid)) {
            size_t n = strlen(ssid->valuestring);
            if (n > 64 || n % 2) goto done;
            ap->ssid_len = (uint8_t)(n / 2);
            for (size_t j = 0; j < n; j += 2) {
                int a = helper_hex(ssid->valuestring[j]), b = helper_hex(ssid->valuestring[j + 1]);
                if (a < 0 || b < 0) goto done;
                int c = a * 16 + b;
                /* Untrusted RF names must never emit terminal control sequences. */
                ap->ssid[j / 2] = c >= 32 && c < 127 ? (char)c : '?';
            }
            ap->hidden = n == 0;
        } else if (cJSON_IsNull(ssid)) ap->redacted = 1;
        else goto done;
        ap->band = (nero_wifi_band)band;
        ap->security = (nero_wifi_security)security;
        ap->has_channel = ap->channel != 0;
        ap->has_rssi = ap->rssi != 0;
        ap->frequency_mhz = nero_wifi_channel_to_frequency(ap->channel, ap->band);
        ap->has_frequency = ap->frequency_mhz > 0;
        ap->first_seen_us = ap->last_seen_us = nero_wifi_now_us();
        redacted |= ap->redacted;
    }
    *count = total < capacity ? total : capacity;
    memcpy(out, parsed, (size_t)*count * sizeof(*out));
    if (caps) {
        memset(caps, 0, sizeof(*caps));
        caps->corewlan_available = caps->active_scan_available = 1;
        caps->identifiers_redacted = redacted;
        snprintf(caps->backend, sizeof(caps->backend), "CoreWLAN app (active scan)");
    }
    result = 0;
done:
    if (result && error && error_len) snprintf(error, error_len, "%s", message);
    free(parsed);
    cJSON_Delete(root);
    return result;
}

/* Parse the neighbor-cache response: {version:1, status:"ok", neighbors:[{ip,mac}]}.
 * Only local-link IPv4 neighbors the kernel already holds are reported. */
static int helper_parse_neighbors(const char *data, size_t length, nero_wifi_neighbor *out,
                                  int capacity, int *count, char *error, size_t error_len)
{
    *count = 0;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(data, length, &end, 1);
    const char *message = "invalid Wi-Fi helper neighbor response";
    int result = -1, version = 0, written = 0;
    if (!root || !cJSON_IsObject(root) || cJSON_GetArraySize(root) != 3 ||
        helper_integer(root, "version", 1, 1, &version)) goto done;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "neighbors");
    if (!cJSON_IsString(status) || !cJSON_IsArray(rows)) goto done;
    if (strcmp(status->valuestring, "ok") != 0) goto done;
    if (capacity <= 0) goto done;
    int total = cJSON_GetArraySize(rows);
    for (int i = 0; i < total && written < capacity; ++i) {
        const cJSON *row = cJSON_GetArrayItem(rows, i);
        const cJSON *ip = cJSON_GetObjectItemCaseSensitive(row, "ip");
        const cJSON *mac = cJSON_GetObjectItemCaseSensitive(row, "mac");
        if (!cJSON_IsObject(row) || cJSON_GetArraySize(row) != 2 ||
            !cJSON_IsString(ip) || !cJSON_IsString(mac)) goto done;
        uint8_t parsed_mac[NERO_WIFI_MAC_LEN];
        if (strlen(mac->valuestring) != 17 || nero_wifi_parse_mac(mac->valuestring, parsed_mac))
            goto done;
        if (strlen(ip->valuestring) >= sizeof(out[0].ip)) goto done;
        nero_wifi_neighbor *neighbor = &out[written++];
        memset(neighbor, 0, sizeof(*neighbor));
        memcpy(neighbor->mac, parsed_mac, NERO_WIFI_MAC_LEN);
        snprintf(neighbor->ip, sizeof(neighbor->ip), "%s", ip->valuestring);
    }
    *count = written;
    result = 0;
done:
    if (result && error && error_len) snprintf(error, error_len, "%s", message);
    cJSON_Delete(root);
    return result;
}
#endif
