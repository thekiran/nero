#ifndef NERO_WIFI_NL80211_H
#define NERO_WIFI_NL80211_H

/* Linux nl80211/Generic Netlink interface control.
 *
 * This module only queries interface/wiphy capabilities and changes an
 * interface type after the caller has obtained explicit confirmation. It never
 * injects frames, changes channels, modifies regulatory settings, or disables
 * NetworkManager. On platforms without nl80211 every operation fails with an
 * explicit error instead of pretending to succeed. */

#include <stddef.h>
#include <stdint.h>

#define NERO_NL80211_IFNAME_MAX 64
#define NERO_NL80211_MAC_TEXT_MAX 18
#define NERO_NL80211_DRIVER_MAX 128
#define NERO_NL80211_PHY_MAX 32
#define NERO_NL80211_REG_MAX 8

/* Netlink nl80211 interface types (stable UAPI values). */
enum {
    NERO_NL80211_IFTYPE_UNSPECIFIED = 0,
    NERO_NL80211_IFTYPE_ADHOC = 1,
    NERO_NL80211_IFTYPE_STATION = 2,
    NERO_NL80211_IFTYPE_AP = 3,
    NERO_NL80211_IFTYPE_AP_VLAN = 4,
    NERO_NL80211_IFTYPE_WDS = 5,
    NERO_NL80211_IFTYPE_MONITOR = 6,
    NERO_NL80211_IFTYPE_MESH_POINT = 7,
    NERO_NL80211_IFTYPE_P2P_CLIENT = 8,
    NERO_NL80211_IFTYPE_P2P_GO = 9,
    NERO_NL80211_IFTYPE_P2P_DEVICE = 10,
    NERO_NL80211_IFTYPE_OCB = 11,
    NERO_NL80211_IFTYPE_NAN = 12
};

/* Bit positions used by nero_nl80211_info.bands. */
enum {
    NERO_NL80211_BAND_2GHZ = 1u << 0,
    NERO_NL80211_BAND_5GHZ = 1u << 1,
    NERO_NL80211_BAND_6GHZ = 1u << 2,
    NERO_NL80211_BAND_60GHZ = 1u << 3
};

typedef struct {
    int available;           /* backend socket/protocol usable */
    char interface[NERO_NL80211_IFNAME_MAX];
    int ifindex;
    int wiphy;
    int has_iftype;
    int iftype;              /* NERO_NL80211_IFTYPE_* */
    char mac[NERO_NL80211_MAC_TEXT_MAX];
    char driver[NERO_NL80211_DRIVER_MAX];
    char phy_name[NERO_NL80211_PHY_MAX];
    int has_supported_iftypes;
    uint32_t supported_iftypes; /* bit (1u << iftype) */
    int monitor_supported;
    unsigned bands;          /* NERO_NL80211_BAND_* bitmask */
    int channel_count;       /* enabled frequencies reported by the wiphy */
    int disabled_channel_count;
    int no_ir_channel_count;
    char regulatory[NERO_NL80211_REG_MAX];
} nero_nl80211_info;

/* Returns non-zero when the Generic Netlink transport can be created. This does
 * not prove CAP_NET_ADMIN or driver monitor-mode support. */
int nero_nl80211_available(void);

/* Queries interface + wiphy metadata. Returns 0 on success, -1 with an explicit
 * error otherwise. The interface must already exist. */
int nero_nl80211_get_info(const char *interface, nero_nl80211_info *out, char *error,
                          size_t error_len);

/* Requests an interface type change, then re-reads the interface and verifies
 * that the kernel reports the requested type. Returns 0 only after verification
 * succeeds. `verified` may be NULL. */
int nero_nl80211_set_type(const char *interface, int iftype, nero_nl80211_info *verified,
                          char *error, size_t error_len);

/* Tunes a monitor-mode interface to `freq_mhz` (a 20 MHz, no-HT channel) via
 * NL80211_CMD_SET_CHANNEL. This changes the radio, so the interface must be in
 * monitor mode and the caller needs CAP_NET_ADMIN/root; the kernel ACK is
 * required. Returns 0 on success, -1 with an explicit error otherwise. Only
 * available on Linux with nl80211; every other build fails explicitly. */
int nero_nl80211_set_frequency(const char *interface, int freq_mhz, char *error, size_t error_len);

const char *nero_nl80211_iftype_name(int iftype);
const char *nero_nl80211_band_summary(unsigned bands, char *buffer, size_t buffer_len);

#if defined(NERO_NL80211_TEST)
/* Test-only transport injection. The exchange callback receives one complete
 * netlink request and must copy one complete reply datagram into `reply`. It
 * returns 0 on success, -1 with `error` filled otherwise. `context` is owned by
 * the caller. Passing a NULL callback restores the real transport. */
typedef int (*nero_nl80211_exchange_fn)(void *context, const void *request, size_t request_len,
                                        void *reply, size_t reply_capacity, size_t *reply_len,
                                        char *error, size_t error_len);
void nero_nl80211_test_set_exchange(void *context, nero_nl80211_exchange_fn exchange);
void nero_nl80211_test_reset(void);
#endif

#endif
