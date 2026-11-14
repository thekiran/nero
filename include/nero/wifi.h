#ifndef NERO_WIFI_H
#define NERO_WIFI_H

/* Passive IEEE 802.11 discovery core.
 *
 * This header is deliberately free of capture, terminal and operating-system
 * dependencies. A platform backend feeds raw link-layer frames into the
 * tracker; the CLI renders snapshots. No function in this module performs
 * active probing, packet injection or decryption. */

#include <stddef.h>
#include <stdint.h>

#define NERO_WIFI_MAC_LEN 6
#define NERO_WIFI_SSID_MAX 32
#define NERO_WIFI_SSID_CAP (NERO_WIFI_SSID_MAX + 1)
#define NERO_WIFI_IFNAME_MAX 64
#define NERO_WIFI_PATH_MAX 1024
#define NERO_WIFI_MAX_APS 256
#define NERO_WIFI_MAX_CLIENTS 1024
#define NERO_WIFI_RSSI_HISTORY 12

#define NERO_WIFI_LINKTYPE_ETHERNET 1
#define NERO_WIFI_LINKTYPE_IEEE802_11 105
#define NERO_WIFI_LINKTYPE_RADIOTAP 127

typedef enum {
    NERO_WIFI_SEC_UNKNOWN = 0,
    NERO_WIFI_SEC_OPEN,
    NERO_WIFI_SEC_WEP,
    NERO_WIFI_SEC_WPA,
    NERO_WIFI_SEC_WPA2,
    NERO_WIFI_SEC_WPA2_WPA3,
    NERO_WIFI_SEC_WPA3,
    NERO_WIFI_SEC_OWE
} nero_wifi_security;

typedef enum {
    NERO_WIFI_BAND_UNKNOWN = 0,
    NERO_WIFI_BAND_2GHZ,
    NERO_WIFI_BAND_5GHZ,
    NERO_WIFI_BAND_6GHZ
} nero_wifi_band;

/* Confidence describes how the client-to-AP relationship was inferred.
 * OBSERVED   - the MAC was seen, but no AP relationship is proven.
 * LIKELY     - 802.11 data/authentication traffic involved a known BSSID.
 * ASSOCIATED - an association/reassociation request was observed.
 * "ASSOCIATED" still means observed on the air, not confirmed by the AP. */
typedef enum {
    NERO_WIFI_CONFIDENCE_OBSERVED = 0,
    NERO_WIFI_CONFIDENCE_LIKELY,
    NERO_WIFI_CONFIDENCE_ASSOCIATED
} nero_wifi_confidence;

typedef enum {
    NERO_WIFI_STATE_OBSERVED = 0,
    NERO_WIFI_STATE_ASSOCIATED,
    NERO_WIFI_STATE_DISASSOCIATED
} nero_wifi_client_state;

typedef struct {
    uint8_t bssid[NERO_WIFI_MAC_LEN];
    int has_bssid; /* 0 when macOS redacted the BSSID */
    char ssid[NERO_WIFI_SSID_CAP];
    uint8_t ssid_len;
    int hidden;   /* zero-length or all-zero SSID; never fabricated */
    int redacted; /* platform withheld the identifier */
    int has_channel;
    int channel;
    int has_frequency;
    int frequency_mhz;
    nero_wifi_band band;
    int has_rssi;
    int rssi;
    nero_wifi_security security;
    int64_t first_seen_us;
    int64_t last_seen_us;
    uint32_t beacon_frames;
    uint32_t probe_response_frames;
    uint32_t other_frames;
    int client_count;          /* LIKELY or ASSOCIATED relationships */
    int observed_client_count; /* every client seen touching this BSSID */
    int rssi_history[NERO_WIFI_RSSI_HISTORY];
    uint8_t rssi_history_count;
    uint8_t rssi_history_next;
} nero_wifi_ap;

typedef struct {
    uint8_t mac[NERO_WIFI_MAC_LEN];
    int has_bssid;
    uint8_t bssid[NERO_WIFI_MAC_LEN];
    char ssid[NERO_WIFI_SSID_CAP]; /* resolved from the AP at snapshot time */
    nero_wifi_confidence confidence;
    nero_wifi_client_state state;
    int randomized; /* locally administered bit set */
    int has_rssi;
    int rssi;
    int has_channel;
    int channel;
    nero_wifi_band band;
    int64_t first_seen_us;
    int64_t last_seen_us;
    uint32_t frames;
    uint32_t management_frames;
    uint32_t data_frames;
    uint32_t duplicates;
    uint32_t ap_changes;
    int rssi_history[NERO_WIFI_RSSI_HISTORY];
    uint8_t rssi_history_count;
    uint8_t rssi_history_next;
} nero_wifi_client;

/* Decoded 802.11 frame header. body points into the caller's receive buffer
 * and is only valid for the duration of nero_wifi_tracker_ingest(). */
typedef struct {
    uint8_t type;
    uint8_t subtype;
    int to_ds;
    int from_ds;
    int retry;
    int protected_frame;
    uint8_t addr1[NERO_WIFI_MAC_LEN];
    uint8_t addr2[NERO_WIFI_MAC_LEN];
    uint8_t addr3[NERO_WIFI_MAC_LEN];
    uint8_t addr4[NERO_WIFI_MAC_LEN];
    int has_addr4;
    uint16_t sequence;
    uint16_t fragment;
    size_t header_len;
    const uint8_t *body;
    size_t body_len;
} nero_wifi_frame;

typedef struct {
    size_t header_len;
    int has_tsft;
    uint64_t tsft;
    int has_flags;
    uint8_t flags;
    int has_rssi;
    int rssi;
    int has_noise;
    int noise;
    int has_channel;
    int channel;
    int has_frequency;
    int frequency_mhz;
    nero_wifi_band band;
    int fcs_at_end;
} nero_wifi_radiotap;

typedef struct {
    uint64_t packets;
    uint64_t frames;
    uint64_t bytes;
    uint64_t management;
    uint64_t control;
    uint64_t data;
    uint64_t malformed;
    uint64_t non_wifi;
    uint64_t duplicates;
    uint64_t dropped;
    uint64_t evicted;
    int64_t started_us;
    int64_t ended_us;
    int linktype;
    int source_is_file;
    int monitor_requested;
    int monitor_set;
    int link_fallback;
    char interface[NERO_WIFI_IFNAME_MAX];
    char backend[64];
    char source[NERO_WIFI_PATH_MAX];
} nero_wifi_stats;

typedef struct nero_wifi_tracker nero_wifi_tracker;

nero_wifi_tracker *nero_wifi_tracker_create(void);
void nero_wifi_tracker_destroy(nero_wifi_tracker *tracker);
void nero_wifi_tracker_reset(nero_wifi_tracker *tracker);

/* Parse helpers. Return 0 on success, -1 on malformed input. */
int nero_wifi_parse_radiotap(const uint8_t *data, size_t len, nero_wifi_radiotap *out, char *error,
                             size_t error_len);
int nero_wifi_parse_frame(const uint8_t *data, size_t len, nero_wifi_frame *out, char *error,
                          size_t error_len);

/* Decode one radiotap/IEEE802_11 packet and update the tracker.
 * Only linktypes 105 and 127 are accepted. */
int nero_wifi_tracker_ingest_l2(nero_wifi_tracker *tracker, const uint8_t *data, size_t len,
                                int linktype, int64_t timestamp_us, char *error, size_t error_len);

/* Ingest an already-decoded frame. Timestamp is microseconds since the epoch
 * for files or an equivalent monotonic value for live capture. */
int nero_wifi_tracker_ingest(nero_wifi_tracker *tracker, const nero_wifi_frame *frame,
                             const nero_wifi_radiotap *radio, int64_t timestamp_us);

void nero_wifi_tracker_get_stats(const nero_wifi_tracker *tracker, nero_wifi_stats *out);

/* Snapshots copy at most capacity entries and return the number copied.
 * channel_filter < 0 disables filtering. Results are ordered predictably. */
int nero_wifi_tracker_get_aps(const nero_wifi_tracker *tracker, nero_wifi_ap *out, int capacity,
                              int channel_filter);
int nero_wifi_tracker_get_clients(const nero_wifi_tracker *tracker, nero_wifi_client *out,
                                  int capacity, int channel_filter);
int nero_wifi_tracker_find_ap(const nero_wifi_tracker *tracker,
                              const uint8_t bssid[NERO_WIFI_MAC_LEN], nero_wifi_ap *out);
int nero_wifi_tracker_find_client(const nero_wifi_tracker *tracker,
                                  const uint8_t mac[NERO_WIFI_MAC_LEN], nero_wifi_client *out);

/* Offline analysis of a classic pcap or pcapng file into a tracker. */
int nero_wifi_analyze_pcap(nero_wifi_tracker *tracker, const char *path, nero_wifi_stats *stats,
                           char *error, size_t error_len);

/* Capture-session manager. A session owns a tracker and one optional worker
 * thread. Live capture is read-only and cancelled cooperatively. */
typedef struct nero_wifi_session nero_wifi_session;

nero_wifi_session *nero_wifi_session_create(void);
void nero_wifi_session_destroy(nero_wifi_session *session);
int nero_wifi_session_start(nero_wifi_session *session, const char *interface_name,
                            int request_monitor_mode, char *error, size_t error_len);
int nero_wifi_session_stop(nero_wifi_session *session);
int nero_wifi_session_running(const nero_wifi_session *session);
int nero_wifi_session_analyze(nero_wifi_session *session, const char *path, char *error,
                              size_t error_len);
void nero_wifi_session_reset(nero_wifi_session *session);
void nero_wifi_session_snapshot_stats(const nero_wifi_session *session, nero_wifi_stats *out);
int nero_wifi_session_get_aps(const nero_wifi_session *session, nero_wifi_ap *out, int capacity,
                              int channel_filter);
int nero_wifi_session_get_clients(const nero_wifi_session *session, nero_wifi_client *out,
                                  int capacity, int channel_filter);
int nero_wifi_session_find_ap(const nero_wifi_session *session,
                              const uint8_t bssid[NERO_WIFI_MAC_LEN], nero_wifi_ap *out);
int nero_wifi_session_find_client(const nero_wifi_session *session,
                                  const uint8_t mac[NERO_WIFI_MAC_LEN], nero_wifi_client *out);
/* Session metadata is copied under the session mutex; the returned length is
 * the number of characters written, excluding the terminator. */
size_t nero_wifi_session_interface(const nero_wifi_session *session, char *out, size_t out_len);
size_t nero_wifi_session_last_error(const nero_wifi_session *session, char *out, size_t out_len);

/* Shared formatting helpers. */
int64_t nero_wifi_now_us(void);
void nero_wifi_format_mac(const uint8_t mac[NERO_WIFI_MAC_LEN], char *out, size_t out_len);
int nero_wifi_parse_mac(const char *text, uint8_t out[NERO_WIFI_MAC_LEN]);
void nero_wifi_format_time(int64_t timestamp_us, char *out, size_t out_len);
int nero_wifi_mac_is_randomized(const uint8_t mac[NERO_WIFI_MAC_LEN]);
int nero_wifi_mac_is_group(const uint8_t mac[NERO_WIFI_MAC_LEN]);
int nero_wifi_channel_to_frequency(int channel, nero_wifi_band band);
const char *nero_wifi_security_name(nero_wifi_security security);
const char *nero_wifi_band_name(nero_wifi_band band);
const char *nero_wifi_confidence_name(nero_wifi_confidence confidence);
const char *nero_wifi_state_name(nero_wifi_client_state state);

#endif
