/* Thread-safe access point and client tracker. */

#include "nero/wifi.h"
#include "wifi_internal.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIFI_DEDUP_SLOTS 512
#define WIFI_DEDUP_WINDOW_US 2000000LL

typedef struct {
    int used;
    uint8_t transmitter[NERO_WIFI_MAC_LEN];
    uint16_t sequence;
    uint8_t fragment;
    uint8_t type;
    uint8_t subtype;
    int64_t timestamp_us;
} wifi_dedup_entry;

struct nero_wifi_tracker {
    pthread_mutex_t mutex;
    nero_wifi_ap aps[NERO_WIFI_MAX_APS];
    int ap_count;
    nero_wifi_client clients[NERO_WIFI_MAX_CLIENTS];
    int client_count;
    wifi_dedup_entry dedup[WIFI_DEDUP_SLOTS];
    uint32_t dedup_cursor;
    nero_wifi_stats stats;
};

static int mac_equal(const uint8_t a[NERO_WIFI_MAC_LEN], const uint8_t b[NERO_WIFI_MAC_LEN])
{
    return memcmp(a, b, NERO_WIFI_MAC_LEN) == 0;
}

static int mac_zero(const uint8_t mac[NERO_WIFI_MAC_LEN])
{
    static const uint8_t zero[NERO_WIFI_MAC_LEN] = {0};
    return mac_equal(mac, zero);
}

int nero_wifi_mac_is_group(const uint8_t mac[NERO_WIFI_MAC_LEN])
{
    return (mac[0] & 0x01u) != 0;
}

int nero_wifi_mac_is_randomized(const uint8_t mac[NERO_WIFI_MAC_LEN])
{
    return (mac[0] & 0x02u) != 0;
}

static int security_rank(nero_wifi_security security)
{
    switch (security) {
    case NERO_WIFI_SEC_UNKNOWN:
        return 0;
    case NERO_WIFI_SEC_OPEN:
        return 1;
    case NERO_WIFI_SEC_WEP:
        return 2;
    case NERO_WIFI_SEC_WPA:
        return 3;
    case NERO_WIFI_SEC_WPA2:
        return 4;
    case NERO_WIFI_SEC_WPA2_WPA3:
        return 5;
    case NERO_WIFI_SEC_WPA3:
        return 6;
    case NERO_WIFI_SEC_OWE:
        return 6;
    }
    return 0;
}

const char *nero_wifi_security_name(nero_wifi_security security)
{
    switch (security) {
    case NERO_WIFI_SEC_OPEN:
        return "OPEN";
    case NERO_WIFI_SEC_WEP:
        return "WEP";
    case NERO_WIFI_SEC_WPA:
        return "WPA";
    case NERO_WIFI_SEC_WPA2:
        return "WPA2";
    case NERO_WIFI_SEC_WPA2_WPA3:
        return "WPA2/WPA3";
    case NERO_WIFI_SEC_WPA3:
        return "WPA3";
    case NERO_WIFI_SEC_OWE:
        return "OWE";
    case NERO_WIFI_SEC_UNKNOWN:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char *nero_wifi_band_name(nero_wifi_band band)
{
    switch (band) {
    case NERO_WIFI_BAND_2GHZ:
        return "2.4 GHz";
    case NERO_WIFI_BAND_5GHZ:
        return "5 GHz";
    case NERO_WIFI_BAND_6GHZ:
        return "6 GHz";
    case NERO_WIFI_BAND_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

const char *nero_wifi_confidence_name(nero_wifi_confidence confidence)
{
    switch (confidence) {
    case NERO_WIFI_CONFIDENCE_OBSERVED:
        return "OBSERVED";
    case NERO_WIFI_CONFIDENCE_LIKELY:
        return "LIKELY";
    case NERO_WIFI_CONFIDENCE_ASSOCIATED:
        return "ASSOC-REQ";
    }
    return "OBSERVED";
}

const char *nero_wifi_state_name(nero_wifi_client_state state)
{
    switch (state) {
    case NERO_WIFI_STATE_OBSERVED:
        return "OBSERVED";
    case NERO_WIFI_STATE_ASSOCIATED:
        return "ASSOCIATED";
    case NERO_WIFI_STATE_DISASSOCIATED:
        return "DISASSOCIATED";
    }
    return "OBSERVED";
}

void nero_wifi_format_mac(const uint8_t mac[NERO_WIFI_MAC_LEN], char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
        return;
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

int nero_wifi_parse_mac(const char *text, uint8_t out[NERO_WIFI_MAC_LEN])
{
    if (text == NULL || out == NULL)
        return -1;
    size_t length = strlen(text);
    char compact[13];
    size_t written = 0;
    for (size_t i = 0; i < length; ++i) {
        char ch = text[i];
        if (ch == ':' || ch == '-' || ch == '.')
            continue;
        if (written >= 12 || hex_value(ch) < 0)
            return -1;
        compact[written++] = ch;
    }
    if (written != 12)
        return -1;
    compact[12] = '\0';
    for (int i = 0; i < NERO_WIFI_MAC_LEN; ++i) {
        out[i] = (uint8_t)((hex_value(compact[i * 2]) << 4) | hex_value(compact[i * 2 + 1]));
    }
    return 0;
}

int64_t nero_wifi_now_us(void)
{
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC)
        return 0;
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

void nero_wifi_format_time(int64_t timestamp_us, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
        return;
    if (timestamp_us <= 0) {
        snprintf(out, out_len, "never");
        return;
    }
    time_t seconds = (time_t)(timestamp_us / 1000000LL);
    struct tm tm_value;
#if defined(_WIN32)
    struct tm *tmp = localtime(&seconds);
    if (tmp == NULL) {
        snprintf(out, out_len, "unknown");
        return;
    }
    tm_value = *tmp;
#else
    if (localtime_r(&seconds, &tm_value) == NULL) {
        snprintf(out, out_len, "unknown");
        return;
    }
#endif
    if (strftime(out, out_len, "%Y-%m-%d %H:%M:%S", &tm_value) == 0) {
        snprintf(out, out_len, "unknown");
    }
}

nero_wifi_tracker *nero_wifi_tracker_create(void)
{
    nero_wifi_tracker *tracker = calloc(1, sizeof(*tracker));
    if (tracker == NULL)
        return NULL;
    if (pthread_mutex_init(&tracker->mutex, NULL) != 0) {
        free(tracker);
        return NULL;
    }
    tracker->stats.started_us = nero_wifi_now_us();
    return tracker;
}

void nero_wifi_tracker_destroy(nero_wifi_tracker *tracker)
{
    if (tracker == NULL)
        return;
    pthread_mutex_destroy(&tracker->mutex);
    free(tracker);
}

void nero_wifi_tracker_reset(nero_wifi_tracker *tracker)
{
    if (tracker == NULL)
        return;
    pthread_mutex_lock(&tracker->mutex);
    memset(tracker->aps, 0, sizeof(tracker->aps));
    memset(tracker->clients, 0, sizeof(tracker->clients));
    memset(tracker->dedup, 0, sizeof(tracker->dedup));
    memset(&tracker->stats, 0, sizeof(tracker->stats));
    tracker->ap_count = 0;
    tracker->client_count = 0;
    tracker->dedup_cursor = 0;
    tracker->stats.started_us = nero_wifi_now_us();
    pthread_mutex_unlock(&tracker->mutex);
}

static nero_wifi_ap *find_ap_locked(nero_wifi_tracker *tracker,
                                    const uint8_t bssid[NERO_WIFI_MAC_LEN])
{
    for (int i = 0; i < tracker->ap_count; ++i) {
        if (mac_equal(tracker->aps[i].bssid, bssid))
            return &tracker->aps[i];
    }
    return NULL;
}

static nero_wifi_ap *ensure_ap_locked(nero_wifi_tracker *tracker,
                                      const uint8_t bssid[NERO_WIFI_MAC_LEN], int64_t now)
{
    if (mac_zero(bssid) || nero_wifi_mac_is_group(bssid))
        return NULL;
    nero_wifi_ap *ap = find_ap_locked(tracker, bssid);
    if (ap != NULL)
        return ap;
    if (tracker->ap_count >= NERO_WIFI_MAX_APS) {
        ++tracker->stats.dropped;
        return NULL;
    }
    ap = &tracker->aps[tracker->ap_count++];
    memset(ap, 0, sizeof(*ap));
    memcpy(ap->bssid, bssid, NERO_WIFI_MAC_LEN);
    ap->has_bssid = 1;
    ap->security = NERO_WIFI_SEC_UNKNOWN;
    ap->first_seen_us = now;
    ap->last_seen_us = now;
    return ap;
}

static nero_wifi_client *find_client_locked(nero_wifi_tracker *tracker,
                                            const uint8_t mac[NERO_WIFI_MAC_LEN])
{
    for (int i = 0; i < tracker->client_count; ++i) {
        if (mac_equal(tracker->clients[i].mac, mac))
            return &tracker->clients[i];
    }
    return NULL;
}

static nero_wifi_client *evict_client_locked(nero_wifi_tracker *tracker)
{
    int victim = -1;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < tracker->client_count; ++i) {
        nero_wifi_client *client = &tracker->clients[i];
        if (client->confidence == NERO_WIFI_CONFIDENCE_OBSERVED && !client->has_bssid &&
            client->last_seen_us < oldest) {
            victim = i;
            oldest = client->last_seen_us;
        }
    }
    if (victim < 0) {
        for (int i = 0; i < tracker->client_count; ++i) {
            if (tracker->clients[i].last_seen_us < oldest) {
                victim = i;
                oldest = tracker->clients[i].last_seen_us;
            }
        }
    }
    if (victim < 0)
        return NULL;
    ++tracker->stats.evicted;
    return &tracker->clients[victim];
}

static nero_wifi_client *ensure_client_locked(nero_wifi_tracker *tracker,
                                              const uint8_t mac[NERO_WIFI_MAC_LEN], int64_t now)
{
    if (mac_zero(mac) || nero_wifi_mac_is_group(mac))
        return NULL;
    /* Never classify a BSSID as one of its own clients. */
    if (find_ap_locked(tracker, mac) != NULL)
        return NULL;

    nero_wifi_client *client = find_client_locked(tracker, mac);
    if (client != NULL)
        return client;

    if (tracker->client_count >= NERO_WIFI_MAX_CLIENTS) {
        client = evict_client_locked(tracker);
        if (client == NULL) {
            ++tracker->stats.dropped;
            return NULL;
        }
    } else {
        client = &tracker->clients[tracker->client_count++];
    }
    memset(client, 0, sizeof(*client));
    memcpy(client->mac, mac, NERO_WIFI_MAC_LEN);
    client->randomized = nero_wifi_mac_is_randomized(mac);
    client->first_seen_us = now;
    client->last_seen_us = now;
    client->confidence = NERO_WIFI_CONFIDENCE_OBSERVED;
    client->state = NERO_WIFI_STATE_OBSERVED;
    return client;
}

static void bind_client(nero_wifi_client *client, const uint8_t bssid[NERO_WIFI_MAC_LEN],
                        nero_wifi_confidence confidence, nero_wifi_client_state state, int64_t now)
{
    if (client == NULL)
        return;
    if (bssid != NULL && !mac_zero(bssid) && !nero_wifi_mac_is_group(bssid)) {
        if (!client->has_bssid) {
            memcpy(client->bssid, bssid, NERO_WIFI_MAC_LEN);
            client->has_bssid = 1;
        } else if (!mac_equal(client->bssid, bssid)) {
            memcpy(client->bssid, bssid, NERO_WIFI_MAC_LEN);
            ++client->ap_changes;
        }
    }
    if ((int)confidence > (int)client->confidence)
        client->confidence = confidence;
    if (state == NERO_WIFI_STATE_DISASSOCIATED) {
        client->state = state;
    } else if (state == NERO_WIFI_STATE_ASSOCIATED) {
        client->state = state;
    }
    client->last_seen_us = now;
}

static void push_rssi(int history[NERO_WIFI_RSSI_HISTORY], uint8_t *count, uint8_t *next, int rssi)
{
    history[*next] = rssi;
    *next = (uint8_t)((*next + 1) % NERO_WIFI_RSSI_HISTORY);
    if (*count < NERO_WIFI_RSSI_HISTORY)
        ++*count;
}

static void update_ap_signal(nero_wifi_ap *ap, const nero_wifi_radiotap *radio)
{
    if (ap == NULL || radio == NULL)
        return;
    if (radio->has_rssi) {
        ap->has_rssi = 1;
        ap->rssi = radio->rssi;
        push_rssi(ap->rssi_history, &ap->rssi_history_count, &ap->rssi_history_next, radio->rssi);
    }
    if (radio->has_frequency) {
        ap->has_frequency = 1;
        ap->frequency_mhz = radio->frequency_mhz;
        ap->band = radio->band;
    }
    if (radio->has_channel) {
        ap->has_channel = 1;
        ap->channel = radio->channel;
    }
}

static void update_ap_channel(nero_wifi_ap *ap, int channel, int frequency, nero_wifi_band band)
{
    if (ap == NULL)
        return;
    if (frequency > 0) {
        ap->has_frequency = 1;
        ap->frequency_mhz = frequency;
        ap->band = nero_wifi_frequency_to_band(frequency);
        ap->has_channel = 1;
        ap->channel = nero_wifi_frequency_to_channel(frequency, NULL);
    } else if (channel > 0) {
        ap->has_channel = 1;
        ap->channel = channel;
        if (band != NERO_WIFI_BAND_UNKNOWN) {
            ap->band = band;
        } else if (ap->band == NERO_WIFI_BAND_UNKNOWN) {
            if (channel <= 14)
                ap->band = NERO_WIFI_BAND_2GHZ;
            else if (channel <= 196)
                ap->band = NERO_WIFI_BAND_5GHZ;
            else
                ap->band = NERO_WIFI_BAND_6GHZ;
        }
        int derived = nero_wifi_channel_to_frequency(channel, ap->band);
        if (derived > 0) {
            ap->has_frequency = 1;
            ap->frequency_mhz = derived;
        }
    }
}

static void merge_ap_info(nero_wifi_ap *ap, const nero_wifi_ies *ies, int64_t now)
{
    if (ap == NULL || ies == NULL)
        return;
    if (ies->has_ssid) {
        if (ies->hidden) {
            ap->hidden = 1;
        } else if (ies->ssid_len > 0) {
            memcpy(ap->ssid, ies->ssid, NERO_WIFI_SSID_CAP);
            ap->ssid_len = ies->ssid_len;
            ap->hidden = 0;
        }
    }
    if (ies->has_ds_channel) {
        update_ap_channel(ap, ies->ds_channel, 0, NERO_WIFI_BAND_UNKNOWN);
    }
    if (ies->security != NERO_WIFI_SEC_UNKNOWN &&
        security_rank(ies->security) >= security_rank(ap->security)) {
        ap->security = ies->security;
    }
    ap->last_seen_us = now;
}

static int is_ap_address(nero_wifi_tracker *tracker, const uint8_t mac[NERO_WIFI_MAC_LEN])
{
    return find_ap_locked(tracker, mac) != NULL;
}

static void note_client_frame(nero_wifi_client *client, uint8_t type)
{
    if (client == NULL)
        return;
    ++client->frames;
    if (type == 0)
        ++client->management_frames;
    else if (type == 2)
        ++client->data_frames;
}

static int dedup_is_duplicate_locked(nero_wifi_tracker *tracker, const nero_wifi_frame *frame,
                                     int64_t now, nero_wifi_client **duplicate_client)
{
    *duplicate_client = NULL;
    if (frame->type == 1)
        return 0;
    uint32_t hash = 2166136261u;
    for (int i = 0; i < NERO_WIFI_MAC_LEN; ++i) {
        hash ^= frame->addr2[i];
        hash *= 16777619u;
    }
    hash ^= frame->sequence;
    hash *= 16777619u;
    hash ^= frame->fragment;
    hash *= 16777619u;
    hash ^= (uint32_t)frame->type << 4 | frame->subtype;

    uint32_t start = hash % WIFI_DEDUP_SLOTS;
    for (uint32_t probe = 0; probe < WIFI_DEDUP_SLOTS; ++probe) {
        wifi_dedup_entry *entry = &tracker->dedup[(start + probe) % WIFI_DEDUP_SLOTS];
        if (!entry->used) {
            entry->used = 1;
            memcpy(entry->transmitter, frame->addr2, NERO_WIFI_MAC_LEN);
            entry->sequence = frame->sequence;
            entry->fragment = (uint8_t)frame->fragment;
            entry->type = frame->type;
            entry->subtype = frame->subtype;
            entry->timestamp_us = now;
            return 0;
        }
        if (entry->sequence == frame->sequence && entry->fragment == frame->fragment &&
            entry->type == frame->type && entry->subtype == frame->subtype &&
            mac_equal(entry->transmitter, frame->addr2)) {
            int64_t delta = now - entry->timestamp_us;
            if (delta < 0)
                delta = -delta;
            if (delta <= WIFI_DEDUP_WINDOW_US) {
                *duplicate_client = find_client_locked(tracker, frame->addr2);
                return 1;
            }
            entry->timestamp_us = now;
            return 0;
        }
    }

    /* Full table: replace in round-robin order. This bounds memory while
     * keeping the most recent sequence numbers visible. */
    wifi_dedup_entry *entry = &tracker->dedup[tracker->dedup_cursor++ % WIFI_DEDUP_SLOTS];
    memset(entry, 0, sizeof(*entry));
    entry->used = 1;
    memcpy(entry->transmitter, frame->addr2, NERO_WIFI_MAC_LEN);
    entry->sequence = frame->sequence;
    entry->fragment = (uint8_t)frame->fragment;
    entry->type = frame->type;
    entry->subtype = frame->subtype;
    entry->timestamp_us = now;
    return 0;
}

static void handle_beacon_like(nero_wifi_tracker *tracker, const nero_wifi_frame *frame,
                               const nero_wifi_radiotap *radio, int64_t now, int probe_response)
{
    const uint8_t *bssid = frame->addr3;
    if (mac_zero(bssid))
        bssid = frame->addr2;
    nero_wifi_ap *ap = ensure_ap_locked(tracker, bssid, now);
    if (ap == NULL)
        return;

    nero_wifi_ies ies;
    memset(&ies, 0, sizeof(ies));
    if (frame->body_len >= 12) {
        uint16_t capability = (uint16_t)(frame->body[10] | ((uint16_t)frame->body[11] << 8));
        nero_wifi_parse_ies(&ies, frame->body + 12, frame->body_len - 12,
                            (capability & 0x0010u) != 0);
    }
    merge_ap_info(ap, &ies, now);
    update_ap_signal(ap, radio);
    if (probe_response)
        ++ap->probe_response_frames;
    else
        ++ap->beacon_frames;
}

static void handle_probe_request(nero_wifi_tracker *tracker, const nero_wifi_frame *frame,
                                 const nero_wifi_radiotap *radio, int64_t now)
{
    nero_wifi_ies ies;
    memset(&ies, 0, sizeof(ies));
    nero_wifi_parse_ies(&ies, frame->body, frame->body_len, 0);

    nero_wifi_client *client = ensure_client_locked(tracker, frame->addr2, now);
    if (client == NULL)
        return;
    const uint8_t *bssid = frame->addr3;
    if (bssid != NULL && !mac_zero(bssid) && !nero_wifi_mac_is_group(bssid) &&
        is_ap_address(tracker, bssid)) {
        bind_client(client, bssid, NERO_WIFI_CONFIDENCE_OBSERVED, NERO_WIFI_STATE_OBSERVED, now);
    }
    if (radio != NULL && radio->has_channel) {
        client->has_channel = 1;
        client->channel = radio->channel;
        client->band = radio->band;
    }
    note_client_frame(client, 0);
}

static void handle_assoc_request(nero_wifi_tracker *tracker, const nero_wifi_frame *frame,
                                 const nero_wifi_radiotap *radio, int64_t now, int reassoc)
{
    const uint8_t *bssid = frame->addr3;
    if (mac_zero(bssid))
        bssid = frame->addr1;
    nero_wifi_ap *ap = ensure_ap_locked(tracker, bssid, now);
    nero_wifi_client *client = ensure_client_locked(tracker, frame->addr2, now);
    if (client == NULL)
        return;

    size_t offset = reassoc ? 10 : 4;
    nero_wifi_ies ies;
    memset(&ies, 0, sizeof(ies));
    if (frame->body_len >= offset) {
        int privacy = 0;
        if (frame->body_len >= 2) {
            uint16_t capability = (uint16_t)(frame->body[0] | ((uint16_t)frame->body[1] << 8));
            privacy = (capability & 0x0010u) != 0;
        }
        nero_wifi_parse_ies(&ies, frame->body + offset, frame->body_len - offset, privacy);
    }
    if (ap != NULL) {
        if (ies.has_ssid && !ies.hidden) {
            merge_ap_info(ap, &ies, now);
        } else if (ies.security != NERO_WIFI_SEC_UNKNOWN) {
            if (security_rank(ies.security) >= security_rank(ap->security)) {
                ap->security = ies.security;
            }
        }
    }
    bind_client(client, bssid, NERO_WIFI_CONFIDENCE_ASSOCIATED, NERO_WIFI_STATE_ASSOCIATED, now);
    if (radio != NULL && radio->has_rssi) {
        client->has_rssi = 1;
        client->rssi = radio->rssi;
        push_rssi(client->rssi_history, &client->rssi_history_count, &client->rssi_history_next,
                  radio->rssi);
    }
    if (radio != NULL && radio->has_channel) {
        client->has_channel = 1;
        client->channel = radio->channel;
        client->band = radio->band;
    }
    note_client_frame(client, 0);
}

static void handle_auth(nero_wifi_tracker *tracker, const nero_wifi_frame *frame,
                        const nero_wifi_radiotap *radio, int64_t now)
{
    const uint8_t *bssid = frame->addr3;
    if (mac_zero(bssid))
        bssid = frame->addr1;
    ensure_ap_locked(tracker, bssid, now);
    nero_wifi_client *client = ensure_client_locked(tracker, frame->addr2, now);
    if (client == NULL)
        return;
    bind_client(client, bssid, NERO_WIFI_CONFIDENCE_LIKELY, NERO_WIFI_STATE_ASSOCIATED, now);
    if (radio != NULL && radio->has_rssi) {
        client->has_rssi = 1;
        client->rssi = radio->rssi;
        push_rssi(client->rssi_history, &client->rssi_history_count, &client->rssi_history_next,
                  radio->rssi);
    }
    note_client_frame(client, 0);
}

static void handle_disconnect(nero_wifi_tracker *tracker, const nero_wifi_frame *frame, int64_t now)
{
    const uint8_t *client_mac = frame->addr2;
    const uint8_t *bssid = frame->addr3;
    if (is_ap_address(tracker, frame->addr2) && !is_ap_address(tracker, frame->addr1)) {
        client_mac = frame->addr1;
    }
    if (mac_zero(bssid))
        bssid = (client_mac == frame->addr1) ? frame->addr2 : frame->addr1;
    nero_wifi_client *client = ensure_client_locked(tracker, client_mac, now);
    if (client == NULL)
        return;
    bind_client(client, bssid, client->confidence, NERO_WIFI_STATE_DISASSOCIATED, now);
    note_client_frame(client, 0);
}

static void handle_data(nero_wifi_tracker *tracker, const nero_wifi_frame *frame,
                        const nero_wifi_radiotap *radio, int64_t now)
{
    const uint8_t *bssid = NULL;
    const uint8_t *client_mac = NULL;

    if (frame->to_ds && !frame->from_ds) {
        bssid = frame->addr1;
        client_mac = frame->addr2;
    } else if (!frame->to_ds && frame->from_ds) {
        bssid = frame->addr2;
        client_mac = frame->addr1;
    } else if (!frame->to_ds && !frame->from_ds) {
        bssid = frame->addr3;
        if (!mac_zero(bssid) && is_ap_address(tracker, bssid)) {
            client_mac = mac_equal(frame->addr2, bssid) ? frame->addr1 : frame->addr2;
        }
    } else {
        /* WDS: all four addresses are present, but no infrastructure AP can be
         * inferred reliably. Do not guess. */
        return;
    }

    if (bssid == NULL || mac_zero(bssid) || nero_wifi_mac_is_group(bssid))
        return;
    if (client_mac == NULL || mac_zero(client_mac) || nero_wifi_mac_is_group(client_mac))
        return;

    nero_wifi_ap *ap = is_ap_address(tracker, bssid) ? find_ap_locked(tracker, bssid) : NULL;
    if (ap == NULL && (frame->to_ds || frame->from_ds)) {
        ap = ensure_ap_locked(tracker, bssid, now);
    }
    if (ap == NULL)
        return;

    if (frame->from_ds && radio != NULL) {
        /* In FromDS, the transmitter is the AP and RSSI describes the AP. */
        update_ap_signal(ap, radio);
        if (radio->has_channel) {
            ap->has_channel = 1;
            ap->channel = radio->channel;
        }
    }

    nero_wifi_client *client = ensure_client_locked(tracker, client_mac, now);
    if (client == NULL)
        return;
    bind_client(client, bssid, NERO_WIFI_CONFIDENCE_LIKELY, NERO_WIFI_STATE_ASSOCIATED, now);
    if (!frame->from_ds && radio != NULL) {
        if (radio->has_rssi) {
            client->has_rssi = 1;
            client->rssi = radio->rssi;
            push_rssi(client->rssi_history, &client->rssi_history_count, &client->rssi_history_next,
                      radio->rssi);
        }
        if (radio->has_channel) {
            client->has_channel = 1;
            client->channel = radio->channel;
            client->band = radio->band;
        }
    }
    if (!client->has_channel && ap->has_channel) {
        client->has_channel = 1;
        client->channel = ap->channel;
        client->band = ap->band;
    }
    note_client_frame(client, 2);
}

int nero_wifi_tracker_ingest(nero_wifi_tracker *tracker, const nero_wifi_frame *frame,
                             const nero_wifi_radiotap *radio, int64_t timestamp_us)
{
    if (tracker == NULL || frame == NULL)
        return -1;
    int64_t now = timestamp_us > 0 ? timestamp_us : nero_wifi_now_us();

    pthread_mutex_lock(&tracker->mutex);

    nero_wifi_client *duplicate_client = NULL;
    if (dedup_is_duplicate_locked(tracker, frame, now, &duplicate_client)) {
        ++tracker->stats.duplicates;
        if (duplicate_client != NULL)
            ++duplicate_client->duplicates;
        pthread_mutex_unlock(&tracker->mutex);
        return 0;
    }

    ++tracker->stats.frames;
    if (frame->type == 0)
        ++tracker->stats.management;
    else if (frame->type == 1)
        ++tracker->stats.control;
    else if (frame->type == 2)
        ++tracker->stats.data;

    if (frame->type == 0) {
        switch (frame->subtype) {
        case 8: /* beacon */
            handle_beacon_like(tracker, frame, radio, now, 0);
            break;
        case 5: /* probe response */
            handle_beacon_like(tracker, frame, radio, now, 1);
            break;
        case 4: /* probe request */
            handle_probe_request(tracker, frame, radio, now);
            break;
        case 0: /* association request */
            handle_assoc_request(tracker, frame, radio, now, 0);
            break;
        case 2: /* reassociation request */
            handle_assoc_request(tracker, frame, radio, now, 1);
            break;
        case 11: /* authentication */
            handle_auth(tracker, frame, radio, now);
            break;
        case 10: /* disassociation */
        case 12: /* deauthentication */
            handle_disconnect(tracker, frame, now);
            break;
        default: {
            nero_wifi_client *client = ensure_client_locked(tracker, frame->addr2, now);
            if (client != NULL) {
                note_client_frame(client, 0);
                if (radio != NULL && radio->has_rssi) {
                    client->has_rssi = 1;
                    client->rssi = radio->rssi;
                    push_rssi(client->rssi_history, &client->rssi_history_count,
                              &client->rssi_history_next, radio->rssi);
                }
            }
            break;
        }
        }
    } else if (frame->type == 2) {
        handle_data(tracker, frame, radio, now);
    }

    pthread_mutex_unlock(&tracker->mutex);
    return 1;
}

int nero_wifi_tracker_ingest_l2(nero_wifi_tracker *tracker, const uint8_t *data, size_t len,
                                int linktype, int64_t timestamp_us, char *error, size_t error_len)
{
    if (tracker == NULL || data == NULL)
        return -1;

    nero_wifi_radiotap radio;
    memset(&radio, 0, sizeof(radio));
    const uint8_t *frame_data = data;
    size_t frame_len = len;

    if (linktype == NERO_WIFI_LINKTYPE_RADIOTAP) {
        if (nero_wifi_parse_radiotap(data, len, &radio, error, error_len) != 0)
            return -1;
        frame_data = data + radio.header_len;
        frame_len = len - radio.header_len;
    } else if (linktype != NERO_WIFI_LINKTYPE_IEEE802_11) {
        if (error != NULL && error_len > 0)
            snprintf(error, error_len, "unsupported link type");
        return -1;
    }

    nero_wifi_frame frame;
    if (nero_wifi_parse_frame(frame_data, frame_len, &frame, error, error_len) != 0) {
        pthread_mutex_lock(&tracker->mutex);
        ++tracker->stats.malformed;
        pthread_mutex_unlock(&tracker->mutex);
        return -1;
    }
    if (radio.fcs_at_end && frame.body_len >= 4)
        frame.body_len -= 4;
    if (radio.has_frequency && radio.has_channel) {
        /* radiotap normally supplies both. */
    }
    return nero_wifi_tracker_ingest(tracker, &frame, &radio, timestamp_us);
}

void nero_wifi_tracker_get_stats(const nero_wifi_tracker *tracker, nero_wifi_stats *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (tracker == NULL)
        return;
    nero_wifi_tracker *mutable_tracker = (nero_wifi_tracker *)tracker;
    pthread_mutex_lock(&mutable_tracker->mutex);
    *out = tracker->stats;
    pthread_mutex_unlock(&mutable_tracker->mutex);
}

static int ap_compare(const void *left, const void *right)
{
    const nero_wifi_ap *a = left;
    const nero_wifi_ap *b = right;
    int by_ssid = strcmp(a->ssid, b->ssid);
    if (by_ssid != 0)
        return by_ssid;
    return memcmp(a->bssid, b->bssid, NERO_WIFI_MAC_LEN);
}

static int client_compare(const void *left, const void *right)
{
    const nero_wifi_client *a = left;
    const nero_wifi_client *b = right;
    int by_ssid = strcmp(a->ssid, b->ssid);
    if (by_ssid != 0)
        return by_ssid;
    int by_bssid = memcmp(a->bssid, b->bssid, NERO_WIFI_MAC_LEN);
    if (by_bssid != 0)
        return by_bssid;
    return memcmp(a->mac, b->mac, NERO_WIFI_MAC_LEN);
}

int nero_wifi_tracker_get_aps(const nero_wifi_tracker *tracker, nero_wifi_ap *out, int capacity,
                              int channel_filter)
{
    if (tracker == NULL || out == NULL || capacity <= 0)
        return 0;
    nero_wifi_tracker *mutable_tracker = (nero_wifi_tracker *)tracker;
    pthread_mutex_lock(&mutable_tracker->mutex);

    int count = 0;
    for (int i = 0; i < tracker->ap_count && count < capacity; ++i) {
        const nero_wifi_ap *ap = &tracker->aps[i];
        if (channel_filter >= 0 && (!ap->has_channel || ap->channel != channel_filter))
            continue;
        out[count] = *ap;
        out[count].client_count = 0;
        out[count].observed_client_count = 0;
        for (int c = 0; c < tracker->client_count; ++c) {
            const nero_wifi_client *client = &tracker->clients[c];
            if (!client->has_bssid || !mac_equal(client->bssid, ap->bssid))
                continue;
            ++out[count].observed_client_count;
            if (client->confidence >= NERO_WIFI_CONFIDENCE_LIKELY)
                ++out[count].client_count;
        }
        ++count;
    }
    pthread_mutex_unlock(&mutable_tracker->mutex);
    qsort(out, (size_t)count, sizeof(out[0]), ap_compare);
    return count;
}

int nero_wifi_tracker_get_clients(const nero_wifi_tracker *tracker, nero_wifi_client *out,
                                  int capacity, int channel_filter)
{
    if (tracker == NULL || out == NULL || capacity <= 0)
        return 0;
    nero_wifi_tracker *mutable_tracker = (nero_wifi_tracker *)tracker;
    pthread_mutex_lock(&mutable_tracker->mutex);

    int count = 0;
    for (int i = 0; i < tracker->client_count && count < capacity; ++i) {
        const nero_wifi_client *client = &tracker->clients[i];
        int client_channel = client->has_channel ? client->channel : -1;
        const nero_wifi_ap *ap = NULL;
        if (client->has_bssid) {
            ap = find_ap_locked(mutable_tracker, client->bssid);
            if (ap != NULL && ap->has_channel)
                client_channel = ap->channel;
        }
        if (channel_filter >= 0 && client_channel != channel_filter)
            continue;
        out[count] = *client;
        memset(out[count].ssid, 0, sizeof(out[count].ssid));
        if (ap != NULL) {
            memcpy(out[count].ssid, ap->ssid, NERO_WIFI_SSID_CAP);
            if (ap->has_channel) {
                out[count].has_channel = 1;
                out[count].channel = ap->channel;
                out[count].band = ap->band;
            }
        }
        ++count;
    }
    pthread_mutex_unlock(&mutable_tracker->mutex);
    qsort(out, (size_t)count, sizeof(out[0]), client_compare);
    return count;
}

int nero_wifi_tracker_find_ap(const nero_wifi_tracker *tracker,
                              const uint8_t bssid[NERO_WIFI_MAC_LEN], nero_wifi_ap *out)
{
    if (tracker == NULL || bssid == NULL || out == NULL)
        return -1;
    nero_wifi_tracker *mutable_tracker = (nero_wifi_tracker *)tracker;
    pthread_mutex_lock(&mutable_tracker->mutex);
    nero_wifi_ap *ap = find_ap_locked(mutable_tracker, bssid);
    if (ap != NULL) {
        *out = *ap;
        out->client_count = 0;
        out->observed_client_count = 0;
        for (int c = 0; c < tracker->client_count; ++c) {
            const nero_wifi_client *client = &tracker->clients[c];
            if (!client->has_bssid || !mac_equal(client->bssid, ap->bssid))
                continue;
            ++out->observed_client_count;
            if (client->confidence >= NERO_WIFI_CONFIDENCE_LIKELY)
                ++out->client_count;
        }
    }
    pthread_mutex_unlock(&mutable_tracker->mutex);
    return ap != NULL ? 0 : -1;
}

int nero_wifi_tracker_find_client(const nero_wifi_tracker *tracker,
                                  const uint8_t mac[NERO_WIFI_MAC_LEN], nero_wifi_client *out)
{
    if (tracker == NULL || mac == NULL || out == NULL)
        return -1;
    nero_wifi_tracker *mutable_tracker = (nero_wifi_tracker *)tracker;
    pthread_mutex_lock(&mutable_tracker->mutex);
    nero_wifi_client *client = find_client_locked(mutable_tracker, mac);
    if (client != NULL) {
        *out = *client;
        memset(out->ssid, 0, sizeof(out->ssid));
        if (client->has_bssid) {
            nero_wifi_ap *ap = find_ap_locked(mutable_tracker, client->bssid);
            if (ap != NULL) {
                memcpy(out->ssid, ap->ssid, NERO_WIFI_SSID_CAP);
                if (ap->has_channel) {
                    out->has_channel = 1;
                    out->channel = ap->channel;
                    out->band = ap->band;
                }
            }
        }
    }
    pthread_mutex_unlock(&mutable_tracker->mutex);
    return client != NULL ? 0 : -1;
}
